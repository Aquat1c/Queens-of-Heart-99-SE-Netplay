#include "game/replay/ReplayIpcWorker.h"

#include "Log.h"
#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"
#include "replay/ReplayPublisherQueueV2.h"
#include "replay/ReplayRecorderQueue.h"
#include "replay/ReplaySimulationContractV2.h"

#include <windows.h>

#include <cstring>
#include <new>

namespace qgame {

namespace {

class NamedPipeChannel final : public ByteChannel {
public:
    NamedPipeChannel(const wchar_t* name, uint32_t writeTimeoutMs)
        : writeTimeoutMs_(writeTimeoutMs) {
        const size_t cap = sizeof(name_) / sizeof(name_[0]);
        size_t i = 0;
        for (; name && name[i] && i + 1u < cap; ++i) name_[i] = name[i];
        name_[i] = L'\0';
    }

    ~NamedPipeChannel() override { close(); }

    bool connect(uint32_t timeoutMs) override {
        if (handle_) return true;
        if (!name_[0]) return false;
        ++connectAttempts_;
        // The recorder and spectator publication servers are PIPE_ACCESS_INBOUND: requesting GENERIC_READ fails with ERROR_ACCESS_DENIED, so fall back to write-only.
        DWORD access = GENERIC_READ | GENERIC_WRITE;
        HANDLE h = CreateFileW(name_, access, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE
            && GetLastError() == ERROR_ACCESS_DENIED) {
            access = GENERIC_WRITE;
            h = CreateFileW(name_, access, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, nullptr);
        }
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_PIPE_BUSY
                && WaitNamedPipeW(name_, timeoutMs)) {
                access = GENERIC_READ | GENERIC_WRITE;
                h = CreateFileW(name_, access, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, nullptr);
                if (h == INVALID_HANDLE_VALUE
                    && GetLastError() == ERROR_ACCESS_DENIED) {
                    access = GENERIC_WRITE;
                    h = CreateFileW(name_, access, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr);
                }
            }
        }
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (connectAttempts_ == 1u || error != lastConnectError_) {
                QLOG("[replay.ipc] pipe connect pending name='%ls' "
                     "attempt=%u error=%lu",
                     name_, connectAttempts_, error);
            }
            lastConnectError_ = error;
            return false;
        }
        QLOG("[replay.ipc] pipe connected name='%ls' attempts=%u access=%s",
             name_, connectAttempts_,
             (access & GENERIC_READ) != 0 ? "duplex" : "write-only");
        lastConnectError_ = ERROR_SUCCESS;
        writeOnly_ = (access & GENERIC_READ) == 0;

        if (!writeOnly_) {
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
        }

        readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!readEvent_ || !writeEvent_) {
            if (readEvent_) CloseHandle(readEvent_);
            if (writeEvent_) CloseHandle(writeEvent_);
            readEvent_ = nullptr;
            writeEvent_ = nullptr;
            CloseHandle(h);
            return false;
        }
        handle_ = h;
        readPending_ = false;
        return true;
    }

    void close() override {
        if (handle_) {
            CancelIo(handle_);
            CloseHandle(handle_);
            handle_ = nullptr;
        }
        if (readEvent_) { CloseHandle(readEvent_); readEvent_ = nullptr; }
        if (writeEvent_) { CloseHandle(writeEvent_); writeEvent_ = nullptr; }
        readPending_ = false;
        writeOnly_ = false;
    }

    bool connected() const override { return handle_ != nullptr; }

    bool read(uint8_t* dst, size_t cap, size_t& read) override {
        read = 0;
        if (!handle_ || !dst || cap == 0) return false;
        if (writeOnly_) return true;
        if (cap > sizeof(staging_)) cap = sizeof(staging_);

        if (!readPending_) {
            ZeroMemory(&readOv_, sizeof(readOv_));
            readOv_.hEvent = readEvent_;
            ResetEvent(readEvent_);
            DWORD got = 0;
            if (ReadFile(handle_, staging_, static_cast<DWORD>(cap), &got,
                         &readOv_)) {
                if (got > 0) { memcpy(dst, staging_, got); read = got; }
                return true;
            }
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) return false;
            readPending_ = true;
            return true;
        }

        DWORD got = 0;
        if (GetOverlappedResult(handle_, &readOv_, &got, FALSE)) {
            readPending_ = false;
            if (got > 0) {
                const size_t copy = (got < cap) ? got : cap;
                memcpy(dst, staging_, copy);
                read = copy;
            }
            return true;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE) return true;
        readPending_ = false;
        return false;
    }

    bool write(const uint8_t* src, size_t len) override {
        if (!handle_) return false;
        if (len == 0) return true;
        size_t off = 0;
        while (off < len) {
            ZeroMemory(&writeOv_, sizeof(writeOv_));
            writeOv_.hEvent = writeEvent_;
            ResetEvent(writeEvent_);
            DWORD wrote = 0;
            const DWORD chunk =
                static_cast<DWORD>((len - off) > 32768u ? 32768u : (len - off));
            if (!WriteFile(handle_, src + off, chunk, &wrote, &writeOv_)) {
                if (GetLastError() != ERROR_IO_PENDING) return false;
                const DWORD wait = WaitForSingleObject(writeEvent_,
                                                       writeTimeoutMs_);
                if (wait != WAIT_OBJECT_0) {
                    CancelIo(handle_);
                    return false;
                }
                if (!GetOverlappedResult(handle_, &writeOv_, &wrote, FALSE))
                    return false;
            }
            if (wrote == 0) return false;
            off += wrote;
        }
        return true;
    }

private:
    wchar_t name_[128]{};
    HANDLE handle_ = nullptr;
    HANDLE readEvent_ = nullptr;
    HANDLE writeEvent_ = nullptr;
    OVERLAPPED readOv_{};
    OVERLAPPED writeOv_{};
    bool readPending_ = false;
    bool writeOnly_ = false;
    uint32_t connectAttempts_ = 0u;
    DWORD lastConnectError_ = ERROR_SUCCESS;
    uint32_t writeTimeoutMs_ = 1000u;
    uint8_t staging_[16384]{};
};

}

std::unique_ptr<ByteChannel> makeNamedPipeChannel(const wchar_t* pipeName,
                                                  uint32_t writeTimeoutMs) {
    return std::unique_ptr<ByteChannel>(
        new (std::nothrow) NamedPipeChannel(pipeName, writeTimeoutMs));
}

namespace replay {

const char* sourceFaultText(SourceFault fault) {
    switch (fault) {
        case SourceFault::None: return "none";
        case SourceFault::ChannelLost: return "channel-lost";
        case SourceFault::ReconnectDeadline: return "reconnect-deadline";
        case SourceFault::BadFraming: return "bad-framing";
        case SourceFault::BadPayloadCrc: return "bad-payload-crc";
        case SourceFault::StaleGeneration: return "stale-generation";
        case SourceFault::WrongNonce: return "wrong-nonce";
        case SourceFault::SequenceViolation: return "sequence-violation";
        case SourceFault::PayloadTooLarge: return "payload-too-large";
        case SourceFault::DecodeFailed: return "decode-failed";
        case SourceFault::ManifestMismatch: return "manifest-mismatch";
        case SourceFault::FrameGap: return "frame-gap";
        case SourceFault::InvalidActionMask: return "invalid-action-mask";
        case SourceFault::InvalidLaneShape: return "invalid-lane-shape";
        case SourceFault::DigestBreak: return "digest-break";
        case SourceFault::UnexpectedMessage: return "unexpected-message";
    }
    return "unknown";
}

namespace {
uint64_t nowMs() { return static_cast<uint64_t>(GetTickCount64()); }

enum class PlaybackReceivePhaseV3 : uint8_t {
    WaitingForSessionInfo = 1,
    WaitingForGameStart,
    ReceivingFrames,
    ReceivingEventsOrEndpoint,
    Ready,
    Faulted,
};
}

struct ReplayIpcWorker::Impl {
    ReplayIpcOptions options{};
    std::unique_ptr<ByteChannel> broker;
    std::unique_ptr<ByteChannel> recorder;
    qreplay::ReplayRecorderQueue* queue = nullptr;
    qreplay::ReplayPublisherQueueV2* queueV2 = nullptr;

    void* threadHandle = nullptr;
    bool threaded = false;

    std::vector<uint8_t> rx;
    size_t rxRead = 0u;
    std::vector<uint8_t> scratch;
    std::vector<uint8_t> recScratch;
    std::vector<uint8_t> hdrScratch;

    uint32_t inboundSequence = 0u;
    uint32_t outboundSequence = 0u;
    uint32_t recorderSequence = 0u;

    bool brokerHelloSent = false;
    bool recorderHelloSent = false;
    bool sessionInfoSeen = false;
    bool startSeen = false;
    bool terminalSeen = false;
    bool faulted = false;

    bool expectFrameKnown = false;
    uint32_t expectedFrame = 0u;
    uint64_t chainedDigest = 0u;
    bool chainedDigestKnown = false;

    std::vector<qreplay::ReplayFrameV1> pendingFrames;
    size_t pendingFrameIndex = 0u;
    std::vector<qreplay::ReplayEvent> pendingEvents;
    size_t pendingEventIndex = 0u;

    std::vector<uint8_t> playbackRxV3;
    std::vector<uint8_t> playbackTxV3;
    uint32_t playbackInboundSequenceV3 = 0u;
    uint32_t playbackOutboundSequenceV3 = 0u;
    uint64_t playbackLastCommandIdV3 = 0u;
    PlaybackReceivePhaseV3 playbackPhaseV3 =
        PlaybackReceivePhaseV3::WaitingForSessionInfo;
    qreplay::ipc::v3::PlaybackSessionInfoPayload playbackInfoV3{};
    qreplay::ConfirmedGameStartV2 playbackStartV3{};
    std::array<qreplay::ReplayPaletteV2, 4> playbackPalettesV3{};
    std::vector<qreplay::ConfirmedGameFrameV2> playbackFramesV3;
    std::vector<qreplay::ConfirmedGameEventV2> playbackEventsV3;
    bool playbackResultSentV3 = false;
    bool playbackByeSentV3 = false;
    bool playbackNextGamePendingV3 = false;
    uint64_t playbackLastSwitchIdV3 = 0u;
    qreplay::ipc::v3::PlaybackNextGamePayload playbackNextGameV3{};

    uint32_t brokerAttempts = 0u;
    uint64_t brokerFirstFailureMs = 0u;
    bool brokerExhausted = false;

    uint32_t recorderAttempts = 0u;
    uint64_t recorderFirstFailureMs = 0u;
    uint64_t recorderLastAttemptMs = 0u;

    std::vector<qreplay::ReplayFrameV1> recFrames;
    std::vector<qreplay::ReplayEvent> recEvents;
    bool recorderTerminalSent = false;
    bool recorderStopped = false;

    std::vector<uint8_t> recorderRxV3;
    uint32_t recorderInboundSequenceV3 = 0u;
    uint32_t recorderOutboundSequenceV3 = 0u;
    uint32_t publisherSequenceV3 = 1u;
    uint32_t gamesBegunV3 = 0u;
    uint32_t gamesEndedV3 = 0u;
    qreplay::GameIdentityV2 currentGameV3{};
    bool waitingAcceptedV3 = false;
    bool waitingCommittedV3 = false;
    bool drainSentV3 = false;
    bool byeSentV3 = false;
    bool haveLastInboundV3 = false;
    qreplay::ipc::v3::Frame lastInboundV3{};

