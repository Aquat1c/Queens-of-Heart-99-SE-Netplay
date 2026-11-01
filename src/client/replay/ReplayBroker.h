#pragma once

#include "ReplayCompatibility.h"
#include "ReplayParser.h"

#include "replay/ReplayIpcWire.h"
#include "replay/ReplayTypes.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qclient::replay {

namespace wire = qreplay::ipc;

constexpr uint32_t kReplayIpcMagic = wire::kReplayIpcMagic;
constexpr size_t kReplayIpcHeaderBytes = wire::kReplayIpcHeaderBytes;
constexpr uint32_t kReplayIpcMaxPayloadBytes = wire::kReplayIpcMaxPayloadBytes;
constexpr uint32_t kReplayIpcWireVersion = wire::kReplayIpcWireVersion;

using ReplayIpcChannel = wire::Channel;
using ReplayIpcType = wire::MessageType;
using ReplayIpcFrame = wire::Frame;
using IpcFrameStatus = wire::FrameStatus;
using ReplayIpcHello = wire::HelloPayload;
using ReplayIpcSessionInfo = wire::SessionInfo;
using ReplayIpcTerminal = wire::TerminalPayload;

inline const char* ipcFrameStatusText(IpcFrameStatus status) {
    return wire::frameStatusText(status);
}

inline bool encodeIpcFrame(const ReplayIpcFrame& frame,
                           std::vector<uint8_t>& out) {
    return wire::encodeFrame(frame, out);
}

inline IpcFrameStatus decodeIpcFrame(const uint8_t* data, size_t len,
                                     ReplayIpcFrame& out, size_t& consumed) {
    return wire::decodeFrame(data, len, out, consumed);
}

struct BrokerSelection {
    std::wstring path;
    uint32_t matchOrdinal = 0u;
    uint32_t startRound = 0u;
};

struct BrokerConfig {
    uint32_t gamePid = 0u;
    uint32_t generation = 1u;
    uint64_t nonce = 0u;
    uint64_t manifestDigest = 0u;
    uint32_t prebufferFrameLimit = 300000u;
    uint32_t connectTimeoutMs = 15000u;
};

enum class BrokerState : uint8_t {
    Idle = 0,
    Prepared,
    Listening,
    Serving,
    Finished,
    Failed,
};

const char* brokerStateText(BrokerState state);

struct BrokerPrepareResult {
    bool ok = false;
    qreplay::ParseResult parse{};
    qreplay::CompatibilityResult compatibility{};
    uint32_t matchId = 0u;
    uint32_t startFrame = 0u;
    uint32_t frameCount = 0u;
    uint32_t eventCount = 0u;
    bool prebuffered = false;
    uint32_t lastFrame = 0u;
    std::string detail;
};

struct BrokerStatus {
    BrokerState state = BrokerState::Idle;
    bool connected = false;
    bool buffering = false;
    uint32_t servedFrames = 0u;
    uint32_t lastServedFrame = 0u;
    uint32_t rejectedFrames = 0u;
    uint32_t generation = 0u;
    std::string detail;
};

class ReplayBroker {
public:
    ReplayBroker();
    ~ReplayBroker();
    ReplayBroker(const ReplayBroker&) = delete;
    ReplayBroker& operator=(const ReplayBroker&) = delete;

    BrokerPrepareResult prepare(const BrokerSelection& selection,
                                const RuntimeIdentity& runtime);

    // Must be called after a successful prepare() and before the game is resumed.
    bool start(const BrokerConfig& config);
    void stop();

    BrokerStatus status() const;
    const std::wstring& pipeName() const { return pipeName_; }

private:
    struct Prepared;

    void workerMain();
    bool serveConnection(void* pipe);

    std::unique_ptr<Prepared> prepared_;
    BrokerConfig config_{};
    std::wstring pipeName_;
    std::thread worker_;
    mutable std::mutex mutex_;
    BrokerStatus status_{};
    std::atomic<bool> running_{ false };
    void* pipe_ = nullptr;
};

}
