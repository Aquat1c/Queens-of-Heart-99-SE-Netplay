#pragma once

#include "ReplayRecordingCoordinator.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qreplay {

constexpr size_t kMinReplayPublisherCapacityV2 = 2u;
constexpr size_t kDefaultReplayPublisherCapacityV2 = 1024u;
constexpr size_t kMaxReplayPublisherCapacityV2 = 8192u;

class ReplayPublisherQueueV2 final : public ReplayRecordPublisherV2 {
public:
    explicit ReplayPublisherQueueV2(size_t capacity);

    bool valid() const noexcept { return valid_; }

    bool tryPublishOrdinary(
        const PublisherRecordV2& record) noexcept override;
    bool publishEndGameReserved(
        const PublisherRecordV2& record) noexcept override;
    bool tryPublishOrdinaryFor(
        uint32_t generation, const PublisherRecordV2& record) noexcept;
    bool publishEndGameReservedFor(
        uint32_t generation, const PublisherRecordV2& record) noexcept;

    bool drain(PublisherRecordV2& out) noexcept;

    // Quiescent point only: producer and consumer must both be stopped.
    void beginGeneration(uint32_t generation) noexcept;
    void disable() noexcept;

    uint32_t generation() const noexcept;
    bool disabled() const noexcept;
    bool ordinaryOverflowed() const noexcept;
    bool reservedEndUsed() const noexcept;
    size_t capacity() const noexcept { return capacity_; }
    size_t pending() const noexcept;
    uint64_t ordinaryRejects() const noexcept;
    uint64_t reservedEndRejects() const noexcept;
    uint64_t staleRejects() const noexcept;
    uint64_t highWater() const noexcept;

private:
    bool publish(
        uint32_t generation, const PublisherRecordV2& record,
        bool reservedEnd) noexcept;

    std::vector<PublisherRecordV2> ring_;
    size_t capacity_ = 0u;
    bool valid_ = false;

    alignas(64) std::atomic<uint32_t> head_{0u};
    alignas(64) std::atomic<uint32_t> tail_{0u};
    std::atomic<uint32_t> generation_{0u};
    std::atomic<uint32_t> highWater_{0u};
    std::atomic<uint64_t> ordinaryRejects_{0u};
    std::atomic<uint64_t> reservedEndRejects_{0u};
    std::atomic<uint64_t> staleRejects_{0u};
    std::atomic<bool> disabled_{false};
    std::atomic<bool> ordinaryOverflowed_{false};
    std::atomic<bool> reservedEndUsed_{false};
};

}
