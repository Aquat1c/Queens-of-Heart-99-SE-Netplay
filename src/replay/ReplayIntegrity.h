#pragma once

#include "ReplayTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace qreplay {

// CRC32C: polynomial 0x1EDC6F41, reflected 0x82F63B78, init 0xFFFFFFFF, final
// inversion.  crc32c("123456789") == 0xE3069283.
uint32_t crc32c(const uint8_t* data, size_t len, uint32_t seed = 0u);

class Crc32c {
public:
    Crc32c() = default;

    void reset();
    void update(const uint8_t* data, size_t len);
    uint32_t value() const;

private:
    uint32_t state_ = 0xFFFFFFFFu;
};

class Sha256 {
public:
    Sha256();

    void reset();
    void update(const uint8_t* data, size_t len);
    void finish(std::array<uint8_t, 32>& out);

private:
    void compress(const uint8_t block[64]);

    uint32_t state_[8];
    uint8_t buffer_[64];
    size_t buffered_;
    uint64_t totalBytes_;
    bool finished_;
    std::array<uint8_t, 32> digest_;
};

std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len);

std::string toHex(const std::array<uint8_t, 32>& digest);

bool fromHex(const std::string& text, std::array<uint8_t, 32>& out);

uint64_t advanceReplayDigest(uint64_t previous, const ReplayFrameV1& frame);
uint64_t advanceReplayDigest(uint64_t previous, const ReplayEvent& event);

uint64_t initialReplayDigestV2(const ConfirmedGameStartV2& start);
uint64_t advanceReplayDigestV2(
    uint64_t previous, const ConfirmedGameFrameV2& frame);
uint64_t advanceReplayDigestV2(
    uint64_t previous, const ConfirmedGameEventV2& event);

class CanonicalGameDigestV2 {
public:
    bool begin(const ConfirmedGameStartV2& start);
    bool append(const ConfirmedGameEventV2& event);
    bool append(const ConfirmedGameFrameV2& frame);
    bool finish(
        const ConfirmedGameEndV2& end, std::array<uint8_t, 32>& out);

    bool started() const { return started_; }
    bool finished() const { return finished_; }
    uint32_t nextRecord() const { return nextRecord_; }

private:
    Sha256 sha_;
    bool started_ = false;
    bool finished_ = false;
    uint32_t nextRecord_ = 0u;
    uint64_t rollingDigest_ = 0u;
};

}
