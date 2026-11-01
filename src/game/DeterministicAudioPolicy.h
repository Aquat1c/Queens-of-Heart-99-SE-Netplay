#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace qgame {

inline constexpr std::array<int, 11> kSimulationStatusSoundIds = {
    2, 4, 6, 8, 162, 166, 167, 168, 169, 170, 171,
};

constexpr int simulationStatusSoundIndex(int sound) {
    for (std::size_t i = 0; i < kSimulationStatusSoundIds.size(); ++i) {
        if (kSimulationStatusSoundIds[i] == sound)
            return static_cast<int>(i);
    }
    return -1;
}

constexpr bool soundStatusAffectsSimulation(int sound) {
    return simulationStatusSoundIndex(sound) >= 0;
}

// The owned tick is 60.6 Hz; RIFF duration uses integer ceiling so every peer expires a one-shot on the same tick.
constexpr std::uint32_t kFallbackSoundDurationFrames = 61u;

// Canonical frame lengths of the simulation-gating sounds: Round.wav 64, 01.wav 33, 02.wav 35, 03.wav 52, 04.wav 41, 05.wav 42, DoubleKO.wav 99.
constexpr std::uint32_t canonicalStatusSoundFrames(int sound) {
    switch (sound) {
        case 162: return 64u;
        case 166: return 33u;
        case 167: return 35u;
        case 168: return 52u;
        case 169: return 41u;
        case 170: return 42u;
        case 171: return 99u;
        default: return kFallbackSoundDurationFrames;
    }
}

constexpr std::uint32_t waveDurationFrames(
    std::uint32_t dataBytes,
    std::uint32_t averageBytesPerSecond) {
    if (dataBytes == 0u || averageBytesPerSecond == 0u)
        return kFallbackSoundDurationFrames;
    const std::uint64_t numerator =
        static_cast<std::uint64_t>(dataBytes) * 606u;
    const std::uint64_t denominator =
        static_cast<std::uint64_t>(averageBytesPerSecond) * 10u;
    const std::uint64_t frames =
        (numerator + denominator - 1u) / denominator;
    return frames == 0u || frames > UINT32_MAX
        ? kFallbackSoundDurationFrames
        : static_cast<std::uint32_t>(frames);
}

// Seven stock character WAVs omit the RIFF pad byte when an odd-sized data chunk ends exactly at EOF.
constexpr std::uint64_t waveChunkNextOffset(
    std::uint64_t payloadOffset,
    std::uint32_t chunkBytes,
    std::uint64_t fileBytes) {
    if (payloadOffset > fileBytes
        || static_cast<std::uint64_t>(chunkBytes)
            > fileBytes - payloadOffset) {
        return UINT64_MAX;
    }
    const std::uint64_t rawEnd =
        payloadOffset + static_cast<std::uint64_t>(chunkBytes);
    return rawEnd
        + (((chunkBytes & 1u) != 0u && rawEnd < fileBytes) ? 1u : 0u);
}

}
