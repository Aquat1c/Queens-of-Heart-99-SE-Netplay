#pragma once

#include "ipc/IpcConfig.h"
#include "replay/ConfirmedTimeline.h"
#include "replay/ReplayIpcWire.h"
#include "replay/ReplayPlaybackSessionV2.h"
#include "replay/ReplayTypes.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace qreplay {
class ReplayRecorderQueue;
class ReplayPublisherQueueV2;
}

namespace qgame {

class ByteChannel {
public:
    virtual ~ByteChannel() = default;

    virtual bool connect(uint32_t timeoutMs) = 0;

    virtual void close() = 0;

    virtual bool connected() const = 0;

    virtual bool read(uint8_t* dst, size_t cap, size_t& read) = 0;

    virtual bool write(const uint8_t* src, size_t len) = 0;
};

std::unique_ptr<ByteChannel> makeNamedPipeChannel(const wchar_t* pipeName,
                                                  uint32_t writeTimeoutMs);

template <typename T, size_t Capacity>
class BoundedSpscRing {
    static_assert(Capacity >= 2u, "ring needs at least two slots");

public:
    BoundedSpscRing() = default;
    BoundedSpscRing(const BoundedSpscRing&) = delete;
    BoundedSpscRing& operator=(const BoundedSpscRing&) = delete;

    bool push(const T& value) {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        const uint32_t used = tail - head;
        if (used >= static_cast<uint32_t>(Capacity)) {
            drops_.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        slots_[tail % Capacity] = value;
        tail_.store(tail + 1u, std::memory_order_release);
        const uint32_t depth = used + 1u;
        uint32_t seen = highWater_.load(std::memory_order_relaxed);
        while (depth > seen
               && !highWater_.compare_exchange_weak(
                      seen, depth, std::memory_order_relaxed)) {
        }
        return true;
    }

    bool pop(T& out) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) return false;
        out = slots_[head % Capacity];
        head_.store(head + 1u, std::memory_order_release);
        return true;
    }

    size_t size() const {
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        const uint32_t head = head_.load(std::memory_order_acquire);
        return static_cast<size_t>(tail - head);
    }

    bool full() const { return size() >= Capacity; }
    bool empty() const { return size() == 0u; }
    static constexpr size_t capacity() { return Capacity; }

    uint32_t highWater() const {
        return highWater_.load(std::memory_order_relaxed);
    }
    uint32_t drops() const { return drops_.load(std::memory_order_relaxed); }

    void reset() {
        head_.store(0u, std::memory_order_relaxed);
        tail_.store(0u, std::memory_order_relaxed);
        highWater_.store(0u, std::memory_order_relaxed);
        drops_.store(0u, std::memory_order_relaxed);
    }

private:
    std::array<T, Capacity> slots_{};
    alignas(64) std::atomic<uint32_t> head_{0u};
    alignas(64) std::atomic<uint32_t> tail_{0u};
    std::atomic<uint32_t> highWater_{0u};
    std::atomic<uint32_t> drops_{0u};
};

