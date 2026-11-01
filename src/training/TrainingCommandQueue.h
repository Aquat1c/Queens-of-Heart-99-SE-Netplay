/* SPSC: the render thread is the sole producer, the game thread the sole consumer; neither may allocate or block. */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "training/TrainingTypes.h"

namespace qtraining {

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "training SPSC queue requires lock-free u32 atomics");

class TrainingCommandQueue {
public:
    TrainingCommandQueue();

    TrainingCommandQueue(const TrainingCommandQueue&) = delete;
    TrainingCommandQueue& operator=(const TrainingCommandQueue&) = delete;

    void publishGeneration(uint32_t generation);
    uint32_t publishedGeneration() const;

    CommandIngressResult tryEnqueue(const TrainingCommandV1* command,
                                    uint32_t incomingSize);

    bool tryDequeue(TrainingCommandV1& command);

    uint32_t approximateDepth() const;
    uint32_t queueFullDrops() const;
    uint32_t rejectedCommands() const;

private:
    std::array<TrainingCommandV1, kTrainingCommandCapacity> entries_{};
    alignas(64) std::atomic<uint32_t> head_{0};
    alignas(64) std::atomic<uint32_t> tail_{0};
    std::atomic<uint32_t> generation_{0};
    std::atomic<uint32_t> queueFullDrops_{0};
    std::atomic<uint32_t> rejectedCommands_{0};

    uint32_t producerGeneration_ = 0;
    uint32_t lastAcceptedCommandId_ = 0;
};

}
