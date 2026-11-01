#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace qclient {

struct PcmSamples {
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    std::vector<double> interleaved;
};

inline bool decodePcmWave(const uint8_t* bytes, std::size_t size,
                          PcmSamples& out) {
    out = PcmSamples{};
    if (bytes == nullptr || size < 12u
        || std::memcmp(bytes, "RIFF", 4) != 0
        || std::memcmp(bytes + 8, "WAVE", 4) != 0) {
        return false;
    }
    const auto u16 = [bytes](std::size_t at) {
        return static_cast<uint16_t>(bytes[at] | (bytes[at + 1u] << 8));
    };
    const auto u32 = [bytes](std::size_t at) {
        return static_cast<uint32_t>(bytes[at])
            | (static_cast<uint32_t>(bytes[at + 1u]) << 8)
            | (static_cast<uint32_t>(bytes[at + 2u]) << 16)
            | (static_cast<uint32_t>(bytes[at + 3u]) << 24);
    };

    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t rate = 0;
    bool haveFormat = false;
    std::size_t position = 12u;
    while (size - position >= 8u) {
        const uint32_t length = u32(position + 4u);
        const std::size_t body = position + 8u;
        if (length > size - body) return false;
        if (std::memcmp(bytes + position, "fmt ", 4) == 0) {
            if (length < 16u) return false;
            format = u16(body);
            channels = u16(body + 2u);
            rate = u32(body + 4u);
            bits = u16(body + 14u);
            haveFormat = true;
        } else if (std::memcmp(bytes + position, "data", 4) == 0) {
            if (!haveFormat || format != 1u || channels == 0u || rate == 0u)
                return false;
            if (bits == 8u) {
                out.interleaved.reserve(length);
                for (std::size_t i = 0; i < length; ++i) {
                    out.interleaved.push_back(
                        (static_cast<double>(bytes[body + i]) - 128.0)
                        / 128.0);
                }
            } else if (bits == 16u) {
                out.interleaved.reserve(length / 2u);
                for (std::size_t i = 0; i + 1u < length; i += 2u) {
                    out.interleaved.push_back(
                        static_cast<double>(
                            static_cast<int16_t>(u16(body + i)))
                        / 32768.0);
                }
            } else {
                return false;
            }
            out.interleaved.resize(out.interleaved.size()
                                   - out.interleaved.size() % channels);
            out.channels = channels;
            out.sampleRate = rate;
            return true;
        }
        // RIFF chunks are word-aligned: an odd length carries one pad byte.
        const std::size_t advance =
            8u + static_cast<std::size_t>(length) + (length & 1u);
        if (advance > size - position) break;
        position += advance;
    }
    return false;
}

inline double loudestWindowRms(const PcmSamples& pcm,
                               double windowSeconds = 0.4) {
    const std::size_t count = pcm.interleaved.size();
    if (count == 0u) return 0.0;
    std::size_t window = static_cast<std::size_t>(
        windowSeconds * static_cast<double>(pcm.sampleRate))
        * pcm.channels;
    if (window == 0u || window > count) window = count;
    const std::size_t step = window >= 4u ? window / 4u : 1u;
    std::vector<double> energy(count + 1u, 0.0);
    for (std::size_t i = 0; i < count; ++i)
        energy[i + 1u] = energy[i] + pcm.interleaved[i] * pcm.interleaved[i];
    double loudest = 0.0;
    for (std::size_t start = 0;; start += step) {
        std::size_t end = start + window;
        if (end > count) {
            start = count - window;
            end = count;
        }
        const double mean =
            (energy[end] - energy[start]) / static_cast<double>(window);
        if (mean > loudest) loudest = mean;
        if (end == count) break;
    }
    return std::sqrt(loudest);
}

inline double gainForLoudness(double loudestRms, double targetDbfs) {
    if (!(loudestRms > 0.0)) return 1.0;
    const double target = std::pow(10.0, targetDbfs / 20.0);
    return target < loudestRms ? target / loudestRms : 1.0;
}

inline std::vector<uint8_t> encodePcm16Wave(const PcmSamples& pcm,
                                            double gain) {
    const uint32_t dataBytes =
        static_cast<uint32_t>(pcm.interleaved.size() * 2u);
    std::vector<uint8_t> w;
    w.reserve(44u + dataBytes);
    const auto u16 = [&w](uint16_t v) {
        w.push_back(static_cast<uint8_t>(v));
        w.push_back(static_cast<uint8_t>(v >> 8));
    };
    const auto u32 = [&w](uint32_t v) {
        for (int shift = 0; shift < 32; shift += 8)
            w.push_back(static_cast<uint8_t>(v >> shift));
    };
    const auto tag = [&w](const char* four) {
        w.insert(w.end(), four, four + 4);
    };
    tag("RIFF"); u32(36u + dataBytes); tag("WAVE");
    tag("fmt "); u32(16u);
    u16(1u); u16(pcm.channels); u32(pcm.sampleRate);
    u32(pcm.sampleRate * pcm.channels * 2u);
    u16(static_cast<uint16_t>(pcm.channels * 2u)); u16(16u);
    tag("data"); u32(dataBytes);
    for (double sample : pcm.interleaved) {
        long value = std::lround(sample * gain * 32767.0);
        if (value < -32768) value = -32768;
        if (value > 32767) value = 32767;
        u16(static_cast<uint16_t>(static_cast<int16_t>(value)));
    }
    return w;
}

inline std::vector<uint8_t> quietedPcmWave(const uint8_t* bytes,
                                           std::size_t size,
                                           double targetDbfs,
                                           double* appliedGainDb = nullptr) {
    PcmSamples pcm;
    if (!(targetDbfs <= 0.0) || !decodePcmWave(bytes, size, pcm)
        || pcm.interleaved.empty()) {
        return {};
    }
    const double gain =
        gainForLoudness(loudestWindowRms(pcm), targetDbfs);
    if (appliedGainDb != nullptr) *appliedGainDb = 20.0 * std::log10(gain);
    return encodePcm16Wave(pcm, gain);
}

}
