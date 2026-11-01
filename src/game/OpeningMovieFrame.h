#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "miniz.h"

namespace qmovie {

constexpr uint32_t kQ99VFourcc = 0x56393951u;
constexpr uint8_t kQ99VVersion = 1u;
constexpr size_t kQ99VHeaderSize = 16u;

enum class FrameKind : uint8_t { Key = 0, Delta = 1 };
enum class Predictor : uint8_t { None = 0, Sub = 1, Up = 2 };

struct FrameHeader {
    FrameKind kind = FrameKind::Key;
    Predictor predictor = Predictor::None;
    uint32_t sequence = 0u;
    uint32_t rawSize = 0u;
};

inline constexpr size_t planeBytes(uint32_t width, uint32_t height) {
    return static_cast<size_t>(width) * height
        + 2u * (static_cast<size_t>(width) / 4u) * (height / 4u);
}

inline bool parseFrameHeader(const uint8_t* payload, size_t size, FrameHeader& out) {
    if (payload == nullptr || size < kQ99VHeaderSize) return false;
    if (std::memcmp(payload, "Q99V", 4) != 0) return false;
    if (payload[4] != kQ99VVersion) return false;
    if (payload[5] > 1u || payload[6] > 2u) return false;
    out.kind = static_cast<FrameKind>(payload[5]);
    out.predictor = static_cast<Predictor>(payload[6]);
    std::memcpy(&out.sequence, payload + 8, 4);
    std::memcpy(&out.rawSize, payload + 12, 4);
    return true;
}

inline void unpredictPlane(uint8_t* plane, uint32_t width, uint32_t height,
                           Predictor predictor) {
    if (predictor == Predictor::Sub) {
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t* row = plane + static_cast<size_t>(y) * width;
            for (uint32_t x = 1; x < width; ++x)
                row[x] = static_cast<uint8_t>(row[x] + row[x - 1]);
        }
    } else if (predictor == Predictor::Up) {
        for (uint32_t y = 1; y < height; ++y) {
            uint8_t* row = plane + static_cast<size_t>(y) * width;
            const uint8_t* above = row - width;
            for (uint32_t x = 0; x < width; ++x)
                row[x] = static_cast<uint8_t>(row[x] + above[x]);
        }
    }
}

inline void predictPlane(const uint8_t* plane, uint8_t* out, uint32_t width,
                         uint32_t height, Predictor predictor) {
    const size_t n = static_cast<size_t>(width) * height;
    std::memcpy(out, plane, n);
    if (predictor == Predictor::Sub) {
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* row = plane + static_cast<size_t>(y) * width;
            uint8_t* o = out + static_cast<size_t>(y) * width;
            for (uint32_t x = 1; x < width; ++x)
                o[x] = static_cast<uint8_t>(row[x] - row[x - 1]);
        }
    } else if (predictor == Predictor::Up) {
        for (uint32_t y = 1; y < height; ++y) {
            const uint8_t* row = plane + static_cast<size_t>(y) * width;
            const uint8_t* above = row - width;
            uint8_t* o = out + static_cast<size_t>(y) * width;
            for (uint32_t x = 0; x < width; ++x)
                o[x] = static_cast<uint8_t>(row[x] - above[x]);
        }
    }
}

enum class DecodeStatus {
    Ok,
    BadHeader,
    BadSize,
    Inflate,
    DeltaWithoutKey,
    BadOutput,
};

class FrameDecoder {
public:
    bool begin(uint32_t width, uint32_t height) {
        if (width == 0u || height == 0u || (width % 4u) != 0u || (height % 4u) != 0u)
            return false;
        width_ = width;
        height_ = height;
        raw_ = planeBytes(width, height);
        cur_.assign(raw_, 0u);
        prev_.assign(raw_, 0u);
        havePrev_ = false;
        expected_ = 0u;
        buildChromaWeights();
        return true;
    }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool ready() const { return raw_ != 0u; }
    const std::vector<uint8_t>& lastPlanes() const { return prev_; }
    bool hasFrame() const { return havePrev_; }

