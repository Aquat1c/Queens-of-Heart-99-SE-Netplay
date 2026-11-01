#include "training/TrainingHitboxPublisher.h"

#include <cstring>

namespace qtraining {

TrainingHitboxPublisher::TrainingHitboxPublisher() {
    HitboxSnapshotV1 initial{};
    initial.structSize = sizeof(HitboxSnapshotV1);
    uint32_t words[kWordCount] = {};
    std::memcpy(words, &initial, sizeof(initial));
    for (Buffer& buffer : buffers_) {
        for (size_t i = 0; i < kWordCount; ++i)
            buffer.words[i].store(words[i], std::memory_order_seq_cst);
    }
}

void TrainingHitboxPublisher::publish(const HitboxSnapshotV1& source) {
    HitboxSnapshotV1 snapshot = source;
    snapshot.apiVersion = kHitboxApiVersion;
    snapshot.structSize = sizeof(HitboxSnapshotV1);
    snapshot.boxCapacity = kHitboxSnapshotMaxBoxes;
    if (snapshot.boxCount > kHitboxSnapshotMaxBoxes)
        snapshot.boxCount = kHitboxSnapshotMaxBoxes;

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

void TrainingHitboxPublisher::publishHidden() {
    HitboxSnapshotV1 hidden{};
    hidden.visible = 0u;
    hidden.boxCount = 0u;
    publish(hidden);
}

bool TrainingHitboxPublisher::read(HitboxSnapshotV1& snapshot) const {
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
        return snapshot.apiVersion == kHitboxApiVersion
            && snapshot.structSize == sizeof(HitboxSnapshotV1)
            && snapshot.boxCount <= kHitboxSnapshotMaxBoxes;
    }
    return false;
}

TrainingHitboxPublisher& trainingHitboxes() {
    static TrainingHitboxPublisher publisher;
    return publisher;
}

}
