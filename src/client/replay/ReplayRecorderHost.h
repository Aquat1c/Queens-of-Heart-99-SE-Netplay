#pragma once

#include "ReplayBattleLog.h"
#include "ReplayBroker.h"
#include "ReplayWriter.h"
#include "ReplayWriterV2.h"

#include "compat/CompatibilityIdentity.h"
#include "replay/ReplayTypes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qclient::replay {

enum class RecorderState : uint8_t {
    Idle = 0,
    Listening,
    Recording,
    Finalized,
    Disabled,
};

const char* recorderStateText(RecorderState state);

enum class RecorderDisableReason : uint16_t {
    None = 0,
    Stopped,
    PipeError,
    ProtocolError,
    HandshakeRejected,
    GameDisconnected,
    RecorderOverflow,
    DigestDrift,
    InvalidRecord,
    DiskFull,
    AccessDenied,
    WriteFailed,
    FlushFailed,
    RenameFailed,
    LimitExceeded,
    RootUnusable,
};

const char* recorderDisableReasonText(RecorderDisableReason reason);

struct RecorderHostConfig {
    std::wstring replayRoot;
    uint32_t gamePid = 0u;
    uint32_t generation = 1u;
    uint64_t nonce = 0u;
    std::string nameHint;
    uint32_t flushEveryChunks = 64u;
    uint32_t flushEveryMs = 2000u;
    uint32_t connectTimeoutMs = 30000u;
};

struct RecorderStatus {
    RecorderState state = RecorderState::Idle;
    RecorderDisableReason reason = RecorderDisableReason::None;
    std::wstring partPath;
    std::wstring finalPath;
    uint32_t chunks = 0u;
    uint32_t matches = 0u;
    uint32_t frames = 0u;
    uint32_t events = 0u;
    uint32_t droppedRecords = 0u;
    uint32_t proofDiscontinuities = 0u;
    uint64_t bytes = 0u;
    uint32_t flushes = 0u;
    std::string detail;

    bool active() const { return state == RecorderState::Recording; }
};

class ReplayRecorderHost {
public:
    ReplayRecorderHost();
    ~ReplayRecorderHost();
    ReplayRecorderHost(const ReplayRecorderHost&) = delete;
    ReplayRecorderHost& operator=(const ReplayRecorderHost&) = delete;

    bool start(const RecorderHostConfig& config);
    void stop();

    RecorderStatus status() const;
    const std::wstring& pipeName() const { return pipeName_; }

    void setWriterForTesting(std::unique_ptr<ReplayWriter> writer);

    void setConfigForTesting(const RecorderHostConfig& config);

    bool feedFrameForTesting(const ReplayIpcFrame& frame);

private:
    class Session;

    void workerMain();
    void disable(RecorderDisableReason reason, const char* detail);

    std::unique_ptr<Session> session_;
    std::unique_ptr<ReplayWriter> injectedWriter_;
    RecorderHostConfig config_{};
    std::wstring pipeName_;
    std::thread worker_;
    mutable std::mutex mutex_;
    RecorderStatus status_{};
    std::atomic<bool> running_{ false };
    void* pipe_ = nullptr;
};

struct RecorderHostConfigV3 {
    std::wstring replayRoot;
    uint32_t gamePid = 0u;
    uint32_t launchGeneration = 0u;
    uint64_t launchNonce = 0u;
    uint64_t runtimeCompatibilityId = 0u;
    qnetplay::compat::CompatibilityIdentity compatibilityIdentity{};
    std::string nameHint;
    uint32_t flushEveryChunks = 64u;
    uint32_t flushEveryMs = 2000u;
    uint32_t connectTimeoutMs = 30000u;

    int8_t battleLogLocalSide = kBattleSideSpectated;
    std::string battleLogLocalName;
    bool battleLogEnabled = true;
};

