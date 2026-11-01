#pragma once

#include <cstddef>
#include <cstdint>

#include "audio/AudioTypes.h"

namespace qaudio {

WaveFacts parseWaveFacts(const uint8_t* bytes, size_t fileBytes);

constexpr uint8_t silenceByteFor(uint16_t bitsPerSample) {
    return bitsPerSample == 8 ? 0x80 : 0x00;
}

constexpr uint32_t kCursorExpired = 0xFFFFFFFFu;

constexpr uint32_t resumeCursorBytes(const WaveFacts& facts,
                                     uint32_t ageFrames) {
    if (!facts.valid || facts.byteRate == 0u) return kCursorExpired;
    if (!facts.looping && facts.durationFrames != 0u
        && ageFrames >= facts.durationFrames) {
        return kCursorExpired;
    }
    const uint64_t raw = static_cast<uint64_t>(ageFrames)
                       * static_cast<uint64_t>(facts.byteRate) * 100ull
                       / 6060ull;
    uint64_t aligned = raw;
    if (facts.blockAlign > 1u)
        aligned -= raw % facts.blockAlign;
    if (aligned >= facts.dataBytes && !facts.looping) return kCursorExpired;
    return static_cast<uint32_t>(
        aligned >= facts.dataBytes ? aligned % facts.dataBytes : aligned);
}

WaveFacts loadWaveFactsFromFile(const char* path);

}