    std::atomic<uint64_t> framesReceived{0u};
    std::atomic<uint64_t> eventsReceived{0u};
    std::atomic<uint64_t> messagesRejected{0u};
    std::atomic<uint64_t> bytesReceived{0u};
    std::atomic<uint64_t> recorderMessagesSent{0u};
    std::atomic<uint64_t> recorderRecordsSentStat{0u};
    std::atomic<uint32_t> reconnects{0u};
    std::atomic<uint16_t> fault{0u};
    std::atomic<uint8_t> brokerConnected{0u};
    std::atomic<uint8_t> recorderConnected{0u};
};

ReplayIpcWorker::ReplayIpcWorker() : impl_(new Impl()) {}

ReplayIpcWorker::~ReplayIpcWorker() { stop(); }

bool ReplayIpcWorker::start(const ReplayIpcOptions& options,
                            std::unique_ptr<ByteChannel> brokerChannel,
                            std::unique_ptr<ByteChannel> recorderChannel,
                            qreplay::ReplayRecorderQueue* recorderQueue,
                            qreplay::ReplayPublisherQueueV2* recorderQueueV2) {
    if (!startForTest(options, std::move(brokerChannel),
                      std::move(recorderChannel), recorderQueue,
                      recorderQueueV2))
        return false;
    impl_->threaded = true;
    impl_->threadHandle =
        CreateThread(nullptr, 0, &ReplayIpcWorker::threadEntry, this, 0,
                     nullptr);
    if (!impl_->threadHandle) {
        impl_->threaded = false;
        running_.store(false, std::memory_order_release);
        QLOG("[replay.play] replay IPC worker thread creation failed "
             "(error=%lu)", GetLastError());
        return false;
    }
    return true;
}

unsigned long __stdcall ReplayIpcWorker::threadEntry(void* self) {
    ReplayIpcWorker* worker = static_cast<ReplayIpcWorker*>(self);
    while (!worker->stopRequested_.load(std::memory_order_acquire)) {
        if (!worker->pumpOnceForTest()) break;
        Sleep(worker->impl_->options.idleSleepMs);
    }
    worker->running_.store(false, std::memory_order_release);
    return 0;
}

bool ReplayIpcWorker::startForTest(const ReplayIpcOptions& options,
                                   std::unique_ptr<ByteChannel> brokerChannel,
                                   std::unique_ptr<ByteChannel> recorderChannel,
                                   qreplay::ReplayRecorderQueue* recorderQueue,
                                   qreplay::ReplayPublisherQueueV2*
                                       recorderQueueV2) {
    if (running_.load(std::memory_order_acquire)) return false;
    if (options.playbackV3) {
        if (!options.playback
            || options.schemaVersion
                != qreplay::ipc::v3::kWireVersion
            || options.featureBits
                != qreplay::ipc::v3::kPlaybackRequiredFeaturesV3
            || !qreplay::isValidGameIdentityV2(options.playbackGame)
            || options.playbackExpectedFrameCount == 0u
            || options.playbackExpectedFrameCount
                > qreplay::kMaxPlaybackPrebufferFramesV2
            || options.playbackExpectedEventCount
                > options.playbackExpectedFrameCount
                    * static_cast<uint32_t>(
                        2u * qreplay::kMaxEventsPerTickV2)
            || options.playbackValidatedSourceByteCount == 0u
            || qreplay::ipc::v3::isZeroDigest(
                options.playbackValidatedSourceRangeDigest)
            || (options.playbackEndpointKind
                    != static_cast<uint8_t>(
                        qreplay::ipc::v3::PlaybackEndpointKind::
                            RecordedGameEnd)
                && options.playbackEndpointKind
                    != static_cast<uint8_t>(
                        qreplay::ipc::v3::PlaybackEndpointKind::
                            UnsealedValidatedPrefix))
            || !qreplay::isValidSimulationContractV2(
                options.playbackRuntimeContract)
            || options.runtimeCompatibilityId
                != options.playbackRuntimeContract.runtimeSimulationId) {
            return false;
        }
    } else if (options.schemaVersion != kReplayIpcSchemaVersion) {
        return false;
    }
    if (options.playback
        && (options.replayGeneration == 0u || options.brokerNonce == 0u))
        return false;
    if (options.recording && options.recordGeneration == 0u) return false;
    if (options.recordingV3
        && (!options.recording || recorderQueueV2 == nullptr
            || options.recorderNonce == 0u
            || options.runtimeCompatibilityId == 0u
            || !recorderQueueV2->valid())) {
        return false;
    }
    if (options.recording && !options.recordingV3
        && recorderQueue == nullptr) return false;

    frames_.reset();
    events_.reset();
    control_.reset();
    seeks_.reset();
    commitAcks_.reset();
    playbackCommandsV3_.reset();
    playbackNextGamesV3_.reset();
    playbackOutboundV3_.reset();
    delete playbackPrebufferV3_.exchange(
        nullptr, std::memory_order_acq_rel);
    playbackFaultV3_.store(
        static_cast<uint16_t>(SourceFault::None),
        std::memory_order_release);
    stopRequested_.store(false, std::memory_order_release);
    recordingDisabled_.store(false, std::memory_order_release);
    recordingDisableReason_.store(0u, std::memory_order_release);
    recorderDrops_.store(0u, std::memory_order_release);
    recorderDrainRequested_.store(false, std::memory_order_release);
    recorderDrainAbandonedOpenGame_.store(false, std::memory_order_release);
    recorderDrainAcked_.store(false, std::memory_order_release);

    impl_.reset(new Impl());
    impl_->options = options;
    impl_->broker = std::move(brokerChannel);
    impl_->recorder = std::move(recorderChannel);
    impl_->queue = recorderQueue;
    impl_->queueV2 = recorderQueueV2;
    impl_->rx.reserve(kIpcReceiveBufferBytes * 2u);
    impl_->scratch.reserve(kMaxIpcPayloadBytes / 4u);
    impl_->recScratch.reserve(64u * 1024u);
    impl_->hdrScratch.reserve(kIpcFrameHeaderBytes);
    impl_->pendingFrames.reserve(qreplay::kMaxRecordsPerInputBlock);
    impl_->pendingEvents.reserve(qreplay::kMaxEventsPerBlock);
    impl_->recFrames.reserve(options.recorderDrainPerTurn + 1u);
    impl_->recEvents.reserve(options.recorderDrainPerTurn + 1u);
    impl_->recorderRxV3.reserve(kIpcReceiveBufferBytes * 2u);
    impl_->playbackRxV3.reserve(
        qreplay::ipc::v3::kHeaderBytes
        + qreplay::ipc::v3::kMaxPayloadBytes);
    impl_->playbackTxV3.reserve(
        qreplay::ipc::v3::kHeaderBytes
        + qreplay::ipc::v3::kPlaybackResultPayloadBytes);
    if (options.playbackV3) {
        try {
            impl_->playbackFramesV3.reserve(
                options.playbackExpectedFrameCount);
            impl_->playbackEventsV3.reserve(
                options.playbackExpectedEventCount);
        } catch (const std::bad_alloc&) {
            impl_.reset(new Impl());
            return false;
        }
    }

    running_.store(true, std::memory_order_release);
    return true;
}

void ReplayIpcWorker::stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (impl_ && impl_->threaded && impl_->threadHandle) {
        WaitForSingleObject(static_cast<HANDLE>(impl_->threadHandle), INFINITE);
        CloseHandle(static_cast<HANDLE>(impl_->threadHandle));
        impl_->threadHandle = nullptr;
    }
    running_.store(false, std::memory_order_release);
    if (impl_) {
        if (impl_->broker) impl_->broker->close();
        if (impl_->recorder) impl_->recorder->close();
        impl_->threaded = false;
    }
    delete playbackPrebufferV3_.exchange(
        nullptr, std::memory_order_acq_rel);
}

std::unique_ptr<PlaybackPrebufferV3>
ReplayIpcWorker::takePlaybackPrebufferV3() {
    return std::unique_ptr<PlaybackPrebufferV3>(
        playbackPrebufferV3_.exchange(
            nullptr, std::memory_order_acq_rel));
}

bool ReplayIpcWorker::submitPlaybackCommandAckV3(
    const qreplay::ipc::v3::PlaybackCommandAckPayload& ack) {
    PlaybackOutboundV3 outbound{};
    outbound.type =
        qreplay::ipc::v3::MessageType::PlaybackCommandAck;
    outbound.commandAck = ack;
    return playbackOutboundV3_.push(outbound);
}

bool ReplayIpcWorker::submitPlaybackStatusV3(
    const qreplay::ipc::v3::PlaybackStatusPayload& status) {
    PlaybackOutboundV3 outbound{};
    outbound.type = qreplay::ipc::v3::MessageType::PlaybackStatus;
    outbound.status = status;
    return playbackOutboundV3_.push(outbound);
}

bool ReplayIpcWorker::submitPlaybackResultV3(
    const qreplay::ipc::v3::PlaybackResultPayload& result) {
    PlaybackOutboundV3 outbound{};
    outbound.type = qreplay::ipc::v3::MessageType::PlaybackResult;
    outbound.result = result;
    return playbackOutboundV3_.push(outbound);
}

bool ReplayIpcWorker::submitPlaybackByeV3(
    const qreplay::ipc::v3::PlaybackByePayload& bye) {
    PlaybackOutboundV3 outbound{};
    outbound.type = qreplay::ipc::v3::MessageType::PlaybackBye;
    outbound.bye = bye;
    return playbackOutboundV3_.push(outbound);
}

bool ReplayIpcWorker::requestSeek(uint32_t fromFrame, uint32_t targetFrame) {
    SeekCommand cmd{};
    cmd.fromFrame = fromFrame;
    cmd.targetFrame = targetFrame;
    return seeks_.push(cmd);
}

bool ReplayIpcWorker::submitPlaybackNextGameAckV3(
    const qreplay::ipc::v3::PlaybackNextGamePayload& next) {
    PlaybackOutboundV3 outbound{};
    outbound.type = qreplay::ipc::v3::MessageType::PlaybackNextGameAck;
    outbound.nextGame = next;
    return playbackOutboundV3_.push(outbound);
}

bool ReplayIpcWorker::requestRecorderDrain(
    const bool abandonedOpenGame) {
    if (!impl_ || !impl_->options.recordingV3) return false;
    bool expected = false;
    if (!recorderDrainRequested_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return recorderDrainAbandonedOpenGame_.load(
                   std::memory_order_acquire) == abandonedOpenGame;
    }
    recorderDrainAbandonedOpenGame_.store(
        abandonedOpenGame, std::memory_order_release);
    return true;
}

void ReplayIpcWorker::disableRecording(uint16_t reason) {
    bool expected = false;
    if (recordingDisabled_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel)) {
        recordingDisableReason_.store(reason, std::memory_order_release);
    }
}

ReplayIpcStats ReplayIpcWorker::stats() const {
    ReplayIpcStats out{};
    if (!impl_) return out;
    out.framesReceived = impl_->framesReceived.load(std::memory_order_relaxed);
    out.eventsReceived = impl_->eventsReceived.load(std::memory_order_relaxed);
    out.messagesRejected =
        impl_->messagesRejected.load(std::memory_order_relaxed);
    out.bytesReceived = impl_->bytesReceived.load(std::memory_order_relaxed);
    out.recorderMessagesSent =
        impl_->recorderMessagesSent.load(std::memory_order_relaxed);
    out.recorderRecordsSent =
        impl_->recorderRecordsSentStat.load(std::memory_order_relaxed);
    out.recorderDrops = recorderDrops_.load(std::memory_order_relaxed);
    if (impl_->queue) out.recorderDrops += impl_->queue->droppedCount();
    out.reconnects = impl_->reconnects.load(std::memory_order_relaxed);
    out.frameRingHighWater = frames_.highWater();
    out.controlRingDrops = control_.drops();
    out.brokerConnected = impl_->brokerConnected.load(std::memory_order_relaxed);
    out.recorderConnected =
        impl_->recorderConnected.load(std::memory_order_relaxed);
    out.recordingDisabled =
        recordingDisabled_.load(std::memory_order_relaxed) ? 1u : 0u;
    out.recordingDisableReason =
        recordingDisableReason_.load(std::memory_order_relaxed);
    out.fault = impl_->fault.load(std::memory_order_relaxed);
    return out;
}

namespace {

bool publishControl(BoundedSpscRing<BrokerControl, kControlCapacity>& ring,
                    const BrokerControl& control) {
    return ring.push(control);
}

}

