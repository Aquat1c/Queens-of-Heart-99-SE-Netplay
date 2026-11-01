#include "ReplayBrokerV2.h"
#include "replay/ReplaySimulationContractV2.h"

#include "Log.h"
#include "ipc/IpcConfig.h"
#include "replay/ReplayCodec.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace qclient::replay {

namespace {

using namespace qreplay;
namespace wire3 = qreplay::ipc::v3;

constexpr size_t kFrameBatchV2 = 256u;
constexpr size_t kEventBatchV2 = 512u;
constexpr size_t kMaximumInboundBufferV2 =
    wire3::kHeaderBytes + wire3::kMaxPayloadBytes;
constexpr uint32_t kPollSliceMsV2 = 50u;

bool isVerifiedReason(wire3::PlaybackResultReason reason) {
    return reason == wire3::PlaybackResultReason::VerifiedCompleted
        || reason == wire3::PlaybackResultReason::VerifiedAbnormalEnd
        || reason
            == wire3::PlaybackResultReason::VerifiedBehaviourOnly
        || reason
            == wire3::PlaybackResultReason::CompletedWithDivergence;
}

bool validLaunch(const ReplayBrokerLaunchV2& launch) {
    return launch.gamePid != 0u && launch.launchGeneration != 0u
        && launch.launchNonce != 0u
        && launch.connectTimeoutMs != 0u
        && launch.helloTimeoutMs != 0u
        && launch.ioProgressTimeoutMs != 0u
        && launch.resultTimeoutMs != 0u
        && launch.drainTimeoutMs != 0u
        && launch.processExitTimeoutMs != 0u;
}

enum class ReadOutcome {
    Ok,
    Timeout,
    Closed,
    Cancelled,
    ProcessExited,
    IoError,
    ProtocolError,
};

class FrameReaderV2 {
public:
    explicit FrameReaderV2(IReplayBrokerConnectionV2& connection)
        : connection_(connection) {}

    ReadOutcome read(
        uint32_t timeoutMs,
        IReplayProcessMonitorV2& process,
        const ReplayBrokerCancellationV2* externalCancel,
        const std::atomic<bool>& localCancel,
        wire3::Frame& out,
        std::string& detail) {
        using Clock = std::chrono::steady_clock;
        const auto deadline =
            Clock::now() + std::chrono::milliseconds(timeoutMs);
        bool tried = false;
        for (;;) {
            const wire3::FrameStatus decoded = decodeOne(out);
            if (decoded == wire3::FrameStatus::Ok)
                return ReadOutcome::Ok;
            if (decoded != wire3::FrameStatus::NeedMore) {
                detail = "invalid wire-v3 frame";
                return ReadOutcome::ProtocolError;
            }
            if ((externalCancel != nullptr
                    && externalCancel->requested())
                || localCancel.load()) {
                detail = "replay broker cancelled";
                return ReadOutcome::Cancelled;
            }
            uint32_t exitCode = 0u;
            if (process.hasExited(exitCode)) {
                detail = "game process exited before the replay exchange";
                return ReadOutcome::ProcessExited;
            }

            const auto now = Clock::now();
            if (tried && now >= deadline) {
                detail = "timed out waiting for replay IPC";
                return ReadOutcome::Timeout;
            }
            const uint64_t remaining = now >= deadline
                ? 0u
                : static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - now).count());
            const uint32_t slice = static_cast<uint32_t>(
                std::min<uint64_t>(
                    kPollSliceMsV2,
                    std::max<uint64_t>(remaining, timeoutMs == 0u ? 0u : 1u)));
            std::vector<uint8_t> bytes;
            const ReplayBrokerIoStatusV2 io =
                connection_.readSome(bytes, slice);
            tried = true;
            switch (io) {
                case ReplayBrokerIoStatusV2::Ok:
                    if (bytes.empty()
                        || bytes.size()
                            > kMaximumInboundBufferV2 - inbox_.size()) {
                        detail = "replay IPC input exceeded its bounded buffer";
                        return ReadOutcome::ProtocolError;
                    }
                    inbox_.insert(
                        inbox_.end(), bytes.begin(), bytes.end());
                    break;
                case ReplayBrokerIoStatusV2::Timeout:
                    if (timeoutMs == 0u) return ReadOutcome::Timeout;
                    break;
                case ReplayBrokerIoStatusV2::Closed:
                    detail = "replay IPC channel closed";
                    return ReadOutcome::Closed;
                case ReplayBrokerIoStatusV2::Cancelled:
                    detail = "replay IPC operation cancelled";
                    return ReadOutcome::Cancelled;
                case ReplayBrokerIoStatusV2::Error:
                    detail = "replay IPC read failed";
                    return ReadOutcome::IoError;
            }
        }
    }

    ReadOutcome poll(
        wire3::Frame& out, bool& hasFrame, std::string& detail) {
        hasFrame = false;
        const wire3::FrameStatus buffered = decodeOne(out);
        if (buffered == wire3::FrameStatus::Ok) {
            hasFrame = true;
            return ReadOutcome::Ok;
        }
        if (buffered != wire3::FrameStatus::NeedMore) {
            detail = "invalid wire-v3 frame during prebuffer";
            return ReadOutcome::ProtocolError;
        }
        std::vector<uint8_t> bytes;
        const ReplayBrokerIoStatusV2 io =
            connection_.readSome(bytes, 0u);
        if (io == ReplayBrokerIoStatusV2::Timeout)
            return ReadOutcome::Ok;
        if (io == ReplayBrokerIoStatusV2::Ok && !bytes.empty()) {
            if (bytes.size() > kMaximumInboundBufferV2 - inbox_.size()) {
                detail = "replay IPC input exceeded its bounded buffer";
                return ReadOutcome::ProtocolError;
            }
            inbox_.insert(inbox_.end(), bytes.begin(), bytes.end());
            const wire3::FrameStatus decoded = decodeOne(out);
            if (decoded == wire3::FrameStatus::Ok) {
                hasFrame = true;
                return ReadOutcome::Ok;
            }
            if (decoded != wire3::FrameStatus::NeedMore) {
                detail = "invalid wire-v3 frame during prebuffer";
                return ReadOutcome::ProtocolError;
            }
            return ReadOutcome::Ok;
        }
        if (io == ReplayBrokerIoStatusV2::Closed) {
            detail = "runtime closed while the replay was being prebuffered";
            return ReadOutcome::Closed;
        }
        if (io == ReplayBrokerIoStatusV2::Cancelled) {
            detail = "replay IPC operation cancelled";
            return ReadOutcome::Cancelled;
        }
        if (io == ReplayBrokerIoStatusV2::Error
            || (io == ReplayBrokerIoStatusV2::Ok && bytes.empty())) {
            detail = "replay IPC read failed";
            return ReadOutcome::IoError;
        }
        return ReadOutcome::Ok;
    }

    bool hasBufferedBytes() const { return !inbox_.empty(); }

private:
    wire3::FrameStatus decodeOne(wire3::Frame& out) {
        if (inbox_.empty()) return wire3::FrameStatus::NeedMore;
        wire3::Frame decoded{};
        size_t consumed = 0u;
        const wire3::FrameStatus status = wire3::decodeFrame(
            inbox_.data(), inbox_.size(), decoded, consumed);
        if (status != wire3::FrameStatus::Ok) return status;
        inbox_.erase(
            inbox_.begin(),
            inbox_.begin() + static_cast<ptrdiff_t>(consumed));
        out = static_cast<wire3::Frame&&>(decoded);
        return wire3::FrameStatus::Ok;
    }

    IReplayBrokerConnectionV2& connection_;
    std::vector<uint8_t> inbox_;
};

bool sameGame(
    const GameIdentityV2& a, const GameIdentityV2& b) {
    return a == b;
}

