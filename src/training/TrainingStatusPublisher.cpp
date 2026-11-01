#include "training/TrainingStatusPublisher.h"

#include <cstring>

namespace qtraining {

TrainingStatusPublisher::TrainingStatusPublisher() {
    TrainingStatusV1 initial{};
    uint32_t words[kWordCount] = {};
    std::memcpy(words, &initial, sizeof(initial));
    for (Buffer& buffer : buffers_) {
        for (size_t i = 0; i < kWordCount; ++i)
            buffer.words[i].store(words[i], std::memory_order_seq_cst);
    }
}

void TrainingStatusPublisher::publish(const TrainingStatusV1& source) {
    TrainingStatusV1 status = source;
    status.apiVersion = kTrainingApiVersion;
    status.structSize = sizeof(TrainingStatusV1);
    status.disableText[sizeof(status.disableText) - 1] = '\0';
    status.statusText[sizeof(status.statusText) - 1] = '\0';
    status.resultText[sizeof(status.resultText) - 1] = '\0';

    writerIndex_ ^= 1u;
    Buffer& buffer = buffers_[writerIndex_];
    uint32_t sequence = buffer.sequence.load(std::memory_order_relaxed);
    if (sequence & 1u) ++sequence;
    buffer.sequence.store(sequence + 1u, std::memory_order_seq_cst);

    uint32_t words[kWordCount] = {};
    std::memcpy(words, &status, sizeof(status));
    for (size_t i = 0; i < kWordCount; ++i)
        buffer.words[i].store(words[i], std::memory_order_seq_cst);

    buffer.sequence.store(sequence + 2u, std::memory_order_seq_cst);
    publishedIndex_.store(writerIndex_, std::memory_order_seq_cst);
}

bool TrainingStatusPublisher::read(TrainingStatusV1& status) const {
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

        std::memcpy(&status, words, sizeof(status));
        return status.apiVersion == kTrainingApiVersion
            && status.structSize >= sizeof(TrainingStatusV1);
    }
    return false;
}

}