bool ReplayIpcWorker::pumpOnceForTest() {
    Impl& s = *impl_;
    if (stopRequested_.load(std::memory_order_acquire)) return false;

    bool progressed = false;

    if (s.options.playback && !s.faulted && s.broker) {
        if (!s.broker->connected() && !s.brokerExhausted) {
            const uint64_t now = nowMs();
            if (s.brokerFirstFailureMs == 0u) s.brokerFirstFailureMs = now;
            if (s.broker->connect(s.options.connectTimeoutMs)) {
                s.brokerAttempts = 0u;
                s.brokerFirstFailureMs = 0u;
                s.brokerHelloSent = false;
                s.inboundSequence = 0u;
                s.playbackInboundSequenceV3 = 0u;
                s.playbackOutboundSequenceV3 = 0u;
                s.rx.clear();
                s.rxRead = 0u;
                s.playbackRxV3.clear();
                s.brokerConnected.store(1u, std::memory_order_relaxed);
                progressed = true;
            } else {
                ++s.brokerAttempts;
                const bool attemptsGone =
                    s.brokerAttempts >= s.options.maxReconnectAttempts;
                const bool deadlineGone =
                    (now - s.brokerFirstFailureMs)
                    >= s.options.reconnectDeadlineMs;
                if (attemptsGone || deadlineGone) {
                    s.brokerExhausted = true;
                    BrokerControl c{};
                    c.kind = BrokerControl::Kind::Fault;
                    c.fault = SourceFault::ReconnectDeadline;
                    c.faultDetail = s.brokerAttempts;
                    publishControl(control_, c);
                    s.fault.store(
                        static_cast<uint16_t>(SourceFault::ReconnectDeadline),
                        std::memory_order_relaxed);
                    s.playbackPhaseV3 =
                        PlaybackReceivePhaseV3::Faulted;
                    playbackFaultV3_.store(
                        static_cast<uint16_t>(
                            SourceFault::ReconnectDeadline),
                        std::memory_order_release);
                    s.faulted = true;
                    QLOG("[replay.play] broker reconnect exhausted attempts=%u "
                         "deadlineMs=%u",
                         s.brokerAttempts, s.options.reconnectDeadlineMs);
                }
            }
        }

        if (s.broker->connected()) {
            if (!s.brokerHelloSent) {
                const bool helloSent = s.options.playbackV3
                    ? sendPlaybackHelloV3()
                    : sendBrokerHello();
                if (helloSent) {
                    s.brokerHelloSent = true;
                    progressed = true;
                } else {
                    if (s.options.playbackV3)
                        failPlaybackV3(
                            SourceFault::ChannelLost, 0u);
                    else
                        dropBroker(SourceFault::ChannelLost, 0u);
                }
            }
            if (s.broker->connected()) {
                if (s.options.playbackV3) {
                    if (pumpPlaybackReceiveV3()) progressed = true;
                    if (s.broker->connected()
                        && flushPlaybackOutboundV3()) {
                        progressed = true;
                    }
                } else {
                    if (drainSeekCommands()) progressed = true;
                    if (pumpBrokerReceive()) progressed = true;
                }
            }
        }
    }

    if (s.options.recording && !s.recorderStopped) {
        if (pumpRecorder()) progressed = true;
    }

    return true;
}

bool ReplayIpcWorker::sendPlaybackFrameV3(
    const qreplay::ipc::v3::MessageType type,
    const uint32_t param0, const uint32_t param1,
    const std::vector<uint8_t>& payload) {
    namespace wire3 = qreplay::ipc::v3;
    Impl& s = *impl_;
    if (!s.broker || !s.broker->connected()) return false;
    wire3::Frame frame{};
    frame.channel = wire3::Channel::Playback;
    frame.type = type;
    frame.launchGeneration = s.options.replayGeneration;
    frame.game = s.options.playbackGame;
    frame.launchNonce = s.options.brokerNonce;
    frame.sequence = s.playbackOutboundSequenceV3 + 1u;
    frame.param0 = param0;
    frame.param1 = param1;
    frame.payload = payload;
    s.playbackTxV3.clear();
    if (!wire3::encodeFrame(frame, s.playbackTxV3)
        || !s.broker->write(
            s.playbackTxV3.data(), s.playbackTxV3.size())) {
        QLOG("[replay.play.v3] playback emit failed type=%u seq=%u wire=%u",
             static_cast<unsigned>(type),
             static_cast<unsigned>(frame.sequence),
             static_cast<unsigned>(s.playbackTxV3.size()));
        return false;
    }
    s.playbackOutboundSequenceV3 = frame.sequence;
    return true;
}

bool ReplayIpcWorker::sendPlaybackHelloV3() {
    namespace wire3 = qreplay::ipc::v3;
    const Impl& s = *impl_;
    wire3::PlaybackHelloPayload hello{};
    hello.gamePid = s.options.gamePid;
    hello.featureBits = wire3::kPlaybackRequiredFeaturesV3;
    hello.headless = s.options.playbackHeadless;
    hello.runtimeCompatibilityId =
        s.options.runtimeCompatibilityId;
    hello.stateHashAlgorithmId =
        s.options.playbackRuntimeContract.stateHashAlgorithmId;
    hello.bootstrapSchemaId =
        s.options.playbackRuntimeContract.bootstrapSchemaId;
    std::vector<uint8_t> payload;
    if (!wire3::encodePlaybackHello(hello, payload)) return false;
    return sendPlaybackFrameV3(
        wire3::MessageType::PlaybackHello, 0u, 0u, payload);
}

void ReplayIpcWorker::failPlaybackV3(
    const SourceFault fault, const uint32_t detail) {
    Impl& s = *impl_;
    if (s.playbackPhaseV3 == PlaybackReceivePhaseV3::Faulted)
        return;
    s.playbackPhaseV3 = PlaybackReceivePhaseV3::Faulted;
    s.faulted = true;
    s.brokerExhausted = true;
    s.messagesRejected.fetch_add(1u, std::memory_order_relaxed);
    s.fault.store(
        static_cast<uint16_t>(fault), std::memory_order_relaxed);
    playbackFaultV3_.store(
        static_cast<uint16_t>(fault), std::memory_order_release);
    s.brokerConnected.store(0u, std::memory_order_relaxed);
    if (s.broker) s.broker->close();
    QLOG(
        "[replay.play.v3] broker fault=%s detail=%u phase=%u "
        "frames=%u/%u events=%u/%u",
        sourceFaultText(fault), detail,
        static_cast<unsigned>(s.playbackPhaseV3),
        static_cast<unsigned>(s.playbackFramesV3.size()),
        s.options.playbackExpectedFrameCount,
        static_cast<unsigned>(s.playbackEventsV3.size()),
        s.options.playbackExpectedEventCount);
}

bool ReplayIpcWorker::publishPlaybackPrebufferV3(
    const qreplay::ipc::v3::PlaybackEndpointPayload& endpoint) {
    namespace wire3 = qreplay::ipc::v3;
    Impl& s = *impl_;

    std::vector<uint8_t> expectedEndpoint;
    std::vector<uint8_t> observedEndpoint;
    if (!wire3::encodePlaybackEndpoint(
            s.playbackInfoV3.endpoint, expectedEndpoint)
        || !wire3::encodePlaybackEndpoint(
            endpoint, observedEndpoint)
        || expectedEndpoint != observedEndpoint
        || endpoint.nextRecord
            != s.options.playbackExpectedFrameCount
        || endpoint.validatedSourceByteCount
            != s.options.playbackValidatedSourceByteCount
        || endpoint.validatedSourceRangeDigest
            != s.options.playbackValidatedSourceRangeDigest
        || static_cast<uint8_t>(endpoint.kind)
            != s.options.playbackEndpointKind
        || s.playbackFramesV3.size()
            != s.options.playbackExpectedFrameCount
        || s.playbackEventsV3.size()
            != s.options.playbackExpectedEventCount) {
        failPlaybackV3(SourceFault::ManifestMismatch, endpoint.nextRecord);
        return false;
    }

    std::unique_ptr<qreplay::ConfirmedTimelineBufferV2> timeline;
    try {
        timeline.reset(new qreplay::ConfirmedTimelineBufferV2(
            s.options.playbackExpectedFrameCount));
    } catch (const std::bad_alloc&) {
        failPlaybackV3(SourceFault::PayloadTooLarge, endpoint.nextRecord);
        return false;
    }

    qreplay::ParseStatus why = qreplay::ParseStatus::Ok;
    if (!timeline->begin(s.playbackStartV3, why)) {
        failPlaybackV3(
            SourceFault::DecodeFailed, static_cast<uint32_t>(why));
        return false;
    }

    size_t eventIndex = 0u;
    for (uint32_t frameIndex = 0u;
         frameIndex < s.options.playbackExpectedFrameCount;
         ++frameIndex) {
        qreplay::CanonicalTickBundleV2 tick{};
        tick.frame = s.playbackFramesV3[frameIndex];
        if (tick.frame.replayFrame != frameIndex) {
            failPlaybackV3(
                SourceFault::FrameGap, tick.frame.replayFrame);
            return false;
        }

        bool sawPost = false;
        while (eventIndex < s.playbackEventsV3.size()
               && s.playbackEventsV3[eventIndex].replayFrame
                    == frameIndex) {
            const qreplay::ConfirmedGameEventV2& event =
                s.playbackEventsV3[eventIndex++];
            if (event.phase == qreplay::GameEventPhaseV2::PreFrame) {
                if (sawPost
                    || tick.preEvents.count
                        >= qreplay::kMaxEventsPerTickV2) {
                    failPlaybackV3(
                        SourceFault::DecodeFailed, event.eventSequence);
                    return false;
                }
                tick.preEvents.events[tick.preEvents.count++] = event;
            } else if (
                event.phase == qreplay::GameEventPhaseV2::PostFrame) {
                sawPost = true;
                if (tick.postEvents.count
                    >= qreplay::kMaxEventsPerTickV2) {
                    failPlaybackV3(
                        SourceFault::DecodeFailed, event.eventSequence);
                    return false;
                }
                tick.postEvents.events[tick.postEvents.count++] = event;
            } else {
                failPlaybackV3(
                    SourceFault::DecodeFailed, event.eventSequence);
                return false;
            }
        }
        if (eventIndex < s.playbackEventsV3.size()
            && s.playbackEventsV3[eventIndex].replayFrame < frameIndex) {
            failPlaybackV3(
                SourceFault::FrameGap,
                s.playbackEventsV3[eventIndex].replayFrame);
            return false;
        }
        if (!timeline->append(tick, why)) {
            failPlaybackV3(
                why == qreplay::ParseStatus::FrameGap
                    ? SourceFault::FrameGap
                    : SourceFault::DigestBreak,
                frameIndex);
            return false;
        }
    }
    if (eventIndex != s.playbackEventsV3.size()) {
        failPlaybackV3(
            SourceFault::FrameGap,
            s.playbackEventsV3[eventIndex].replayFrame);
        return false;
    }
    if (endpoint.kind
        == wire3::PlaybackEndpointKind::RecordedGameEnd) {
        if (!timeline->seal(endpoint.recordedGameEnd, why)) {
            failPlaybackV3(
                SourceFault::DigestBreak,
                static_cast<uint32_t>(why));
            return false;
        }
    }

    std::unique_ptr<PlaybackPrebufferV3> prebuffer(
        new (std::nothrow) PlaybackPrebufferV3());
    if (!prebuffer) {
        failPlaybackV3(SourceFault::PayloadTooLarge, endpoint.nextRecord);
        return false;
    }
    prebuffer->game = s.options.playbackGame;
    prebuffer->sessionInfo = s.playbackInfoV3;
    prebuffer->start = s.playbackStartV3;
    prebuffer->palettes = s.playbackPalettesV3;
    prebuffer->binding.endpoint = endpoint;
    prebuffer->binding.runtimeContract =
        s.options.playbackRuntimeContract;
    prebuffer->binding.headless = s.options.playbackHeadless;
    prebuffer->binding.hasFileDigest =
        s.playbackInfoV3.hasFileDigest;
    prebuffer->binding.fileDigest = s.playbackInfoV3.fileDigest;
    prebuffer->binding.supersededStateSchema =
        s.options.supersededStateSchema;
    prebuffer->timeline = static_cast<
        std::unique_ptr<qreplay::ConfirmedTimelineBufferV2>&&>(timeline);

    std::vector<qreplay::ConfirmedGameFrameV2>().swap(
        s.playbackFramesV3);
    std::vector<qreplay::ConfirmedGameEventV2>().swap(
        s.playbackEventsV3);

    PlaybackPrebufferV3* expected = nullptr;
    PlaybackPrebufferV3* released = prebuffer.release();
    if (!playbackPrebufferV3_.compare_exchange_strong(
            expected, released, std::memory_order_release,
            std::memory_order_relaxed)) {
        delete released;
        failPlaybackV3(SourceFault::UnexpectedMessage, endpoint.nextRecord);
        return false;
    }
    s.playbackPhaseV3 = PlaybackReceivePhaseV3::Ready;
    s.framesReceived.store(
        s.options.playbackExpectedFrameCount,
        std::memory_order_relaxed);
    s.eventsReceived.store(
        s.options.playbackExpectedEventCount,
        std::memory_order_relaxed);
    QLOG(
        "[replay.play.v3] full prebuffer ready game=%08X:%016llX%016llX "
        "frames=%u events=%u endpoint=%u headless=%u",
        s.options.playbackGame.gameGeneration,
        static_cast<unsigned long long>(
            s.options.playbackGame.gameUuidHigh),
        static_cast<unsigned long long>(
            s.options.playbackGame.gameUuidLow),
        s.options.playbackExpectedFrameCount,
        s.options.playbackExpectedEventCount,
        s.options.playbackEndpointKind,
        s.options.playbackHeadless ? 1u : 0u);
    return true;
}