bool validateResultBinding(
    const wire3::PlaybackResultPayload& result,
    const PreparedReplayV2& prepared,
    bool headless,
    std::string& detail) {
    const wire3::PlaybackEndpointPayload& endpoint =
        prepared.endpoint();
    const wire3::PlaybackSessionInfoPayload& info =
        prepared.sessionInfo();
    if (endpoint.kind != wire3::PlaybackEndpointKind::RecordedGameEnd
        || !endpoint.hasRecordedGameEnd) {
        detail = "the broker received a non-finalized replay endpoint";
        return false;
    }
    const SimulationContractV2& boundContract =
        prepared.start().manifest.contract;
    if (!replayContractAgreesForPlaybackV2(boundContract,
                                           result.runtimeContract)) {
        detail = "PlaybackResult echoed a different simulation contract than "
                 "the recorded one the runtime was bound to";
        return false;
    }
    if (result.endpointKind != endpoint.kind
        || result.headless != headless
        || result.validatedSourceByteCount
            != endpoint.validatedSourceByteCount
        || result.validatedSourceRangeDigest
            != endpoint.validatedSourceRangeDigest
        || result.hasFileDigest != info.hasFileDigest
        || result.fileDigest != info.fileDigest
        || result.hasCanonicalGameDigest
            != info.hasCanonicalGameDigest
        || result.canonicalGameDigest
            != info.canonicalGameDigest
        || result.hasRecordedTerminal
            != endpoint.hasRecordedGameEnd
        || result.finalRollingDigest
            != endpoint.finalRollingDigest
        || result.postTailStateHash
            != endpoint.postTailStateHash
        || result.nextRecordConsumed > endpoint.nextRecord) {
        detail = "PlaybackResult does not bind the selected game and endpoint";
        return false;
    }
    if (result.recordedTerminal
        != static_cast<uint16_t>(endpoint.recordedGameEnd.reason)) {
        detail = "PlaybackResult terminal differs from recorded GameEnd";
        return false;
    }

    if (!isVerifiedReason(result.reason)) return true;
    if (result.nextRecordConsumed != endpoint.nextRecord) {
        detail = "a verified result did not consume the complete endpoint";
        return false;
    }
    if (result.reason
        == wire3::PlaybackResultReason::CompletedWithDivergence) {
        return true;
    }
    const qreplay::ReplayVerificationModeV2 mode =
        prepared.verificationMode();
    if (mode == qreplay::ReplayVerificationModeV2::BehaviourVerified) {
        if (result.reason
            != wire3::PlaybackResultReason::VerifiedBehaviourOnly) {
            detail = "a rematch recording returned a byte-exact verified "
                     "result it cannot have proven";
            return false;
        }
        return true;
    }
    if (result.reason
        == wire3::PlaybackResultReason::VerifiedBehaviourOnly) {
        detail = "a first-game recording returned a behaviour-only result "
                 "instead of the required byte-exact proof";
        return false;
    }
    if (endpoint.recordedGameEnd.reason == TerminalReason::Completed) {
        if (result.reason
            != wire3::PlaybackResultReason::VerifiedCompleted) {
            detail = "a completed GameEnd returned the wrong verified result";
            return false;
        }
    } else if (result.reason
               != wire3::PlaybackResultReason::VerifiedAbnormalEnd) {
        detail = "an abnormal GameEnd returned the wrong verified result";
        return false;
    }
    return true;
}

bool byeMatchesResult(
    const wire3::PlaybackByePayload& bye,
    const wire3::PlaybackResultPayload& result,
    uint64_t drainCommandId) {
    wire3::PlaybackByeReason expected =
        wire3::PlaybackByeReason::FailureAfterResult;
    if (isVerifiedReason(result.reason))
        expected = wire3::PlaybackByeReason::NormalAfterResult;
    else if (result.reason == wire3::PlaybackResultReason::UserExit)
        expected = wire3::PlaybackByeReason::UserExitAfterResult;
    return bye.reason == expected && bye.resultSent
        && bye.nextRecord == result.nextRecordConsumed
        && bye.lastCommandId == drainCommandId;
}

}

const char* replayBrokerStateTextV2(ReplayBrokerStateV2 state) {
    switch (state) {
        case ReplayBrokerStateV2::Idle: return "idle";
        case ReplayBrokerStateV2::Prepared: return "prepared";
        case ReplayBrokerStateV2::ListeningBeforeInjection:
            return "listening before injection";
        case ReplayBrokerStateV2::InjectionStarted:
            return "injection started";
        case ReplayBrokerStateV2::AwaitingHello: return "awaiting hello";
        case ReplayBrokerStateV2::SendingPrebuffer:
            return "sending prebuffer";
        case ReplayBrokerStateV2::AwaitingResult:
            return "awaiting result";
        case ReplayBrokerStateV2::Draining: return "draining";
        case ReplayBrokerStateV2::Complete: return "complete";
        case ReplayBrokerStateV2::Cancelled: return "cancelled";
        case ReplayBrokerStateV2::TimedOut: return "timed out";
        case ReplayBrokerStateV2::ProcessExitedWithoutResult:
            return "process exited without result";
        case ReplayBrokerStateV2::MissingResult: return "missing result";
        case ReplayBrokerStateV2::ProtocolFailure:
            return "protocol failure";
        case ReplayBrokerStateV2::RuntimeFailure: return "runtime failure";
        case ReplayBrokerStateV2::IoFailure: return "I/O failure";
    }
    return "unknown";
}

ReplayBrokerV2::ReplayBrokerV2() = default;

ReplayBrokerV2::~ReplayBrokerV2() {
    cancel();
}

bool ReplayBrokerV2::terminalState(ReplayBrokerStateV2 state) const {
    return state == ReplayBrokerStateV2::Complete
        || state == ReplayBrokerStateV2::Cancelled
        || state == ReplayBrokerStateV2::TimedOut
        || state == ReplayBrokerStateV2::ProcessExitedWithoutResult
        || state == ReplayBrokerStateV2::MissingResult
        || state == ReplayBrokerStateV2::ProtocolFailure
        || state == ReplayBrokerStateV2::RuntimeFailure
        || state == ReplayBrokerStateV2::IoFailure;
}

void ReplayBrokerV2::setState(
    ReplayBrokerStateV2 state, const std::string& detail) {
    std::lock_guard<std::mutex> guard(mutex_);
    status_.state = state;
    status_.detail = detail;
}

ReplayBrokerRunResultV2 ReplayBrokerV2::finish(
    ReplayBrokerStateV2 state, const std::string& detail,
    bool success) {
    setState(state, detail);
    ReplayBrokerRunResultV2 result{};
    result.state = state;
    result.success = success;
    result.detail = detail;
    return result;
}

ReplayPreflightResultV2 ReplayBrokerV2::prepare(
    const ReplayPreflightInputV2& input,
    const ReplayRuntimeIdentityV2& runtime) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (status_.state != ReplayBrokerStateV2::Idle
            && !terminalState(status_.state)) {
            ReplayPreflightResultV2 busy{};
            busy.status = ReplayPreflightStatusV2::StructuralInvalid;
            busy.detail =
                "the broker must finish or reset before another prepare";
            return busy;
        }
    }
    std::unique_ptr<PreparedReplayV2> candidate;
    ReplayPreflightResultV2 result =
        prepareReplayV2(input, runtime, candidate);
    if (!result.ok()) return result;

    if (listener_) listener_->cancel();
    listener_.reset();
    prepared_ = static_cast<std::unique_ptr<PreparedReplayV2>&&>(candidate);
    launch_ = ReplayBrokerLaunchV2{};
    pipeName_.clear();
    commands_.clear();
    nextCommandId_ = 1u;
    nextGameProvider_ = {};
    injectionStarted_ = false;
    localCancel_.store(false);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_ = ReplayBrokerStatusV2{};
        status_.state = ReplayBrokerStateV2::Prepared;
        status_.detail = result.detail;
    }
    return result;
}

