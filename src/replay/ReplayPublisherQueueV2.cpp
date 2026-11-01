#include "ReplayPublisherQueueV2.h"

namespace qreplay {

ReplayPublisherQueueV2::ReplayPublisherQueueV2(const size_t capacity) {
    if (capacity < kMinReplayPublisherCapacityV2
        || capacity > kMaxReplayPublisherCapacityV2) {
        return;
    }
    capacity_ = capacity;
    ring_.resize(capacity_);
    valid_ = true;
}

bool ReplayPublisherQueueV2::tryPublishOrdinary(
    const PublisherRecordV2& record) noexcept {
    return tryPublishOrdinaryFor(
        generation_.load(std::memory_order_acquire), record);
}

bool ReplayPublisherQueueV2::publishEndGameReserved(
    const PublisherRecordV2& record) noexcept {
    return publishEndGameReservedFor(
        generation_.load(std::memory_order_acquire), record);
}

bool ReplayPublisherQueueV2::tryPublishOrdinaryFor(
    const uint32_t generation,
    const PublisherRecordV2& record) noexcept {
    return publish(generation, record, false);
}

bool ReplayPublisherQueueV2::publishEndGameReservedFor(
    const uint32_t generation,
    const PublisherRecordV2& record) noexcept {
    return publish(generation, record, true);
}

bool ReplayPublisherQueueV2::publish(
    const uint32_t generation, const PublisherRecordV2& record,
    const bool reservedEnd) noexcept {
    if (!valid_ || generation == 0u
        || generation != generation_.load(std::memory_order_acquire)) {
        staleRejects_.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    if (disabled_.load(std::memory_order_acquire)) {
        if (reservedEnd)
            reservedEndRejects_.fetch_add(1u, std::memory_order_relaxed);
        else
            ordinaryRejects_.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    if (reservedEnd) {
        if (record.kind != PublisherRecordKindV2::EndGame
            || reservedEndUsed_.load(std::memory_order_acquire)) {
            reservedEndRejects_.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
    } else if ((record.kind != PublisherRecordKindV2::BeginGame
                && record.kind != PublisherRecordKindV2::Tick
                && record.kind != PublisherRecordKindV2::Palette)
               || ordinaryOverflowed_.load(std::memory_order_acquire)) {
        ordinaryRejects_.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    const uint32_t used = head - tail;
    const uint32_t limit = static_cast<uint32_t>(
        reservedEnd ? capacity_ : capacity_ - 1u);
    if (used >= limit) {
        if (reservedEnd) {
            reservedEndRejects_.fetch_add(1u, std::memory_order_relaxed);
            disabled_.store(true, std::memory_order_release);
        } else {
            ordinaryRejects_.fetch_add(1u, std::memory_order_relaxed);
            ordinaryOverflowed_.store(true, std::memory_order_release);
        }
        return false;
    }

    ring_[static_cast<size_t>(
        head % static_cast<uint32_t>(capacity_))] = record;
    const uint32_t filled = used + 1u;
    uint32_t observed = highWater_.load(std::memory_order_relaxed);
    while (filled > observed
           && !highWater_.compare_exchange_weak(
               observed, filled, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    if (reservedEnd) {
        reservedEndUsed_.store(true, std::memory_order_release);
    }
    head_.store(head + 1u, std::memory_order_release);
    return true;
}

bool ReplayPublisherQueueV2::drain(PublisherRecordV2& out) noexcept {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_acquire);
    if (tail == head) return false;
    out = ring_[static_cast<size_t>(
        tail % static_cast<uint32_t>(capacity_))];
    tail_.store(tail + 1u, std::memory_order_release);
    if (out.kind == PublisherRecordKindV2::EndGame)
        reservedEndUsed_.store(false, std::memory_order_release);
    return true;
}

void ReplayPublisherQueueV2::beginGeneration(
    const uint32_t generation) noexcept {
    head_.store(0u, std::memory_order_relaxed);
    tail_.store(0u, std::memory_order_relaxed);
    highWater_.store(0u, std::memory_order_relaxed);
    ordinaryRejects_.store(0u, std::memory_order_relaxed);
    reservedEndRejects_.store(0u, std::memory_order_relaxed);
    staleRejects_.store(0u, std::memory_order_relaxed);
    ordinaryOverflowed_.store(false, std::memory_order_relaxed);
    reservedEndUsed_.store(false, std::memory_order_relaxed);
    disabled_.store(generation == 0u, std::memory_order_relaxed);
    generation_.store(generation, std::memory_order_release);
}

void ReplayPublisherQueueV2::disable() noexcept {
    disabled_.store(true, std::memory_order_release);
}

uint32_t ReplayPublisherQueueV2::generation() const noexcept {
    return generation_.load(std::memory_order_acquire);
}

bool ReplayPublisherQueueV2::disabled() const noexcept {
    return disabled_.load(std::memory_order_acquire);
}

bool ReplayPublisherQueueV2::ordinaryOverflowed() const noexcept {
    return ordinaryOverflowed_.load(std::memory_order_acquire);
}

bool ReplayPublisherQueueV2::reservedEndUsed() const noexcept {
    return reservedEndUsed_.load(std::memory_order_acquire);
}

size_t ReplayPublisherQueueV2::pending() const noexcept {
    const uint32_t head = head_.load(std::memory_order_acquire);
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    return static_cast<size_t>(head - tail);
}

uint64_t ReplayPublisherQueueV2::ordinaryRejects() const noexcept {
    return ordinaryRejects_.load(std::memory_order_relaxed);
}

uint64_t ReplayPublisherQueueV2::reservedEndRejects() const noexcept {
    return reservedEndRejects_.load(std::memory_order_relaxed);
}

uint64_t ReplayPublisherQueueV2::staleRejects() const noexcept {
    return staleRejects_.load(std::memory_order_relaxed);
}

uint64_t ReplayPublisherQueueV2::highWater() const noexcept {
    return highWater_.load(std::memory_order_relaxed);
}

}