bool ReplayIpcWorker::handlePlaybackMessageV3(
    const qreplay::ipc::v3::Frame& frame) {
    namespace wire3 = qreplay::ipc::v3;
    Impl& s = *impl_;
    if (s.playbackNextGamePendingV3 && frame.type != wire3::MessageType::PlaybackPalette) {
        failPlaybackV3(SourceFault::UnexpectedMessage, frame.sequence);
        return false;
    }
    switch (frame.type) {
        case wire3::MessageType::PlaybackNextGame: {
            if (s.playbackPhaseV3 != PlaybackReceivePhaseV3::Ready
                || !s.playbackResultSentV3 || s.playbackByeSentV3
                || frame.param0 != 0u || frame.param1 != 0u) {
                failPlaybackV3(SourceFault::UnexpectedMessage, frame.sequence);
                return false;
            }
            wire3::PlaybackNextGamePayload next{};
            if (!wire3::decodePlaybackNextGame(
                    frame.payload.data(), frame.payload.size(), next)) {
                failPlaybackV3(SourceFault::DecodeFailed, frame.sequence);
                return false;
            }
            if (next.switchId <= s.playbackLastSwitchIdV3) {
                failPlaybackV3(SourceFault::SequenceViolation, frame.sequence);
                return false;
            }
            auto localContract = s.options.playbackRuntimeContract;
            localContract.contentManifestRoot = next.runtimeContract.contentManifestRoot;
            if (next.runtimeContract != localContract) {
                failPlaybackV3(SourceFault::ManifestMismatch, frame.sequence);
                return false;
            }
            s.playbackNextGameV3 = next;
            s.playbackNextGamePendingV3 = true;
            if (!playbackNextGamesV3_.push(next)) {
                failPlaybackV3(SourceFault::UnexpectedMessage, frame.sequence);
                return false;
            }
            return true;
        }
        case wire3::MessageType::PlaybackSessionInfo: {
            if (s.playbackPhaseV3
                    != PlaybackReceivePhaseV3::WaitingForSessionInfo
                || frame.param0 != 0u || frame.param1 != 0u) {
                failPlaybackV3(
                    SourceFault::UnexpectedMessage,
                    static_cast<uint32_t>(frame.type));
                return false;
            }
            wire3::PlaybackSessionInfoPayload info{};
            if (!wire3::decodePlaybackSessionInfo(
                    frame.payload.data(), frame.payload.size(), info)
                || info.frameCount
                    != s.options.playbackExpectedFrameCount
                || info.eventCount
                    != s.options.playbackExpectedEventCount
                || info.validatedSourceByteCount
                    != s.options.playbackValidatedSourceByteCount
                || info.validatedSourceRangeDigest
                    != s.options.playbackValidatedSourceRangeDigest
                || static_cast<uint8_t>(info.endpoint.kind)
                    != s.options.playbackEndpointKind
                || info.endpoint.nextRecord != info.frameCount
                || !qreplay::replayContractAgreesForPlaybackV2(
                       info.recordedContract,
                       s.options.playbackRuntimeContract)) {
                QLOG("[replay.play.v3] session-info mismatch "
                     "frames=%u/%u events=%u/%u bytes=%llu/%llu "
                     "endpointKind=%u/%u contract=%s",
                     info.frameCount, s.options.playbackExpectedFrameCount,
                     info.eventCount, s.options.playbackExpectedEventCount,
                     static_cast<unsigned long long>(
                         info.validatedSourceByteCount),
                     static_cast<unsigned long long>(
                         s.options.playbackValidatedSourceByteCount),
                     static_cast<unsigned>(info.endpoint.kind),
                     static_cast<unsigned>(s.options.playbackEndpointKind),
                     qreplay::replayContractAgreesForPlaybackV2(
                         info.recordedContract,
                         s.options.playbackRuntimeContract)
                         ? "equal"
                         : (info.recordedContract.contentManifestRoot
                                    != s.options.playbackRuntimeContract
                                           .contentManifestRoot
                                ? "content-manifest-root-differs"
                                : "non-root-field-differs"));
                failPlaybackV3(
                    SourceFault::ManifestMismatch, frame.sequence);
                return false;
            }
            s.playbackInfoV3 = info;
            s.playbackPhaseV3 =
                PlaybackReceivePhaseV3::WaitingForGameStart;
            return true;
        }
        case wire3::MessageType::PlaybackGameStart: {
            if (s.playbackPhaseV3
                    != PlaybackReceivePhaseV3::WaitingForGameStart
                || frame.param0 != 0u || frame.param1 != 0u) {
                failPlaybackV3(
                    SourceFault::UnexpectedMessage,
                    static_cast<uint32_t>(frame.type));
                return false;
            }
            qreplay::ConfirmedGameStartV2 start{};
            if (wire3::decodePlaybackGameStart(
                    frame.payload.data(), frame.payload.size(), start)
                    != qreplay::ParseStatus::Ok
                || start.manifest.identity
                    != s.options.playbackGame
                || !qreplay::replayContractAgreesForPlaybackV2(
                       start.manifest.contract,
                       s.options.playbackRuntimeContract)
                || start.manifest.contract
                    != s.playbackInfoV3.recordedContract
                || start.manifest.activeLaneMask
                    != s.playbackInfoV3.activeLaneMask
                || start.manifest.activeFighterMask
                    != s.playbackInfoV3.activeFighterMask) {
                failPlaybackV3(
                    SourceFault::ManifestMismatch, frame.sequence);
                return false;
            }
            s.playbackStartV3 = start;
            s.playbackPhaseV3 =
                PlaybackReceivePhaseV3::ReceivingFrames;
            return true;
        }
        case wire3::MessageType::PlaybackPalette: {
            if (s.playbackPhaseV3 != PlaybackReceivePhaseV3::ReceivingFrames
                && s.playbackPhaseV3 != PlaybackReceivePhaseV3::ReceivingEventsOrEndpoint)
                return true;
            qreplay::ReplayPaletteV2 palette{};
            if (wire3::decodePlaybackPalette(
                    frame.payload.data(), frame.payload.size(), palette)
                    == qreplay::ParseStatus::Ok && palette.present != 0u) {
                s.playbackPalettesV3[palette.side] = palette;
            }
            return true;
        }
        case wire3::MessageType::PlaybackFrames: {
            if (s.playbackPhaseV3
                    != PlaybackReceivePhaseV3::ReceivingFrames
                || frame.param1 == 0u
                || frame.param0 != s.playbackFramesV3.size()
                || frame.param1
                    > s.options.playbackExpectedFrameCount
                        - static_cast<uint32_t>(
                            s.playbackFramesV3.size())) {
                failPlaybackV3(SourceFault::FrameGap, frame.param0);
                return false;
            }
            std::vector<qreplay::ConfirmedGameFrameV2> decoded;
            const qreplay::ParseStatus status =
                wire3::decodePlaybackFrames(
                    frame.payload.data(), frame.payload.size(),
                    frame.param0, frame.param1,
                    s.playbackInfoV3.activeLaneMask, decoded);
            if (status != qreplay::ParseStatus::Ok
                || decoded.size() != frame.param1) {
                failPlaybackV3(
                    status == qreplay::ParseStatus::BadActionMask
                        ? SourceFault::InvalidActionMask
                    : status == qreplay::ParseStatus::BadLaneShape
                        ? SourceFault::InvalidLaneShape
                        : SourceFault::DecodeFailed,
                    frame.param0);
                return false;
            }
            s.playbackFramesV3.insert(
                s.playbackFramesV3.end(),
                decoded.begin(), decoded.end());
            if (s.playbackFramesV3.size()
                == s.options.playbackExpectedFrameCount) {
                s.playbackPhaseV3 =
                    PlaybackReceivePhaseV3::ReceivingEventsOrEndpoint;
            }
            return true;
        }
        case wire3::MessageType::PlaybackEvents: {
            if (s.playbackPhaseV3
                    != PlaybackReceivePhaseV3::
                        ReceivingEventsOrEndpoint
                || frame.param1 == 0u
                || frame.param0 != s.playbackEventsV3.size()
                || frame.param1
                    > s.options.playbackExpectedEventCount
                        - static_cast<uint32_t>(
                            s.playbackEventsV3.size())) {
                failPlaybackV3(SourceFault::FrameGap, frame.param0);
                return false;
            }
            std::vector<qreplay::ConfirmedGameEventV2> decoded;
            const qreplay::ParseStatus status =
                wire3::decodePlaybackEvents(
                    frame.payload.data(), frame.payload.size(),
                    frame.param0, frame.param1, decoded);
            if (status != qreplay::ParseStatus::Ok
                || decoded.size() != frame.param1) {
                failPlaybackV3(
                    SourceFault::DecodeFailed, frame.param0);
                return false;
            }
            s.playbackEventsV3.insert(
                s.playbackEventsV3.end(),
                decoded.begin(), decoded.end());
            return true;
        }
        case wire3::MessageType::PlaybackEndpoint: {
            if (s.playbackPhaseV3
                    != PlaybackReceivePhaseV3::
                        ReceivingEventsOrEndpoint
                || s.playbackFramesV3.size()
                    != s.options.playbackExpectedFrameCount
                || s.playbackEventsV3.size()
                    != s.options.playbackExpectedEventCount
                || frame.param0
                    != s.options.playbackExpectedFrameCount
                || frame.param1
                    != s.options.playbackExpectedEventCount) {
                failPlaybackV3(
                    SourceFault::UnexpectedMessage, frame.param0);
                return false;
            }
            wire3::PlaybackEndpointPayload endpoint{};
            if (!wire3::decodePlaybackEndpoint(
                    frame.payload.data(), frame.payload.size(), endpoint)) {
                failPlaybackV3(
                    SourceFault::DecodeFailed, frame.sequence);
                return false;
            }
            return publishPlaybackPrebufferV3(endpoint);
        }
        case wire3::MessageType::PlaybackCommand: {
            if (s.playbackPhaseV3
                    != PlaybackReceivePhaseV3::Ready
                || frame.param0 != 0u || frame.param1 != 0u) {
                failPlaybackV3(
                    SourceFault::UnexpectedMessage,
                    static_cast<uint32_t>(frame.type));
                return false;
            }
            wire3::PlaybackCommandPayload command{};
            if (!wire3::decodePlaybackCommand(
                    frame.payload.data(), frame.payload.size(), command)
                || command.commandId <= s.playbackLastCommandIdV3
                || !playbackCommandsV3_.push(command)) {
                failPlaybackV3(
                    SourceFault::SequenceViolation, frame.sequence);
                return false;
            }
            s.playbackLastCommandIdV3 = command.commandId;
            return true;
        }
        default:
            failPlaybackV3(
                SourceFault::UnexpectedMessage,
                static_cast<uint32_t>(frame.type));
            return false;
    }
}

