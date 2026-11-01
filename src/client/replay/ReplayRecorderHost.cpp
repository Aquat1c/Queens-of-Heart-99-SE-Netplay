#include "ReplayRecorderHost.h"
#include "Log.h"

#include "ReplayBattleLogFile.h"
#include "ReplaySetNaming.h"

#include "ipc/IpcConfig.h"
#include "platform/WidePath.h"
#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace qclient::replay {

using namespace qreplay;

const char* recorderStateText(RecorderState state) {
    switch (state) {
        case RecorderState::Idle: return "idle";
        case RecorderState::Listening: return "waiting for the game";
        case RecorderState::Recording: return "recording";
        case RecorderState::Finalized: return "finalized";
        case RecorderState::Disabled: return "disabled";
    }
    return "unknown";
}

const char* recorderDisableReasonText(RecorderDisableReason reason) {
    switch (reason) {
        case RecorderDisableReason::None: return "";
        case RecorderDisableReason::Stopped: return "stopped by the caster";
        case RecorderDisableReason::PipeError: return "recorder pipe error";
        case RecorderDisableReason::ProtocolError: return "recorder protocol error";
        case RecorderDisableReason::HandshakeRejected: return "recorder handshake rejected";
        case RecorderDisableReason::GameDisconnected: return "the game disconnected";
        case RecorderDisableReason::RecorderOverflow: return "the runtime publish queue overflowed";
        case RecorderDisableReason::DigestDrift: return "the ordered timeline digest did not match";
        case RecorderDisableReason::InvalidRecord: return "a record failed validation";
        case RecorderDisableReason::DiskFull: return "disk full";
        case RecorderDisableReason::AccessDenied: return "access denied";
        case RecorderDisableReason::WriteFailed: return "write failed";
        case RecorderDisableReason::FlushFailed: return "flush failed";
        case RecorderDisableReason::RenameFailed: return "final rename failed";
        case RecorderDisableReason::LimitExceeded: return "a container limit was reached";
        case RecorderDisableReason::RootUnusable: return "the replay folder is not writable";
    }
    return "unknown";
}

namespace {

RecorderDisableReason mapWriterError(WriterError error) {
    switch (error) {
        case WriterError::DiskFull: return RecorderDisableReason::DiskFull;
        case WriterError::AccessDenied: return RecorderDisableReason::AccessDenied;
        case WriterError::FlushFailed: return RecorderDisableReason::FlushFailed;
        case WriterError::RenameFailed: return RecorderDisableReason::RenameFailed;
        case WriterError::LimitExceeded: return RecorderDisableReason::LimitExceeded;
        case WriterError::RootUnusable:
        case WriterError::NameExhausted:
        case WriterError::CreateFailed: return RecorderDisableReason::RootUnusable;
        case WriterError::InvalidRecord:
        case WriterError::WrongOrder: return RecorderDisableReason::InvalidRecord;
        default: return RecorderDisableReason::WriteFailed;
    }
}

}

class ReplayRecorderHost::Session {
public:
    Session(const RecorderHostConfig& config, RecorderStatus& status,
            std::unique_ptr<ReplayWriter> writer)
        : config_(config), status_(status),
          writer_(writer ? std::move(writer)
                         : std::unique_ptr<ReplayWriter>(new ReplayWriter())) {}

    ReplayWriter& writer() { return *writer_; }

    bool helloSeen() const { return helloSeen_; }
    bool finished() const { return finished_; }
    RecorderDisableReason reason() const { return reason_; }
    const char* detail() const { return detail_; }

