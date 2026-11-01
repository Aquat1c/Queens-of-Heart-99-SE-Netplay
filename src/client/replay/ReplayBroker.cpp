#include "ReplayBroker.h"

#include "ipc/IpcConfig.h"
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

const char* brokerStateText(BrokerState state) {
    switch (state) {
        case BrokerState::Idle: return "idle";
        case BrokerState::Prepared: return "prepared";
        case BrokerState::Listening: return "waiting for the game";
        case BrokerState::Serving: return "serving";
        case BrokerState::Finished: return "finished";
        case BrokerState::Failed: return "failed";
    }
    return "unknown";
}

struct ReplayBroker::Prepared {
    ReplayPrefix prefix{};
    SessionManifest manifest{};
    DeterministicStart sessionStart{};
    MatchStartRecord matchStart{};
    bool hasMatchStart = false;
    bool useMatchStart = false;
    std::vector<ReplayFrameV1> frames;
    std::vector<ReplayEvent> events;
    uint32_t matchId = 0u;
    uint32_t matchOrdinal = 0u;
    uint32_t matchCount = 0u;
    uint32_t startFrame = 0u;
    uint32_t startRound = 0u;
    bool prebuffered = false;
    TerminalReason terminalReason = TerminalReason::Completed;
    uint64_t postTailStateHash = 0u;
    uint64_t finalRollingDigest = 0u;
};

ReplayBroker::ReplayBroker() = default;

ReplayBroker::~ReplayBroker() { stop(); }