    DecodeStatus decode(const uint8_t* payload, size_t payloadSize,
                        uint8_t* dib, size_t dibSize, int32_t dibHeight,
                        uint32_t bytesPerPixel, bool* outOfSequence) {
        if (outOfSequence != nullptr) *outOfSequence = false;
        if (!ready()) return DecodeStatus::BadOutput;
        FrameHeader header;
        if (!parseFrameHeader(payload, payloadSize, header)) return DecodeStatus::BadHeader;
        if (header.rawSize != raw_) return DecodeStatus::BadSize;
        if (header.kind == FrameKind::Delta && !havePrev_) return DecodeStatus::DeltaWithoutKey;
        if (bytesPerPixel != 3u && bytesPerPixel != 4u) return DecodeStatus::BadOutput;
        const size_t stride = dibStride(bytesPerPixel);
        if (dib == nullptr || dibSize < stride * height_) return DecodeStatus::BadOutput;

        mz_ulong produced = static_cast<mz_ulong>(raw_);
        const int rc = mz_uncompress(cur_.data(), &produced, payload + kQ99VHeaderSize,
                                     static_cast<mz_ulong>(payloadSize - kQ99VHeaderSize));
        if (rc != MZ_OK || produced != raw_) return DecodeStatus::Inflate;

        if (header.kind == FrameKind::Key) {
            uint8_t* y = cur_.data();
            uint8_t* u = y + static_cast<size_t>(width_) * height_;
            uint8_t* v = u + (static_cast<size_t>(width_) / 4u) * (height_ / 4u);
            unpredictPlane(y, width_, height_, header.predictor);
            unpredictPlane(u, width_ / 4u, height_ / 4u, header.predictor);
            unpredictPlane(v, width_ / 4u, height_ / 4u, header.predictor);
        } else {
            for (size_t i = 0; i < raw_; ++i)
                cur_[i] = static_cast<uint8_t>(cur_[i] + prev_[i]);
        }
        if (outOfSequence != nullptr && havePrev_ && header.sequence != expected_)
            *outOfSequence = true;
        expected_ = header.sequence + 1u;
        prev_.swap(cur_);
        havePrev_ = true;
        convertToDib(prev_.data(), dib, dibHeight, bytesPerPixel);
        return DecodeStatus::Ok;
    }

    size_t dibStride(uint32_t bytesPerPixel) const {
        return (static_cast<size_t>(width_) * bytesPerPixel + 3u) & ~static_cast<size_t>(3u);
    }

private:
    struct Tap { uint32_t lo, hi; uint32_t wHi; };

    void buildChromaWeights() {
        xTaps_.assign(width_, Tap{});
        yTaps_.assign(height_, Tap{});
        fillTaps(xTaps_, width_, width_ / 4u);
        fillTaps(yTaps_, height_, height_ / 4u);
    }

    static void fillTaps(std::vector<Tap>& taps, uint32_t full, uint32_t sub) {
        for (uint32_t i = 0; i < full; ++i) {
            const int32_t pos256 = static_cast<int32_t>(i) * 64 - 96;
            int32_t lo = pos256 >> 8;
            int32_t frac = pos256 - (lo << 8);
            if (lo < 0) { lo = 0; frac = 0; }
            int32_t hi = lo + 1;
            if (hi >= static_cast<int32_t>(sub)) { hi = static_cast<int32_t>(sub) - 1; frac = 0; }
            if (lo >= static_cast<int32_t>(sub)) lo = static_cast<int32_t>(sub) - 1;
            taps[i] = Tap{static_cast<uint32_t>(lo), static_cast<uint32_t>(hi),
                          static_cast<uint32_t>(frac)};
        }
    }