bool ReplayIpcWorker::pumpPlaybackReceiveV3() {
    namespace wire3 = qreplay::ipc::v3;
    Impl& s = *impl_;
    if (!s.broker || !s.broker->connected()) return false;
    if (s.playbackPhaseV3 == PlaybackReceivePhaseV3::Ready
        && playbackCommandsV3_.full()) {
        return false;
    }

    std::array<uint8_t, 64u * 1024u> incoming{};
    size_t got = 0u;
    if (!s.broker->read(incoming.data(), incoming.size(), got)) {
        if (s.playbackByeSentV3) {
            s.broker->close();
            s.brokerExhausted = true;
            s.brokerConnected.store(0u, std::memory_order_relaxed);
        } else {
            failPlaybackV3(SourceFault::ChannelLost, 0u);
        }
        return false;
    }
    if (got > 0u) {
        if (s.playbackRxV3.size() + got
            > static_cast<size_t>(wire3::kMaxPayloadBytes)
                + wire3::kHeaderBytes) {
            failPlaybackV3(SourceFault::PayloadTooLarge, 0u);
            return false;
        }
        s.playbackRxV3.insert(
            s.playbackRxV3.end(),
            incoming.begin(), incoming.begin() + got);
        s.bytesReceived.fetch_add(got, std::memory_order_relaxed);
    }
    bool progressed = got != 0u;
    for (;;) {
        if (s.playbackPhaseV3 == PlaybackReceivePhaseV3::Ready
            && playbackCommandsV3_.full()) {
            break;
        }
        wire3::Frame frame{};
        size_t consumed = 0u;
        const wire3::FrameStatus decoded = wire3::decodeFrame(
            s.playbackRxV3.data(), s.playbackRxV3.size(),
            frame, consumed);
        if (decoded == wire3::FrameStatus::NeedMore) break;
        if (decoded != wire3::FrameStatus::Ok) {
            failPlaybackV3(
                decoded == wire3::FrameStatus::TooLarge
                    ? SourceFault::PayloadTooLarge
                    : SourceFault::BadFraming,
                static_cast<uint32_t>(decoded));
            return progressed;
        }
        if (frame.channel != wire3::Channel::Playback
            || frame.launchGeneration != s.options.replayGeneration
            || frame.launchNonce != s.options.brokerNonce
            || frame.game != s.options.playbackGame) {
            failPlaybackV3(
                frame.launchGeneration != s.options.replayGeneration
                    ? SourceFault::StaleGeneration
                : frame.launchNonce != s.options.brokerNonce
                    ? SourceFault::WrongNonce
                    : SourceFault::ManifestMismatch,
                frame.sequence);
            return progressed;
        }
        if (frame.sequence != s.playbackInboundSequenceV3 + 1u) {
            failPlaybackV3(
                SourceFault::SequenceViolation, frame.sequence);
            return progressed;
        }
        s.playbackRxV3.erase(
            s.playbackRxV3.begin(),
            s.playbackRxV3.begin()
                + static_cast<ptrdiff_t>(consumed));
        s.playbackInboundSequenceV3 = frame.sequence;
        progressed = true;
        if (!handlePlaybackMessageV3(frame)) return progressed;
    }
    return progressed;
}

bool ReplayIpcWorker::flushPlaybackOutboundV3() {
    namespace wire3 = qreplay::ipc::v3;
    Impl& s = *impl_;
    bool progressed = false;
    PlaybackOutboundV3 outbound{};
    while (playbackOutboundV3_.pop(outbound)) {
        std::vector<uint8_t> payload;
        bool encoded = false;
        switch (outbound.type) {
            case wire3::MessageType::PlaybackCommandAck:
                encoded = wire3::encodePlaybackCommandAck(
                    outbound.commandAck, payload);
                break;
            case wire3::MessageType::PlaybackStatus:
                encoded = wire3::encodePlaybackStatus(
                    outbound.status, payload);
                break;
            case wire3::MessageType::PlaybackResult:
                if (s.playbackResultSentV3) encoded = false;
                else {
                    encoded = wire3::encodePlaybackResult(
                        outbound.result, payload);
                }
                break;
            case wire3::MessageType::PlaybackBye:
                if (!s.playbackResultSentV3
                    || s.playbackByeSentV3 || s.playbackNextGamePendingV3) {
                    encoded = false;
                } else {
                    encoded = wire3::encodePlaybackBye(
                        outbound.bye, payload);
                }
                break;
            case wire3::MessageType::PlaybackNextGameAck: {
                std::vector<uint8_t> expected;
                encoded = s.playbackNextGamePendingV3
                    && s.playbackResultSentV3 && !s.playbackByeSentV3
                    && !playbackPrebufferReadyV3()
                    && playbackCommandsV3_.empty()
                    && playbackNextGamesV3_.empty()
                    && wire3::encodePlaybackNextGame(s.playbackNextGameV3, expected)
                    && wire3::encodePlaybackNextGame(outbound.nextGame, payload)
                    && expected == payload;
                if (encoded) {
                    try {
                        s.playbackFramesV3.reserve(outbound.nextGame.frameCount);
                        s.playbackEventsV3.reserve(outbound.nextGame.eventCount);
                    } catch (const std::bad_alloc&) {
                        failPlaybackV3(SourceFault::PayloadTooLarge, 0u);
                        return progressed;
                    }
                }
                break;
            }
            default:
                encoded = false;
                break;
        }
        if (!encoded
            || !sendPlaybackFrameV3(
                outbound.type, 0u, 0u, payload)) {
            failPlaybackV3(SourceFault::ChannelLost, 0u);
            return progressed;
        }
        if (outbound.type
            == wire3::MessageType::PlaybackResult) {
            s.playbackResultSentV3 = true;
        } else if (
            outbound.type == wire3::MessageType::PlaybackBye) {
            s.playbackByeSentV3 = true;
        } else if (outbound.type == wire3::MessageType::PlaybackNextGameAck) {
            const auto& next = outbound.nextGame;
            s.options.playbackGame = next.game;
            s.options.playbackExpectedFrameCount = next.frameCount;
            s.options.playbackExpectedEventCount = next.eventCount;
            s.options.playbackValidatedSourceByteCount = next.validatedSourceByteCount;
            s.options.playbackValidatedSourceRangeDigest = next.validatedSourceRangeDigest;
            s.options.playbackEndpointKind = static_cast<uint8_t>(next.endpointKind);
            s.options.playbackRuntimeContract.contentManifestRoot =
                next.runtimeContract.contentManifestRoot;
            s.options.supersededStateSchema = next.supersededStateSchema;
            s.playbackLastSwitchIdV3 = next.switchId;
            s.playbackNextGamePendingV3 = false;
            s.playbackNextGameV3 = {};
            s.playbackResultSentV3 = s.playbackByeSentV3 = false;
            s.playbackPhaseV3 = PlaybackReceivePhaseV3::WaitingForSessionInfo;
            s.playbackInfoV3 = {};
            s.playbackStartV3 = {};
            s.playbackPalettesV3 = {};
            s.playbackFramesV3.clear();
            s.playbackEventsV3.clear();
            s.framesReceived.store(0u, std::memory_order_relaxed);
            s.eventsReceived.store(0u, std::memory_order_relaxed);
        }
        progressed = true;
    }
    return progressed;
}

void ReplayIpcWorker::buildHelloPayload(std::vector<uint8_t>& out) const {
    const Impl& s = *impl_;
    wire::HelloPayload hello{};
    hello.nonce = s.options.brokerNonce;
    hello.gamePid = s.options.gamePid;
    hello.wireVersion = s.options.schemaVersion;
    hello.featureBits = s.options.featureBits;
    hello.manifestDigest = s.options.manifestDigest;
    out.clear();
    wire::encodeHello(hello, out);
}

bool ReplayIpcWorker::sendBrokerHello() {
    Impl& s = *impl_;
    buildHelloPayload(s.scratch);
    const uint32_t resume = s.expectFrameKnown ? s.expectedFrame : 0u;
    return sendFramed(*s.broker, IpcMessageKind::Hello, s.options.matchOrdinal,
                      resume, s.scratch, s.outboundSequence);
}

bool ReplayIpcWorker::drainSeekCommands() {
    Impl& s = *impl_;
    bool any = false;
    SeekCommand cmd{};
    while (seeks_.pop(cmd)) {
        s.scratch.clear();
        if (!sendFramed(*s.broker, IpcMessageKind::SeekRequest, cmd.fromFrame,
                        cmd.targetFrame, s.scratch, s.outboundSequence)) {
            dropBroker(SourceFault::ChannelLost, 0u);
            return any;
        }
        s.expectFrameKnown = false;
        s.chainedDigestKnown = false;
        s.pendingFrames.clear();
        s.pendingFrameIndex = 0u;
        s.pendingEvents.clear();
        s.pendingEventIndex = 0u;
        any = true;
    }
    return any;
}

bool ReplayIpcWorker::sendFramed(ByteChannel& channel, IpcMessageKind kind,
                                 uint32_t param0, uint32_t param1,
                                 const std::vector<uint8_t>& payload,
                                 uint32_t& sequence) {
    Impl& s = *impl_;
    if (payload.size() > kMaxIpcPayloadBytes) return false;
    const uint8_t typeByte = static_cast<uint8_t>(kind);
    IpcFrameHeader h{};
    h.magic = kReplayIpcMagic;
    h.wireVersion = s.options.schemaVersion;
    h.channel = static_cast<uint8_t>(wire::channelForMessageType(typeByte));
    h.type = typeByte;
    const bool recorderDirection = wire::isRecorderType(typeByte);
    h.generation = recorderDirection ? s.options.recordGeneration
                                     : s.options.replayGeneration;
    h.nonce = s.options.brokerNonce;
    h.sequence = ++sequence;
    h.param0 = param0;
    h.param1 = param1;
    h.payloadBytes = static_cast<uint32_t>(payload.size());
    h.payloadCrc32c =
        payload.empty() ? 0u : qreplay::crc32c(payload.data(), payload.size());

    s.hdrScratch.clear();
    encodeIpcHeader(h, s.hdrScratch);
    if (s.hdrScratch.size() != kIpcFrameHeaderBytes) return false;

    if (!channel.write(s.hdrScratch.data(), kIpcFrameHeaderBytes)) return false;
    if (!payload.empty() && !channel.write(payload.data(), payload.size()))
        return false;
    return true;
}

void ReplayIpcWorker::dropBroker(SourceFault fault, uint32_t detail) {
    Impl& s = *impl_;
    s.messagesRejected.fetch_add(1u, std::memory_order_relaxed);
    s.brokerConnected.store(0u, std::memory_order_relaxed);
    if (s.broker) s.broker->close();

    const bool retryable = (fault == SourceFault::ChannelLost);
    if (!retryable) {
        s.faulted = true;
        s.brokerExhausted = true;
    } else {
        s.reconnects.fetch_add(1u, std::memory_order_relaxed);
    }
    s.fault.store(static_cast<uint16_t>(fault), std::memory_order_relaxed);

    if (!retryable) {
        BrokerControl c{};
        c.kind = BrokerControl::Kind::Fault;
        c.fault = fault;
        c.faultDetail = detail;
        publishControl(control_, c);
        QLOG("[replay.play] broker fault=%s detail=%u",
             sourceFaultText(fault), detail);
    }
}

bool ReplayIpcWorker::flushPending() {
    Impl& s = *impl_;
    bool moved = false;
    while (s.pendingFrameIndex < s.pendingFrames.size()) {
        if (!frames_.push(s.pendingFrames[s.pendingFrameIndex])) return moved;
        ++s.pendingFrameIndex;
        s.framesReceived.fetch_add(1u, std::memory_order_relaxed);
        moved = true;
    }
    if (s.pendingFrameIndex >= s.pendingFrames.size()) {
        s.pendingFrames.clear();
        s.pendingFrameIndex = 0u;
    }
    while (s.pendingEventIndex < s.pendingEvents.size()) {
        if (!events_.push(s.pendingEvents[s.pendingEventIndex])) return moved;
        ++s.pendingEventIndex;
        s.eventsReceived.fetch_add(1u, std::memory_order_relaxed);
        moved = true;
    }
    if (s.pendingEventIndex >= s.pendingEvents.size()) {
        s.pendingEvents.clear();
        s.pendingEventIndex = 0u;
    }
    return moved;
}