bool ReplayBrokerV2::listenBeforeInjection(
    const ReplayBrokerLaunchV2& launch,
    std::unique_ptr<IReplayBrokerListenerV2> listener) {
    if (!prepared_ || !listener || !validLaunch(launch))
        return false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (status_.state != ReplayBrokerStateV2::Prepared)
            return false;
    }
    wchar_t name[256] = {};
    qipc::ipcReplayBrokerPipeName(
        name, sizeof(name) / sizeof(name[0]),
        launch.gamePid, launch.launchGeneration);
    std::string detail;
    if (!listener->create(name, detail)
        || !listener->isListening()) {
        setState(
            ReplayBrokerStateV2::IoFailure,
            detail.empty()
                ? "the replay pipe did not enter listening state"
                : detail);
        return false;
    }
    launch_ = launch;
    pipeName_ = name;
    listener_ = static_cast<
        std::unique_ptr<IReplayBrokerListenerV2>&&>(listener);
    injectionStarted_ = false;
    localCancel_.store(false);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.state =
            ReplayBrokerStateV2::ListeningBeforeInjection;
        status_.launchGeneration = launch.launchGeneration;
        status_.listenerReady = true;
        status_.detail =
            "named pipe is listening; DLL injection may begin";
    }
    return true;
}

bool ReplayBrokerV2::setNextGameProvider(
    ReplayBrokerNextGameProviderV2 provider) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (status_.state != ReplayBrokerStateV2::Prepared) return false;
    nextGameProvider_ = std::move(provider);
    return true;
}

bool ReplayBrokerV2::readyForInjection() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return status_.state
            == ReplayBrokerStateV2::ListeningBeforeInjection
        && status_.listenerReady && listener_
        && listener_->isListening();
}

bool ReplayBrokerV2::markInjectionStarted() {
    if (!readyForInjection()) return false;
    injectionStarted_ = true;
    setState(
        ReplayBrokerStateV2::InjectionStarted,
        "DLL injection/config transfer completed");
    return true;
}

uint64_t ReplayBrokerV2::enqueueCommand(
    qreplay::ipc::v3::PlaybackCommandKind kind,
    uint64_t argument) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!prepared_ || terminalState(status_.state)
        || status_.resultReceived
        || !wire3::isValidPlaybackCommandArgument(kind, argument)
        || kind == wire3::PlaybackCommandKind::Exit
        || nextCommandId_ == 0u
        || nextCommandId_
            == (std::numeric_limits<uint64_t>::max)()) {
        return 0u;
    }
    PendingCommand pending{};
    pending.command.commandId = nextCommandId_++;
    pending.command.kind = kind;
    pending.command.argument = argument;
    commands_.push_back(pending);
    return pending.command.commandId;
}

ReplayBrokerStatusV2 ReplayBrokerV2::status() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return status_;
}

void ReplayBrokerV2::cancel() {
    localCancel_.store(true);
    if (listener_) listener_->cancel();
    std::lock_guard<std::mutex> guard(mutex_);
    if (!terminalState(status_.state)
        && status_.state != ReplayBrokerStateV2::Idle
        && status_.state != ReplayBrokerStateV2::Prepared) {
        status_.state = ReplayBrokerStateV2::Cancelled;
        status_.detail = "replay broker cancelled";
    }
}

bool ReplayBrokerV2::reset() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (status_.state != ReplayBrokerStateV2::Idle
            && status_.state != ReplayBrokerStateV2::Prepared
            && !terminalState(status_.state)) {
            return false;
        }
    }
    if (listener_) listener_->cancel();
    listener_.reset();
    prepared_.reset();
    launch_ = ReplayBrokerLaunchV2{};
    pipeName_.clear();
    commands_.clear();
    nextCommandId_ = 1u;
    nextGameProvider_ = {};
    injectionStarted_ = false;
    localCancel_.store(false);
    std::lock_guard<std::mutex> guard(mutex_);
    status_ = ReplayBrokerStatusV2{};
    return true;
}

