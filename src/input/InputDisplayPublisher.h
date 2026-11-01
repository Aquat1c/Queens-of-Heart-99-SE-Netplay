#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "input/InputDisplayTypes.h"

namespace qinput {

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "input display publication requires lock-free u32 atomics");

class InputDisplayPublisher {
public:
    InputDisplayPublisher();

    InputDisplayPublisher(const InputDisplayPublisher&) = delete;
    InputDisplayPublisher& operator=(const InputDisplayPublisher&) = delete;

    void publish(const InputDisplaySnapshotV1& snapshot);

    bool read(InputDisplaySnapshotV1& snapshot) const;

private:
    static constexpr size_t kWordCount =
        sizeof(InputDisplaySnapshotV1) / sizeof(uint32_t);
    static_assert(sizeof(InputDisplaySnapshotV1) % sizeof(uint32_t) == 0,
                  "input display ABI must be a u32 word sequence");

    struct Buffer {
        std::atomic<uint32_t> sequence{0};
        std::array<std::atomic<uint32_t>, kWordCount> words{};
    };

    Buffer buffers_[2];
    std::atomic<uint32_t> publishedIndex_{0};
    uint32_t writerIndex_ = 0;
};

struct InputDisplayStep {
    InputDisplayMode mode = InputDisplayMode::None;
    uint32_t generation = 0;
    uint64_t stepSerial = 0;
    uint32_t drivenLane = kInputDisplayNoLane;
    uint32_t laneActiveMask = 0;
    uint32_t laneLogicalMask[kInputDisplayLaneCount] = {};
};

enum class InputDisplayPublishResult : uint32_t {
    Published = 0,
    RejectedMode = 1,
    RejectedLaneMask = 2,
    RejectedDrivenLane = 3,
    RejectedNonCanonicalMask = 4,
    RejectedNonMonotonicSerial = 5,
};

class InputDisplayComposer {
public:
    InputDisplayComposer();

    InputDisplayComposer(const InputDisplayComposer&) = delete;
    InputDisplayComposer& operator=(const InputDisplayComposer&) = delete;

    void reset(uint32_t generation);

    void publishIdle();

    InputDisplayPublishResult publishStep(const InputDisplayStep& step);

    bool read(InputDisplaySnapshotV1& snapshot) const;

    const InputDisplaySnapshotV1& pending() const { return pending_; }

private:
    void clearAllHistory();

    InputDisplaySnapshotV1 pending_{};
    InputDisplayPublisher publisher_;
    bool haveStep_ = false;
    uint64_t lastStepSerial_ = 0;
    uint32_t lastMode_ = static_cast<uint32_t>(InputDisplayMode::None);
    uint32_t lastGeneration_ = 0;
    uint32_t lastDrivenLane_ = kInputDisplayNoLane;
};

InputDisplayComposer& inputDisplay();

}