namespace replay {

namespace wire = qreplay::ipc;

constexpr uint32_t kReplayIpcMagic = wire::kReplayIpcMagic;
constexpr uint16_t kReplayIpcSchemaVersion = wire::kReplayIpcWireVersion;
constexpr size_t kIpcFrameHeaderBytes = wire::kReplayIpcHeaderBytes;
constexpr uint32_t kMaxIpcPayloadBytes = wire::kReplayIpcMaxPayloadBytes;

constexpr size_t kIpcReceiveBufferBytes = 128u * 1024u;

using IpcChannel = wire::Channel;
using IpcMessageKind = wire::MessageType;
using IpcFrameHeader = wire::FrameHeader;
using IpcFrameStatus = wire::FrameStatus;

constexpr bool isBrokerInboundKind(uint8_t kind) {
    return wire::isBrokerStreamType(kind);
}

constexpr bool isRecorderOutboundKind(uint8_t kind) {
    return wire::isRecorderType(kind);
}

inline void encodeIpcHeader(const IpcFrameHeader& header,
                            std::vector<uint8_t>& out) {
    wire::encodeHeader(header, out);
}

inline IpcFrameStatus decodeIpcHeaderStatus(const uint8_t* data, size_t len,
                                            IpcFrameHeader& out) {
    return wire::decodeHeader(data, len, out);
}

inline bool decodeIpcHeader(const uint8_t* data, size_t len,
                            IpcFrameHeader& out) {
    return wire::decodeHeader(data, len, out) == IpcFrameStatus::Ok;
}

enum class SourceFault : uint16_t {
    None = 0,
    ChannelLost,
    ReconnectDeadline,
    BadFraming,
    BadPayloadCrc,
    StaleGeneration,
    WrongNonce,
    SequenceViolation,
    PayloadTooLarge,
    DecodeFailed,
    ManifestMismatch,
    FrameGap,
    InvalidActionMask,
    InvalidLaneShape,
    DigestBreak,
    UnexpectedMessage,
};

const char* sourceFaultText(SourceFault fault);

using BrokerSessionInfo = wire::SessionInfo;

constexpr size_t kBrokerSessionInfoBytes = wire::kSessionInfoBytes;

inline bool encodeBrokerSessionInfo(const BrokerSessionInfo& info,
                                    std::vector<uint8_t>& out) {
    return wire::encodeSessionInfo(info, out);
}

inline bool decodeBrokerSessionInfo(const uint8_t* data, size_t len,
                                    BrokerSessionInfo& out) {
    return wire::decodeSessionInfo(data, len, out);
}

struct BrokerControl {
    enum class Kind : uint8_t {
        SessionInfo = 1,
        Start = 2,
        Terminal = 3,
        Fault = 4,
    };

    Kind kind = Kind::Fault;
    BrokerSessionInfo info{};
    qreplay::DeterministicStart start{};
    uint16_t terminalReason = 0u;
    uint32_t lastTimelineFrame = 0u;
    uint64_t postTailStateHash = 0u;
    uint64_t rollingDigest = 0u;
    SourceFault fault = SourceFault::None;
    uint32_t faultDetail = 0u;
};

struct ReplayIpcOptions {
    uint32_t gamePid = 0u;
    uint32_t replayGeneration = 0u;
    uint32_t recordGeneration = 0u;
    uint64_t brokerNonce = 0u;
    uint64_t recorderNonce = 0u;
    uint64_t runtimeCompatibilityId = 0u;
    uint64_t manifestDigest = 0u;
    uint16_t schemaVersion = kReplayIpcSchemaVersion;
    uint16_t featureBits = 0u;
    bool supersededStateSchema = false;
    uint32_t matchOrdinal = 0u;
    uint8_t recordSourceKind = 0u;
    bool playback = false;
    bool playbackV3 = false;
    bool recording = false;
    bool recordingV3 = false;
    qreplay::GameIdentityV2 playbackGame{};
    uint32_t playbackExpectedFrameCount = 0u;
    uint32_t playbackExpectedEventCount = 0u;
    uint64_t playbackValidatedSourceByteCount = 0u;
    std::array<uint8_t, 32> playbackValidatedSourceRangeDigest{};
    uint8_t playbackEndpointKind = 0u;
    bool playbackHeadless = false;
    qreplay::SimulationContractV2 playbackRuntimeContract{};
    uint32_t connectTimeoutMs = 2000u;
    uint32_t writeTimeoutMs = 1000u;
    uint32_t maxReconnectAttempts = 8u;
    uint32_t reconnectDeadlineMs = 6000u;
    uint32_t playbackPrebufferDeadlineMs =
        qipc::kReplayPrebufferDeadlineMs;
    uint32_t reconnectBackoffMs = 25u;
    uint32_t idleSleepMs = 1u;
    uint32_t recorderDrainPerTurn = 256u;
};

struct ReplayIpcStats {
    uint64_t framesReceived = 0u;
    uint64_t eventsReceived = 0u;
    uint64_t messagesRejected = 0u;
    uint64_t bytesReceived = 0u;
    uint64_t recorderMessagesSent = 0u;
    uint64_t recorderRecordsSent = 0u;
    uint64_t recorderDrops = 0u;
    uint32_t reconnects = 0u;
    uint32_t frameRingHighWater = 0u;
    uint32_t controlRingDrops = 0u;
    uint8_t brokerConnected = 0u;
    uint8_t recorderConnected = 0u;
    uint8_t recordingDisabled = 0u;
    uint16_t recordingDisableReason = 0u;
    uint16_t fault = 0u;
};

constexpr size_t kFramePrefetchCapacity = 2048u;
constexpr size_t kEventPrefetchCapacity = 256u;
constexpr size_t kControlCapacity = 16u;
constexpr size_t kSeekCommandCapacity = 8u;
constexpr size_t kRecorderCommitAckCapacity = 16u;
constexpr size_t kPlaybackCommandCapacityV3 = 32u;
constexpr size_t kPlaybackOutboundCapacityV3 = 32u;

struct PlaybackPrebufferV3 {
    qreplay::GameIdentityV2 game{};
    qreplay::ipc::v3::PlaybackSessionInfoPayload sessionInfo{};
    qreplay::ConfirmedGameStartV2 start{};
    std::array<qreplay::ReplayPaletteV2, 4> palettes{};
    std::unique_ptr<qreplay::ConfirmedTimelineBufferV2> timeline;
    qreplay::ReplayPlaybackBindingV2 binding{};
};

class ReplayIpcWorker {
public:
    ReplayIpcWorker();
    ~ReplayIpcWorker();