BrokerPrepareResult ReplayBroker::prepare(const BrokerSelection& selection,
                                          const RuntimeIdentity& runtime) {
    BrokerPrepareResult result{};
    prepared_.reset();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_ = BrokerStatus{};
    }

    if (!isSafeReplayPath(selection.path)) {
        result.detail = "unsafe replay path";
        return result;
    }

    ParseOptions options{};
    options.mode = ParseMode::Full;
    options.requireFooter = true;
    ParsedReplay parsed;

    result.parse = ReplayParser::parseFile(selection.path, options, parsed);
    if (!result.parse.ok()) {
        result.detail = std::string("replay rejected: ")
            + parseStatusText(result.parse.status)
            + " (only finalized replays can be played)";
        return result;
    }

    ReplayIdentitySnapshot file{};
    file.prefix = parsed.file.prefix;
    file.contract = parsed.file.manifest.contract;
    file.structurallyValid = true;
    file.hasFooter = parsed.hasFooter;
    file.hasSessionStart = parsed.hasSessionStart;
    file.digestVerified = parsed.hasFooter && parsed.fileDigestMatched;
    file.recoverable = false;
    file.activeFighterCount = 0u;
    for (const FighterSlotDescriptor& slot : parsed.file.manifest.slots) {
        if (slot.ownership != SlotOwnership::Inactive)
            ++file.activeFighterCount;
    }
    file.cpuContract = parsed.file.manifest.cpuContract;
    file.inputDomain = parsed.file.frames.empty()
        ? InputDomain::LogicalSourceV1
        : parsed.file.frames.front().inputDomain;
    for (size_t i = 0; i < kFighterSlots; ++i) {
        if (parsed.file.manifest.slotToSourceRoute[i]
            != static_cast<uint8_t>(i)) {
            file.usesNonIdentityRoute = true;
        }
    }
    result.compatibility = evaluate(file, runtime);
    if (!result.compatibility.watchable) {
        result.detail = "not watchable: " + result.compatibility.detail;
        return result;
    }

    std::unique_ptr<Prepared> prepared(new Prepared());
    prepared->prefix = parsed.file.prefix;
    prepared->manifest = parsed.file.manifest;
    prepared->sessionStart = parsed.file.sessionStart;
    prepared->startRound = selection.startRound;
    prepared->matchOrdinal = selection.matchOrdinal;
    prepared->matchCount =
        static_cast<uint32_t>(parsed.file.matchStarts.size());

    const MatchStartRecord* chosen = nullptr;
    if (selection.matchOrdinal != 0u) {
        for (const MatchStartRecord& candidate : parsed.file.matchStarts) {
            if (candidate.ordinal == selection.matchOrdinal) {
                chosen = &candidate;
                break;
            }
        }
        if (chosen == nullptr) {
            result.detail = "the requested match is not in this file";
            return result;
        }
        if (!chosen->independentBootstrap) {
            const MatchStartRecord* dependency = chosen;
            for (int guard = 0; guard < static_cast<int>(kMaxMatchesPerSet)
                 && dependency != nullptr && !dependency->independentBootstrap;
                 ++guard) {
                const MatchStartRecord* next = nullptr;
                for (const MatchStartRecord& candidate : parsed.file.matchStarts) {
                    if (candidate.matchId == dependency->dependsOnMatchId) {
                        next = &candidate;
                        break;
                    }
                }
                dependency = next;
            }
            if (dependency == nullptr) {
                result.detail = "the requested match has no reachable bootstrap";
                return result;
            }
            chosen = dependency;
        }
        prepared->matchStart = *chosen;
        prepared->hasMatchStart = true;
        prepared->useMatchStart = true;
        prepared->matchId = chosen->matchId;
        prepared->startFrame = chosen->firstTimelineFrame;
    } else {
        prepared->startFrame = parsed.file.sessionStart.firstTimelineFrame;
        if (!parsed.file.matchStarts.empty()) {
            prepared->matchStart = parsed.file.matchStarts.front();
            prepared->hasMatchStart = true;
        }
    }

    uint32_t lastFrame = 0xFFFFFFFFu;
    if (selection.matchOrdinal != 0u) {
        for (const MatchSpan& span : parsed.matches) {
            if (span.matchId == prepared->matchId) {
                lastFrame = span.lastTimelineFrame;
                break;
            }
        }
    }
    for (const ReplayFrameV1& frame : parsed.file.frames) {
        if (frame.timelineFrame < prepared->startFrame) continue;
        if (lastFrame != 0xFFFFFFFFu && frame.timelineFrame > lastFrame) break;
        prepared->frames.push_back(frame);
    }
    for (const ReplayEvent& event : parsed.file.events) {
        if (event.timelineFrame < prepared->startFrame) continue;
        if (lastFrame != 0xFFFFFFFFu && event.timelineFrame > lastFrame) break;
        prepared->events.push_back(event);
    }
    if (prepared->frames.empty()) {
        result.detail = "the selected range contains no canonical frames";
        return result;
    }
    prepared->prebuffered =
        prepared->frames.size() <= config_.prebufferFrameLimit
        || prepared->frames.size() <= 300000u;
    prepared->terminalReason = parsed.file.sessionEnd.reason;
    prepared->postTailStateHash = parsed.file.sessionEnd.postTailStateHash;
    prepared->finalRollingDigest = parsed.file.footer.finalRollingDigest;

    result.ok = true;
    result.matchId = prepared->matchId;
    result.startFrame = prepared->startFrame;
    result.frameCount = static_cast<uint32_t>(prepared->frames.size());
    result.eventCount = static_cast<uint32_t>(prepared->events.size());
    result.prebuffered = prepared->prebuffered;
    result.lastFrame = prepared->frames.back().timelineFrame;
    result.detail = "prepared";
    prepared_ = std::move(prepared);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.state = BrokerState::Prepared;
    }
    return result;
}

bool ReplayBroker::start(const BrokerConfig& config) {
    if (!prepared_) return false;
    stop();
    config_ = config;

    wchar_t name[256];
    qipc::ipcReplayBrokerPipeName(name, 256, config.gamePid, config.generation);
    pipeName_ = name;

    HANDLE pipe = ::CreateNamedPipeW(
        pipeName_.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
        64u * 1024u, 64u * 1024u, config.connectTimeoutMs, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.state = BrokerState::Failed;
        status_.detail = "could not create the replay broker pipe";
        return false;
    }
    pipe_ = pipe;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.state = BrokerState::Listening;
        status_.generation = config.generation;
        status_.detail = "waiting for the replay process";
    }
    running_.store(true);
    worker_ = std::thread(&ReplayBroker::workerMain, this);
    return true;
}

void ReplayBroker::stop() {
    if (!running_.load() && !worker_.joinable()) {
        if (pipe_ != nullptr) {
            ::CloseHandle(static_cast<HANDLE>(pipe_));
            pipe_ = nullptr;
        }
        return;
    }
    running_.store(false);
    if (pipe_ != nullptr) {
        ::CancelIoEx(static_cast<HANDLE>(pipe_), nullptr);
        ::DisconnectNamedPipe(static_cast<HANDLE>(pipe_));
    }
    if (worker_.joinable()) worker_.join();
    if (pipe_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(pipe_));
        pipe_ = nullptr;
    }
}