bool ReplayIpcWorker::pumpBrokerReceive() {
    Impl& s = *impl_;
    bool progressed = flushPending();

    const bool pendingWork = s.pendingFrameIndex < s.pendingFrames.size()
                          || s.pendingEventIndex < s.pendingEvents.size();
    if (pendingWork) return progressed;
    if (frames_.full() || events_.full()) return progressed;

    uint8_t buffer[8192];
    size_t got = 0;
    if (!s.broker->read(buffer, sizeof(buffer), got)) {
        dropBroker(SourceFault::ChannelLost, 0u);
        return progressed;
    }
    if (got > 0) {
        if (s.rx.size() - s.rxRead + got
            > kIpcReceiveBufferBytes + kMaxIpcPayloadBytes) {
            dropBroker(SourceFault::PayloadTooLarge, 0u);
            return progressed;
        }
        s.rx.insert(s.rx.end(), buffer, buffer + got);
        s.bytesReceived.fetch_add(got, std::memory_order_relaxed);
        progressed = true;
    }

    while (!s.faulted) {
        const size_t available = s.rx.size() - s.rxRead;
        if (available < kIpcFrameHeaderBytes) break;
        const uint8_t* head = s.rx.data() + s.rxRead;

        IpcFrameHeader h{};
        const IpcFrameStatus status =
            decodeIpcHeaderStatus(head, available, h);
        if (status != IpcFrameStatus::Ok) {
            switch (status) {
                case IpcFrameStatus::TooLarge:
                    dropBroker(SourceFault::PayloadTooLarge, 0u);
                    break;
                case IpcFrameStatus::UnknownChannel:
                case IpcFrameStatus::UnknownType:
                    dropBroker(SourceFault::UnexpectedMessage, h.type);
                    break;
                default:
                    dropBroker(SourceFault::BadFraming,
                               static_cast<uint32_t>(status));
                    break;
            }
            return progressed;
        }
        if (h.wireVersion != s.options.schemaVersion) {
            dropBroker(SourceFault::BadFraming, h.wireVersion);
            return progressed;
        }
        if (h.channel != static_cast<uint8_t>(IpcChannel::Broker)) {
            dropBroker(SourceFault::UnexpectedMessage, h.channel);
            return progressed;
        }
        size_t total = 0;
        if (!wire::frameBytes(h, total)) {
            dropBroker(SourceFault::BadFraming, 0u);
            return progressed;
        }
        if (available < total) break;

        if (h.generation != s.options.replayGeneration) {
            dropBroker(SourceFault::StaleGeneration, h.generation);
            return progressed;
        }
        if (h.nonce != s.options.brokerNonce) {
            dropBroker(SourceFault::WrongNonce, 0u);
            return progressed;
        }
        if (h.sequence != s.inboundSequence + 1u) {
            dropBroker(SourceFault::SequenceViolation, h.sequence);
            return progressed;
        }
        if (!isBrokerInboundKind(h.type)) {
            dropBroker(SourceFault::UnexpectedMessage, h.type);
            return progressed;
        }

        const uint8_t* payload = head + kIpcFrameHeaderBytes;
        const uint32_t crc = h.payloadBytes == 0u
            ? 0u
            : qreplay::crc32c(payload, h.payloadBytes);
        if (crc != h.payloadCrc32c) {
            dropBroker(SourceFault::BadPayloadCrc, h.sequence);
            return progressed;
        }

        s.inboundSequence = h.sequence;
        s.rxRead += total;
        progressed = true;

        if (!handleBrokerMessage(h, payload, h.payloadBytes)) return progressed;
        if (!flushPending()) {
            break;
        }
    }

    if (s.rxRead > 0u
        && (s.rxRead == s.rx.size() || s.rxRead >= kIpcReceiveBufferBytes)) {
        s.rx.erase(s.rx.begin(),
                   s.rx.begin() + static_cast<std::ptrdiff_t>(s.rxRead));
        s.rxRead = 0u;
    }
    return progressed;
}

bool ReplayIpcWorker::handleBrokerMessage(const IpcFrameHeader& header,
                                          const uint8_t* payload,
                                          uint32_t payloadBytes) {
    Impl& s = *impl_;
    switch (static_cast<IpcMessageKind>(header.type)) {
        case IpcMessageKind::SessionInfo: {
            BrokerSessionInfo info{};
            if (!decodeBrokerSessionInfo(payload, payloadBytes, info)) {
                dropBroker(SourceFault::DecodeFailed, header.sequence);
                return false;
            }
            if (s.options.manifestDigest != 0u
                && info.manifestDigest != s.options.manifestDigest) {
                dropBroker(SourceFault::ManifestMismatch, 0u);
                return false;
            }
            BrokerControl c{};
            c.kind = BrokerControl::Kind::SessionInfo;
            c.info = info;
            publishControl(control_, c);
            s.sessionInfoSeen = true;
            return true;
        }
        case IpcMessageKind::SessionStart: {
            if (!s.sessionInfoSeen) {
                dropBroker(SourceFault::UnexpectedMessage, header.type);
                return false;
            }
            qreplay::DeterministicStart start{};
            if (qreplay::decodeDeterministicStart(payload, payloadBytes, start)
                != qreplay::ParseStatus::Ok) {
                dropBroker(SourceFault::DecodeFailed, header.sequence);
                return false;
            }
            BrokerControl c{};
            c.kind = BrokerControl::Kind::Start;
            c.start = start;
            publishControl(control_, c);
            s.startSeen = true;
            s.expectFrameKnown = true;
            s.expectedFrame = start.firstTimelineFrame;
            s.chainedDigest = start.initialDigest;
            s.chainedDigestKnown = true;
            return true;
        }
        case IpcMessageKind::InputBlock: {
            if (!s.startSeen) {
                dropBroker(SourceFault::UnexpectedMessage, header.type);
                return false;
            }
            if (header.param1 == 0u
                || header.param1 > qreplay::kMaxRecordsPerInputBlock) {
                dropBroker(SourceFault::DecodeFailed, header.param1);
                return false;
            }
            if (s.expectFrameKnown && header.param0 != s.expectedFrame) {
                dropBroker(SourceFault::FrameGap, header.param0);
                return false;
            }
            s.pendingFrames.clear();
            s.pendingFrameIndex = 0u;
            const qreplay::ParseStatus status = qreplay::decodeInputBlock(
                payload, payloadBytes, header.param0, header.param1,
                s.pendingFrames);
            if (status != qreplay::ParseStatus::Ok) {
                SourceFault fault = SourceFault::DecodeFailed;
                if (status == qreplay::ParseStatus::BadActionMask)
                    fault = SourceFault::InvalidActionMask;
                else if (status == qreplay::ParseStatus::BadLaneShape)
                    fault = SourceFault::InvalidLaneShape;
                else if (status == qreplay::ParseStatus::FrameGap)
                    fault = SourceFault::FrameGap;
                dropBroker(fault, header.param0);
                return false;
            }
            uint32_t expect = header.param0;
            for (const qreplay::ReplayFrameV1& frame : s.pendingFrames) {
                if (frame.timelineFrame != expect) {
                    dropBroker(SourceFault::FrameGap, frame.timelineFrame);
                    return false;
                }
                if (!qreplay::isValidFrame(frame)) {
                    bool maskBad = false;
                    for (size_t lane = 0; lane < qreplay::kSourceLanes; ++lane)
                        if (!qreplay::isValidActionMask(frame.actions[lane]))
                            maskBad = true;
                    dropBroker(maskBad ? SourceFault::InvalidActionMask
                                       : SourceFault::InvalidLaneShape,
                               frame.timelineFrame);
                    return false;
                }
                if (s.chainedDigestKnown) {
                    const uint64_t expected =
                        qreplay::advanceReplayDigest(s.chainedDigest, frame);
                    if (expected != frame.rollingDigest) {
                        dropBroker(SourceFault::DigestBreak,
                                   frame.timelineFrame);
                        return false;
                    }
                    s.chainedDigest = expected;
                }
                ++expect;
            }
            s.expectedFrame = expect;
            s.expectFrameKnown = true;
            return true;
        }
        case IpcMessageKind::EventBlock: {
            if (!s.startSeen) {
                dropBroker(SourceFault::UnexpectedMessage, header.type);
                return false;
            }
            if (header.param1 == 0u
                || header.param1 > qreplay::kMaxEventsPerBlock) {
                dropBroker(SourceFault::DecodeFailed, header.param1);
                return false;
            }
            s.pendingEvents.clear();
            s.pendingEventIndex = 0u;
            if (qreplay::decodeEventBlock(payload, payloadBytes, header.param1,
                                          s.pendingEvents)
                != qreplay::ParseStatus::Ok) {
                dropBroker(SourceFault::DecodeFailed, header.sequence);
                return false;
            }
            for (const qreplay::ReplayEvent& event : s.pendingEvents) {
                if (!qreplay::isValidEvent(event)) {
                    dropBroker(SourceFault::DecodeFailed, event.timelineFrame);
                    return false;
                }
            }
            return true;
        }
        case IpcMessageKind::Terminal: {
            wire::TerminalPayload terminal{};
            if (!wire::decodeTerminal(payload, payloadBytes, terminal)) {
                dropBroker(SourceFault::DecodeFailed, payloadBytes);
                return false;
            }
            BrokerControl c{};
            c.kind = BrokerControl::Kind::Terminal;
            c.terminalReason = terminal.reason;
            c.lastTimelineFrame = terminal.lastTimelineFrame;
            c.postTailStateHash = terminal.postTailStateHash;
            c.rollingDigest = terminal.rollingDigest;
            publishControl(control_, c);
            s.terminalSeen = true;
            return true;
        }
        default:
            dropBroker(SourceFault::UnexpectedMessage, header.type);
            return false;
    }
}

bool ReplayIpcWorker::sendRecorderFrameV3(
    const qreplay::ipc::v3::MessageType type,
    const qreplay::GameIdentityV2& game,
    const uint32_t param0, const uint32_t param1,
    const std::vector<uint8_t>& payload) {
    namespace wire3 = qreplay::ipc::v3;
    Impl& s = *impl_;
    if (!s.recorder || !s.recorder->connected()) return false;
    wire3::Frame frame{};
    frame.channel = wire3::Channel::Recorder;
    frame.type = type;
    frame.launchGeneration = s.options.recordGeneration;
    frame.game = game;
    frame.launchNonce = s.options.recorderNonce;
    frame.sequence = s.recorderOutboundSequenceV3 + 1u;
    frame.param0 = param0;
    frame.param1 = param1;
    frame.payload = payload;
    std::vector<uint8_t> encoded;
    encoded.reserve(wire3::kHeaderBytes + payload.size());
    if (!wire3::encodeFrame(frame, encoded)
        || !s.recorder->write(encoded.data(), encoded.size())) {
        return false;
    }
    s.recorderOutboundSequenceV3 = frame.sequence;
    s.recorderMessagesSent.fetch_add(1u, std::memory_order_relaxed);
    return true;
}