ReplayBrokerRunResultV2 ReplayBrokerV2::serve(
    IReplayProcessMonitorV2& process,
    ReplayBrokerCancellationV2* cancellation) {
    if (!prepared_ || !listener_ || !injectionStarted_) {
        return finish(
            ReplayBrokerStateV2::ProtocolFailure,
            "serve requires a prepared replay and a listener created before "
            "injection");
    }
    bool wrongState = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        wrongState =
            status_.state != ReplayBrokerStateV2::InjectionStarted;
        if (!wrongState) {
            status_.state = ReplayBrokerStateV2::AwaitingHello;
            status_.detail = "waiting for the bound replay runtime";
        }
    }
    if (wrongState) {
        return finish(
            ReplayBrokerStateV2::ProtocolFailure,
            "the replay broker is not at the injection-started boundary");
    }

    std::unique_ptr<IReplayBrokerConnectionV2> connection;
    using Clock = std::chrono::steady_clock;
    const auto connectDeadline =
        Clock::now()
        + std::chrono::milliseconds(launch_.connectTimeoutMs);
    for (;;) {
        if (localCancel_.load()
            || (cancellation != nullptr
                && cancellation->requested())) {
            return finish(
                ReplayBrokerStateV2::Cancelled,
                "cancelled while waiting for replay connection");
        }
        uint32_t earlyExit = 0u;
        if (process.hasExited(earlyExit)) {
            ReplayBrokerRunResultV2 result = finish(
                ReplayBrokerStateV2::ProcessExitedWithoutResult,
                "game exited before connecting to the replay pipe");
            result.processExited = true;
            result.processExitCode = earlyExit;
            return result;
        }
        const auto now = Clock::now();
        if (now >= connectDeadline) {
            return finish(
                ReplayBrokerStateV2::TimedOut,
                "timed out waiting for the replay process to connect");
        }
        const uint64_t left = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                connectDeadline - now).count());
        const ReplayBrokerIoStatusV2 accepted = listener_->accept(
            connection,
            static_cast<uint32_t>(
                std::min<uint64_t>(kPollSliceMsV2, left)));
        if (accepted == ReplayBrokerIoStatusV2::Ok && connection)
            break;
        if (accepted == ReplayBrokerIoStatusV2::Timeout)
            continue;
        if (accepted == ReplayBrokerIoStatusV2::Cancelled) {
            return finish(
                ReplayBrokerStateV2::Cancelled,
                "replay listener was cancelled");
        }
        return finish(
            ReplayBrokerStateV2::IoFailure,
            "replay listener could not accept the game process");
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.connected = true;
    }

    FrameReaderV2 reader(*connection);
    uint32_t expectedInboundSequence = 1u;
    uint32_t outboundSequence = 0u;
    GameIdentityV2 game = prepared_->start().manifest.identity;
    uint64_t switchId = 0u;

    auto bindingValid = [&](const wire3::Frame& frame) {
        return frame.channel == wire3::Channel::Playback
            && frame.launchGeneration == launch_.launchGeneration
            && frame.launchNonce == launch_.launchNonce
            && sameGame(frame.game, game)
            && frame.sequence == expectedInboundSequence
            && frame.param0 == 0u && frame.param1 == 0u;
    };

    auto bindingFailureDetail = [&](const wire3::Frame& frame,
                                    const char* phase) {
        char message[512] = {};
        std::snprintf(
            message, sizeof(message),
            "%s replay frame binding failed: type=%u sequence=%u "
            "expected=%u channel=%u generation=%u/%u nonce=%016llX/%016llX "
            "game=%08X:%016llX%016llX expectedGame=%08X:%016llX%016llX "
            "params=%u/%u",
            phase,
            static_cast<unsigned>(frame.type),
            static_cast<unsigned>(frame.sequence),
            static_cast<unsigned>(expectedInboundSequence),
            static_cast<unsigned>(frame.channel),
            static_cast<unsigned>(frame.launchGeneration),
            static_cast<unsigned>(launch_.launchGeneration),
            static_cast<unsigned long long>(frame.launchNonce),
            static_cast<unsigned long long>(launch_.launchNonce),
            static_cast<unsigned>(frame.game.gameGeneration),
            static_cast<unsigned long long>(frame.game.gameUuidHigh),
            static_cast<unsigned long long>(frame.game.gameUuidLow),
            static_cast<unsigned>(game.gameGeneration),
            static_cast<unsigned long long>(game.gameUuidHigh),
            static_cast<unsigned long long>(game.gameUuidLow),
            static_cast<unsigned>(frame.param0),
            static_cast<unsigned>(frame.param1));
        return std::string(message);
    };

    bool transportClosed = false;
    auto transportFailure = [&]() {
        return transportClosed ? ReplayBrokerStateV2::MissingResult
                               : ReplayBrokerStateV2::IoFailure;
    };
    auto sendPayload = [&](
        wire3::MessageType type, uint32_t param0, uint32_t param1,
        std::vector<uint8_t>& payload, std::string& detail) {
        wire3::Frame frame{};
        frame.channel = wire3::Channel::Playback;
        frame.type = type;
        frame.launchGeneration = launch_.launchGeneration;
        frame.game = game;
        frame.launchNonce = launch_.launchNonce;
        frame.sequence = ++outboundSequence;
        frame.param0 = param0;
        frame.param1 = param1;
        frame.payload = static_cast<std::vector<uint8_t>&&>(payload);
        std::vector<uint8_t> bytes;
        if (!wire3::encodeFrame(frame, bytes)) {
            detail = "could not encode a canonical wire-v3 broker frame";
            return false;
        }
        const ReplayBrokerIoStatusV2 io = connection->writeAll(
            bytes.data(), bytes.size(), launch_.ioProgressTimeoutMs);
        if (io != ReplayBrokerIoStatusV2::Ok) {
            transportClosed = io == ReplayBrokerIoStatusV2::Closed;
            detail = io == ReplayBrokerIoStatusV2::Timeout
                ? "timed out writing the replay prebuffer"
                : transportClosed ? "replay IPC channel closed while writing"
                : "replay IPC write failed";
            return false;
        }
        return true;
    };

    std::string detail;
    wire3::Frame helloFrame{};
    ReadOutcome read = reader.read(
        launch_.helloTimeoutMs, process, cancellation, localCancel_,
        helloFrame, detail);
    if (read != ReadOutcome::Ok) {
        connection->close();
        const ReplayBrokerStateV2 state =
            read == ReadOutcome::Timeout ? ReplayBrokerStateV2::TimedOut
            : read == ReadOutcome::Cancelled
                ? ReplayBrokerStateV2::Cancelled
            : read == ReadOutcome::ProcessExited
                ? ReplayBrokerStateV2::ProcessExitedWithoutResult
            : read == ReadOutcome::ProtocolError
                ? ReplayBrokerStateV2::ProtocolFailure
            : read == ReadOutcome::Closed
                ? ReplayBrokerStateV2::MissingResult
                : transportFailure();
        return finish(state, detail);
    }
    if (!bindingValid(helloFrame)
        || helloFrame.type != wire3::MessageType::PlaybackHello) {
        connection->close();
        return finish(
            ReplayBrokerStateV2::ProtocolFailure,
            "PlaybackHello has the wrong nonce, generation, UUID, sequence, "
            "or type");
    }
    ++expectedInboundSequence;
    wire3::PlaybackHelloPayload hello{};
    if (!wire3::decodePlaybackHello(
            helloFrame.payload.data(), helloFrame.payload.size(), hello)
        || hello.gamePid != launch_.gamePid
        || hello.headless != launch_.headless
        || hello.runtimeCompatibilityId
            != prepared_->runtime().contract.runtimeSimulationId
        || hello.stateHashAlgorithmId
            != prepared_->runtime().contract.stateHashAlgorithmId
        || hello.bootstrapSchemaId
            != prepared_->runtime().contract.bootstrapSchemaId) {
        connection->close();
        return finish(
            ReplayBrokerStateV2::ProtocolFailure,
            "PlaybackHello does not match the prepared runtime contract");
    }

    for (;;) {
    setState(
        ReplayBrokerStateV2::SendingPrebuffer,
        "sending the fully validated replay prebuffer");
    auto acceptPrebufferStatuses = [&]() {
        for (;;) {
            wire3::Frame inbound{};
            bool hasFrame = false;
            const ReadOutcome outcome =
                reader.poll(inbound, hasFrame, detail);
            if (outcome != ReadOutcome::Ok) {
                transportClosed = outcome == ReadOutcome::Closed;
                return false;
            }
            if (!hasFrame) return true;
            if (!bindingValid(inbound)
                || inbound.type
                    != wire3::MessageType::PlaybackStatus) {
                detail =
                    "runtime sent a non-status message before the finite "
                    "endpoint";
                return false;
            }
            ++expectedInboundSequence;
            wire3::PlaybackStatusPayload runtimeStatus{};
            if (!wire3::decodePlaybackStatus(
                    inbound.payload.data(), inbound.payload.size(),
                    runtimeStatus)
                || runtimeStatus.headless != launch_.headless
                || runtimeStatus.nextRecord != 0u
                || (runtimeStatus.endpointNextRecord != 0u
                    && runtimeStatus.endpointNextRecord
                        != prepared_->endpoint().nextRecord)
                || (runtimeStatus.phase
                        != wire3::PlaybackRuntimePhase::
                            WaitingForStart
                    && runtimeStatus.phase
                        != wire3::PlaybackRuntimePhase::
                            Bootstrapping
                    && runtimeStatus.phase
                        != wire3::PlaybackRuntimePhase::Buffering)) {
                detail = "invalid prebuffer PlaybackStatus";
                return false;
            }
            std::lock_guard<std::mutex> guard(mutex_);
            ++status_.receivedStatuses;
        }
    };

    std::vector<uint8_t> payload;
    if (!wire3::encodePlaybackSessionInfo(
            prepared_->sessionInfo(), payload)
        || !sendPayload(
            wire3::MessageType::PlaybackSessionInfo,
            0u, 0u, payload, detail)
        || !acceptPrebufferStatuses()) {
        connection->close();
        return finish(
            detail.find("before the finite") != std::string::npos
                || detail.find("prebuffer PlaybackStatus")
                    != std::string::npos
                ? ReplayBrokerStateV2::ProtocolFailure
                : transportFailure(),
            detail);
    }
    payload.clear();
    if (!wire3::encodePlaybackGameStart(prepared_->start(), payload)
        || !sendPayload(
            wire3::MessageType::PlaybackGameStart,
            0u, 0u, payload, detail)
        || !acceptPrebufferStatuses()) {
        connection->close();
        return finish(
            detail.find("before the finite") != std::string::npos
                || detail.find("prebuffer PlaybackStatus")
                    != std::string::npos
                ? ReplayBrokerStateV2::ProtocolFailure
                : transportFailure(),
            detail);
    }

    for (const qreplay::ReplayPaletteV2& palette : prepared_->palettes()) {
        payload.clear();
        if (wire3::encodePlaybackPalette(palette, payload)) {
            std::string paletteDetail;
            (void)sendPayload(
                wire3::MessageType::PlaybackPalette,
                palette.side, 1u, payload, paletteDetail);
        }
    }

    for (uint32_t first = 0u; first < prepared_->frameCount();) {
        const uint32_t count = static_cast<uint32_t>(
            std::min<size_t>(
                kFrameBatchV2,
                prepared_->frameCount() - first));
        std::vector<ConfirmedGameFrameV2> frames;
        frames.reserve(count);
        for (uint32_t i = 0u; i < count; ++i)
            frames.push_back(prepared_->tick(first + i).frame);
        payload.clear();
        if (!wire3::encodePlaybackFrames(
                frames.data(), frames.size(), payload)
            || !sendPayload(
                wire3::MessageType::PlaybackFrames,
                first, count, payload, detail)
            || !acceptPrebufferStatuses()) {
            connection->close();
            return finish(
                detail.find("before the finite")
                        != std::string::npos
                    || detail.find("prebuffer PlaybackStatus")
                        != std::string::npos
                    ? ReplayBrokerStateV2::ProtocolFailure
                    : transportFailure(),
                detail);
        }
        first += count;
        std::lock_guard<std::mutex> guard(mutex_);
        status_.sentFrames = first;
    }

    std::vector<ConfirmedGameEventV2> eventBatch;
    eventBatch.reserve(kEventBatchV2);
    uint32_t sentEvents = 0u;
    auto flushEvents = [&]() {
        if (eventBatch.empty()) return true;
        payload.clear();
        const uint32_t firstSequence =
            eventBatch.front().eventSequence;
        const uint32_t count =
            static_cast<uint32_t>(eventBatch.size());
        if (!wire3::encodePlaybackEvents(
                eventBatch.data(), eventBatch.size(), payload)
            || !sendPayload(
                wire3::MessageType::PlaybackEvents,
                firstSequence, count, payload, detail)
            || !acceptPrebufferStatuses()) {
            return false;
        }
        sentEvents += count;
        eventBatch.clear();
        std::lock_guard<std::mutex> guard(mutex_);
        status_.sentEvents = sentEvents;
        return true;
    };
    bool eventSendOk = true;
    for (uint32_t frame = 0u;
         frame < prepared_->frameCount() && eventSendOk; ++frame) {
        const CanonicalTickBundleV2& tick = prepared_->tick(frame);
        for (size_t i = 0u; i < tick.preEvents.count; ++i) {
            eventBatch.push_back(tick.preEvents.events[i]);
            if (eventBatch.size() == kEventBatchV2)
                eventSendOk = flushEvents();
            if (!eventSendOk) break;
        }
        for (size_t i = 0u;
             i < tick.postEvents.count && eventSendOk; ++i) {
            eventBatch.push_back(tick.postEvents.events[i]);
            if (eventBatch.size() == kEventBatchV2)
                eventSendOk = flushEvents();
        }
    }
    if (eventSendOk) eventSendOk = flushEvents();
    if (!eventSendOk || sentEvents != prepared_->eventCount()) {
        connection->close();
        return finish(
            detail.find("before the finite") != std::string::npos
                || detail.find("prebuffer PlaybackStatus")
                    != std::string::npos
                ? ReplayBrokerStateV2::ProtocolFailure
                : transportFailure(),
            detail.empty() ? "could not send canonical replay events"
                           : detail);
    }

    if (reader.hasBufferedBytes()) {
        connection->close();
        return finish(
            ReplayBrokerStateV2::ProtocolFailure,
            "runtime left a partial message before the finite endpoint");
    }

    payload.clear();
    if (!wire3::encodePlaybackEndpoint(
            prepared_->endpoint(), payload)
        || !sendPayload(
            wire3::MessageType::PlaybackEndpoint,
            prepared_->endpoint().nextRecord,
            prepared_->eventCount(), payload, detail)) {
        connection->close();
        return finish(transportFailure(), detail);
    }
    setState(
        ReplayBrokerStateV2::AwaitingResult,
        "finite replay endpoint sent; awaiting bound PlaybackResult");

    auto sendCommand = [&](PendingCommand& pending) {
        payload.clear();
        if (!wire3::encodePlaybackCommand(
                pending.command, payload)) {
            detail = "could not encode replay command";
            return false;
        }
        if (!sendPayload(
                wire3::MessageType::PlaybackCommand,
                0u, 0u, payload, detail)) {
            return false;
        }
        pending.sent = true;
        return true;
    };

    auto flushCommands = [&]() {
        std::lock_guard<std::mutex> guard(mutex_);
        for (PendingCommand& command : commands_) {
            if (!command.sent && !sendCommand(command))
                return false;
        }
        return true;
    };

    wire3::PlaybackResultPayload playbackResult{};
    bool hasResult = false;
    auto resultDeadline =
        Clock::now()
        + std::chrono::milliseconds(launch_.resultTimeoutMs);
    while (!hasResult) {
        if (!flushCommands()) {
            connection->close();
            return finish(transportFailure(), detail);
        }
        const auto now = Clock::now();
        if (now >= resultDeadline) {
            connection->close();
            return finish(
                ReplayBrokerStateV2::TimedOut,
                "the replay runtime stopped reporting before a PlaybackResult "
                "arrived");
        }
        const uint64_t left = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                resultDeadline - now).count());
        wire3::Frame inbound{};
        read = reader.read(
            static_cast<uint32_t>(
                std::min<uint64_t>(kPollSliceMsV2, left)),
            process, cancellation, localCancel_, inbound, detail);
        if (read == ReadOutcome::Timeout) continue;
        if (read != ReadOutcome::Ok) {
            connection->close();
            const ReplayBrokerStateV2 state =
                read == ReadOutcome::Cancelled
                    ? ReplayBrokerStateV2::Cancelled
                : read == ReadOutcome::ProcessExited
                    ? ReplayBrokerStateV2::ProcessExitedWithoutResult
                : read == ReadOutcome::Closed
                    ? ReplayBrokerStateV2::MissingResult
                : read == ReadOutcome::ProtocolError
                    ? ReplayBrokerStateV2::ProtocolFailure
                    : transportFailure();
            return finish(state, detail);
        }
        if (!bindingValid(inbound)) {
            connection->close();
            return finish(
                ReplayBrokerStateV2::ProtocolFailure,
                bindingFailureDetail(inbound, "runtime"));
        }
        ++expectedInboundSequence;

        if (inbound.type == wire3::MessageType::PlaybackStatus) {
            wire3::PlaybackStatusPayload status{};
            if (!wire3::decodePlaybackStatus(
                    inbound.payload.data(), inbound.payload.size(), status)
                || status.headless != launch_.headless
                || (status.endpointNextRecord != 0u
                    && status.endpointNextRecord
                        != prepared_->endpoint().nextRecord)) {
                connection->close();
                return finish(
                    ReplayBrokerStateV2::ProtocolFailure,
                    "invalid or cross-game PlaybackStatus");
            }
            resultDeadline = Clock::now()
                + std::chrono::milliseconds(launch_.resultTimeoutMs);
            std::lock_guard<std::mutex> guard(mutex_);
            ++status_.receivedStatuses;
            continue;
        }
        if (inbound.type
            == wire3::MessageType::PlaybackCommandAck) {
            wire3::PlaybackCommandAckPayload ack{};
            if (!wire3::decodePlaybackCommandAck(
                    inbound.payload.data(), inbound.payload.size(), ack)) {
                connection->close();
                return finish(
                    ReplayBrokerStateV2::ProtocolFailure,
                    "malformed PlaybackCommandAck");
            }
            bool matched = false;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                for (auto it = commands_.begin();
                     it != commands_.end(); ++it) {
                    if (it->sent
                        && it->command.commandId == ack.commandId
                        && it->command.kind == ack.kind) {
                        commands_.erase(it);
                        ++status_.acknowledgedCommands;
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) {
                connection->close();
                return finish(
                    ReplayBrokerStateV2::ProtocolFailure,
                    "PlaybackCommandAck does not match an outstanding command");
            }
            resultDeadline = Clock::now()
                + std::chrono::milliseconds(launch_.resultTimeoutMs);
            continue;
        }
        if (inbound.type == wire3::MessageType::PlaybackResult) {
            bool commandsPending = false;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                commandsPending = !commands_.empty();
            }
            if (commandsPending) {
                connection->close();
                return finish(
                    ReplayBrokerStateV2::ProtocolFailure,
                    "PlaybackResult arrived before command ACK drain");
            }
            if (!wire3::decodePlaybackResult(
                    inbound.payload.data(), inbound.payload.size(),
                    playbackResult)
                || !validateResultBinding(
                    playbackResult, *prepared_, launch_.headless,
                    detail)) {
                connection->close();
                return finish(
                    ReplayBrokerStateV2::ProtocolFailure,
                    detail.empty()
                        ? "malformed PlaybackResult"
                        : detail);
            }
            hasResult = true;
            QLOG("[replay.play.v2] result accepted reason=%u consumed=%u/%u "
                 "contentRootDrift=%u",
                 static_cast<unsigned>(playbackResult.reason),
                 static_cast<unsigned>(playbackResult.nextRecordConsumed),
                 static_cast<unsigned>(prepared_->endpoint().nextRecord),
                 (prepared_->start().manifest.contract.contentManifestRoot
                  != prepared_->runtime().contract.contentManifestRoot)
                     ? 1u : 0u);
            std::lock_guard<std::mutex> guard(mutex_);
            status_.resultReceived = true;
            break;
        }
        connection->close();
        return finish(
            ReplayBrokerStateV2::ProtocolFailure,
            "unexpected runtime message before PlaybackResult");
    }

    ReplayBrokerNextGameV2 next{};
    if (nextGameProvider_
        && qreplay::ipc::v3::isPlayedThroughPlaybackResultReason(playbackResult.reason)
        && playbackResult.hasRecordedTerminal
        && playbackResult.recordedTerminal
            == static_cast<uint16_t>(TerminalReason::Completed)) {
        next = nextGameProvider_(playbackResult);
    }
    if (next.replay && next.error.empty()) {
        setState(ReplayBrokerStateV2::Draining,
                 "PlaybackResult accepted; switching to the next replay");
        if (switchId == (std::numeric_limits<uint64_t>::max)()) {
            connection->close();
            return finish(ReplayBrokerStateV2::ProtocolFailure,
                          "replay switch ID space exhausted");
        }
        wire3::PlaybackNextGamePayload advance{};
        advance.switchId = ++switchId;
        advance.game = next.replay->start().manifest.identity;
        advance.frameCount = next.replay->frameCount();
        advance.eventCount = next.replay->eventCount();
        advance.validatedSourceByteCount = next.replay->source().validatedSourceByteCount;
        advance.validatedSourceRangeDigest = next.replay->source().validatedSourceRangeDigest;
        advance.endpointKind = next.replay->endpoint().kind;
        advance.supersededStateSchema = next.replay->supersededStateSchema();
        advance.presentationFlags = next.presentationFlags | qipc::kReplayAutoStart;
        advance.runtimeContract = next.replay->runtime().contract;
        advance.runtimeContract.contentManifestRoot =
            next.replay->start().manifest.contract.contentManifestRoot;
        std::vector<uint8_t> expectedAck;
        if (!wire3::encodePlaybackNextGame(advance, expectedAck)) {
            connection->close();
            return finish(ReplayBrokerStateV2::ProtocolFailure,
                          "invalid next replay binding");
        }
        payload = expectedAck;
        if (!sendPayload(wire3::MessageType::PlaybackNextGame,
                         0u, 0u, payload, detail)) {
            connection->close();
            return finish(transportFailure(), detail);
        }
        const auto switchDeadline = Clock::now()
            + std::chrono::milliseconds(launch_.drainTimeoutMs);
        for (;;) {
            const auto now = Clock::now();
            if (now >= switchDeadline) {
                connection->close();
                return finish(ReplayBrokerStateV2::TimedOut,
                              "next replay handoff was not acknowledged");
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                switchDeadline - now).count();
            wire3::Frame inbound{};
            read = reader.read(static_cast<uint32_t>(
                std::min<int64_t>(kPollSliceMsV2, left)),
                process, cancellation, localCancel_, inbound, detail);
            if (read == ReadOutcome::Timeout) continue;
            if (read != ReadOutcome::Ok) {
                connection->close();
                return finish(read == ReadOutcome::Cancelled
                                  ? ReplayBrokerStateV2::Cancelled
                                  : read == ReadOutcome::ProtocolError
                                      ? ReplayBrokerStateV2::ProtocolFailure
                                      : ReplayBrokerStateV2::MissingResult,
                              detail);
            }
            if (!bindingValid(inbound)) {
                connection->close();
                return finish(ReplayBrokerStateV2::ProtocolFailure,
                              bindingFailureDetail(inbound, "next game"));
            }
            ++expectedInboundSequence;
            if (inbound.type == wire3::MessageType::PlaybackStatus) {
                wire3::PlaybackStatusPayload status{};
                if (!wire3::decodePlaybackStatus(inbound.payload.data(),
                        inbound.payload.size(), status)
                    || status.headless != launch_.headless
                    || status.nextRecord > prepared_->endpoint().nextRecord
                    || (status.endpointNextRecord != 0u
                        && status.endpointNextRecord != prepared_->endpoint().nextRecord)) {
                    connection->close();
                    return finish(ReplayBrokerStateV2::ProtocolFailure,
                                  "invalid old-game status during next replay handoff");
                }
                std::lock_guard<std::mutex> guard(mutex_);
                ++status_.receivedStatuses;
                continue;
            }
            if (inbound.type != wire3::MessageType::PlaybackNextGameAck
                || inbound.payload != expectedAck) {
                connection->close();
                return finish(ReplayBrokerStateV2::ProtocolFailure,
                              "next replay acknowledgment does not match its switch binding");
            }
            break;
        }
        // The ACK is the FIFO barrier: earlier packets belong to the old game, later ones
        // must use the new binding.
        {
            std::lock_guard<std::mutex> guard(mutex_);
            prepared_ = std::move(next.replay);
            game = prepared_->start().manifest.identity;
            status_.resultReceived = false;
            status_.byeReceived = false;
            status_.sentFrames = 0u;
            status_.sentEvents = 0u;
        }
        continue;
    }

    setState(
        ReplayBrokerStateV2::Draining,
        "PlaybackResult accepted; acknowledging result and draining");
    PendingCommand drain{};
    bool commandIdsExhausted = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        commandIdsExhausted = nextCommandId_ == 0u
            || nextCommandId_
                == (std::numeric_limits<uint64_t>::max)();
        if (!commandIdsExhausted)
            drain.command.commandId = nextCommandId_++;
    }
    if (commandIdsExhausted) {
        connection->close();
        ReplayBrokerRunResultV2 result = finish(
            ReplayBrokerStateV2::ProtocolFailure,
            "replay command ID space exhausted");
        result.hasPlaybackResult = true;
        result.playbackResult = playbackResult;
        return result;
    }
    drain.command.kind = wire3::PlaybackCommandKind::Exit;
    drain.command.argument = 0u;
    if (!sendCommand(drain)) {
        connection->close();
        return finish(transportFailure(), detail);
    }

    bool drainAcked = false;
    bool byeReceived = false;
    const auto drainDeadline =
        Clock::now()
        + std::chrono::milliseconds(launch_.drainTimeoutMs);
    while (!byeReceived) {
        const auto now = Clock::now();
        if (now >= drainDeadline) {
            connection->close();
            ReplayBrokerRunResultV2 result = finish(
                ReplayBrokerStateV2::TimedOut,
                drainAcked
                    ? "PlaybackBye was not received before drain timeout"
                    : "PlaybackResult acknowledgment was not received");
            result.hasPlaybackResult = true;
            result.playbackResult = playbackResult;
            return result;
        }
        const uint64_t left = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                drainDeadline - now).count());
        wire3::Frame inbound{};
        read = reader.read(
            static_cast<uint32_t>(
                std::min<uint64_t>(kPollSliceMsV2, left)),
            process, cancellation, localCancel_, inbound, detail);
        if (read == ReadOutcome::Timeout) continue;
        if (read != ReadOutcome::Ok) {
            connection->close();
            ReplayBrokerRunResultV2 result = finish(
                read == ReadOutcome::Cancelled
                    ? ReplayBrokerStateV2::Cancelled
                : read == ReadOutcome::ProtocolError
                    ? ReplayBrokerStateV2::ProtocolFailure
                    : ReplayBrokerStateV2::MissingResult,
                detail);
            result.hasPlaybackResult = true;
            result.playbackResult = playbackResult;
            return result;
        }
        if (!bindingValid(inbound)) {
            connection->close();
            ReplayBrokerRunResultV2 result = finish(
                ReplayBrokerStateV2::ProtocolFailure,
                bindingFailureDetail(inbound, "drain"));
            result.hasPlaybackResult = true;
            result.playbackResult = playbackResult;
            return result;
        }
        ++expectedInboundSequence;

        if (inbound.type
            == wire3::MessageType::PlaybackCommandAck) {
            wire3::PlaybackCommandAckPayload ack{};
            if (drainAcked
                || !wire3::decodePlaybackCommandAck(
                    inbound.payload.data(), inbound.payload.size(), ack)
                || ack.commandId != drain.command.commandId
                || ack.kind != wire3::PlaybackCommandKind::Exit
                || (ack.result
                        != wire3::PlaybackCommandAckResult::Applied
                    && ack.result
                        != wire3::PlaybackCommandAckResult::Queued)) {
                connection->close();
                ReplayBrokerRunResultV2 result = finish(
                    ReplayBrokerStateV2::ProtocolFailure,
                    "invalid PlaybackResult/Exit acknowledgment");
                result.hasPlaybackResult = true;
                result.playbackResult = playbackResult;
                return result;
            }
            drainAcked = true;
            std::lock_guard<std::mutex> guard(mutex_);
            ++status_.acknowledgedCommands;
            continue;
        }
        if (inbound.type == wire3::MessageType::PlaybackStatus) {
            wire3::PlaybackStatusPayload runtimeStatus{};
            if (!wire3::decodePlaybackStatus(
                    inbound.payload.data(), inbound.payload.size(),
                    runtimeStatus)
                || runtimeStatus.headless != launch_.headless) {
                connection->close();
                ReplayBrokerRunResultV2 result = finish(
                    ReplayBrokerStateV2::ProtocolFailure,
                    "invalid drain PlaybackStatus");
                result.hasPlaybackResult = true;
                result.playbackResult = playbackResult;
                return result;
            }
            std::lock_guard<std::mutex> guard(mutex_);
            ++status_.receivedStatuses;
            continue;
        }
        if (inbound.type == wire3::MessageType::PlaybackBye) {
            wire3::PlaybackByePayload bye{};
            if (!drainAcked
                || !wire3::decodePlaybackBye(
                    inbound.payload.data(), inbound.payload.size(), bye)
                || !byeMatchesResult(
                    bye, playbackResult, drain.command.commandId)) {
                connection->close();
                ReplayBrokerRunResultV2 result = finish(
                    ReplayBrokerStateV2::ProtocolFailure,
                    "PlaybackBye does not close the accepted result");
                result.hasPlaybackResult = true;
                result.playbackResult = playbackResult;
                return result;
            }
            byeReceived = true;
            std::lock_guard<std::mutex> guard(mutex_);
            status_.byeReceived = true;
            break;
        }
        connection->close();
        ReplayBrokerRunResultV2 result = finish(
            ReplayBrokerStateV2::ProtocolFailure,
            "unexpected message while draining PlaybackResult");
        result.hasPlaybackResult = true;
        result.playbackResult = playbackResult;
        return result;
    }
    connection->close();

    uint32_t exitCode = 0u;
    const ReplayProcessWaitStatusV2 processWait =
        process.waitForExit(launch_.processExitTimeoutMs, exitCode);
    if (processWait != ReplayProcessWaitStatusV2::Exited) {
        ReplayBrokerRunResultV2 result = finish(
            processWait == ReplayProcessWaitStatusV2::Timeout
                ? ReplayBrokerStateV2::TimedOut
                : ReplayBrokerStateV2::IoFailure,
            processWait == ReplayProcessWaitStatusV2::Timeout
                ? "the replay process did not exit after Result/Bye"
                : "could not observe replay process exit");
        result.hasPlaybackResult = true;
        result.playbackResult = playbackResult;
        return result;
    }

    const bool verified = isVerifiedReason(playbackResult.reason);
    ReplayBrokerStateV2 finalState = verified
        ? ReplayBrokerStateV2::Complete
        : (playbackResult.reason
                == wire3::PlaybackResultReason::UserExit
            ? ReplayBrokerStateV2::Cancelled
            : ReplayBrokerStateV2::RuntimeFailure);
    ReplayBrokerRunResultV2 result = finish(
        finalState,
        verified
            ? "bound replay result, drain, Bye, and process exit verified"
            : "runtime returned a complete bound non-success result",
        verified);
    result.hasPlaybackResult = true;
    result.playbackResult = playbackResult;
    result.processExited = true;
    result.processExitCode = exitCode;
    if (!next.error.empty()) {
        result.state = ReplayBrokerStateV2::RuntimeFailure;
        result.success = false;
        result.nextGameRefused = true;
        result.detail = next.error;
        setState(result.state, result.detail);
    }
    return result;
    }
}

