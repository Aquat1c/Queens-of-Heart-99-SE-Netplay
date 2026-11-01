#pragma once

#include "ReplayCompatibilityV2.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace qclient::replay {

enum class ReplayBrokerIoStatusV2 : uint8_t {
    Ok = 0,
    Timeout,
    Closed,
    Cancelled,
    Error,
};

class IReplayBrokerConnectionV2 {
public:
    virtual ~IReplayBrokerConnectionV2() = default;

    virtual ReplayBrokerIoStatusV2 readSome(
        std::vector<uint8_t>& out, uint32_t timeoutMs) = 0;
    virtual ReplayBrokerIoStatusV2 writeAll(
        const uint8_t* data, size_t bytes, uint32_t timeoutMs) = 0;
    virtual void close() = 0;
};

class IReplayBrokerListenerV2 {
public:
    virtual ~IReplayBrokerListenerV2() = default;

    virtual bool create(
        const std::wstring& pipeName, std::string& detail) = 0;
    virtual bool isListening() const = 0;
    virtual ReplayBrokerIoStatusV2 accept(
        std::unique_ptr<IReplayBrokerConnectionV2>& out,
        uint32_t timeoutMs) = 0;
    virtual void cancel() = 0;
};

enum class ReplayProcessWaitStatusV2 : uint8_t {
    Exited = 0,
    Timeout,
    Error,
};

class IReplayProcessMonitorV2 {
public:
    virtual ~IReplayProcessMonitorV2() = default;
    virtual bool hasExited(uint32_t& exitCode) = 0;
    virtual ReplayProcessWaitStatusV2 waitForExit(
        uint32_t timeoutMs, uint32_t& exitCode) = 0;
};

class ReplayBrokerCancellationV2 {
public:
    void request() { requested_.store(true); }
    bool requested() const { return requested_.load(); }

private:
    std::atomic<bool> requested_{false};
};

struct ReplayBrokerLaunchV2 {
    uint32_t gamePid = 0u;
    uint32_t launchGeneration = 0u;
    uint64_t launchNonce = 0u;
    bool headless = false;
    uint32_t connectTimeoutMs = 15000u;
    uint32_t helloTimeoutMs = 8000u;
    uint32_t ioProgressTimeoutMs = 8000u;
    uint32_t resultTimeoutMs = 300000u;
    uint32_t drainTimeoutMs = 8000u;
    uint32_t processExitTimeoutMs = 8000u;
};

enum class ReplayBrokerStateV2 : uint8_t {
    Idle = 0,
    Prepared,
    ListeningBeforeInjection,
    InjectionStarted,
    AwaitingHello,
    SendingPrebuffer,
    AwaitingResult,
    Draining,
    Complete,
    Cancelled,
    TimedOut,
    ProcessExitedWithoutResult,
    MissingResult,
    ProtocolFailure,
    RuntimeFailure,
    IoFailure,
};

const char* replayBrokerStateTextV2(ReplayBrokerStateV2 state);

struct ReplayBrokerStatusV2 {
    ReplayBrokerStateV2 state = ReplayBrokerStateV2::Idle;
    uint32_t launchGeneration = 0u;
    uint32_t sentFrames = 0u;
    uint32_t sentEvents = 0u;
    uint32_t receivedStatuses = 0u;
    uint32_t acknowledgedCommands = 0u;
    bool listenerReady = false;
    bool connected = false;
    bool resultReceived = false;
    bool byeReceived = false;
    std::string detail;
};

struct ReplayBrokerRunResultV2 {
    ReplayBrokerStateV2 state = ReplayBrokerStateV2::Idle;
    bool success = false;
    bool hasPlaybackResult = false;
    bool processExited = false;
    bool nextGameRefused = false;
    uint32_t processExitCode = 0u;
    qreplay::ipc::v3::PlaybackResultPayload playbackResult{};
    std::string detail;
};

struct ReplayBrokerNextGameV2 {
    std::unique_ptr<PreparedReplayV2> replay;
    uint16_t presentationFlags = 0u;
    std::string error;
};
using ReplayBrokerNextGameProviderV2 = std::function<ReplayBrokerNextGameV2(
    const qreplay::ipc::v3::PlaybackResultPayload&)>;

class ReplayBrokerV2 {
public:
    ReplayBrokerV2();
    ~ReplayBrokerV2();
    ReplayBrokerV2(const ReplayBrokerV2&) = delete;
    ReplayBrokerV2& operator=(const ReplayBrokerV2&) = delete;

    ReplayPreflightResultV2 prepare(
        const ReplayPreflightInputV2& input,
        const ReplayRuntimeIdentityV2& runtime);

    bool setNextGameProvider(ReplayBrokerNextGameProviderV2 provider);

    bool listenBeforeInjection(
        const ReplayBrokerLaunchV2& launch,
        std::unique_ptr<IReplayBrokerListenerV2> listener);
    bool readyForInjection() const;

    bool markInjectionStarted();

    ReplayBrokerRunResultV2 serve(
        IReplayProcessMonitorV2& process,
        ReplayBrokerCancellationV2* cancellation = nullptr);

    uint64_t enqueueCommand(
        qreplay::ipc::v3::PlaybackCommandKind kind,
        uint64_t argument);

    void cancel();
    bool reset();

    ReplayBrokerStatusV2 status() const;
    const std::wstring& pipeName() const { return pipeName_; }
    const PreparedReplayV2* prepared() const { return prepared_.get(); }

private:
    struct PendingCommand {
        qreplay::ipc::v3::PlaybackCommandPayload command{};
        bool sent = false;
    };

    void setState(ReplayBrokerStateV2 state, const std::string& detail);
    ReplayBrokerRunResultV2 finish(
        ReplayBrokerStateV2 state, const std::string& detail,
        bool success = false);
    bool terminalState(ReplayBrokerStateV2 state) const;

    std::unique_ptr<PreparedReplayV2> prepared_;
    std::unique_ptr<IReplayBrokerListenerV2> listener_;
    ReplayBrokerLaunchV2 launch_{};
    std::wstring pipeName_;
    std::vector<PendingCommand> commands_;
    uint64_t nextCommandId_ = 1u;
    ReplayBrokerNextGameProviderV2 nextGameProvider_;

    mutable std::mutex mutex_;
    ReplayBrokerStatusV2 status_{};
    std::atomic<bool> localCancel_{false};
    bool injectionStarted_ = false;
};

std::unique_ptr<IReplayBrokerListenerV2>
createWin32ReplayBrokerListenerV2();
std::unique_ptr<IReplayProcessMonitorV2>
createWin32ReplayProcessMonitorV2(uint32_t processId);

}