    bool handle(const ReplayIpcFrame& frame) {
        if (frame.channel != ReplayIpcChannel::Recorder)
            return fail(RecorderDisableReason::ProtocolError, "wrong channel");
        if (frame.generation != config_.generation)
            return fail(RecorderDisableReason::ProtocolError, "stale generation");
        if (frame.nonce != config_.nonce)
            return fail(RecorderDisableReason::ProtocolError,
                        "recorder frame carries the wrong launch nonce");
        if (frame.sequence != expectedSequence_ + 1u) {
            return fail(RecorderDisableReason::ProtocolError,
                        "recorder sequence regression or gap");
        }
        ++expectedSequence_;

        const uint8_t* body = frame.payload.data();
        const size_t len = frame.payload.size();

        switch (frame.type) {
            case ReplayIpcType::RecorderHello: {
                ReplayIpcHello hello{};
                if (!wire::decodeHello(body, len, hello)
                    || hello.nonce != config_.nonce
                    || hello.gamePid != config_.gamePid
                    || hello.wireVersion != kReplayIpcWireVersion) {
                    return fail(RecorderDisableReason::HandshakeRejected,
                                "recorder nonce, pid, or wire version mismatch");
                }
                helloSeen_ = true;
                return true;
            }
            case ReplayIpcType::RecorderManifest: {
                if (!helloSeen_)
                    return fail(RecorderDisableReason::ProtocolError,
                                "manifest before handshake");
                if (started_)
                    return fail(RecorderDisableReason::ProtocolError,
                                "duplicate manifest");
                if (len < wire::kRecorderManifestHeaderBytes)
                    return fail(RecorderDisableReason::InvalidRecord,
                                "truncated manifest message");
                ReplayPrefix prefix{};
                SessionManifest manifest{};
                if (!wire::decodeRecorderManifest(body, len, prefix,
                                                  manifest)) {
                    return fail(RecorderDisableReason::InvalidRecord,
                                "manifest rejected");
                }
                WriterConfig writerConfig{};
                writerConfig.rootDirectory = config_.replayRoot;
                writerConfig.nameHint = config_.nameHint;
                writerConfig.recordedUnixSeconds =
                    manifest.display.recordedUnixSeconds;
                writerConfig.flushEveryChunks = config_.flushEveryChunks;
                writerConfig.flushEveryMs = config_.flushEveryMs;
                if (!writer_->begin(writerConfig, prefix, manifest))
                    return failWriter();
                started_ = true;
                status_.partPath = writer_->partPath();
                return true;
            }
            case ReplayIpcType::RecorderSessionStart: {
                if (!started_)
                    return fail(RecorderDisableReason::ProtocolError,
                                "session start before manifest");
                DeterministicStart start{};
                if (decodeDeterministicStart(body, len, start)
                    != ParseStatus::Ok) {
                    return fail(RecorderDisableReason::InvalidRecord,
                                "session start rejected");
                }
                if (!writer_->writeSessionStart(start)) return failWriter();
                digest_ = start.initialDigest;
                digestReady_ = true;
                return true;
            }
            case ReplayIpcType::RecorderMatchStart: {
                MatchStartRecord record{};
                if (decodeMatchStart(body, len, record) != ParseStatus::Ok)
                    return fail(RecorderDisableReason::InvalidRecord,
                                "match start rejected");
                if (!writer_->writeMatchStart(record)) return failWriter();
                ++status_.matches;
                return true;
            }
            case ReplayIpcType::RecorderFrames: {
                if (!digestReady_)
                    return fail(RecorderDisableReason::ProtocolError,
                                "input before the session start");
                if (len < kInputBlockHeaderBytes)
                    return fail(RecorderDisableReason::InvalidRecord,
                                "truncated input block");
                const size_t bodyBytes = len - kInputBlockHeaderBytes;
                if (bodyBytes % kInputRecordBytes != 0u)
                    return fail(RecorderDisableReason::InvalidRecord,
                                "input block is not a whole number of records");
                const uint32_t count =
                    static_cast<uint32_t>(bodyBytes / kInputRecordBytes);
                if (count == 0u || count > kMaxRecordsPerInputBlock)
                    return fail(RecorderDisableReason::LimitExceeded,
                                "input block record count out of range");
                if (frame.param1 != count)
                    return fail(RecorderDisableReason::ProtocolError,
                                "input block count disagrees with its header");
                std::vector<ReplayFrameV1> frames;
                if (decodeInputBlock(body, len, writer_->nextTimelineFrame(),
                                     count, frames) != ParseStatus::Ok) {
                    return fail(RecorderDisableReason::InvalidRecord,
                                "input block rejected");
                }
                for (const ReplayFrameV1& frameRecord : frames) {
                    const uint64_t expected =
                        advanceReplayDigest(digest_, frameRecord);
                    if (frameRecord.rollingDigest != expected) {
                        return fail(RecorderDisableReason::DigestDrift,
                                    "frame rolling digest mismatch");
                    }
                    if (haveProof_ && frameRecord.preStateHash != lastPostHash_)
                        ++status_.proofDiscontinuities;
                    lastPostHash_ = frameRecord.postStateHash;
                    haveProof_ = true;
                    digest_ = expected;
                }
                if (!writer_->writeInputBlock(frames.data(), frames.size()))
                    return failWriter();
                status_.frames += count;
                return true;
            }
            case ReplayIpcType::RecorderEvents: {
                if (!digestReady_)
                    return fail(RecorderDisableReason::ProtocolError,
                                "events before the session start");
                if (len % kEventRecordBytes != 0u)
                    return fail(RecorderDisableReason::InvalidRecord,
                                "event block is not a whole number of records");
                const uint32_t count =
                    static_cast<uint32_t>(len / kEventRecordBytes);
                if (count == 0u || count > kMaxEventsPerBlock)
                    return fail(RecorderDisableReason::LimitExceeded,
                                "event block record count out of range");
                if (frame.param1 != count)
                    return fail(RecorderDisableReason::ProtocolError,
                                "event block count disagrees with its header");
                std::vector<ReplayEvent> events;
                if (decodeEventBlock(body, len, count, events)
                    != ParseStatus::Ok) {
                    return fail(RecorderDisableReason::InvalidRecord,
                                "event block rejected");
                }
                for (const ReplayEvent& event : events) {
                    const uint64_t expected = advanceReplayDigest(digest_, event);
                    if (event.rollingDigest != expected) {
                        return fail(RecorderDisableReason::DigestDrift,
                                    "event rolling digest mismatch");
                    }
                    digest_ = expected;
                }
                if (!writer_->writeEventBlock(events.data(), events.size()))
                    return failWriter();
                status_.events += count;
                return true;
            }
            case ReplayIpcType::RecorderMatchEnd: {
                MatchEndRecord record{};
                if (decodeMatchEnd(body, len, record) != ParseStatus::Ok)
                    return fail(RecorderDisableReason::InvalidRecord,
                                "match end rejected");
                if (digestReady_ && record.rollingDigest != digest_) {
                    return fail(RecorderDisableReason::DigestDrift,
                                "match terminal digest mismatch");
                }
                if (!writer_->writeMatchEnd(record)) return failWriter();
                return true;
            }
            case ReplayIpcType::RecorderTerminal: {
                ReplayIpcTerminal terminal{};
                if (!wire::decodeTerminal(body, len, terminal))
                    return fail(RecorderDisableReason::InvalidRecord,
                                "session end rejected");
                SessionEndRecord record{};
                record.lastTimelineFrame = terminal.lastTimelineFrame;
                record.reason =
                    static_cast<TerminalReason>(terminal.reason);
                record.postTailStateHash = terminal.postTailStateHash;
                record.rollingDigest = terminal.rollingDigest;

                if (record.reason == TerminalReason::Completed) {
                    if (digestReady_ && record.rollingDigest != digest_) {
                        return fail(RecorderDisableReason::DigestDrift,
                                    "session terminal digest mismatch");
                    }
                    if (!writer_->finalize(record)) return failWriter();
                    finished_ = true;
                    status_.finalPath = writer_->finalPath();
                    return true;
                }

                if (record.lastTimelineFrame == 0u
                    && writer_->nextTimelineFrame() != 0u) {
                    record.lastTimelineFrame = writer_->nextTimelineFrame() - 1u;
                }
                if (record.postTailStateHash == 0u)
                    record.postTailStateHash = lastPostHash_;
                if (record.rollingDigest == 0u) record.rollingDigest = digest_;
                if (writer_->isOpen()) writer_->writeTerminalAndAbandon(record);
                finished_ = true;
                if (record.reason == TerminalReason::RecorderOverflow) {
                    return fail(RecorderDisableReason::RecorderOverflow,
                                "the runtime dropped records; the recording is "
                                "incomplete recording discarded");
                }
                return fail(RecorderDisableReason::GameDisconnected,
                            "the session ended without a clean terminal");
            }
            default:
                return fail(RecorderDisableReason::ProtocolError,
                            "unexpected recorder message");
        }
    }