    ReplayIpcWorker(const ReplayIpcWorker&) = delete;
    ReplayIpcWorker& operator=(const ReplayIpcWorker&) = delete;

    bool start(const ReplayIpcOptions& options,
               std::unique_ptr<ByteChannel> brokerChannel,
               std::unique_ptr<ByteChannel> recorderChannel,
               qreplay::ReplayRecorderQueue* recorderQueue,
               qreplay::ReplayPublisherQueueV2* recorderQueueV2 = nullptr);

    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    bool popFrame(qreplay::ReplayFrameV1& out) { return frames_.pop(out); }
    bool popEvent(qreplay::ReplayEvent& out) { return events_.pop(out); }
    bool popControl(BrokerControl& out) { return control_.pop(out); }
    size_t bufferedFrames() const { return frames_.size(); }
    size_t bufferedEvents() const { return events_.size(); }

    bool playbackPrebufferReadyV3() const {
        return playbackPrebufferV3_.load(std::memory_order_acquire) != nullptr;
    }
    std::unique_ptr<PlaybackPrebufferV3> takePlaybackPrebufferV3();
    SourceFault playbackFaultV3() const {
        return static_cast<SourceFault>(
            playbackFaultV3_.load(std::memory_order_acquire));
    }
    bool popPlaybackCommandV3(
        qreplay::ipc::v3::PlaybackCommandPayload& out) {
        return playbackCommandsV3_.pop(out);
    }
    bool submitPlaybackCommandAckV3(
        const qreplay::ipc::v3::PlaybackCommandAckPayload& ack);
    bool submitPlaybackStatusV3(
        const qreplay::ipc::v3::PlaybackStatusPayload& status);
    bool submitPlaybackResultV3(
        const qreplay::ipc::v3::PlaybackResultPayload& result);
    bool submitPlaybackByeV3(
        const qreplay::ipc::v3::PlaybackByePayload& bye);
    bool popPlaybackNextGameV3(
        qreplay::ipc::v3::PlaybackNextGamePayload& out) {
        return playbackNextGamesV3_.pop(out);
    }
    bool submitPlaybackNextGameAckV3(
        const qreplay::ipc::v3::PlaybackNextGamePayload& next);

    bool requestSeek(uint32_t fromFrame, uint32_t targetFrame);

    void disableRecording(uint16_t reason);
    bool recordingDisabled() const {
        return recordingDisabled_.load(std::memory_order_acquire);
    }
    void noteRecorderDrop() {
        recorderDrops_.fetch_add(1u, std::memory_order_relaxed);
    }
    bool popGameCommitAck(qreplay::GameCommitAckV2& out) {
        return commitAcks_.pop(out);
    }
    bool requestRecorderDrain(bool abandonedOpenGame);
    bool recorderDrainAcknowledged() const {
        return recorderDrainAcked_.load(std::memory_order_acquire);
    }

    ReplayIpcStats stats() const;

