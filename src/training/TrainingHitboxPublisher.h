#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "training/TrainingHitboxOverlay.h"

namespace qtraining {

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "hitbox publication requires lock-free u32 atomics");

class TrainingHitboxPublisher {
public:
    TrainingHitboxPublisher();

    TrainingHitboxPublisher(const TrainingHitboxPublisher&) = delete;
    TrainingHitboxPublisher& operator=(const TrainingHitboxPublisher&) = delete;

    void publish(const HitboxSnapshotV1& snapshot);

    void publishHidden();

    bool read(HitboxSnapshotV1& snapshot) const;

private:
    static constexpr size_t kWordCount =
        sizeof(HitboxSnapshotV1) / sizeof(uint32_t);

    struct Buffer {
        std::atomic<uint32_t> sequence{0};
        std::array<std::atomic<uint32_t>, kWordCount> words{};
    };

    Buffer buffers_[2];
    std::atomic<uint32_t> publishedIndex_{0};
    uint32_t writerIndex_ = 0;
};

TrainingHitboxPublisher& trainingHitboxes();

}
