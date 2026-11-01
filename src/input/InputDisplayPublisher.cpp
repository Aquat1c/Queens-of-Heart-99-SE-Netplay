#include "input/InputDisplayPublisher.h"

#include <cstring>

namespace qinput {

InputDisplayPublisher::InputDisplayPublisher() {
    InputDisplaySnapshotV1 initial{};
    uint32_t words[kWordCount] = {};
    std::memcpy(words, &initial, sizeof(initial));
    for (Buffer& buffer : buffers_) {
        for (size_t i = 0; i < kWordCount; ++i)
            buffer.words[i].store(words[i], std::memory_order_seq_cst);
    }
}

void InputDisplayPublisher::publish(const InputDisplaySnapshotV1& source) {
    InputDisplaySnapshotV1 snapshot = source;
    snapshot.apiVersion = kInputDisplayApiVersion;
    snapshot.structSize = sizeof(InputDisplaySnapshotV1);
    snapshot.laneCount = kInputDisplayLaneCount;
    snapshot.historyCapacity = kInputDisplayHistoryCapacity;
    snapshot.holdCap = kInputDisplayHoldCap;

    writerIndex_ ^= 1u;
    Buffer& buffer = buffers_[writerIndex_];
    uint32_t sequence = buffer.sequence.load(std::memory_order_relaxed);
    if (sequence & 1u) ++sequence;
    buffer.sequence.store(sequence + 1u, std::memory_order_seq_cst);

    uint32_t words[kWordCount] = {};
    std::memcpy(words, &snapshot, sizeof(snapshot));
    for (size_t i = 0; i < kWordCount; ++i)
        buffer.words[i].store(words[i], std::memory_order_seq_cst);

    buffer.sequence.store(sequence + 2u, std::memory_order_seq_cst);
    publishedIndex_.store(writerIndex_, std::memory_order_seq_cst);
}

bool InputDisplayPublisher::read(InputDisplaySnapshotV1& snapshot) const {
    for (uint32_t attempt = 0; attempt < 8; ++attempt) {
        const uint32_t index =
            publishedIndex_.load(std::memory_order_seq_cst) & 1u;
        const Buffer& buffer = buffers_[index];
        const uint32_t before =
            buffer.sequence.load(std::memory_order_seq_cst);
        if (before & 1u) continue;

        uint32_t words[kWordCount] = {};
        for (size_t i = 0; i < kWordCount; ++i)
            words[i] = buffer.words[i].load(std::memory_order_seq_cst);

        const uint32_t after =
            buffer.sequence.load(std::memory_order_seq_cst);
        const uint32_t finalIndex =
            publishedIndex_.load(std::memory_order_seq_cst) & 1u;
        if (before != after || (after & 1u) || index != finalIndex)
            continue;

        std::memcpy(&snapshot, words, sizeof(snapshot));
        return snapshot.apiVersion == kInputDisplayApiVersion
            && snapshot.structSize >= sizeof(InputDisplaySnapshotV1);
    }
    return false;
}

InputDisplayComposer::InputDisplayComposer() {
    reset(0u);
}

void InputDisplayComposer::clearAllHistory() {
    for (uint32_t lane = 0; lane < kInputDisplayLaneCount; ++lane) {
        inputDisplayLaneClearHistory(pending_.lanes[lane]);
        pending_.lanes[lane].active = 0u;
    }
}

void InputDisplayComposer::reset(uint32_t generation) {
    pending_ = InputDisplaySnapshotV1{};
    pending_.mode = static_cast<uint32_t>(InputDisplayMode::None);
    pending_.producerGeneration = generation;
    pending_.drivenLane = kInputDisplayNoLane;
    haveStep_ = false;
    lastStepSerial_ = 0;
    lastMode_ = static_cast<uint32_t>(InputDisplayMode::None);
    lastGeneration_ = generation;
    lastDrivenLane_ = kInputDisplayNoLane;
    publisher_.publish(pending_);
}

void InputDisplayComposer::publishIdle() {
    pending_.mode = static_cast<uint32_t>(InputDisplayMode::None);
    for (uint32_t lane = 0; lane < kInputDisplayLaneCount; ++lane) {
        pending_.lanes[lane].active = 0u;
        pending_.lanes[lane].currentMask = 0u;
    }
    publisher_.publish(pending_);
}

InputDisplayPublishResult InputDisplayComposer::publishStep(
        const InputDisplayStep& step) {
    if (!isPublishableInputDisplayMode(step.mode))
        return InputDisplayPublishResult::RejectedMode;

    const uint32_t laneBits = (1u << kInputDisplayLaneCount) - 1u;
    if (step.laneActiveMask == 0u || (step.laneActiveMask & ~laneBits) != 0u)
        return InputDisplayPublishResult::RejectedLaneMask;

    if (step.mode == InputDisplayMode::Training) {
        if (step.drivenLane >= kInputDisplayLaneCount)
            return InputDisplayPublishResult::RejectedDrivenLane;
        if (step.laneActiveMask != (1u << step.drivenLane))
            return InputDisplayPublishResult::RejectedDrivenLane;
    } else if (step.drivenLane != kInputDisplayNoLane) {
        return InputDisplayPublishResult::RejectedDrivenLane;
    }

    for (uint32_t lane = 0; lane < kInputDisplayLaneCount; ++lane) {
        if ((step.laneActiveMask & (1u << lane)) == 0u) continue;
        if (!isCanonicalLogicalFightInput(step.laneLogicalMask[lane]))
            return InputDisplayPublishResult::RejectedNonCanonicalMask;
    }

    const uint32_t mode = static_cast<uint32_t>(step.mode);
    const bool newTimeline = !haveStep_
        || mode != lastMode_
        || step.generation != lastGeneration_
        || step.drivenLane != lastDrivenLane_;
    if (!newTimeline && step.stepSerial <= lastStepSerial_) {
        return InputDisplayPublishResult::RejectedNonMonotonicSerial;
    }
    if (newTimeline) clearAllHistory();

    pending_.mode = mode;
    pending_.producerGeneration = step.generation;
    pending_.stepSerialLow = static_cast<uint32_t>(step.stepSerial & 0xFFFFFFFFull);
    pending_.stepSerialHigh =
        static_cast<uint32_t>((step.stepSerial >> 32) & 0xFFFFFFFFull);
    pending_.drivenLane = step.drivenLane;

    for (uint32_t lane = 0; lane < kInputDisplayLaneCount; ++lane) {
        InputDisplayLaneV1& view = pending_.lanes[lane];
        if ((step.laneActiveMask & (1u << lane)) == 0u) {
            view.active = 0u;
            view.currentMask = 0u;
            continue;
        }
        view.active = 1u;
        (void)inputDisplayLaneAppend(view, step.laneLogicalMask[lane]);
    }

    haveStep_ = true;
    lastStepSerial_ = step.stepSerial;
    lastMode_ = mode;
    lastGeneration_ = step.generation;
    lastDrivenLane_ = step.drivenLane;
    publisher_.publish(pending_);
    return InputDisplayPublishResult::Published;
}

bool InputDisplayComposer::read(InputDisplaySnapshotV1& snapshot) const {
    return publisher_.read(snapshot);
}

InputDisplayComposer& inputDisplay() {
    static InputDisplayComposer composer;
    return composer;
}

}