    void abandonOpen(TerminalReason reason) {
        if (!writer_->isOpen()) return;
        SessionEndRecord terminal{};
        terminal.lastTimelineFrame = writer_->nextTimelineFrame() != 0u
            ? writer_->nextTimelineFrame() - 1u
            : 0u;
        terminal.reason = reason;
        terminal.postTailStateHash = lastPostHash_;
        terminal.rollingDigest = digest_;
        writer_->writeTerminalAndAbandon(terminal);
    }

private:
    bool fail(RecorderDisableReason reason, const char* detail) {
        reason_ = reason;
        detail_ = detail;
        return false;
    }
    bool failWriter() {
        return fail(mapWriterError(writer_->error()), writer_->errorText());
    }

    RecorderHostConfig config_;
    RecorderStatus& status_;
    std::unique_ptr<ReplayWriter> writer_;
    uint32_t expectedSequence_ = 0u;
    uint64_t digest_ = 0u;
    uint64_t lastPostHash_ = 0u;
    bool digestReady_ = false;
    bool haveProof_ = false;
    bool helloSeen_ = false;
    bool started_ = false;
    bool finished_ = false;
    RecorderDisableReason reason_ = RecorderDisableReason::None;
    const char* detail_ = "";
};

ReplayRecorderHost::ReplayRecorderHost() = default;

ReplayRecorderHost::~ReplayRecorderHost() { stop(); }

void ReplayRecorderHost::setWriterForTesting(
    std::unique_ptr<ReplayWriter> writer) {
    injectedWriter_ = std::move(writer);
}

void ReplayRecorderHost::setConfigForTesting(const RecorderHostConfig& config) {
    config_ = config;
    session_.reset();
}

void ReplayRecorderHost::disable(RecorderDisableReason reason,
                                 const char* detail) {
    std::lock_guard<std::mutex> guard(mutex_);
    status_.state = RecorderState::Disabled;
    status_.reason = reason;
    status_.detail = detail != nullptr ? detail : "";
}

bool ReplayRecorderHost::start(const RecorderHostConfig& config) {
    stop();
    config_ = config;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_ = RecorderStatus{};
        status_.state = RecorderState::Listening;
    }
    session_.reset(new Session(config_, status_, std::move(injectedWriter_)));

    wchar_t name[256];
    qipc::ipcRecorderPipeName(name, 256, config.gamePid, config.generation);
    pipeName_ = name;

    HANDLE pipe = ::CreateNamedPipeW(
        pipeName_.c_str(), PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0u,
        256u * 1024u, config.connectTimeoutMs, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        disable(RecorderDisableReason::PipeError,
                "could not create the recorder pipe");
        return false;
    }
    pipe_ = pipe;
    running_.store(true);
    worker_ = std::thread(&ReplayRecorderHost::workerMain, this);
    return true;
}

void ReplayRecorderHost::stop() {
    if (running_.load() || worker_.joinable()) {
        running_.store(false);
        if (pipe_ != nullptr) {
            ::CancelIoEx(static_cast<HANDLE>(pipe_), nullptr);
            ::DisconnectNamedPipe(static_cast<HANDLE>(pipe_));
        }
        if (worker_.joinable()) worker_.join();
    }
    if (pipe_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(pipe_));
        pipe_ = nullptr;
    }
    if (session_) {
        session_->abandonOpen(TerminalReason::ProcessExit);
        session_.reset();
    }
}

RecorderStatus ReplayRecorderHost::status() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return status_;
}

bool ReplayRecorderHost::feedFrameForTesting(const ReplayIpcFrame& frame) {
    if (!session_) {
        session_.reset(
            new Session(config_, status_, std::move(injectedWriter_)));
    }
    const bool ok = session_->handle(frame);
    if (!ok) disable(session_->reason(), session_->detail());
    else {
        std::lock_guard<std::mutex> guard(mutex_);
        if (status_.state != RecorderState::Disabled) {
            status_.state = session_->finished() ? RecorderState::Finalized
                                                 : RecorderState::Recording;
            status_.chunks = session_->writer().chunkCount();
            status_.bytes = session_->writer().bytesWritten();
            status_.flushes = session_->writer().flushCount();
            status_.partPath = session_->writer().partPath();
            if (session_->finished())
                status_.finalPath = session_->writer().finalPath();
        }
    }
    return ok;
}

void ReplayRecorderHost::workerMain() {
    HANDLE pipe = static_cast<HANDLE>(pipe_);
    const BOOL connected = ::ConnectNamedPipe(pipe, nullptr);
    if (connected == 0 && ::GetLastError() != ERROR_PIPE_CONNECTED) {
        disable(RecorderDisableReason::PipeError,
                "the game never connected to the recorder pipe");
        running_.store(false);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.state = RecorderState::Recording;
    }

    std::vector<uint8_t> inbox;
    std::vector<uint8_t> chunk;
    chunk.resize(64u * 1024u);
    bool disabled = false;

    while (running_.load()) {
        DWORD got = 0u;
        if (::ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()),
                       &got, nullptr) == 0
            || got == 0u) {
            break;
        }
        if (inbox.size() + got > 32u * 1024u * 1024u) {
            disable(RecorderDisableReason::ProtocolError,
                    "recorder stream exceeded its buffer budget");
            disabled = true;
            break;
        }
        inbox.insert(inbox.end(), chunk.begin(), chunk.begin() + got);

        for (;;) {
            ReplayIpcFrame frame{};
            size_t consumed = 0u;
            const IpcFrameStatus status =
                decodeIpcFrame(inbox.data(), inbox.size(), frame, consumed);
            if (status == IpcFrameStatus::NeedMore) break;
            if (status != IpcFrameStatus::Ok) {
                disable(RecorderDisableReason::ProtocolError,
                        ipcFrameStatusText(status));
                disabled = true;
                break;
            }
            inbox.erase(inbox.begin(),
                        inbox.begin() + static_cast<ptrdiff_t>(consumed));

            if (!session_->handle(frame)) {
                disable(session_->reason(), session_->detail());
                disabled = true;
                break;
            }
            std::lock_guard<std::mutex> guard(mutex_);
            status_.chunks = session_->writer().chunkCount();
            status_.bytes = session_->writer().bytesWritten();
            status_.flushes = session_->writer().flushCount();
            status_.partPath = session_->writer().partPath();
            if (session_->finished()) {
                status_.state = RecorderState::Finalized;
                status_.finalPath = session_->writer().finalPath();
            }
        }
        if (disabled) break;
        if (session_->finished()) break;
    }

    if (!disabled && !session_->finished()) {
        session_->abandonOpen(TerminalReason::ProcessExit);
        disable(RecorderDisableReason::GameDisconnected,
                "the game closed the recorder pipe without a terminal");
    }
    running_.store(false);
}