struct RecorderStatusV3 {
    qreplay::ipc::v3::RecorderHostState state =
        qreplay::ipc::v3::RecorderHostState::Listening;
    qreplay::GameIdentityV2 currentGame{};
    std::wstring partPath;
    std::wstring lastFinalPath;
    uint32_t gamesBegun = 0u;
    uint32_t palettesRecorded = 0u;
    uint32_t palettesRejected = 0u;
    uint32_t gamesCommitted = 0u;
    uint32_t gamesFailed = 0u;
    uint32_t nextRecord = 0u;
    uint32_t nextEventSequence = 0u;
    uint32_t inboundSequence = 0u;
    uint32_t outboundSequence = 0u;
    uint64_t bytesWritten = 0u;
    bool helloAccepted = false;
    bool drainAcknowledged = false;
    bool failed = false;
    bool lastProcessAlive = false;
    qreplay::TerminalReason lastTerminalReason =
        qreplay::TerminalReason::RecordingFailure;
    qreplay::ipc::v3::RecorderFailureCode failure =
        qreplay::ipc::v3::RecorderFailureCode::Internal;
    uint16_t failureStage = 0u;
    std::string detail;
};

using ReplayWriterFactoryV2 =
    std::function<std::unique_ptr<ReplayWriterV2>()>;

class ReplayRecorderHostV3 {
public:
    ReplayRecorderHostV3();
    ~ReplayRecorderHostV3();
    ReplayRecorderHostV3(const ReplayRecorderHostV3&) = delete;
    ReplayRecorderHostV3& operator=(const ReplayRecorderHostV3&) = delete;

    bool start(const RecorderHostConfigV3& config);
    void stop();
    RecorderStatusV3 status() const;
    const std::wstring& pipeName() const { return pipeName_; }

    void setConfigForTesting(const RecorderHostConfigV3& config);
    void setWriterFactoryForTesting(ReplayWriterFactoryV2 factory);
    void setProcessAliveProbeForTesting(
        std::function<bool(uint32_t)> probe);
    void setBattleClockForTesting(std::function<int64_t()> clock);
    bool feedFrameForTesting(
        const qreplay::ipc::v3::Frame& frame);
    std::vector<qreplay::ipc::v3::Frame>
    takeOutboundFramesForTesting();

private:
    bool handleFrame(const qreplay::ipc::v3::Frame& frame);
    bool handleFreshFrame(const qreplay::ipc::v3::Frame& frame);
    bool flushPendingTick();
    bool addEvents(
        const std::vector<qreplay::ConfirmedGameEventV2>& events);
    bool addFrames(
        const std::vector<qreplay::ConfirmedGameFrameV2>& frames);
    bool emit(
        qreplay::ipc::v3::MessageType type,
        const qreplay::GameIdentityV2& game,
        const std::vector<uint8_t>& payload,
        uint32_t param0 = 0u, uint32_t param1 = 0u);
    bool failProtocol(
        qreplay::ipc::v3::RecorderFailureCode code,
        uint16_t stage, const qreplay::GameIdentityV2& game,
        const char* detail);
    void resetGame();
    void sealOpenGameOnDisconnect();
    void appendBattleLogEntry(const qreplay::ConfirmedGameEndV2& end,
                              const WriterSummaryV2& summary);
    int64_t battleClockNow() const;
    void workerMain();

    RecorderHostConfigV3 config_{};
    ReplayWriterFactoryV2 writerFactory_;
    std::function<bool(uint32_t)> processAliveProbe_;
    std::function<int64_t()> battleClock_;
    std::unique_ptr<ReplayWriterV2> writer_;
    qreplay::CanonicalTickBundleV2 pendingTick_{};
    bool hasPendingFrame_ = false;
    uint32_t expectedFrame_ = 0u;
    uint32_t expectedEventSequence_ = 0u;
    uint8_t activeLaneMask_ = 0u;
    qreplay::GameManifestV2 retainedManifest_{};
    qreplay::MatchConfigurationV2 retainedConfiguration_{};
    std::string retainedSetFolder_;
    int64_t gameStartUnix_ = 0;
    int64_t setStartUnix_ = 0;
    bool haveRetainedGame_ = false;
    bool drainSeen_ = false;
    qreplay::ipc::v3::Frame lastInbound_{};
    bool haveLastInbound_ = false;
    std::vector<qreplay::ipc::v3::Frame> lastResponses_;
    std::vector<qreplay::ipc::v3::Frame> testOutbound_;

    std::wstring pipeName_;
    std::thread worker_;
    mutable std::mutex mutex_;
    RecorderStatusV3 status_{};
    std::atomic<bool> running_{false};
    void* pipe_ = nullptr;
};

}