BrokerStatus ReplayBroker::status() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return status_;
}

namespace {

bool writeAll(HANDLE pipe, const std::vector<uint8_t>& bytes) {
    size_t done = 0u;
    while (done < bytes.size()) {
        DWORD wrote = 0u;
        const DWORD ask =
            static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 65536u));
        if (::WriteFile(pipe, bytes.data() + done, ask, &wrote, nullptr) == 0)
            return false;
        if (wrote == 0u) return false;
        done += static_cast<size_t>(wrote);
    }
    return true;
}

}

void ReplayBroker::workerMain() {
    HANDLE pipe = static_cast<HANDLE>(pipe_);
    const BOOL connected = ::ConnectNamedPipe(pipe, nullptr);
    if (connected == 0 && ::GetLastError() != ERROR_PIPE_CONNECTED) {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.state = BrokerState::Failed;
        status_.detail = "the replay process never connected";
        running_.store(false);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.connected = true;
        status_.state = BrokerState::Serving;
        status_.detail = "serving";
    }
    const bool ok = serveConnection(pipe);
    std::lock_guard<std::mutex> guard(mutex_);
    status_.state = ok ? BrokerState::Finished : BrokerState::Failed;
    status_.connected = false;
    running_.store(false);
}

namespace {

constexpr size_t kFramesPerBlock = 256u;
static_assert(kFramesPerBlock <= kMaxRecordsPerInputBlock,
              "an input block may never exceed the container limit");

uint8_t laneMaskOf(const SessionManifest& manifest) {
    uint8_t mask = 0u;
    for (size_t i = 0; i < kSourceLanes && i < 4u; ++i)
        if (manifest.lanes[i].active) mask |= static_cast<uint8_t>(1u << i);
    return mask;
}

uint8_t slotMaskOf(const SessionManifest& manifest) {
    uint8_t mask = 0u;
    for (size_t i = 0; i < kFighterSlots && i < 4u; ++i)
        if (manifest.slots[i].ownership != SlotOwnership::Inactive)
            mask |= static_cast<uint8_t>(1u << i);
    return mask;
}

uint64_t contentDigestOf(const SessionManifest& manifest) {
    uint64_t value = 0u;
    for (size_t i = 0; i < 8u; ++i) {
        value |= static_cast<uint64_t>(manifest.contract.contentManifestRoot[i])
              << (i * 8u);
    }
    return value;
}

}