ReplayRecorderHostV3::ReplayRecorderHostV3() = default;

ReplayRecorderHostV3::~ReplayRecorderHostV3() { stop(); }

void ReplayRecorderHostV3::setConfigForTesting(
    const RecorderHostConfigV3& config) {
    stop();
    config_ = config;
    std::lock_guard<std::mutex> guard(mutex_);
    status_ = RecorderStatusV3{};
}

void ReplayRecorderHostV3::setWriterFactoryForTesting(
    ReplayWriterFactoryV2 factory) {
    if (!running_.load()) writerFactory_ = std::move(factory);
}

void ReplayRecorderHostV3::setProcessAliveProbeForTesting(
    std::function<bool(uint32_t)> probe) {
    if (!running_.load()) processAliveProbe_ = std::move(probe);
}

RecorderStatusV3 ReplayRecorderHostV3::status() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return status_;
}

bool ReplayRecorderHostV3::start(const RecorderHostConfigV3& config) {
    stop();
    if (config.gamePid == 0u || config.launchGeneration == 0u
        || config.launchNonce == 0u
        || config.runtimeCompatibilityId == 0u
        || !qnetplay::compat::isValidCompatibilityIdentity(
            config.compatibilityIdentity)
        || config.replayRoot.empty()) {
        return false;
    }
    config_ = config;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_ = RecorderStatusV3{};
        status_.state = wire::v3::RecorderHostState::Listening;
    }
    wchar_t name[256];
    qipc::ipcRecorderPipeName(
        name, _countof(name), config.gamePid, config.launchGeneration);
    pipeName_ = name;
    HANDLE pipe = ::CreateNamedPipeW(
        pipeName_.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
        512u * 1024u, 512u * 1024u, config.connectTimeoutMs, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.failed = true;
        status_.state = wire::v3::RecorderHostState::FailedTransport;
        status_.detail = "could not create wire-v3 recorder pipe";
        return false;
    }
    pipe_ = pipe;
    running_.store(true);
    worker_ = std::thread(&ReplayRecorderHostV3::workerMain, this);
    return true;
}

void ReplayRecorderHostV3::stop() {
    if (running_.load() || worker_.joinable()) {
        running_.store(false);
        if (pipe_) {
            ::CancelIoEx(static_cast<HANDLE>(pipe_), nullptr);
            ::DisconnectNamedPipe(static_cast<HANDLE>(pipe_));
        }
        if (worker_.joinable()) worker_.join();
    }
    if (pipe_) {
        ::CloseHandle(static_cast<HANDLE>(pipe_));
        pipe_ = nullptr;
    }
    sealOpenGameOnDisconnect();
}

namespace {

bool sameWireV3Frame(
    const qreplay::ipc::v3::Frame& a,
    const qreplay::ipc::v3::Frame& b) {
    return a.channel == b.channel && a.type == b.type
        && a.launchGeneration == b.launchGeneration && a.game == b.game
        && a.launchNonce == b.launchNonce && a.sequence == b.sequence
        && a.param0 == b.param0 && a.param1 == b.param1
        && a.payload == b.payload;
}

wire::v3::RecorderFailureCode mapWriterErrorV3(
    const WriterError error) {
    switch (error) {
        case WriterError::FlushFailed:
            return wire::v3::RecorderFailureCode::Flush;
        case WriterError::RenameFailed:
            return wire::v3::RecorderFailureCode::Rename;
        case WriterError::DiskFull:
        case WriterError::AccessDenied:
        case WriterError::WriteFailed:
        case WriterError::CreateFailed:
        case WriterError::RootUnusable:
        case WriterError::NameExhausted:
            return wire::v3::RecorderFailureCode::Storage;
        case WriterError::WrongOrder:
        case WriterError::InvalidRecord:
        case WriterError::LimitExceeded:
            return wire::v3::RecorderFailureCode::Validation;
        default:
            return wire::v3::RecorderFailureCode::Internal;
    }
}

bool processIsAliveV3(const uint32_t pid) {
    if (pid == 0u) return false;
    HANDLE process = ::OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return ::GetLastError() != ERROR_INVALID_PARAMETER;
    }
    const DWORD wait = ::WaitForSingleObject(process, 0u);
    ::CloseHandle(process);
    return wait == WAIT_TIMEOUT || wait == WAIT_FAILED;
}

}

bool ReplayRecorderHostV3::emit(
    const wire::v3::MessageType type, const GameIdentityV2& game,
    const std::vector<uint8_t>& payload, const uint32_t param0,
    const uint32_t param1) {
    wire::v3::Frame frame{};
    frame.channel = wire::v3::Channel::Recorder;
    frame.type = type;
    frame.launchGeneration = config_.launchGeneration;
    frame.game = game;
    frame.launchNonce = config_.launchNonce;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        frame.sequence = ++status_.outboundSequence;
    }
    frame.param0 = param0;
    frame.param1 = param1;
    frame.payload = payload;
    std::vector<uint8_t> encoded;
    if (!wire::v3::encodeFrame(frame, encoded)) return false;
    lastResponses_.push_back(frame);
    testOutbound_.push_back(frame);
    if (pipe_ && running_.load()) {
        DWORD wrote = 0u;
        if (::WriteFile(
                static_cast<HANDLE>(pipe_), encoded.data(),
                static_cast<DWORD>(encoded.size()), &wrote, nullptr) == 0
            || wrote != encoded.size()) {
            return false;
        }
    }
    return true;
}

bool ReplayRecorderHostV3::failProtocol(
    const wire::v3::RecorderFailureCode code, const uint16_t stage,
    const GameIdentityV2& game, const char* detail) {
    QLOG_ISSUE("[recorder] failed code=%u stage=%u: %s",
        static_cast<unsigned>(code), stage, detail ? detail : "unknown");
    wire::v3::CasterGameFailedPayload failed{};
    failed.code = code;
    failed.stage = stage;
    failed.nextRecord = writer_ ? writer_->nextRecord() : 0u;
    failed.nextEventSequence =
        writer_ ? writer_->nextEventSequence() : 0u;
    (void)emit(
        wire::v3::MessageType::CasterGameFailed, game,
        [&]() {
            std::vector<uint8_t> bytes;
            wire::v3::encodeCasterGameFailed(failed, bytes);
            return bytes;
        }());
    if (writer_) writer_->abandon();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.failed = true;
        status_.failure = code;
        status_.failureStage = stage;
        status_.detail = detail != nullptr ? detail : "";
        status_.state =
            code == wire::v3::RecorderFailureCode::Storage
                || code == wire::v3::RecorderFailureCode::Flush
                || code == wire::v3::RecorderFailureCode::Rename
            ? wire::v3::RecorderHostState::FailedFile
            : wire::v3::RecorderHostState::FailedTransport;
        ++status_.gamesFailed;
    }
    resetGame();
    return false;
}