#if defined(_WIN32)

namespace {

ReplayBrokerIoStatusV2 overlappedResult(
    HANDLE handle, OVERLAPPED& operation, DWORD timeoutMs,
    DWORD& transferred) {
    const DWORD wait = ::WaitForSingleObject(operation.hEvent, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        // The cancel races the completion: honour any bytes the kernel transferred, or the
        // sequence-bound frame stream loses a whole message.
        ::CancelIoEx(handle, &operation);
        ::WaitForSingleObject(operation.hEvent, INFINITE);
        DWORD done = 0u;
        if (::GetOverlappedResult(handle, &operation, &done, FALSE) != 0) {
            QLOG("[replay.io] overlapped completion beat its cancel; "
                 "retained %u byte(s)", static_cast<unsigned>(done));
            transferred = done;
            return ReplayBrokerIoStatusV2::Ok;
        }
        const DWORD error = ::GetLastError();
        if (error == ERROR_BROKEN_PIPE
            || error == ERROR_PIPE_NOT_CONNECTED
            || error == ERROR_NO_DATA) {
            return ReplayBrokerIoStatusV2::Closed;
        }
        if (error == ERROR_OPERATION_ABORTED && done != 0u) {
            QLOG("[replay.io] cancelled overlapped transfer consumed "
                 "%u byte(s)", static_cast<unsigned>(done));
            transferred = done;
            return ReplayBrokerIoStatusV2::Ok;
        }
        return ReplayBrokerIoStatusV2::Timeout;
    }
    if (wait != WAIT_OBJECT_0)
        return ReplayBrokerIoStatusV2::Error;
    if (::GetOverlappedResult(
            handle, &operation, &transferred, FALSE) == 0) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_BROKEN_PIPE
            || error == ERROR_PIPE_NOT_CONNECTED
            || error == ERROR_NO_DATA) {
            return ReplayBrokerIoStatusV2::Closed;
        }
        if (error == ERROR_OPERATION_ABORTED)
            return ReplayBrokerIoStatusV2::Cancelled;
        return ReplayBrokerIoStatusV2::Error;
    }
    return ReplayBrokerIoStatusV2::Ok;
}