bool ReplayIpcWorker::pumpRecorderResponsesV3() {
    namespace wire3 = qreplay::ipc::v3;
    Impl& s = *impl_;
    if (!s.recorder || !s.recorder->connected()) return false;
    std::array<uint8_t, 64u * 1024u> incoming{};
    size_t got = 0u;
    if (!s.recorder->read(incoming.data(), incoming.size(), got)) {
        s.recorder->close();
        s.recorderConnected.store(0u, std::memory_order_relaxed);
        disableRecording(static_cast<uint16_t>(
            qreplay::TerminalReason::RecordingFailure));
        if (s.queueV2) s.queueV2->disable();
        s.recorderStopped = true;
        return false;
    }
    if (got == 0u) return false;
    if (s.recorderRxV3.size() + got
        > static_cast<size_t>(wire3::kMaxPayloadBytes)
            + wire3::kHeaderBytes) {
        disableRecording(static_cast<uint16_t>(
            qreplay::TerminalReason::RecordingFailure));
        s.queueV2->disable();
        s.recorderStopped = true;
        return false;
    }
    s.recorderRxV3.insert(
        s.recorderRxV3.end(), incoming.begin(), incoming.begin() + got);
    bool progressed = false;
    for (;;) {
        wire3::Frame frame{};
        size_t consumed = 0u;
        const wire3::FrameStatus decoded = wire3::decodeFrame(
            s.recorderRxV3.data(), s.recorderRxV3.size(), frame, consumed);
        if (decoded == wire3::FrameStatus::NeedMore) break;
        if (decoded != wire3::FrameStatus::Ok
            || frame.launchGeneration != s.options.recordGeneration
            || frame.launchNonce != s.options.recorderNonce) {
            disableRecording(static_cast<uint16_t>(
                qreplay::TerminalReason::RecordingFailure));
            s.queueV2->disable();
            s.recorderStopped = true;
            return progressed;
        }
        s.recorderRxV3.erase(
            s.recorderRxV3.begin(),
            s.recorderRxV3.begin() + static_cast<ptrdiff_t>(consumed));

        const auto sameFrame = [](const wire3::Frame& a,
                                  const wire3::Frame& b) {
            return a.channel == b.channel && a.type == b.type
                && a.launchGeneration == b.launchGeneration
                && a.game == b.game
                && a.launchNonce == b.launchNonce
                && a.sequence == b.sequence && a.param0 == b.param0
                && a.param1 == b.param1 && a.payload == b.payload;
        };
        if (frame.sequence == s.recorderInboundSequenceV3) {
            if (!s.haveLastInboundV3
                || !sameFrame(frame, s.lastInboundV3)) {
                disableRecording(static_cast<uint16_t>(
                    qreplay::TerminalReason::RecordingFailure));
                s.queueV2->disable();
                s.recorderStopped = true;
                return progressed;
            }
            progressed = true;
            continue;
        }
        if (frame.sequence != s.recorderInboundSequenceV3 + 1u) {
            disableRecording(static_cast<uint16_t>(
                qreplay::TerminalReason::RecordingFailure));
            s.queueV2->disable();
            s.recorderStopped = true;
            return progressed;
        }
        s.recorderInboundSequenceV3 = frame.sequence;
        s.lastInboundV3 = frame;
        s.haveLastInboundV3 = true;

        bool accepted = false;
        switch (frame.type) {
            case wire3::MessageType::CasterGameAccepted: {
                wire3::CasterGameAcceptedPayload body{};
                accepted = s.waitingAcceptedV3
                    && frame.game == s.currentGameV3
                    && wire3::decodeCasterGameAccepted(
                        frame.payload.data(), frame.payload.size(), body);
                if (accepted) s.waitingAcceptedV3 = false;
                break;
            }
            case wire3::MessageType::CasterGameCommitted: {
                wire3::CasterGameCommittedPayload body{};
                accepted = s.waitingCommittedV3
                    && frame.game == s.currentGameV3
                    && wire3::decodeCasterGameCommitted(
                        frame.payload.data(), frame.payload.size(), body);
                if (accepted) {
                    qreplay::GameCommitAckV2 ack{};
                    ack.identity = frame.game;
                    ack.status =
                        qreplay::GameCommitStatusV2::Committed;
                    ack.nextRecord = body.nextRecord;
                    ack.canonicalGameDigest =
                        body.canonicalGameDigest;
                    ack.fileDigest = body.fileDigest;
                    if (!commitAcks_.push(ack)) accepted = false;
                    s.waitingCommittedV3 = false;
                    s.currentGameV3 = qreplay::GameIdentityV2{};
                }
                break;
            }
            case wire3::MessageType::CasterGameFailed: {
                wire3::CasterGameFailedPayload body{};
                const bool failedBeforeEnd = s.waitingAcceptedV3;
                accepted =
                    (s.waitingAcceptedV3 || s.waitingCommittedV3)
                    && frame.game == s.currentGameV3
                    && wire3::decodeCasterGameFailed(
                        frame.payload.data(), frame.payload.size(), body);
                if (accepted) {
                    if (failedBeforeEnd) ++s.gamesEndedV3;
                    qreplay::GameCommitAckV2 ack{};
                    ack.identity = frame.game;
                    ack.status = qreplay::GameCommitStatusV2::Failed;
                    ack.nextRecord = body.nextRecord;
                    if (!commitAcks_.push(ack)) accepted = false;
                    s.waitingAcceptedV3 = false;
                    s.waitingCommittedV3 = false;
                    s.currentGameV3 = qreplay::GameIdentityV2{};
                }
                break;
            }
            case wire3::MessageType::CasterRecorderStatus: {
                wire3::CasterRecorderStatusPayload body{};
                accepted = wire3::decodeCasterRecorderStatus(
                    frame.payload.data(), frame.payload.size(), body);
                break;
            }
            case wire3::MessageType::CasterDrainAck: {
                wire3::CasterDrainAckPayload body{};
                accepted = s.drainSentV3
                    && wire3::decodeCasterDrainAck(
                        frame.payload.data(), frame.payload.size(), body)
                    && body.unresolvedGames == 0u;
                if (accepted)
                    recorderDrainAcked_.store(
                        true, std::memory_order_release);
                break;
            }
            default:
                accepted = false;
                break;
        }
        if (!accepted) {
            disableRecording(static_cast<uint16_t>(
                qreplay::TerminalReason::RecordingFailure));
            s.queueV2->disable();
            s.recorderStopped = true;
            return progressed;
        }
        progressed = true;
    }
    return progressed;
}

bool ReplayIpcWorker::pumpRecorderV3() {
    namespace wire3 = qreplay::ipc::v3;
    Impl& s = *impl_;
    if (!s.recorder || !s.queueV2) return false;

    if (!s.recorder->connected()) {
        const uint64_t now = nowMs();
        if (s.recorderFirstFailureMs == 0u)
            s.recorderFirstFailureMs = now;
        if (s.recorderLastAttemptMs != 0u
            && now - s.recorderLastAttemptMs
                < s.options.reconnectBackoffMs) {
            return false;
        }
        s.recorderLastAttemptMs = now == 0u ? 1u : now;
        if (s.recorder->connect(s.options.connectTimeoutMs)) {
            s.recorderAttempts = 0u;
            s.recorderFirstFailureMs = 0u;
            s.recorderLastAttemptMs = 0u;
            s.recorderConnected.store(1u, std::memory_order_relaxed);
        } else {
            ++s.recorderAttempts;
            const bool attemptsGone =
                s.recorderAttempts >= s.options.maxReconnectAttempts;
            const bool deadlineGone =
                now - s.recorderFirstFailureMs
                >= s.options.reconnectDeadlineMs;
            if (attemptsGone && deadlineGone) {
                disableRecording(static_cast<uint16_t>(
                    qreplay::TerminalReason::RecordingFailure));
                s.queueV2->disable();
                s.recorderStopped = true;
            }
            return false;
        }
    }

    bool progressed = pumpRecorderResponsesV3();
    if (s.recorderStopped || !s.recorder->connected()) return progressed;

    if (!s.recorderHelloSent) {
        wire3::RecorderHelloPayload hello{};
        hello.gamePid = s.options.gamePid;
        hello.runtimeCompatibilityId =
            s.options.runtimeCompatibilityId;
        hello.queueCapacity =
            static_cast<uint32_t>(s.queueV2->capacity());
        s.recScratch.clear();
        if (!wire3::encodeRecorderHello(hello, s.recScratch)
            || !sendRecorderFrameV3(
                wire3::MessageType::RecorderHello,
                qreplay::GameIdentityV2{}, 0u, 0u, s.recScratch)) {
            disableRecording(static_cast<uint16_t>(
                qreplay::TerminalReason::RecordingFailure));
            s.queueV2->disable();
            s.recorderStopped = true;
            return progressed;
        }
        s.recorderHelloSent = true;
        progressed = true;
    }

    if (!s.waitingAcceptedV3 && !s.waitingCommittedV3
        && !s.drainSentV3) {
        uint32_t drained = 0u;
        while (drained < s.options.recorderDrainPerTurn) {
            qreplay::PublisherRecordV2 record{};
            if (!s.queueV2->drain(record)) break;
            ++drained;
            ++s.publisherSequenceV3;
            bool sent = false;
            switch (record.kind) {
                case qreplay::PublisherRecordKindV2::BeginGame: {
                    s.currentGameV3 =
                        record.begin.start.manifest.identity;
                    s.recScratch.clear();
                    sent = wire3::encodeRecorderGameBegin(
                               record.begin, s.recScratch)
                        && sendRecorderFrameV3(
                            wire3::MessageType::RecorderGameBegin,
                            s.currentGameV3, 0u, 1u, s.recScratch);
                    if (sent) {
                        ++s.gamesBegunV3;
                        s.waitingAcceptedV3 = true;
                    }
                    break;
                }
                case qreplay::PublisherRecordKindV2::Palette: {
                    if (!qreplay::isValidGameIdentityV2(s.currentGameV3)) {
                        sent = true;
                        break;
                    }
                    s.recScratch.clear();
                    sent = wire3::encodeRecorderPalette(
                               record.palette, s.recScratch)
                        && sendRecorderFrameV3(
                            wire3::MessageType::RecorderPalette,
                            s.currentGameV3, record.palette.side, 1u,
                            s.recScratch);
                    break;
                }
                case qreplay::PublisherRecordKindV2::Tick: {
                    if (!qreplay::isValidGameIdentityV2(s.currentGameV3)) {
                        sent = false;
                        break;
                    }
                    sent = true;
                    if (record.tick.preEvents.count != 0u) {
                        s.recScratch.clear();
                        sent = wire3::encodeRecorderEvents(
                                   record.tick.preEvents.events.data(),
                                   record.tick.preEvents.count,
                                   s.recScratch)
                            && sendRecorderFrameV3(
                                wire3::MessageType::RecorderEvents,
                                s.currentGameV3,
                                record.tick.preEvents.events[0]
                                    .eventSequence,
                                record.tick.preEvents.count,
                                s.recScratch);
                    }
                    if (sent) {
                        s.recScratch.clear();
                        sent = wire3::encodeRecorderFrames(
                                   &record.tick.frame, 1u,
                                   s.recScratch)
                            && sendRecorderFrameV3(
                                wire3::MessageType::RecorderFrames,
                                s.currentGameV3,
                                record.tick.frame.replayFrame, 1u,
                                s.recScratch);
                    }
                    if (sent && record.tick.postEvents.count != 0u) {
                        s.recScratch.clear();
                        sent = wire3::encodeRecorderEvents(
                                   record.tick.postEvents.events.data(),
                                   record.tick.postEvents.count,
                                   s.recScratch)
                            && sendRecorderFrameV3(
                                wire3::MessageType::RecorderEvents,
                                s.currentGameV3,
                                record.tick.postEvents.events[0]
                                    .eventSequence,
                                record.tick.postEvents.count,
                                s.recScratch);
                    }
                    if (sent) {
                        s.recorderRecordsSentStat.fetch_add(
                            1u, std::memory_order_relaxed);
                    }
                    break;
                }
                case qreplay::PublisherRecordKindV2::EndGame: {
                    if (!qreplay::isValidGameIdentityV2(s.currentGameV3)) {
                        sent = false;
                        break;
                    }
                    s.recScratch.clear();
                    sent = wire3::encodeRecorderGameEnd(
                               record.end, s.recScratch)
                        && sendRecorderFrameV3(
                            wire3::MessageType::RecorderGameEnd,
                            s.currentGameV3, record.end.end.nextRecord,
                            record.end.containsFinalFrame ? 1u : 0u,
                            s.recScratch);
                    if (sent) {
                        ++s.gamesEndedV3;
                        s.waitingCommittedV3 = true;
                    }
                    break;
                }
            }
            if (!sent) {
                disableRecording(static_cast<uint16_t>(
                    qreplay::TerminalReason::RecordingFailure));
                s.queueV2->disable();
                s.recorderStopped = true;
                return progressed;
            }
            progressed = true;
            if (s.waitingAcceptedV3 || s.waitingCommittedV3) break;
        }
    }

    if (recorderDrainRequested_.load(std::memory_order_acquire)
        && !s.drainSentV3 && !s.waitingAcceptedV3
        && !s.waitingCommittedV3 && s.queueV2->pending() == 0u) {
        wire3::RecorderDrainPayload drain{};
        drain.nextPublisherSequence = s.publisherSequenceV3;
        drain.gamesBegun = s.gamesBegunV3;
        drain.gamesEnded = s.gamesEndedV3;
        drain.abandonedOpenGame =
            recorderDrainAbandonedOpenGame_.load(
                std::memory_order_acquire);
        s.recScratch.clear();
        if (!wire3::encodeRecorderDrain(drain, s.recScratch)
            || !sendRecorderFrameV3(
                wire3::MessageType::RecorderDrain,
                qreplay::GameIdentityV2{}, 0u, 0u, s.recScratch)) {
            disableRecording(static_cast<uint16_t>(
                qreplay::TerminalReason::RecordingFailure));
            s.queueV2->disable();
            s.recorderStopped = true;
            return progressed;
        }
        s.drainSentV3 = true;
        progressed = true;
    }

    if (recorderDrainAcked_.load(std::memory_order_acquire)
        && !s.byeSentV3) {
        wire3::RecorderByePayload bye{};
        bye.reason = wire3::RecorderByeReason::NormalAfterDrain;
        bye.drainCompleted = true;
        bye.nextPublisherSequence = s.publisherSequenceV3;
        s.recScratch.clear();
        if (!wire3::encodeRecorderBye(bye, s.recScratch)
            || !sendRecorderFrameV3(
                wire3::MessageType::RecorderBye,
                qreplay::GameIdentityV2{}, 0u, 0u, s.recScratch)) {
            disableRecording(static_cast<uint16_t>(
                qreplay::TerminalReason::RecordingFailure));
            s.queueV2->disable();
            s.recorderStopped = true;
            return progressed;
        }
        s.byeSentV3 = true;
        s.recorderStopped = true;
        progressed = true;
    }
    return progressed;
}