void ReplayRecorderHostV3::setBattleClockForTesting(
    std::function<int64_t()> clock) {
    battleClock_ = std::move(clock);
}

int64_t ReplayRecorderHostV3::battleClockNow() const {
    if (battleClock_) return battleClock_();
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    const uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32)
                         | static_cast<uint64_t>(ft.dwLowDateTime);
    const int64_t seconds =
        static_cast<int64_t>(ticks / 10000000ull) - 11644473600ll;
    return seconds > 0 ? seconds : 0;
}

void ReplayRecorderHostV3::appendBattleLogEntry(
    const qreplay::ConfirmedGameEndV2& end,
    const WriterSummaryV2& summary) {
    if (!config_.battleLogEnabled || config_.replayRoot.empty()) return;
    if (!haveRetainedGame_) return;

    const qreplay::GameManifestV2& manifest = retainedManifest_;
    BattleLogRowV1 row;
    row.gameUuidHigh = manifest.identity.gameUuidHigh;
    row.gameUuidLow = manifest.identity.gameUuidLow;
    row.setUuidHigh = manifest.display.seriesUuidHigh;
    row.setUuidLow = manifest.display.seriesUuidLow;
    row.gameNo = manifest.identity.gameGeneration;

    row.startUnix = gameStartUnix_;
    row.endUnix = battleClockNow();
    row.setStartUnix = setStartUnix_;
    row.timezoneOffsetMinutes = manifest.display.timezoneOffsetMinutes;

    row.sourceKind = static_cast<uint8_t>(manifest.display.sourceKind);
    row.mySide = config_.battleLogLocalSide;
    row.characterOne = manifest.slots[0].identity.nativeCharacterId;
    row.characterTwo = manifest.slots[1].identity.nativeCharacterId;
    row.paletteOne = manifest.slots[0].identity.paletteIndex;
    row.paletteTwo = manifest.slots[1].identity.paletteIndex;
    row.characterCatalogId = manifest.contract.characterCatalogId;

    row.winningSideMask = end.winningSideMask;
    row.roundsOne = static_cast<int16_t>(end.roundTallies[0]);
    row.roundsTwo = static_cast<int16_t>(end.roundTallies[1]);
    row.terminalReason = static_cast<uint16_t>(end.reason);

    row.frames = summary.frameCount;
    row.roundsToWin = retainedConfiguration_.roundsToWinMinusOne + 1u;
    row.stageId = manifest.stage.nativeStageId;
    row.bytes = summary.bytes;
    uint64_t digest = 0u;
    for (size_t i = 0u; i < 8u && i < summary.canonicalGameDigest.size();
         ++i) {
        digest = (digest << 8)
            | static_cast<uint64_t>(summary.canonicalGameDigest[i]);
    }
    row.digest = digest;

    row.nameOne = qreplay::replayTextV2(manifest.lanes[0].name);
    row.nameTwo = qreplay::replayTextV2(manifest.lanes[1].name);
    row.myName = config_.battleLogLocalName;

    const std::wstring& finalPath = summary.finalPath;
    std::wstring fileName = finalPath;
    const size_t slash = fileName.find_last_of(L"\\/");
    if (slash != std::wstring::npos) fileName = fileName.substr(slash + 1u);
    std::string relative;
    if (!retainedSetFolder_.empty()) relative = retainedSetFolder_ + "\\";
    relative += qplat::narrow(fileName);
    row.relativePath = relative;

    (void)appendBattleLogRow(config_.replayRoot, row);
}

void ReplayRecorderHostV3::resetGame() {
    writer_.reset();
    pendingTick_ = CanonicalTickBundleV2{};
    hasPendingFrame_ = false;
    expectedFrame_ = 0u;
    expectedEventSequence_ = 0u;
    activeLaneMask_ = 0u;
    retainedManifest_ = qreplay::GameManifestV2{};
    retainedConfiguration_ = qreplay::MatchConfigurationV2{};
    retainedSetFolder_.clear();
    gameStartUnix_ = 0;
    setStartUnix_ = 0;
    haveRetainedGame_ = false;
    std::lock_guard<std::mutex> guard(mutex_);
    status_.currentGame = GameIdentityV2{};
    status_.partPath.clear();
    status_.nextRecord = 0u;
    status_.nextEventSequence = 0u;
}

bool ReplayRecorderHostV3::flushPendingTick() {
    if (!hasPendingFrame_) return true;
    if (!writer_ || !writer_->append(pendingTick_))
        return false;
    pendingTick_ = CanonicalTickBundleV2{};
    hasPendingFrame_ = false;
    std::lock_guard<std::mutex> guard(mutex_);
    status_.nextRecord = writer_->nextRecord();
    status_.nextEventSequence = writer_->nextEventSequence();
    status_.bytesWritten = writer_->bytesWritten();
    return true;
}

bool ReplayRecorderHostV3::addFrames(
    const std::vector<ConfirmedGameFrameV2>& frames) {
    for (const ConfirmedGameFrameV2& frame : frames) {
        if (hasPendingFrame_ && !flushPendingTick()) return false;
        if (!writer_ || frame.replayFrame != expectedFrame_)
            return false;
        if (pendingTick_.preEvents.count != 0u
            && pendingTick_.preEvents.events[0].replayFrame
                != frame.replayFrame) {
            return false;
        }
        pendingTick_.frame = frame;
        hasPendingFrame_ = true;
        ++expectedFrame_;
    }
    return true;
}