class Win32ReplayBrokerConnectionV2 final
    : public IReplayBrokerConnectionV2 {
public:
    explicit Win32ReplayBrokerConnectionV2(HANDLE pipe)
        : pipe_(pipe) {}
    ~Win32ReplayBrokerConnectionV2() override { close(); }

    ReplayBrokerIoStatusV2 readSome(
        std::vector<uint8_t>& out, uint32_t timeoutMs) override {
        if (pipe_ == INVALID_HANDLE_VALUE)
            return ReplayBrokerIoStatusV2::Closed;
        if (timeoutMs == 0u) {
            DWORD available = 0u;
            if (::PeekNamedPipe(
                    pipe_, nullptr, 0u, nullptr, &available, nullptr) == 0) {
                const DWORD error = ::GetLastError();
                return error == ERROR_BROKEN_PIPE
                    || error == ERROR_PIPE_NOT_CONNECTED
                    ? ReplayBrokerIoStatusV2::Closed
                    : ReplayBrokerIoStatusV2::Error;
            }
            if (available == 0u)
                return ReplayBrokerIoStatusV2::Timeout;
        }
        std::vector<uint8_t> bytes(64u * 1024u);
        HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) return ReplayBrokerIoStatusV2::Error;
        OVERLAPPED operation{};
        operation.hEvent = event;
        DWORD got = 0u;
        BOOL ok = ::ReadFile(
            pipe_, bytes.data(), static_cast<DWORD>(bytes.size()),
            &got, &operation);
        ReplayBrokerIoStatusV2 status = ReplayBrokerIoStatusV2::Ok;
        if (ok == 0) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_IO_PENDING) {
                status = overlappedResult(
                    pipe_, operation, timeoutMs, got);
            } else if (error == ERROR_BROKEN_PIPE
                       || error == ERROR_PIPE_NOT_CONNECTED
                       || error == ERROR_NO_DATA) {
                status = ReplayBrokerIoStatusV2::Closed;
            } else {
                status = ReplayBrokerIoStatusV2::Error;
            }
        }
        ::CloseHandle(event);
        if (status != ReplayBrokerIoStatusV2::Ok) return status;
        if (got == 0u) return ReplayBrokerIoStatusV2::Closed;
        bytes.resize(got);
        out = static_cast<std::vector<uint8_t>&&>(bytes);
        return ReplayBrokerIoStatusV2::Ok;
    }

    ReplayBrokerIoStatusV2 writeAll(
        const uint8_t* data, size_t bytes,
        uint32_t timeoutMs) override {
        if (pipe_ == INVALID_HANDLE_VALUE)
            return ReplayBrokerIoStatusV2::Closed;
        size_t offset = 0u;
        while (offset < bytes) {
            const DWORD count = static_cast<DWORD>(
                std::min<size_t>(64u * 1024u, bytes - offset));
            HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (event == nullptr) return ReplayBrokerIoStatusV2::Error;
            OVERLAPPED operation{};
            operation.hEvent = event;
            DWORD wrote = 0u;
            BOOL ok = ::WriteFile(
                pipe_, data + offset, count, &wrote, &operation);
            ReplayBrokerIoStatusV2 status =
                ReplayBrokerIoStatusV2::Ok;
            if (ok == 0) {
                const DWORD error = ::GetLastError();
                if (error == ERROR_IO_PENDING) {
                    status = overlappedResult(
                        pipe_, operation, timeoutMs, wrote);
                } else if (error == ERROR_BROKEN_PIPE
                           || error == ERROR_PIPE_NOT_CONNECTED
                           || error == ERROR_NO_DATA) {
                    status = ReplayBrokerIoStatusV2::Closed;
                } else {
                    status = ReplayBrokerIoStatusV2::Error;
                }
            }
            ::CloseHandle(event);
            if (status != ReplayBrokerIoStatusV2::Ok) return status;
            if (wrote == 0u) return ReplayBrokerIoStatusV2::Error;
            offset += wrote;
        }
        return ReplayBrokerIoStatusV2::Ok;
    }

    void close() override {
        if (pipe_ == INVALID_HANDLE_VALUE) return;
        ::CancelIoEx(pipe_, nullptr);
        ::DisconnectNamedPipe(pipe_);
        ::CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

class Win32ReplayBrokerListenerV2 final
    : public IReplayBrokerListenerV2 {
public:
    ~Win32ReplayBrokerListenerV2() override { cancel(); }

    bool create(
        const std::wstring& pipeName, std::string& detail) override {
        cancel();
        pipe_ = ::CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1u, 64u * 1024u, 64u * 1024u, 0u, nullptr);
        if (pipe_ == INVALID_HANDLE_VALUE) {
            detail = "CreateNamedPipeW failed";
            return false;
        }
        listening_ = true;
        return true;
    }

    bool isListening() const override {
        return listening_ && pipe_ != INVALID_HANDLE_VALUE;
    }

    ReplayBrokerIoStatusV2 accept(
        std::unique_ptr<IReplayBrokerConnectionV2>& out,
        uint32_t timeoutMs) override {
        if (!isListening()) return ReplayBrokerIoStatusV2::Closed;
        HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) return ReplayBrokerIoStatusV2::Error;
        OVERLAPPED operation{};
        operation.hEvent = event;
        BOOL ok = ::ConnectNamedPipe(pipe_, &operation);
        ReplayBrokerIoStatusV2 status = ReplayBrokerIoStatusV2::Ok;
        if (ok == 0) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                ::SetEvent(event);
            } else if (error == ERROR_IO_PENDING) {
                DWORD ignored = 0u;
                status = overlappedResult(
                    pipe_, operation, timeoutMs, ignored);
            } else {
                status = error == ERROR_OPERATION_ABORTED
                    ? ReplayBrokerIoStatusV2::Cancelled
                    : ReplayBrokerIoStatusV2::Error;
            }
        }
        ::CloseHandle(event);
        if (status != ReplayBrokerIoStatusV2::Ok) return status;
        listening_ = false;
        HANDLE connected = pipe_;
        pipe_ = INVALID_HANDLE_VALUE;
        out.reset(new (std::nothrow)
            Win32ReplayBrokerConnectionV2(connected));
        if (!out) {
            ::CloseHandle(connected);
            return ReplayBrokerIoStatusV2::Error;
        }
        return ReplayBrokerIoStatusV2::Ok;
    }

    void cancel() override {
        listening_ = false;
        if (pipe_ == INVALID_HANDLE_VALUE) return;
        ::CancelIoEx(pipe_, nullptr);
        ::DisconnectNamedPipe(pipe_);
        ::CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    bool listening_ = false;
};