bool ReplayIpcWorker::pumpRecorder() {
    Impl& s = *impl_;
    if (s.options.recordingV3) return pumpRecorderV3();
    if (!s.recorder || !s.queue) return false;

    if (s.queue->disabled())
        disableRecording(
            static_cast<uint16_t>(qreplay::TerminalReason::RecorderOverflow));

    const bool disabled = recordingDisabled_.load(std::memory_order_acquire);

    if (!s.recorder->connected()) {
        const uint64_t now = nowMs();
        if (s.recorderFirstFailureMs == 0u) s.recorderFirstFailureMs = now;
        if (s.recorderLastAttemptMs != 0u
            && now - s.recorderLastAttemptMs < s.options.reconnectBackoffMs) {
            return false;
        }
        s.recorderLastAttemptMs = now == 0u ? 1u : now;
        if (s.recorder->connect(s.options.connectTimeoutMs)) {
            s.recorderAttempts = 0u;
            s.recorderFirstFailureMs = 0u;
            s.recorderLastAttemptMs = 0u;
            s.recorderHelloSent = false;
            s.recorderConnected.store(1u, std::memory_order_relaxed);
        } else {
            ++s.recorderAttempts;
            const bool attemptsGone =
                s.recorderAttempts >= s.options.maxReconnectAttempts;
            const bool deadlineGone =
                (now - s.recorderFirstFailureMs)
                >= s.options.reconnectDeadlineMs;
            if (attemptsGone && deadlineGone) {
                disableRecording(static_cast<uint16_t>(
                    qreplay::TerminalReason::RecordingFailure));
                s.queue->disable();
                s.recorderStopped = true;
                QLOG("[replay.record] generation=%u source=%u session=- "
                     "match=- state=channel-unavailable",
                     s.options.recordGeneration, s.options.recordSourceKind);
            }
            return false;
        }
    }

    if (!s.recorderHelloSent) {
        buildHelloPayload(s.recScratch);
        if (!sendFramed(*s.recorder, IpcMessageKind::RecorderHello,
                        s.options.recordSourceKind, s.options.recordGeneration,
                        s.recScratch, s.recorderSequence)) {
            failRecorderChannel();
            return false;
        }
        s.recorderHelloSent = true;
        QLOG("[replay.record] generation=%u source=%u session=%016llX "
             "match=%u state=connected",
             s.options.recordGeneration, s.options.recordSourceKind,
             static_cast<unsigned long long>(s.options.manifestDigest),
             s.options.matchOrdinal);
    }

    if (disabled && s.queue->empty()) {
        if (!s.recorderTerminalSent) {
            emitRecorderTerminal(recordingDisableReason_.load(
                std::memory_order_acquire));
            s.recorderStopped = true;
        }
        return false;
    }

    s.recFrames.clear();
    s.recEvents.clear();
    bool terminalPending = false;
    uint16_t terminalReason = 0u;
    uint32_t terminalFrame = 0u;
    uint64_t terminalHash = 0u;
    uint64_t terminalDigest = 0u;

    qreplay::RecorderMessage message{};
    uint32_t drained = 0u;
    while (drained < s.options.recorderDrainPerTurn
           && s.queue->drain(message)) {
        ++drained;
        switch (message.kind) {
            case qreplay::RecorderMessageKind::Frame:
                if (!s.recFrames.empty()
                    && message.frame.timelineFrame
                           != s.recFrames.back().timelineFrame + 1u) {
                    if (!flushRecorderFrames()) return false;
                }
                s.recFrames.push_back(message.frame);
                break;
            case qreplay::RecorderMessageKind::Event:
                s.recEvents.push_back(message.event);
                break;
            case qreplay::RecorderMessageKind::SessionEnd:
            case qreplay::RecorderMessageKind::Terminal:
                terminalPending = true;
                terminalReason =
                    static_cast<uint16_t>(message.sessionEnd.reason);
                terminalFrame = message.sessionEnd.lastTimelineFrame;
                terminalHash = message.sessionEnd.postTailStateHash;
                terminalDigest = message.sessionEnd.rollingDigest;
                break;
            case qreplay::RecorderMessageKind::Manifest: {
                if (!flushRecorderFrames()) return false;
                if (!flushRecorderEvents()) return false;
                s.recScratch.clear();
                const qreplay::RecorderManifestBlob& blob = message.manifest;
                if (blob.bytes == 0u
                    || blob.bytes > qreplay::kMaxRecorderManifestBytes
                    || !wire::encodeRecorderManifest(blob.prefix,
                                                     blob.encoded.data(),
                                                     blob.bytes,
                                                     s.recScratch)) {
                    disableRecording(static_cast<uint16_t>(
                        qreplay::TerminalReason::RecordingFailure));
                    s.queue->disable();
                    QLOG("[replay.record] generation=%u source=%u "
                         "state=disabled(manifest-unencodable,bytes=%u)",
                         s.options.recordGeneration,
                         s.options.recordSourceKind, blob.bytes);
                    break;
                }
                if (!sendFramed(*s.recorder, IpcMessageKind::RecorderManifest,
                                0u, 0u, s.recScratch, s.recorderSequence)) {
                    failRecorderChannel();
                    return false;
                }
                s.recorderMessagesSent.fetch_add(1u, std::memory_order_relaxed);
                break;
            }
            case qreplay::RecorderMessageKind::SessionStart:
            case qreplay::RecorderMessageKind::MatchStart:
            case qreplay::RecorderMessageKind::MatchEnd: {
                if (!flushRecorderFrames()) return false;
                if (!flushRecorderEvents()) return false;
                s.recScratch.clear();
                IpcMessageKind kind = IpcMessageKind::RecorderSessionStart;
                uint32_t param0 = 0u;
                bool encoded = false;
                if (message.kind
                    == qreplay::RecorderMessageKind::SessionStart) {
                    kind = IpcMessageKind::RecorderSessionStart;
                    param0 = message.matchStart.start.firstTimelineFrame;
                    encoded = qreplay::encodeDeterministicStart(
                        message.matchStart.start, s.recScratch);
                } else if (message.kind
                           == qreplay::RecorderMessageKind::MatchStart) {
                    kind = IpcMessageKind::RecorderMatchStart;
                    param0 = message.matchStart.matchId;
                    encoded = qreplay::encodeMatchStart(message.matchStart,
                                                        s.recScratch);
                } else {
                    kind = IpcMessageKind::RecorderMatchEnd;
                    param0 = message.matchEnd.matchId;
                    encoded =
                        qreplay::encodeMatchEnd(message.matchEnd, s.recScratch);
                }
                if (!encoded) {
                    disableRecording(static_cast<uint16_t>(
                        qreplay::TerminalReason::RecordingFailure));
                    s.queue->disable();
                    break;
                }
                if (!sendFramed(*s.recorder, kind, param0, 0u, s.recScratch,
                                s.recorderSequence)) {
                    failRecorderChannel();
                    return false;
                }
                s.recorderMessagesSent.fetch_add(1u, std::memory_order_relaxed);
                break;
            }
            default:
                break;
        }
    }

    if (!s.recFrames.empty() && !flushRecorderFrames()) return false;
    if (!s.recEvents.empty() && !flushRecorderEvents()) return false;

    if (terminalPending && !s.recorderTerminalSent) {
        wire::TerminalPayload terminal{};
        terminal.reason = terminalReason;
        terminal.lastTimelineFrame = terminalFrame;
        terminal.postTailStateHash = terminalHash;
        terminal.rollingDigest = terminalDigest;
        s.recScratch.clear();
        wire::encodeTerminal(terminal, s.recScratch);
        if (!sendFramed(*s.recorder, IpcMessageKind::RecorderTerminal,
                        terminalFrame, 0u, s.recScratch, s.recorderSequence)) {
            failRecorderChannel();
            return false;
        }
        s.recorderTerminalSent = true;
        s.recorderStopped = true;
        QLOG("[replay.record] published=%llu confirmed=%u written=%llu "
             "queueHighWater=%llu",
             static_cast<unsigned long long>(
                 s.recorderRecordsSentStat.load(std::memory_order_relaxed)),
             terminalFrame,
             static_cast<unsigned long long>(
                 s.recorderMessagesSent.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(s.queue->highWater()));
    }

    return drained > 0u;
}

bool ReplayIpcWorker::flushRecorderFrames() {
    Impl& s = *impl_;
    if (s.recFrames.empty()) return true;
    s.recScratch.clear();
    if (!qreplay::encodeInputBlock(s.recFrames.data(), s.recFrames.size(),
                                   s.recScratch)) {
        disableRecording(
            static_cast<uint16_t>(qreplay::TerminalReason::RecordingFailure));
        s.queue->disable();
        s.recFrames.clear();
        return true;
    }
    const uint32_t first = s.recFrames.front().timelineFrame;
    const uint32_t count = static_cast<uint32_t>(s.recFrames.size());
    if (!sendFramed(*s.recorder, IpcMessageKind::RecorderFrames, first, count,
                    s.recScratch, s.recorderSequence)) {
        failRecorderChannel();
        return false;
    }
    s.recorderMessagesSent.fetch_add(1u, std::memory_order_relaxed);
    s.recorderRecordsSentStat.fetch_add(count, std::memory_order_relaxed);
    s.recFrames.clear();
    return true;
}

bool ReplayIpcWorker::flushRecorderEvents() {
    Impl& s = *impl_;
    if (s.recEvents.empty()) return true;
    s.recScratch.clear();
    if (!qreplay::encodeEventBlock(s.recEvents.data(), s.recEvents.size(),
                                   s.recScratch)) {
        disableRecording(
            static_cast<uint16_t>(qreplay::TerminalReason::RecordingFailure));
        s.queue->disable();
        s.recEvents.clear();
        return true;
    }
    const uint32_t count = static_cast<uint32_t>(s.recEvents.size());
    if (!sendFramed(*s.recorder, IpcMessageKind::RecorderEvents, 0u, count,
                    s.recScratch, s.recorderSequence)) {
        failRecorderChannel();
        return false;
    }
    s.recorderMessagesSent.fetch_add(1u, std::memory_order_relaxed);
    s.recEvents.clear();
    return true;
}

void ReplayIpcWorker::emitRecorderTerminal(uint16_t reason) {
    Impl& s = *impl_;
    if (s.recorderTerminalSent) return;
    s.recorderTerminalSent = true;
    if (!s.recorder || !s.recorder->connected()) return;
    wire::TerminalPayload terminal{};
    terminal.reason = reason;
    s.recScratch.clear();
    wire::encodeTerminal(terminal, s.recScratch);
    (void)sendFramed(*s.recorder, IpcMessageKind::RecorderTerminal, 0u, 0u,
                     s.recScratch, s.recorderSequence);
    QLOG("[replay.record] generation=%u source=%u session=%016llX match=%u "
         "state=disabled(reason=%u,drops=%llu)",
         s.options.recordGeneration, s.options.recordSourceKind,
         static_cast<unsigned long long>(s.options.manifestDigest),
         s.options.matchOrdinal, reason,
         static_cast<unsigned long long>(
             s.queue ? s.queue->droppedCount() : 0ull));
}

void ReplayIpcWorker::failRecorderChannel() {
    Impl& s = *impl_;
    s.recorderConnected.store(0u, std::memory_order_relaxed);
    if (s.recorder) s.recorder->close();
    disableRecording(
        static_cast<uint16_t>(qreplay::TerminalReason::RecordingFailure));
    if (s.queue) s.queue->disable();
    s.recorderStopped = true;
    QLOG("[replay.record] generation=%u source=%u session=%016llX match=%u "
         "state=channel-failed",
         s.options.recordGeneration, s.options.recordSourceKind,
         static_cast<unsigned long long>(s.options.manifestDigest),
         s.options.matchOrdinal);
}

}
}