bool ReplayRecorderHostV3::addEvents(
    const std::vector<ConfirmedGameEventV2>& events) {
    for (const ConfirmedGameEventV2& event : events) {
        if (event.eventSequence != expectedEventSequence_) return false;
        if (event.phase == GameEventPhaseV2::PreFrame) {
            if (hasPendingFrame_ && !flushPendingTick()) return false;
            if (event.replayFrame != expectedFrame_
                || pendingTick_.preEvents.count
                    >= kMaxEventsPerTickV2) {
                return false;
            }
            pendingTick_.preEvents.events[
                pendingTick_.preEvents.count++] = event;
        } else {
            if (!hasPendingFrame_ || expectedFrame_ == 0u
                || event.replayFrame != expectedFrame_ - 1u
                || pendingTick_.postEvents.count
                    >= kMaxEventsPerTickV2) {
                return false;
            }
            pendingTick_.postEvents.events[
                pendingTick_.postEvents.count++] = event;
        }
        ++expectedEventSequence_;
    }
    return true;
}

bool ReplayRecorderHostV3::handleFreshFrame(
    const wire::v3::Frame& frame) {
    const uint8_t* body = frame.payload.data();
    const size_t bytes = frame.payload.size();
    switch (frame.type) {
        case wire::v3::MessageType::RecorderHello: {
            wire::v3::RecorderHelloPayload hello{};
            if (status().helloAccepted
                || frame.game != GameIdentityV2{}
                || frame.param0 != 0u || frame.param1 != 0u
                || !wire::v3::decodeRecorderHello(body, bytes, hello)
                || hello.gamePid != config_.gamePid
                || hello.runtimeCompatibilityId
                    != config_.runtimeCompatibilityId) {
                return failProtocol(
                    wire::v3::RecorderFailureCode::Identity, 1u,
                    GameIdentityV2{}, "wire-v3 hello rejected");
            }
            std::lock_guard<std::mutex> guard(mutex_);
            status_.helloAccepted = true;
            status_.state = wire::v3::RecorderHostState::WaitingForGame;
            return true;
        }
        case wire::v3::MessageType::RecorderGameBegin: {
            if (!status().helloAccepted || writer_
                || frame.param0 != 0u || frame.param1 != 1u) {
                return failProtocol(
                    wire::v3::RecorderFailureCode::Protocol, 2u,
                    frame.game, "game begin in the wrong state");
            }
            BeginGameBundleV2 begin{};
            if (wire::v3::decodeRecorderGameBegin(body, bytes, begin)
                    != ParseStatus::Ok
                || begin.start.manifest.identity != frame.game) {
                return failProtocol(
                    wire::v3::RecorderFailureCode::Validation, 2u,
                    frame.game, "game begin composite rejected");
            }
            const int64_t setStartUnix = begin.start.manifest.display.recordedUnixSeconds;
            const int64_t gameStartUnix = battleClockNow();
            writer_ = writerFactory_ ? writerFactory_()
                                     : std::unique_ptr<ReplayWriterV2>(
                                           new ReplayWriterV2());
            if (!writer_) {
                return failProtocol(
                    wire::v3::RecorderFailureCode::Internal, 2u,
                    frame.game, "writer allocation failed");
            }
            WriterConfig config{};
            config.rootDirectory = config_.replayRoot;
            std::string setFolder;
            if (!config_.replayRoot.empty()) {
                std::wstring parent = config_.replayRoot;
                const std::string sourceFolder =
                    sourceFolderNameFor(begin.start.manifest);
                const std::wstring sourceDirectory =
                    parent + L"\\" + qplat::widen(sourceFolder);
                if (qplat::ensureDirW(sourceDirectory)) {
                    parent = sourceDirectory;
                    setFolder = sourceFolder + "\\";
                    config.rootDirectory = parent;
                }
                const std::string setName =
                    setFolderNameFor(begin.start.manifest);
                const std::wstring setDirectory =
                    parent + L"\\" + qplat::widen(setName);
                if (qplat::ensureDirW(setDirectory)) {
                    config.rootDirectory = setDirectory;
                    setFolder += setName;
                } else if (!setFolder.empty()) {
                    setFolder.pop_back();
                }
            }
            config.nameHint =
                gameNameHintFor(begin.start.manifest, config_.nameHint);
            begin.start.manifest.display.recordedUnixSeconds = gameStartUnix;
            config.recordedUnixSeconds = gameStartUnix;
            config.flushEveryChunks = config_.flushEveryChunks;
            config.flushEveryMs = config_.flushEveryMs;
            qreplay::ReplayCompatibilityIdentityEnvelopeV3 identity{};
            identity.status =
                qreplay::ReplayCompatibilityIdentityStatusV3::Attested;
            identity.identity = config_.compatibilityIdentity;
            const bool began = qnetplay::compat::isValidCompatibilityIdentity(
                config_.compatibilityIdentity)
                ? writer_->begin(config, begin, identity)
                : writer_->begin(config, begin);
            if (!began) {
                return failProtocol(
                    mapWriterErrorV3(writer_->error()),
                    static_cast<uint16_t>(writer_->errorStage()),
                    frame.game, writer_->errorText());
            }
            expectedFrame_ = 1u;
            expectedEventSequence_ = writer_->nextEventSequence();
            activeLaneMask_ =
                begin.start.manifest.activeLaneMask;
            retainedManifest_ = begin.start.manifest;
            retainedConfiguration_ = begin.start.start.configuration;
            retainedSetFolder_ = setFolder;
            gameStartUnix_ = gameStartUnix;
            setStartUnix_ = setStartUnix;
            haveRetainedGame_ = true;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                ++status_.gamesBegun;
                status_.currentGame = frame.game;
                status_.partPath = writer_->partPath();
                status_.nextRecord = 1u;
                status_.nextEventSequence = expectedEventSequence_;
                status_.state = wire::v3::RecorderHostState::WritingGame;
            }
            wire::v3::CasterGameAcceptedPayload accepted{};
            accepted.nextRecord = 1u;
            accepted.nextEventSequence = expectedEventSequence_;
            std::vector<uint8_t> payload;
            if (!wire::v3::encodeCasterGameAccepted(accepted, payload)
                || !emit(
                    wire::v3::MessageType::CasterGameAccepted,
                    frame.game, payload)) {
                return false;
            }
            return true;
        }
        case wire::v3::MessageType::RecorderFrames: {
            if (!writer_ || frame.game != status().currentGame
                || frame.param0 != expectedFrame_
                || frame.param1 == 0u) {
                return failProtocol(
                    wire::v3::RecorderFailureCode::Sequence, 3u,
                    frame.game, "frame batch cursor mismatch");
            }
            std::vector<ConfirmedGameFrameV2> frames;
            if (wire::v3::decodeRecorderFrames(
                    body, bytes, frame.param0, frame.param1,
                    activeLaneMask_, frames) != ParseStatus::Ok
                || !addFrames(frames)) {
                return failProtocol(
                    wire::v3::RecorderFailureCode::Validation, 3u,
                    frame.game, "frame batch rejected");
            }
            return true;
        }
        case wire::v3::MessageType::RecorderEvents: {
            if (!writer_ || frame.game != status().currentGame
                || frame.param0 != expectedEventSequence_
                || frame.param1 == 0u) {
                return failProtocol(
                    wire::v3::RecorderFailureCode::Sequence, 4u,
                    frame.game, "event batch cursor mismatch");
            }
            std::vector<ConfirmedGameEventV2> events;
            if (wire::v3::decodeRecorderEvents(
                    body, bytes, frame.param0, frame.param1, events)
                    != ParseStatus::Ok
                || !addEvents(events)) {
                return failProtocol(
                    wire::v3::RecorderFailureCode::Validation, 4u,
                    frame.game, "event batch rejected");
            }
            return true;
        }
        case wire::v3::MessageType::RecorderPalette: {
            if (!writer_ || frame.game != status().currentGame) return true;
            ReplayPaletteV2 palette{};
            if (wire::v3::decodeRecorderPalette(body, bytes, palette)
                    != ParseStatus::Ok) {
                std::lock_guard<std::mutex> guard(mutex_);
                ++status_.palettesRejected;
                return true;
            }
            if (writer_->writePalette(palette)) {
                std::lock_guard<std::mutex> guard(mutex_);
                ++status_.palettesRecorded;
            } else {
                std::lock_guard<std::mutex> guard(mutex_);
                ++status_.palettesRejected;
            }
            return true;
        }
        case wire::v3::MessageType::RecorderGameEnd: {
            if (!writer_ || frame.game != status().currentGame) {
                return failProtocol(
                    wire::v3::RecorderFailureCode::Protocol, 5u,
                    frame.game, "game end without an active transaction");
            }
            EndGameBundleV2 end{};
            if (wire::v3::decodeRecorderGameEnd(
                    body, bytes, activeLaneMask_, end) != ParseStatus::Ok
                || frame.param0 != end.end.nextRecord
                || frame.param1
                    != (end.containsFinalFrame ? 1u : 0u)
                || !flushPendingTick()
                || !writer_->finalize(end)) {
                const WriterError error =
                    writer_ ? writer_->error()
                            : WriterError::InvalidRecord;
                return failProtocol(
                    mapWriterErrorV3(error),
                    writer_
                        ? static_cast<uint16_t>(writer_->errorStage())
                        : 5u,
                    frame.game,
                    writer_ ? writer_->errorText()
                            : "game end rejected");
            }
            const WriterSummaryV2 summary = writer_->summary();
            appendBattleLogEntry(end.end, summary);
            wire::v3::CasterGameCommittedPayload committed{};
            committed.nextRecord = summary.frameCount;
            committed.reason = summary.terminalReason;
            committed.canonicalGameDigest =
                summary.canonicalGameDigest;
            committed.fileDigest = summary.fileDigest;
            committed.fileBytes = summary.bytes;
            std::vector<uint8_t> payload;
            if (!wire::v3::encodeCasterGameCommitted(
                    committed, payload)
                || !emit(
                    wire::v3::MessageType::CasterGameCommitted,
                    frame.game, payload)) {
                return false;
            }
            {
                std::lock_guard<std::mutex> guard(mutex_);
                ++status_.gamesCommitted;
                status_.lastFinalPath = summary.finalPath;
                status_.state =
                    wire::v3::RecorderHostState::WaitingForGame;
            }
            resetGame();
            {
                std::lock_guard<std::mutex> guard(mutex_);
                status_.state =
                    wire::v3::RecorderHostState::WaitingForGame;
            }
            return true;
        }
        case wire::v3::MessageType::RecorderDrain: {
            wire::v3::RecorderDrainPayload drain{};
            const RecorderStatusV3 snapshot = status();
            if (writer_ || drainSeen_
                || !wire::v3::decodeRecorderDrain(body, bytes, drain)
                || drain.gamesBegun != snapshot.gamesBegun
                || drain.gamesEnded != snapshot.gamesCommitted
                        + snapshot.gamesFailed
                || drain.abandonedOpenGame) {
                return failProtocol(
                    wire::v3::RecorderFailureCode::Protocol, 6u,
                    GameIdentityV2{}, "recorder drain rejected");
            }
            drainSeen_ = true;
            wire::v3::CasterDrainAckPayload ack{};
            ack.committedGames = snapshot.gamesCommitted;
            ack.failedGames = snapshot.gamesFailed;
            std::vector<uint8_t> payload;
            if (!wire::v3::encodeCasterDrainAck(ack, payload)
                || !emit(
                    wire::v3::MessageType::CasterDrainAck,
                    GameIdentityV2{}, payload)) {
                return false;
            }
            std::lock_guard<std::mutex> guard(mutex_);
            status_.drainAcknowledged = true;
            status_.state = wire::v3::RecorderHostState::Draining;
            return true;
        }
        case wire::v3::MessageType::RecorderBye: {
            wire::v3::RecorderByePayload bye{};
            if (!drainSeen_
                || !wire::v3::decodeRecorderBye(body, bytes, bye)
                || !bye.drainCompleted) {
                return failProtocol(
                    wire::v3::RecorderFailureCode::Protocol, 7u,
                    GameIdentityV2{}, "recorder bye rejected");
            }
            std::lock_guard<std::mutex> guard(mutex_);
            status_.state = wire::v3::RecorderHostState::Finished;
            return true;
        }
        default:
            return failProtocol(
                wire::v3::RecorderFailureCode::Protocol, 8u,
                frame.game, "unexpected recorder message");
    }
}