    bool startForTest(const ReplayIpcOptions& options,
                      std::unique_ptr<ByteChannel> brokerChannel,
                      std::unique_ptr<ByteChannel> recorderChannel,
                      qreplay::ReplayRecorderQueue* recorderQueue,
                      qreplay::ReplayPublisherQueueV2* recorderQueueV2 = nullptr);
    bool pumpOnceForTest();

private:
    // Raw CreateThread, never std::thread: a shutdown under the loader lock would deadlock joining a CRT thread object.
    static unsigned long __stdcall threadEntry(void* self);

    void buildHelloPayload(std::vector<uint8_t>& out) const;
    bool sendBrokerHello();
    bool drainSeekCommands();
    bool sendFramed(ByteChannel& channel, IpcMessageKind kind, uint32_t param0,
                    uint32_t param1, const std::vector<uint8_t>& payload,
                    uint32_t& sequence);
    void dropBroker(SourceFault fault, uint32_t detail);
    bool flushPending();
    bool pumpBrokerReceive();
    bool handleBrokerMessage(const IpcFrameHeader& header,
                             const uint8_t* payload, uint32_t payloadBytes);
    bool sendPlaybackHelloV3();
    bool pumpPlaybackReceiveV3();
    bool handlePlaybackMessageV3(
        const qreplay::ipc::v3::Frame& frame);
    bool flushPlaybackOutboundV3();
    bool sendPlaybackFrameV3(
        qreplay::ipc::v3::MessageType type,
        uint32_t param0, uint32_t param1,
        const std::vector<uint8_t>& payload);
    void failPlaybackV3(SourceFault fault, uint32_t detail);
    bool publishPlaybackPrebufferV3(
        const qreplay::ipc::v3::PlaybackEndpointPayload& endpoint);
    bool pumpRecorder();
    bool pumpRecorderV3();
    bool pumpRecorderResponsesV3();
    bool sendRecorderFrameV3(
        qreplay::ipc::v3::MessageType type,
        const qreplay::GameIdentityV2& game,
        uint32_t param0, uint32_t param1,
        const std::vector<uint8_t>& payload);
    bool flushRecorderFrames();
    bool flushRecorderEvents();
    void emitRecorderTerminal(uint16_t reason);
    void failRecorderChannel();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    BoundedSpscRing<qreplay::ReplayFrameV1, kFramePrefetchCapacity> frames_;
    BoundedSpscRing<qreplay::ReplayEvent, kEventPrefetchCapacity> events_;
    BoundedSpscRing<BrokerControl, kControlCapacity> control_;

    struct SeekCommand {
        uint32_t fromFrame = 0u;
        uint32_t targetFrame = 0u;
    };
    BoundedSpscRing<SeekCommand, kSeekCommandCapacity> seeks_;
    BoundedSpscRing<
        qreplay::GameCommitAckV2, kRecorderCommitAckCapacity> commitAcks_;

    struct PlaybackOutboundV3 {
        qreplay::ipc::v3::MessageType type =
            qreplay::ipc::v3::MessageType::PlaybackStatus;
        qreplay::ipc::v3::PlaybackCommandAckPayload commandAck{};
        qreplay::ipc::v3::PlaybackStatusPayload status{};
        qreplay::ipc::v3::PlaybackResultPayload result{};
        qreplay::ipc::v3::PlaybackByePayload bye{};
        qreplay::ipc::v3::PlaybackNextGamePayload nextGame{};
    };
    BoundedSpscRing<
        qreplay::ipc::v3::PlaybackCommandPayload,
        kPlaybackCommandCapacityV3> playbackCommandsV3_;
    BoundedSpscRing<qreplay::ipc::v3::PlaybackNextGamePayload, 2u>
        playbackNextGamesV3_;
    BoundedSpscRing<
        PlaybackOutboundV3,
        kPlaybackOutboundCapacityV3> playbackOutboundV3_;
    std::atomic<PlaybackPrebufferV3*> playbackPrebufferV3_{nullptr};
    std::atomic<uint16_t> playbackFaultV3_{
        static_cast<uint16_t>(SourceFault::None)};

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> recordingDisabled_{false};
    std::atomic<uint16_t> recordingDisableReason_{0u};
    std::atomic<uint64_t> recorderDrops_{0u};
    std::atomic<bool> recorderDrainRequested_{false};
    std::atomic<bool> recorderDrainAbandonedOpenGame_{false};
    std::atomic<bool> recorderDrainAcked_{false};
};

}
}
