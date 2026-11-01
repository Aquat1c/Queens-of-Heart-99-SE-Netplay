#include "training/TrainingCommandQueue.h"

namespace qtraining {

TrainingCommandQueue::TrainingCommandQueue() = default;

void TrainingCommandQueue::publishGeneration(uint32_t generation) {
    generation_.store(generation, std::memory_order_release);
}

uint32_t TrainingCommandQueue::publishedGeneration() const {
    return generation_.load(std::memory_order_acquire);
}

CommandIngressResult TrainingCommandQueue::tryEnqueue(
    const TrainingCommandV1* command, uint32_t incomingSize) {
    if (command == nullptr) {
        rejectedCommands_.fetch_add(1, std::memory_order_relaxed);
        return CommandIngressResult::NullCommand;
    }
    if (incomingSize < sizeof(TrainingCommandV1)
        || command->structSize < sizeof(TrainingCommandV1)
        || command->structSize > incomingSize) {
        rejectedCommands_.fetch_add(1, std::memory_order_relaxed);
        return CommandIngressResult::StructTooSmall;
    }
    if (command->apiVersion != kTrainingApiVersion) {
        rejectedCommands_.fetch_add(1, std::memory_order_relaxed);
        return CommandIngressResult::ApiVersionMismatch;
    }

    const uint32_t expectedGeneration =
        generation_.load(std::memory_order_acquire);
    if (expectedGeneration == 0
        || command->trainingGeneration != expectedGeneration) {
        rejectedCommands_.fetch_add(1, std::memory_order_relaxed);
        return CommandIngressResult::StaleGeneration;
    }
    if (producerGeneration_ != expectedGeneration) {
        producerGeneration_ = expectedGeneration;
        lastAcceptedCommandId_ = 0;
    }
    if (command->commandId == 0
        || command->commandId <= lastAcceptedCommandId_) {
        rejectedCommands_.fetch_add(1, std::memory_order_relaxed);
        return CommandIngressResult::NonMonotonicId;
    }

    const auto kind =
        static_cast<TrainingCommandKind>(command->commandKind);
    if (!isValidCommandKind(kind)) {
        rejectedCommands_.fetch_add(1, std::memory_order_relaxed);
        return CommandIngressResult::InvalidKind;
    }
    if (!validCommandPayload(*command)) {
        rejectedCommands_.fetch_add(1, std::memory_order_relaxed);
        return CommandIngressResult::InvalidPayload;
    }

    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_acquire);
    if (tail - head >= kTrainingCommandCapacity) {
        queueFullDrops_.fetch_add(1, std::memory_order_relaxed);
        return CommandIngressResult::QueueFull;
    }

    entries_[tail % kTrainingCommandCapacity] = *command;

    // The consumer's generation check rejects a stale entry; do not overwrite a live slot or rewind either cursor.
    tail_.store(tail + 1, std::memory_order_release);
    lastAcceptedCommandId_ = command->commandId;
    return CommandIngressResult::Accepted;
}

bool TrainingCommandQueue::tryDequeue(TrainingCommandV1& command) {
    for (;;) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) return false;
        command = entries_[head % kTrainingCommandCapacity];
        head_.store(head + 1, std::memory_order_release);
        if (command.trainingGeneration
            == generation_.load(std::memory_order_acquire)) {
            return true;
        }
        rejectedCommands_.fetch_add(1, std::memory_order_relaxed);
    }
}

uint32_t TrainingCommandQueue::approximateDepth() const {
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    const uint32_t head = head_.load(std::memory_order_acquire);
    const uint32_t depth = tail - head;
    return depth < kTrainingCommandCapacity
        ? depth : kTrainingCommandCapacity;
}

uint32_t TrainingCommandQueue::queueFullDrops() const {
    return queueFullDrops_.load(std::memory_order_relaxed);
}

uint32_t TrainingCommandQueue::rejectedCommands() const {
    return rejectedCommands_.load(std::memory_order_relaxed);
}

}