bool ReplayRecorderHostV3::handleFrame(
    const wire::v3::Frame& frame) {
    if (frame.channel != wire::v3::Channel::Recorder
        || frame.launchGeneration != config_.launchGeneration
        || frame.launchNonce != config_.launchNonce
        || frame.sequence == 0u) {
        return failProtocol(
            wire::v3::RecorderFailureCode::Identity, 0u,
            frame.game, "wire-v3 launch provenance mismatch");
    }
    const RecorderStatusV3 snapshot = status();
    if (frame.sequence == snapshot.inboundSequence) {
        if (!haveLastInbound_ || frame.type
                == wire::v3::MessageType::RecorderHello
            || !sameWireV3Frame(frame, lastInbound_)) {
            return failProtocol(
                wire::v3::RecorderFailureCode::Sequence, 0u,
                frame.game, "conflicting wire-v3 duplicate");
        }
        for (const wire::v3::Frame& response : lastResponses_) {
            testOutbound_.push_back(response);
            if (pipe_ && running_.load()) {
                std::vector<uint8_t> encoded;
                if (!wire::v3::encodeFrame(response, encoded)) return false;
                DWORD wrote = 0u;
                if (::WriteFile(
                        static_cast<HANDLE>(pipe_), encoded.data(),
                        static_cast<DWORD>(encoded.size()), &wrote, nullptr)
                        == 0
                    || wrote != encoded.size()) {
                    return false;
                }
            }
        }
        return true;
    }
    if (frame.sequence != snapshot.inboundSequence + 1u) {
        return failProtocol(
            wire::v3::RecorderFailureCode::Sequence, 0u,
            frame.game, "wire-v3 sequence gap");
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.inboundSequence = frame.sequence;
    }
    lastInbound_ = frame;
    haveLastInbound_ = true;
    lastResponses_.clear();
    return handleFreshFrame(frame);
}