bool ReplayBroker::serveConnection(void* pipeHandle) {
    HANDLE pipe = static_cast<HANDLE>(pipeHandle);
    const Prepared& prepared = *prepared_;

    ReplayIpcSessionInfo info{};
    info.manifestDigest = config_.manifestDigest != 0u
        ? config_.manifestDigest
        : wire::sessionBindingDigest(
              prepared.matchId, prepared.startFrame,
              static_cast<uint32_t>(prepared.frames.size()), config_.nonce);
    info.sessionUuidLow = prepared.prefix.sessionUuidLow;
    info.sessionUuidHigh = prepared.prefix.sessionUuidHigh;
    info.simulationContractId = prepared.manifest.contract.runtimeSimulationId;
    info.contentDigest = contentDigestOf(prepared.manifest);
    info.matchId = prepared.matchId;
    info.matchOrdinal = prepared.matchOrdinal;
    info.firstTimelineFrame = prepared.startFrame;
    info.lastTimelineFrame = prepared.frames.empty()
        ? prepared.startFrame
        : prepared.frames.back().timelineFrame;
    info.totalFrames = static_cast<uint32_t>(prepared.frames.size());
    info.matchCount = prepared.matchCount;
    info.bootstrapKind = static_cast<uint8_t>(prepared.manifest.bootstrapKind);
    info.vsRow = prepared.manifest.vsRow;
    info.cpuMask = prepared.manifest.cpuMask;
    info.activeLaneMask = laneMaskOf(prepared.manifest);
    info.activeSlotMask = slotMaskOf(prepared.manifest);
    for (size_t i = 0; i < info.slotToSourceRoute.size(); ++i)
        info.slotToSourceRoute[i] = prepared.manifest.slotToSourceRoute[i];
    const DeterministicStart& chosenStart =
        prepared.useMatchStart ? prepared.matchStart.start
                               : prepared.sessionStart;
    info.bootstrapFingerprint = chosenStart.bootstrapHash;

    std::vector<uint8_t> sessionInfoBytes;
    if (!wire::encodeSessionInfo(info, sessionInfoBytes)) {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.detail = "this replay's session shape cannot be described "
                         "to the game";
        return false;
    }

    std::vector<uint8_t> inbox;
    std::vector<uint8_t> chunk;
    chunk.resize(64u * 1024u);
    uint32_t outSequence = 0u;
    uint32_t expectedInSequence = 0u;
    bool helloSeen = false;
    bool streamOpened = false;
    bool terminalSent = false;
    bool byeSeen = false;
    size_t frameCursor = 0u;
    size_t eventCursor = 0u;

    auto send = [&](ReplayIpcType type, uint32_t param0, uint32_t param1,
                    const std::vector<uint8_t>& payload) {
        ReplayIpcFrame frame{};
        frame.channel = ReplayIpcChannel::Broker;
        frame.type = type;
        frame.generation = config_.generation;
        frame.nonce = config_.nonce;
        frame.sequence = ++outSequence;
        frame.param0 = param0;
        frame.param1 = param1;
        frame.payload = payload;
        std::vector<uint8_t> bytes;
        if (!encodeIpcFrame(frame, bytes)) return false;
        return writeAll(pipe, bytes);
    };

    auto openStream = [&](uint32_t fromFrame) {
        info.firstTimelineFrame = fromFrame;
        std::vector<uint8_t> infoBytes;
        if (!wire::encodeSessionInfo(info, infoBytes)) return false;
        if (!send(ReplayIpcType::SessionInfo, 0u, 0u, infoBytes)) return false;

        DeterministicStart start = chosenStart;
        start.firstTimelineFrame = fromFrame;
        std::vector<uint8_t> startBytes;
        if (!encodeDeterministicStart(start, startBytes)) return false;
        return send(ReplayIpcType::SessionStart, 0u, 0u, startBytes);
    };

    auto positionAt = [&](uint32_t frame) {
        frameCursor = prepared.frames.size();
        for (size_t i = 0; i < prepared.frames.size(); ++i) {
            if (prepared.frames[i].timelineFrame == frame) {
                frameCursor = i;
                break;
            }
        }
        eventCursor = 0u;
        while (eventCursor < prepared.events.size()
               && prepared.events[eventCursor].timelineFrame < frame) {
            ++eventCursor;
        }
        return frameCursor < prepared.frames.size();
    };

    auto pushNext = [&]() -> bool {
        if (!streamOpened) {
            if (!openStream(prepared.frames.empty()
                                ? prepared.startFrame
                                : prepared.frames[frameCursor].timelineFrame)) {
                return false;
            }
            streamOpened = true;
            return true;
        }
        if (frameCursor < prepared.frames.size()) {
            const size_t take = std::min<size_t>(
                kFramesPerBlock, prepared.frames.size() - frameCursor);
            const uint32_t first = prepared.frames[frameCursor].timelineFrame;
            std::vector<uint8_t> block;
            if (!encodeInputBlock(&prepared.frames[frameCursor], take, block))
                return false;
            if (!send(ReplayIpcType::InputBlock, first,
                      static_cast<uint32_t>(take), block)) {
                return false;
            }
            const uint32_t upTo =
                prepared.frames[frameCursor + take - 1u].timelineFrame;

            std::vector<ReplayEvent> batch;
            while (eventCursor < prepared.events.size()
                   && prepared.events[eventCursor].timelineFrame <= upTo
                   && batch.size() < kMaxEventsPerBlock) {
                batch.push_back(prepared.events[eventCursor]);
                ++eventCursor;
            }
            if (!batch.empty()) {
                std::vector<uint8_t> eventBytes;
                if (!encodeEventBlock(batch.data(), batch.size(), eventBytes))
                    return false;
                if (!send(ReplayIpcType::EventBlock, 0u,
                          static_cast<uint32_t>(batch.size()), eventBytes)) {
                    return false;
                }
            }
            frameCursor += take;
            std::lock_guard<std::mutex> guard(mutex_);
            status_.servedFrames += static_cast<uint32_t>(take);
            status_.lastServedFrame = upTo;
            status_.buffering = !prepared.prebuffered
                && frameCursor >= prepared.frames.size();
            return true;
        }
        if (!terminalSent) {
            ReplayIpcTerminal terminal{};
            terminal.reason = static_cast<uint16_t>(prepared.terminalReason);
            terminal.lastTimelineFrame = prepared.frames.empty()
                ? prepared.startFrame
                : prepared.frames.back().timelineFrame;
            terminal.postTailStateHash = prepared.postTailStateHash;
            terminal.rollingDigest = prepared.finalRollingDigest;
            std::vector<uint8_t> payload;
            if (!wire::encodeTerminal(terminal, payload)) return false;
            if (!send(ReplayIpcType::Terminal, terminal.lastTimelineFrame, 0u,
                      payload)) {
                return false;
            }
            terminalSent = true;
        }
        return true;
    };

    auto refuse = [&](const char* why) {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.rejectedFrames += 1u;
        status_.detail = why;
    };

    while (running_.load() && !byeSeen) {
        DWORD available = 0u;
        if (::PeekNamedPipe(pipe, nullptr, 0u, nullptr, &available, nullptr)
            == 0) {
            break;
        }
        if (available > 0u) {
            DWORD got = 0u;
            const DWORD ask = static_cast<DWORD>(
                std::min<size_t>(chunk.size(), static_cast<size_t>(available)));
            if (::ReadFile(pipe, chunk.data(), ask, &got, nullptr) == 0
                || got == 0u) {
                break;
            }
            if (inbox.size() + got > 1u * 1024u * 1024u) {
                refuse("the replay process sent an oversized stream");
                return false;
            }
            inbox.insert(inbox.end(), chunk.begin(), chunk.begin() + got);
        }

        for (;;) {
            ReplayIpcFrame frame{};
            size_t consumed = 0u;
            const IpcFrameStatus status =
                decodeIpcFrame(inbox.data(), inbox.size(), frame, consumed);
            if (status == IpcFrameStatus::NeedMore) break;
            if (status != IpcFrameStatus::Ok) {
                refuse(ipcFrameStatusText(status));
                return false;
            }
            inbox.erase(inbox.begin(),
                        inbox.begin() + static_cast<ptrdiff_t>(consumed));

            if (frame.channel != ReplayIpcChannel::Broker
                || frame.generation != config_.generation
                || frame.nonce != config_.nonce
                || frame.sequence != expectedInSequence + 1u) {
                refuse("stale, forged, or out-of-order replay request");
                return false;
            }
            ++expectedInSequence;

            switch (frame.type) {
                case ReplayIpcType::Hello: {
                    if (helloSeen) {
                        refuse("duplicate replay handshake");
                        return false;
                    }
                    ReplayIpcHello hello{};
                    if (!wire::decodeHello(frame.payload.data(),
                                           frame.payload.size(), hello)
                        || hello.nonce != config_.nonce
                        || hello.gamePid != config_.gamePid
                        || hello.wireVersion != kReplayIpcWireVersion) {
                        refuse("replay handshake rejected");
                        return false;
                    }
                    helloSeen = true;
                    if (frame.param1 != 0u && !positionAt(frame.param1)) {
                        refuse("the game asked to resume at a frame this "
                               "replay does not contain");
                        return false;
                    }
                    break;
                }
                case ReplayIpcType::SeekRequest: {
                    if (!helloSeen) {
                        refuse("request before handshake");
                        return false;
                    }
                    if (!positionAt(frame.param1)) {
                        refuse("the requested frame is not in this replay");
                        return false;
                    }
                    streamOpened = false;
                    terminalSent = false;
                    break;
                }
                case ReplayIpcType::Bye: {
                    byeSeen = true;
                    break;
                }
                default: {
                    refuse("unexpected replay message");
                    return false;
                }
            }
            if (byeSeen) break;
        }

        if (byeSeen) break;
        if (helloSeen && !terminalSent) {
            if (!pushNext()) {
                refuse("the replay stream could not be written to the game");
                return false;
            }
        } else if (available == 0u) {
            ::Sleep(1);
        }
    }
    return true;
}

}