class Win32ReplayProcessMonitorV2 final
    : public IReplayProcessMonitorV2 {
public:
    explicit Win32ReplayProcessMonitorV2(uint32_t processId) {
        process_ = ::OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE, processId);
    }
    ~Win32ReplayProcessMonitorV2() override {
        if (process_ != nullptr) ::CloseHandle(process_);
    }
    bool valid() const { return process_ != nullptr; }

    bool hasExited(uint32_t& exitCode) override {
        if (process_ == nullptr) return false;
        const DWORD wait = ::WaitForSingleObject(process_, 0u);
        if (wait != WAIT_OBJECT_0) return false;
        DWORD code = 0u;
        if (::GetExitCodeProcess(process_, &code) == 0) return false;
        exitCode = code;
        return true;
    }

    ReplayProcessWaitStatusV2 waitForExit(
        uint32_t timeoutMs, uint32_t& exitCode) override {
        if (process_ == nullptr) return ReplayProcessWaitStatusV2::Error;
        const DWORD wait =
            ::WaitForSingleObject(process_, timeoutMs);
        if (wait == WAIT_TIMEOUT)
            return ReplayProcessWaitStatusV2::Timeout;
        if (wait != WAIT_OBJECT_0)
            return ReplayProcessWaitStatusV2::Error;
        DWORD code = 0u;
        if (::GetExitCodeProcess(process_, &code) == 0)
            return ReplayProcessWaitStatusV2::Error;
        exitCode = code;
        return ReplayProcessWaitStatusV2::Exited;
    }

private:
    HANDLE process_ = nullptr;
};

}

std::unique_ptr<IReplayBrokerListenerV2>
createWin32ReplayBrokerListenerV2() {
    return std::unique_ptr<IReplayBrokerListenerV2>(
        new (std::nothrow) Win32ReplayBrokerListenerV2());
}

std::unique_ptr<IReplayProcessMonitorV2>
createWin32ReplayProcessMonitorV2(uint32_t processId) {
    std::unique_ptr<Win32ReplayProcessMonitorV2> monitor(
        new (std::nothrow) Win32ReplayProcessMonitorV2(processId));
    if (!monitor || !monitor->valid()) return nullptr;
    return std::unique_ptr<IReplayProcessMonitorV2>(monitor.release());
}

#else

std::unique_ptr<IReplayBrokerListenerV2>
createWin32ReplayBrokerListenerV2() {
    return nullptr;
}

std::unique_ptr<IReplayProcessMonitorV2>
createWin32ReplayProcessMonitorV2(uint32_t) {
    return nullptr;
}

#endif

}
