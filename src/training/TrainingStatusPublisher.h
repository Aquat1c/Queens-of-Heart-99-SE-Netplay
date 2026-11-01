#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "training/TrainingTypes.h"

namespace qtraining {

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "training status publication requires lock-free u32 atomics");

class TrainingStatusPublisher {
public:
    TrainingStatusPublisher();

    TrainingStatusPublisher(const TrainingStatusPublisher&) = delete;
    TrainingStatusPublisher& operator=(const TrainingStatusPublisher&) =
        delete;

    // Sole game-thread writer.
    void publish(const TrainingStatusV1& status);

    bool read(TrainingStatusV1& status) const;

private:
    static constexpr size_t kWordCount =
        sizeof(TrainingStatusV1) / sizeof(uint32_t);
    static_assert(sizeof(TrainingStatusV1) % sizeof(uint32_t) == 0,
                  "status ABI must be a u32 word sequence");

    struct Buffer {
        std::atomic<uint32_t> sequence{0};
        std::array<std::atomic<uint32_t>, kWordCount> words{};
    };

    Buffer buffers_[2];
    std::atomic<uint32_t> publishedIndex_{0};
    uint32_t writerIndex_ = 0;
};

}
