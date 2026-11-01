#include "ReplayRecorderQueue.h"

namespace qreplay {

ReplayRecorderQueue::ReplayRecorderQueue(size_t capacity) {
    capacity_ = capacity == 0u ? 1u : capacity;
    if (capacity_ > kMaxRecorderQueueCapacity) {
        capacity_ = kMaxRecorderQueueCapacity;
    }
    ring_.resize(capacity_);
}

bool ReplayRecorderQueue::publish(const RecorderMessage& message) noexcept {
    return publishFor(generation_.load(std::memory_order_relaxed), message);
}

bool ReplayRecorderQueue::publishFor(uint32_t generation,
                                     const RecorderMessage& message) noexcept {
    if (generation != generation_.load(std::memory_order_acquire)) {
        staleRejects_.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    if (disabled_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    const uint32_t used = head - tail;
    if (used >= static_cast<uint32_t>(capacity_)) {
        dropped_.fetch_add(1u, std::memory_order_relaxed);
        disabled_.store(true, std::memory_order_release);
        return false;
    }

    RecorderMessage& slot = ring_[static_cast<size_t>(
        head % static_cast<uint32_t>(capacity_))];
    slot = message;
    slot.sequence = ++sequence_;

    const uint32_t filled = used + 1u;
    if (filled > highWater_.load(std::memory_order_relaxed)) {
        highWater_.store(filled, std::memory_order_relaxed);
    }

    head_.store(head + 1u, std::memory_order_release);
    return true;
}

bool ReplayRecorderQueue::drain(RecorderMessage& out) noexcept {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_acquire);
    if (tail == head) return false;
    out = ring_[static_cast<size_t>(tail % static_cast<uint32_t>(capacity_))];
    tail_.store(tail + 1u, std::memory_order_release);
    return true;
}

uint64_t ReplayRecorderQueue::droppedCount() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
}

uint64_t ReplayRecorderQueue::highWater() const noexcept {
    return highWater_.load(std::memory_order_relaxed);
}

uint64_t ReplayRecorderQueue::staleRejectCount() const noexcept {
    return staleRejects_.load(std::memory_order_relaxed);
}

bool ReplayRecorderQueue::disabled() const noexcept {
    return disabled_.load(std::memory_order_acquire);
}

void ReplayRecorderQueue::disable() noexcept {
    disabled_.store(true, std::memory_order_release);
}

uint32_t ReplayRecorderQueue::generation() const noexcept {
    return generation_.load(std::memory_order_acquire);
}

void ReplayRecorderQueue::beginGeneration(uint32_t generation) noexcept {
    head_.store(0u, std::memory_order_relaxed);
    tail_.store(0u, std::memory_order_relaxed);
    sequence_ = 0u;
    highWater_.store(0u, std::memory_order_relaxed);
    dropped_.store(0u, std::memory_order_relaxed);
    staleRejects_.store(0u, std::memory_order_relaxed);
    disabled_.store(false, std::memory_order_relaxed);
    generation_.store(generation, std::memory_order_release);
}

size_t ReplayRecorderQueue::pending() const noexcept {
    const uint32_t head = head_.load(std::memory_order_acquire);
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    return static_cast<size_t>(head - tail);
}

bool ReplayRecorderQueue::full() const noexcept {
    return pending() >= capacity_;
}

bool ReplayRecorderQueue::empty() const noexcept {
    return pending() == 0u;
}

}