bool ReplayRecorderHostV3::feedFrameForTesting(
    const wire::v3::Frame& frame) {
    return handleFrame(frame);
}

std::vector<wire::v3::Frame>
ReplayRecorderHostV3::takeOutboundFramesForTesting() {
    std::vector<wire::v3::Frame> out;
    out.swap(testOutbound_);
    return out;
}

void ReplayRecorderHostV3::sealOpenGameOnDisconnect() {
    if (!writer_) return;
    const bool processAlive = processAliveProbe_
        ? processAliveProbe_(config_.gamePid)
        : processIsAliveV3(config_.gamePid);
    const TerminalReason reason = processAlive
        ? TerminalReason::RecordingFailure
        : TerminalReason::ProcessExit;
    if (hasPendingFrame_ && !flushPendingTick()) {
        writer_->abandon();
        {
            std::lock_guard<std::mutex> guard(mutex_);
            ++status_.gamesFailed;
            status_.failed = true;
            status_.failure = mapWriterErrorV3(writer_->error());
            status_.failureStage =
                static_cast<uint16_t>(writer_->errorStage());
            status_.state = wire::v3::RecorderHostState::FailedFile;
            status_.lastProcessAlive = processAlive;
            status_.lastTerminalReason = reason;
            status_.detail =
                "could not flush the validated tail after pipe close";
        }
        resetGame();
        return;
    }
    ConfirmedGameEndV2 end{};
    if (writer_->makeAbnormalEnd(reason, end)) {
        EndGameBundleV2 bundle{};
        bundle.end = end;
        if (writer_->finalize(bundle)) {
            const WriterSummaryV2 summary = writer_->summary();
            appendBattleLogEntry(end, summary);
            std::lock_guard<std::mutex> guard(mutex_);
            ++status_.gamesCommitted;
            status_.lastFinalPath = summary.finalPath;
            status_.lastProcessAlive = processAlive;
            status_.lastTerminalReason = reason;
            status_.failed = processAlive;
            status_.failure =
                wire::v3::RecorderFailureCode::Protocol;
            status_.state = processAlive
                ? wire::v3::RecorderHostState::FailedTransport
                : wire::v3::RecorderHostState::Finished;
            status_.detail = processAlive
                ? "recorder pipe closed while QOH was still alive"
                : "QOH exited; validated replay tail sealed";
        } else {
            std::lock_guard<std::mutex> guard(mutex_);
            ++status_.gamesFailed;
            status_.failed = true;
            status_.failure = mapWriterErrorV3(writer_->error());
            status_.failureStage =
                static_cast<uint16_t>(writer_->errorStage());
            status_.state = wire::v3::RecorderHostState::FailedFile;
            status_.lastProcessAlive = processAlive;
            status_.lastTerminalReason = reason;
            status_.detail = writer_->errorText();
        }
    } else {
        writer_->abandon();
        std::lock_guard<std::mutex> guard(mutex_);
        ++status_.gamesFailed;
        status_.failed = true;
        status_.failure =
            wire::v3::RecorderFailureCode::Validation;
        status_.state = wire::v3::RecorderHostState::FailedTransport;
        status_.lastProcessAlive = processAlive;
        status_.lastTerminalReason = reason;
        status_.detail =
            "validated replay tail could not form an abnormal terminal";
    }
    resetGame();
}

void ReplayRecorderHostV3::workerMain() {
    HANDLE pipe = static_cast<HANDLE>(pipe_);
    const BOOL connected = ::ConnectNamedPipe(pipe, nullptr);
    if (connected == 0 && ::GetLastError() != ERROR_PIPE_CONNECTED) {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.failed = true;
        status_.state = wire::v3::RecorderHostState::FailedTransport;
        status_.detail = "game never connected to wire-v3 recorder";
        running_.store(false);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.state = wire::v3::RecorderHostState::Connected;
    }
    std::vector<uint8_t> inbox;
    std::vector<uint8_t> block(64u * 1024u);
    while (running_.load()) {
        DWORD got = 0u;
        if (::ReadFile(
                pipe, block.data(), static_cast<DWORD>(block.size()),
                &got, nullptr) == 0
            || got == 0u) {
            break;
        }
        if (inbox.size() + got
            > wire::v3::kMaxPayloadBytes + wire::v3::kHeaderBytes) {
            break;
        }
        inbox.insert(inbox.end(), block.begin(), block.begin() + got);
        for (;;) {
            wire::v3::Frame frame{};
            size_t consumed = 0u;
            const wire::v3::FrameStatus decoded =
                wire::v3::decodeFrame(
                    inbox.data(), inbox.size(), frame, consumed);
            if (decoded == wire::v3::FrameStatus::NeedMore) break;
            if (decoded != wire::v3::FrameStatus::Ok
                || !handleFrame(frame)) {
                running_.store(false);
                break;
            }
            inbox.erase(
                inbox.begin(),
                inbox.begin() + static_cast<ptrdiff_t>(consumed));
            if (frame.type == wire::v3::MessageType::RecorderBye) {
                running_.store(false);
                break;
            }
        }
    }
    if (!status().drainAcknowledged) sealOpenGameOnDisconnect();
    running_.store(false);
}

}