    static uint8_t clamp8(int32_t v) {
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    void convertToDib(const uint8_t* planes, uint8_t* dib, int32_t dibHeight,
                      uint32_t bytesPerPixel) const {
        const uint32_t cw = width_ / 4u;
        const uint8_t* yPlane = planes;
        const uint8_t* uPlane = yPlane + static_cast<size_t>(width_) * height_;
        const uint8_t* vPlane = uPlane + static_cast<size_t>(cw) * (height_ / 4u);
        const size_t stride = dibStride(bytesPerPixel);
        const bool bottomUp = dibHeight >= 0;
        for (uint32_t y = 0; y < height_; ++y) {
            const Tap ty = yTaps_[y];
            const uint8_t* uRowLo = uPlane + static_cast<size_t>(ty.lo) * cw;
            const uint8_t* uRowHi = uPlane + static_cast<size_t>(ty.hi) * cw;
            const uint8_t* vRowLo = vPlane + static_cast<size_t>(ty.lo) * cw;
            const uint8_t* vRowHi = vPlane + static_cast<size_t>(ty.hi) * cw;
            const uint8_t* yRow = yPlane + static_cast<size_t>(y) * width_;
            uint8_t* out = dib + (bottomUp ? static_cast<size_t>(height_ - 1u - y)
                                           : static_cast<size_t>(y)) * stride;
            for (uint32_t x = 0; x < width_; ++x) {
                const Tap tx = xTaps_[x];
                const uint32_t wx = tx.wHi, wy = ty.wHi;
                const uint32_t uTop = uRowLo[tx.lo] * (256u - wx) + uRowLo[tx.hi] * wx;
                const uint32_t uBot = uRowHi[tx.lo] * (256u - wx) + uRowHi[tx.hi] * wx;
                const uint32_t vTop = vRowLo[tx.lo] * (256u - wx) + vRowLo[tx.hi] * wx;
                const uint32_t vBot = vRowHi[tx.lo] * (256u - wx) + vRowHi[tx.hi] * wx;
                const int32_t u = static_cast<int32_t>((uTop * (256u - wy) + uBot * wy + 32768u) >> 16) - 128;
                const int32_t v = static_cast<int32_t>((vTop * (256u - wy) + vBot * wy + 32768u) >> 16) - 128;
                // BT.601 limited range, 16.16 fixed point.
                const int32_t c = 76284 * (static_cast<int32_t>(yRow[x]) - 16);
                const int32_t r = (c + 104595 * v + 32768) >> 16;
                const int32_t g = (c - 25624 * u - 53281 * v + 32768) >> 16;
                const int32_t b = (c + 132251 * u + 32768) >> 16;
                uint8_t* px = out + static_cast<size_t>(x) * bytesPerPixel;
                px[0] = clamp8(b);
                px[1] = clamp8(g);
                px[2] = clamp8(r);
                if (bytesPerPixel == 4u) px[3] = 0u;
            }
        }
    }

    uint32_t width_ = 0u;
    uint32_t height_ = 0u;
    size_t raw_ = 0u;
    std::vector<uint8_t> cur_;
    std::vector<uint8_t> prev_;
    std::vector<Tap> xTaps_;
    std::vector<Tap> yTaps_;
    bool havePrev_ = false;
    uint32_t expected_ = 0u;
};

inline std::vector<uint8_t> encodeFrame(const uint8_t* planes, uint32_t width,
                                        uint32_t height, FrameKind kind,
                                        Predictor predictor, uint32_t sequence,
                                        const uint8_t* previous = nullptr) {
    const size_t raw = planeBytes(width, height);
    std::vector<uint8_t> body(raw);
    if (kind == FrameKind::Delta) {
        for (size_t i = 0; i < raw; ++i)
            body[i] = static_cast<uint8_t>(planes[i] - (previous ? previous[i] : 0u));
    } else {
        const uint32_t cw = width / 4u, ch = height / 4u;
        const size_t ySize = static_cast<size_t>(width) * height;
        const size_t cSize = static_cast<size_t>(cw) * ch;
        predictPlane(planes, body.data(), width, height, predictor);
        predictPlane(planes + ySize, body.data() + ySize, cw, ch, predictor);
        predictPlane(planes + ySize + cSize, body.data() + ySize + cSize, cw, ch, predictor);
    }
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(raw));
    std::vector<uint8_t> out(kQ99VHeaderSize + bound);
    std::memcpy(out.data(), "Q99V", 4);
    out[4] = kQ99VVersion;
    out[5] = static_cast<uint8_t>(kind);
    out[6] = static_cast<uint8_t>(kind == FrameKind::Delta ? Predictor::None : predictor);
    out[7] = 0u;
    std::memcpy(out.data() + 8, &sequence, 4);
    const uint32_t rawSize = static_cast<uint32_t>(raw);
    std::memcpy(out.data() + 12, &rawSize, 4);
    mz_ulong produced = bound;
    if (mz_compress2(out.data() + kQ99VHeaderSize, &produced, body.data(),
                     static_cast<mz_ulong>(raw), MZ_BEST_COMPRESSION) != MZ_OK)
        return {};
    out.resize(kQ99VHeaderSize + produced);
    return out;
}

}
