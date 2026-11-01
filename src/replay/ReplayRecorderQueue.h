#pragma once

#include "ReplayCodec.h"
#include "ReplayTypes.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace qreplay {

enum class RecorderMessageKind : uint8_t {
    Frame = 1,
    Event = 2,
    MatchStart = 3,
    MatchEnd = 4,
    SessionStart = 5,
    SessionEnd = 6,
    Terminal = 7,
    Manifest = 8,
};

constexpr bool isKnownRecorderMessageKind(uint8_t value) {
    return value >= 1u && value <= 8u;
}

constexpr size_t kWorstCaseSessionManifestBytes =
      kSimulationContractBytes
    + 4u
    + kFighterSlots
    + kSourceLanes * (1u + 2u + kMaxLabelBytes)
    + kFighterSlots * kFighterSlotDescriptorBytes
    + 4u
    + 8u + 4u + 1u + 1u + 2u + 2u + 2u + 4u + 4u
    + 2u * (2u + kMaxLabelBytes)
    + 4u;

constexpr size_t kMaxRecorderManifestBytes = 768u;

static_assert(kMaxRecorderManifestBytes >= kWorstCaseSessionManifestBytes,
              "the manifest carrier must hold every manifest this build can "
              "encode; it bounds a refusal, it never truncates a record");

struct RecorderManifestBlob {
    ReplayPrefix prefix{};
    uint32_t bytes = 0u;
    std::array<uint8_t, kMaxRecorderManifestBytes> encoded{};
};

static_assert(std::is_trivially_copyable<RecorderManifestBlob>::value,
              "the manifest carrier travels through the publish path and must "
              "stay a bounded copy");

struct RecorderMessage {
    RecorderMessageKind kind = RecorderMessageKind::Frame;
    uint32_t sequence = 0u;
    ReplayFrameV1 frame{};
    ReplayEvent event{};
    MatchStartRecord matchStart{};
    MatchEndRecord matchEnd{};
    SessionEndRecord sessionEnd{};
    RecorderManifestBlob manifest{};
};

static_assert(std::is_trivially_copyable<RecorderMessage>::value,
              "RecorderMessage must be trivially copyable: publish() is a "
              "bounded copy on the game thread and may never allocate");

constexpr size_t kMaxRecorderQueueCapacity = 65536u;
constexpr size_t kDefaultRecorderQueueCapacity = 1024u;

class ReplayRecorderQueue {
public:
    explicit ReplayRecorderQueue(size_t capacity);

    bool publish(const RecorderMessage& message) noexcept;

    bool publishFor(uint32_t generation, const RecorderMessage& message) noexcept;

    bool drain(RecorderMessage& out) noexcept;

    uint64_t droppedCount() const noexcept;
    uint64_t highWater() const noexcept;
    uint64_t staleRejectCount() const noexcept;

    bool disabled() const noexcept;
    void disable() noexcept;

    uint32_t generation() const noexcept;

    // Call only at a quiescent point, with producer and consumer both stopped.
    void beginGeneration(uint32_t generation) noexcept;

    size_t capacity() const noexcept { return capacity_; }
    size_t pending() const noexcept;
    bool full() const noexcept;
    bool empty() const noexcept;

private:
    std::vector<RecorderMessage> ring_;
    size_t capacity_ = 1u;

    std::atomic<uint32_t> head_{0u};
    std::atomic<uint32_t> tail_{0u};
    std::atomic<uint32_t> generation_{0u};
    std::atomic<uint32_t> highWater_{0u};
    std::atomic<uint64_t> dropped_{0u};
    std::atomic<uint64_t> staleRejects_{0u};
    std::atomic<bool> disabled_{false};

    uint32_t sequence_ = 0u;
};

}
