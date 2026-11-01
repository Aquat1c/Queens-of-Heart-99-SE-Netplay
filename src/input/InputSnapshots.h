#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "input/InputLaunchConfig.h"

namespace qinput {

constexpr uint32_t kNoSnapshotReader = 2;
constexpr uint32_t kSnapshotReaderRetryCap = 4;

enum class SnapshotStatus : uint32_t {
    PendingNeutral = 1,
    Connected = 2,
    Disconnected = 3,
    Fault = 4,
};

struct KeyboardSnapshotPayload {
    uint32_t status = static_cast<uint32_t>(SnapshotStatus::PendingNeutral);
    uint32_t sampleTickMs = 0;
    uint32_t sampleSerial = 0;
    uint32_t workerEpoch = 0;
    uint32_t deviceGeneration = 0;
    uint32_t keyBits[8] = {};
};

struct ControllerSnapshotPayload {
    uint32_t status = static_cast<uint32_t>(SnapshotStatus::PendingNeutral);
    uint32_t sampleTickMs = 0;
    uint32_t sampleSerial = 0;
    uint32_t workerEpoch = 0;
    uint32_t deviceGeneration = 0;
    uint32_t probe[kMaxControllerProbes] = {};
};

template <class Payload>
struct PublishedSnapshot {
    std::atomic<uint32_t> activeIndex{0};
    std::atomic<uint32_t> readerIndex{kNoSnapshotReader};
    Payload slot[2];

    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "32-bit control atomics must be lock-free on x86");

    void initialize(const Payload& pending) {
        slot[0] = pending;
        slot[1] = pending;
        activeIndex.store(0, std::memory_order_seq_cst);
        readerIndex.store(kNoSnapshotReader, std::memory_order_seq_cst);
    }

    bool publish(const Payload& next) {
        const uint32_t current =
            activeIndex.load(std::memory_order_seq_cst);
        const uint32_t target = current ^ 1u;
        if (readerIndex.load(std::memory_order_seq_cst) == target)
            return false;
        slot[target] = next;
        activeIndex.store(target, std::memory_order_seq_cst);
        return true;
    }

    bool read(Payload& out) {
        struct HazardGuard {
            std::atomic<uint32_t>* reader;
            ~HazardGuard() {
                reader->store(kNoSnapshotReader,
                              std::memory_order_seq_cst);
            }
        } guard{&readerIndex};
        for (uint32_t attempt = 0; attempt < kSnapshotReaderRetryCap;
             ++attempt) {
            const uint32_t candidate =
                activeIndex.load(std::memory_order_seq_cst);
            readerIndex.store(candidate, std::memory_order_seq_cst);
            if (activeIndex.load(std::memory_order_seq_cst)
                != candidate) {
                readerIndex.store(kNoSnapshotReader,
                                  std::memory_order_seq_cst);
                continue;
            }
            out = slot[candidate];
            return true;
        }
        return false;
    }
};

// Packed window-focus word: bit 0 = active, bits 1..31 = generation.
struct PublishedWindowFocus {
    std::atomic<uint32_t> state{0};

    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "focus word must be lock-free");

    void publish(bool active, uint32_t generation) {
        state.store((generation << 1) | (active ? 1u : 0u),
                    std::memory_order_release);
    }
    void readInto(bool& active, uint32_t& generation) const {
        const uint32_t word = state.load(std::memory_order_acquire);
        active = (word & 1u) != 0u;
        generation = word >> 1;
    }
};

// Modular unsigned freshness age; thresholds must be below 2^31 ms.
inline uint32_t snapshotAgeMs(uint32_t nowLowMs, uint32_t sampleTickMs) {
    return nowLowMs - sampleTickMs;
}

}
