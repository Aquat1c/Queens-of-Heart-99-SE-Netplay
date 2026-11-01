/* replayNextFrame(), replayInjectedAction(), and replayRecordConfirmed() must never log: QLOG is a buffered file write. */
#include "game/replay/ReplayRuntime.h"

#include "Log.h"
#include "game/PaletteBridge.h"
#include "input/InputDisplayOverlayBridge.h"
#include "input/InputDisplayPublisher.h"
#include "replay/CharacterCatalog.h"
#include "replay/GameEpochTracker.h"
#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"
#include "replay/ReplayNavigation.h"
#include "replay/ReplayWinScreen.h"
#include "replay/ReplayPlaybackController.h"
#include "replay/ReplayPublisherQueueV2.h"
#include "replay/ReplayRecorderQueue.h"
#include "replay/ReplaySimulationContractV2.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace qgame::replay {
namespace {

constexpr uint32_t kSeekRetainFrames = 4096u;
constexpr uint32_t kSeekBatchTicks = 16u;
constexpr uint32_t kFrameLogInterval = 300u;
constexpr uint64_t kRecordLogInterval = 600u;

uint64_t qpcFrequency() {
    static uint64_t freq = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart ? f.QuadPart : 1);
    }();
    return freq;
}

uint64_t nowMicros() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart) * 1000000ull / qpcFrequency();
}

enum class DeferredLog : uint8_t {
    None = 0,
    Buffering,
    Halt,
    SeekComplete,
};

struct PresentationSlot {
    uint32_t generation = 0u;
    uint32_t command = 0u;
    uint64_t argument = 0u;
};

struct RuntimeState {
    bool active = false;
    bool halted = false;
    bool complete = false;
    bool exitRequested = false;
    bool buffering = false;
    bool inlineWorker = false;

    ReplayIpcWorker worker;
    ReplayTimelineSource source;
    std::unique_ptr<qreplay::PlaybackController> controller;
    std::unique_ptr<qreplay::ReplayPlaybackSessionV2> sessionV2;
    qreplay::ReplayTakeoverController takeover;
    bool takeoverAvailable = false;
    bool takeoverSnapshotReady = false;
    uint8_t takeoverFallbackLaneMask = 3u;
    SimulationHooks hooks{};

    bool playbackV3 = false;
    bool headlessV3 = false;
    bool prebufferInstalledV3 = false;
    bool bootstrapCompleteV3 = false;
    qreplay::ReplayNavigation navigation;
    uint16_t presentationFlagsV3 = 0u;
    bool displayFlagsAppliedV3 = false;
    uint64_t resultReadyAtMsV3 = 0u;
    bool winScreenFinishedV3 = false;
    bool winScreenFailedV3 = false;
    bool nextGamePendingV3 = false;
    qreplay::ipc::v3::PlaybackNextGamePayload nextGameV3{};
    bool pausedV3 = false;
    uint32_t pendingStepsV3 = 0u;
    uint32_t speedPermilleV3 = qreplay::kSpeedNormal;
    bool collisionDisplayV3 = false;
    uint32_t speedCreditV3 = 0u;
    bool transportStatusDirtyV3 = false;
    uint64_t lastCommandIdV3 = 0u;
    uint64_t terminalPresentationSerialV3 = 0u;
    uint32_t terminalPresentationWaitSlotsV3 = 0u;
    bool resultQueuedV3 = false;
    bool byeQueuedV3 = false;
    uint64_t byeQueuedAtMsV3 = 0u;
    uint64_t lastStatusAtMsV3 = 0u;

    uint64_t manifestDigest = 0u;
    uint32_t matchOrdinal = 0u;
    uint32_t speedPermille = qreplay::kSpeedNormal;

    bool haveExpectedFrame = false;
    uint32_t expectedFrame = 0u;
    bool haveObservedPost = false;
    uint64_t lastObservedPost = 0u;
    uint64_t lastSlotMicros = 0u;
    uint64_t lastTickStartMicros = 0u;

    std::array<uint16_t, qreplay::kSourceLanes> latched{};
    uint8_t latchedLaneMask = 0u;
    bool latchValid = false;
    uint64_t latchTick = 0u;
    uint64_t injectedReads = 0u;
    uint32_t inputDisplayGeneration = 0u;

    uint32_t fastForwardRemaining = 0u;
    bool seekActive = false;
    SeekOutcome seek{};
    uint64_t seekStartMs = 0u;
    uint32_t seekFrom = 0u;
    uint32_t seekFastForwarded = 0u;

    ReplayHaltDiagnostic halt{};
    DeferredLog deferred = DeferredLog::None;
    char regionDetail[96]{};
    uint32_t framesSinceLog = 0u;
    bool logNextFrame = true;

    uint64_t ticksExecuted = 0u;
    uint64_t hiddenTicks = 0u;
    uint64_t presentedSlots = 0u;
    uint64_t underruns = 0u;

    BoundedSpscRing<PresentationSlot, kPresentationQueueCapacity> commands;
    std::atomic<uint32_t> presentationGeneration{0u};
    std::atomic<uint64_t> commandDrops{0u};
    bool transportProviderRegistered = false;
    uint64_t transportProviderRetryMsV3 = 0u;

    std::atomic<uint64_t> deviceCalls{0u};
    const char* firstDeviceCallSite = nullptr;
};

RuntimeState& rt() {
    static RuntimeState state;
    return state;
}

struct RecorderState {
    bool active = false;
    bool ownWorker = false;
    bool v3 = false;
    uint32_t generation = 0u;
    uint8_t sourceKind = 0u;
    uint64_t published = 0u;
    uint64_t lastLoggedPublished = 0u;
    std::unique_ptr<qreplay::ReplayRecorderQueue> queue;
    std::unique_ptr<qreplay::ReplayPublisherQueueV2> queueV2;
    std::unique_ptr<
        qreplay::ReplayRecordingCoordinatorV2> coordinatorV2;
    std::unique_ptr<ReplayIpcWorker> worker;

    qreplay::GameEpochTrackerV2 epochTracker{};
    qreplay::SimulationContractV2 contractV2{};
    std::array<
        qreplay::SourceLaneDescriptorV2,
        qreplay::kSourceLanes> lanesV2{};
    qreplay::DisplayMetadataV2 displayV2{};
    uint64_t seriesUuidLow = 0u;
    uint64_t seriesUuidHigh = 0u;
    uint32_t nextGameGeneration = 1u;
    uint32_t nextGameOrdinal = 0u;
    qreplay::GameIdentityV2 stagedIdentity{};
    qreplay::ConfirmedGameStartV2 stagedStart{};
    uint32_t stagedLaunchSourceFrame = 0u;
    qreplay::GameIdentityV2 activeIdentity{};
    qreplay::ReplayRecordingFaultV2 lastV2Fault =
        qreplay::ReplayRecordingFaultV2::None;

    bool manifestReady = false;
    bool manifestPublished = false;
    const char* manifestFailure = "not built";
    qreplay::RecorderMessage manifestMessage{};

    bool sessionStartPublished = false;
    uint32_t matchesStarted = 0u;
    uint32_t matchesEnded = 0u;
};

RecorderState& rec() {
    static RecorderState state;
    return state;
}

void disableRecordingGeneration(RecorderState& r, uint16_t reason) {
    if (r.queue) r.queue->disable();
    if (r.queueV2) r.queueV2->disable();
    if (r.worker) r.worker->disableRecording(reason);
}

void fillIdentity(ReplayHaltDiagnostic& diag) {
    RuntimeState& s = rt();
    const BrokerSessionInfo& info = s.source.sessionInfo();
    diag.matchId = info.matchId;
    diag.matchOrdinal = info.matchOrdinal;
    diag.sessionUuidLow = info.sessionUuidLow;
    diag.sessionUuidHigh = info.sessionUuidHigh;
    diag.manifestDigest = s.manifestDigest;
    diag.simulationContractId = info.simulationContractId;
    diag.contentDigest = info.contentDigest;
    diag.expectedLaneMask = s.source.startDescriptor().activeLaneMask;
    diag.sourceFault = static_cast<uint16_t>(s.source.fault());
    diag.timelineStatus = static_cast<uint16_t>(s.source.timelineStatus());
    diag.actualDigest = s.source.rollingDigest();
    std::memcpy(diag.regionDetail, s.regionDetail, sizeof(diag.regionDetail));
    diag.regionDetail[sizeof(diag.regionDetail) - 1u] = '\0';
}

void haltWith(HaltReason reason, uint32_t frame, uint64_t expected,
              uint64_t actual, const qreplay::ReplayFrameV1* context) {
    RuntimeState& s = rt();
    if (s.halted) return;
    s.halted = true;

    ReplayHaltDiagnostic diag{};
    diag.reason = reason;
    diag.replayFrame = frame;
    diag.expectedStateHash = expected;
    diag.actualStateHash = actual;
    if (context) {
        diag.lifecycleBefore = context->lifecycleBefore;
        diag.lifecycleAfter = context->lifecycleAfter;
        diag.matchSubstate = context->lifecycleBefore.matchState;
        diag.inputMasks = context->actions;
        diag.activeLaneMask = context->activeLaneMask;
        diag.expectedDigest = context->rollingDigest;
    }
    fillIdentity(diag);
    s.halt = diag;
    s.deferred = DeferredLog::Halt;
}

void haltFromSource() {
    RuntimeState& s = rt();
    if (s.halted) return;
    HaltReason reason = HaltReason::ChannelFault;
    switch (s.source.fault()) {
        case SourceFault::ManifestMismatch:
            reason = HaltReason::ManifestMismatch;
            break;
        case SourceFault::InvalidActionMask:
            reason = HaltReason::InvalidActionMask;
            break;
        case SourceFault::InvalidLaneShape:
            reason = HaltReason::LaneShapeMismatch;
            break;
        case SourceFault::DigestBreak:
            reason = HaltReason::DigestBreak;
            break;
        case SourceFault::FrameGap:
            reason = HaltReason::ExpectedFrameMismatch;
            break;
        default:
            reason = HaltReason::ChannelFault;
            break;
    }
    haltWith(reason, s.source.faultDetail(), 0u, 0u, nullptr);
}

void flushDeferredLogs() {
    RuntimeState& s = rt();
    switch (s.deferred) {
        case DeferredLog::None:
            return;
        case DeferredLog::Buffering:
            QLOG("[replay.play] local replay buffering frame=%u buffered=%u "
                 "underruns=%llu",
                 s.source.nextFrame(),
                 static_cast<unsigned>(s.source.bufferedFrames()),
                 static_cast<unsigned long long>(s.underruns));
            break;
        case DeferredLog::Halt:
            QLOG("[replay.verify] mismatch frame=%u expected=%016llX "
                 "actual=%016llX region=%s",
                 s.halt.replayFrame,
                 static_cast<unsigned long long>(s.halt.expectedStateHash),
                 static_cast<unsigned long long>(s.halt.actualStateHash),
                 s.halt.regionDetail[0] ? s.halt.regionDetail
                                        : haltReasonText(s.halt.reason));
            QLOG("[replay.verify] halt reason=%s session=%016llX match=%u "
                 "digest=%016llX lanes=%02X masks=%04X/%04X/%04X/%04X "
                 "phase=%u/%u substate=%u contract=%016llX content=%016llX",
                 haltReasonText(s.halt.reason),
                 static_cast<unsigned long long>(s.halt.sessionUuidLow),
                 s.halt.matchId,
                 static_cast<unsigned long long>(s.halt.actualDigest),
                 s.halt.activeLaneMask, s.halt.inputMasks[0],
                 s.halt.inputMasks[1], s.halt.inputMasks[2],
                 s.halt.inputMasks[3], s.halt.lifecycleBefore.scene,
                 s.halt.lifecycleBefore.sceneThreeState, s.halt.matchSubstate,
                 static_cast<unsigned long long>(s.halt.simulationContractId),
                 static_cast<unsigned long long>(s.halt.contentDigest));
            break;
        case DeferredLog::SeekComplete:
            QLOG("[replay.seek] from=%u target=%u checkpoint=%u "
                 "fastForwarded=%u elapsedMs=%u",
                 s.seekFrom, s.seek.targetFrame, s.seek.checkpointFrame,
                 s.seekFastForwarded,
                 static_cast<uint32_t>(GetTickCount64() - s.seekStartMs));
            break;
    }
    s.deferred = DeferredLog::None;
}

void drainPresentationCommands() {
    RuntimeState& s = rt();
    if (!s.controller) return;
    PresentationSlot slot{};
    while (s.commands.pop(slot)) {
        if (slot.generation != s.presentationGeneration.load(std::memory_order_acquire))
            continue;
        switch (static_cast<PresentationCommand>(slot.command)) {
            case PresentationCommand::Pause:
                s.controller->submit(qreplay::PlaybackCommand::Pause, 0u);
                break;
            case PresentationCommand::Resume:
                s.controller->submit(qreplay::PlaybackCommand::Resume, 0u);
                break;
            case PresentationCommand::TogglePause:
                s.controller->submit(s.controller->paused()
                                         ? qreplay::PlaybackCommand::Resume
                                         : qreplay::PlaybackCommand::Pause,
                                     0u);
                break;
            case PresentationCommand::Step:
                s.controller->submit(qreplay::PlaybackCommand::Step, 0u);
                break;
            case PresentationCommand::SetSpeedPermille:
                s.controller->submit(qreplay::PlaybackCommand::SpeedSet,
                                     slot.argument);
                break;
            case PresentationCommand::SeekMatch:
                s.controller->submit(qreplay::PlaybackCommand::SeekMatch,
                                     slot.argument);
                break;
            case PresentationCommand::SeekRound:
                s.controller->submit(qreplay::PlaybackCommand::SeekRound,
                                     slot.argument);
                break;
            case PresentationCommand::Restart:
                s.controller->submit(qreplay::PlaybackCommand::Restart,
                                     slot.argument);
                break;
            case PresentationCommand::NextMatch:
                s.controller->submit(qreplay::PlaybackCommand::NextMatch, 0u);
                break;
            case PresentationCommand::PreviousMatch:
                s.controller->submit(qreplay::PlaybackCommand::PreviousMatch,
                                     0u);
                break;
            case PresentationCommand::Exit:
                s.exitRequested = true;
                s.controller->submit(qreplay::PlaybackCommand::Exit, 0u);
                break;
            case PresentationCommand::SeekFrame: {
                const uint32_t target = (slot.argument > 0xFFFFFFFFull)
                    ? 0xFFFFFFFFu
                    : static_cast<uint32_t>(slot.argument);
                s.seekFrom = s.source.consumeCursor();
                s.seekStartMs = GetTickCount64();
                const SeekOutcome outcome = s.source.positionFor(target);
                if (!outcome.ok) {
                    haltWith(HaltReason::SeekFailed, target, 0u, 0u, nullptr);
                    break;
                }
                s.seek = outcome;
                s.seekActive = true;
                s.seekFastForwarded = 0u;
                s.fastForwardRemaining = outcome.fastForwardFrames;
                s.haveExpectedFrame = !outcome.requestedRestream;
                s.expectedFrame = outcome.checkpointFrame;
                s.haveObservedPost = false;
                s.logNextFrame = true;
                s.controller->resetDeadlineAfterPauseOrSeek();
                break;
            }
            case PresentationCommand::None:
            default:
                break;
        }
    }
}

std::unique_ptr<ByteChannel> adoptChannel(ByteChannel* raw) {
    return std::unique_ptr<ByteChannel>(raw);
}

bool ownExecutableSha256(std::array<uint8_t, 32>& out);

std::array<uint8_t, 32> contractExecutableSha256() {
    std::array<uint8_t, 32> digest{};
    ownExecutableSha256(digest);
    return digest;
}

bool buildOptions(const qipc::IpcConfig& cfg, ReplayIpcOptions& out,
                  bool playback, bool recording) {
    out = ReplayIpcOptions{};
    out.gamePid = static_cast<uint32_t>(GetCurrentProcessId());
    out.replayGeneration = cfg.replayChannelGeneration;
    out.recordGeneration = cfg.recordChannelGeneration;
    out.brokerNonce =
        playback ? cfg.replayBrokerNonce : cfg.recordChannelNonce;
    out.manifestDigest = cfg.replayManifestDigest;
    out.schemaVersion = kReplayIpcSchemaVersion;
    out.supersededStateSchema =
        (cfg.replayFeatureBits
         & qipc::kReplayPlaybackSupersededStateSchemaBit) != 0u;
    out.featureBits = static_cast<uint16_t>(
        cfg.replayFeatureBits
        & ~static_cast<uint16_t>(qipc::kReplayPlaybackOptionalFeatureBits));
    out.matchOrdinal = cfg.replayMatchOrdinal;
    out.playback = playback;
    out.recording = recording;
    if (playback
        && cfg.replayIpcSchemaVersion
            == qipc::kReplayPlaybackIpcSchemaVersion) {
        out.playbackV3 = true;
        out.schemaVersion = qreplay::ipc::v3::kWireVersion;
        out.runtimeCompatibilityId = qipc::kRuntimeCompatibilityId;
        out.playbackGame.gameUuidLow = cfg.replayGameUuidLow;
        out.playbackGame.gameUuidHigh = cfg.replayGameUuidHigh;
        out.playbackGame.gameGeneration = cfg.replayGameGeneration;
        out.playbackExpectedFrameCount = cfg.replayExpectedFrameCount;
        out.playbackExpectedEventCount = cfg.replayExpectedEventCount;
        out.playbackValidatedSourceByteCount =
            cfg.replayValidatedSourceByteCount;
        std::memcpy(
            out.playbackValidatedSourceRangeDigest.data(),
            cfg.replayValidatedSourceRangeDigest,
            out.playbackValidatedSourceRangeDigest.size());
        out.playbackEndpointKind = cfg.replayEndpointKind;
        out.playbackHeadless = cfg.replayHeadless != 0u;
        std::array<uint8_t, 32> contentRoot{};
        std::memcpy(
            contentRoot.data(), cfg.replayContentManifestRoot,
            contentRoot.size());
        out.playbackRuntimeContract =
            qreplay::makeSeSimulationContractV2(
                qipc::kRuntimeCompatibilityId,
                contractExecutableSha256(), contentRoot);
    }
    switch (cfg.mode) {
        case qipc::MODE_NETPLAY:
            out.recordSourceKind =
                static_cast<uint8_t>(qreplay::ReplaySourceKind::NetplayPlayer);
            break;
        case qipc::MODE_SPECTATOR:
            out.recordSourceKind =
                static_cast<uint8_t>(qreplay::ReplaySourceKind::SpectatorSave);
            break;
        default:
            out.recordSourceKind = static_cast<uint8_t>(
                qreplay::ReplaySourceKind::OfflineVsPlayer);
            break;
    }
    if (playback && cfg.replayIpcSchemaVersion != 0u
        && cfg.replayIpcSchemaVersion != out.schemaVersion)
        return false;
    return true;
}

bool transportMayHoldV3(const RuntimeState& s) {
    return !s.takeover.active() && !s.navigation.active && s.sessionV2 != nullptr
        && s.sessionV2->transportMayHold();
}

bool requestNavigationV3(RuntimeState& s, uint64_t target) {
    if (!s.bootstrapCompleteV3 || !transportMayHoldV3(s)
        || s.headlessV3 || target >= s.sessionV2->endpoint().nextRecord)
        return false;
    if (!s.navigation.request(static_cast<uint32_t>(target),
                              s.sessionV2->endpoint().nextRecord)) return false;
    s.pausedV3 = true;
    s.pendingStepsV3 = 0u;
    s.speedCreditV3 = replaySpeedInitialCreditV3(s.speedPermilleV3);
    s.transportStatusDirtyV3 = true;
    return true;
}

void applyDisplayFlagsV3(RuntimeState& s) {
    if (s.displayFlagsAppliedV3) return;
    const HMODULE wrapper = GetModuleHandleW(L"ddraw.dll");
    if (!wrapper) return;
    using Setter = void (__stdcall*)(DWORD);
    const auto setter = reinterpret_cast<Setter>(
        GetProcAddress(wrapper, "EFZDX9_SetReplayDisplayFlags"));
    if (setter) {
        setter(s.presentationFlagsV3 & qipc::kReplayDisplayFlagsMask);
        s.displayFlagsAppliedV3 = true;
    }
}

uint16_t currentDisplayFlagsV3(const RuntimeState& s) {
    uint16_t flags = s.presentationFlagsV3;
    const HMODULE wrapper = GetModuleHandleW(L"ddraw.dll");
    if (wrapper) {
        using Query = DWORD (__stdcall*)();
        const auto query = reinterpret_cast<Query>(
            GetProcAddress(wrapper, "EFZDX9_QueryReplayDisplayFlags"));
        if (query) flags = static_cast<uint16_t>(query());
    }
    return (flags & (qipc::kReplayShowInputs | qipc::kReplayShowTransport))
        | (s.collisionDisplayV3 ? qipc::kReplayShowHitboxes : 0u);
}

bool transportHoldingV3(const RuntimeState& s) {
    return s.pausedV3 && transportMayHoldV3(s);
}

void applyPlaybackSpeedV3(RuntimeState& s, uint32_t permille) {
    s.speedPermilleV3 = permille;
    s.speedPermille = s.speedPermilleV3;
    s.speedCreditV3 = replaySpeedInitialCreditV3(permille);
    s.transportStatusDirtyV3 = true;
}

void drainPresentationCommandsV3(RuntimeState& s) {
    PresentationSlot slot{};
    while (s.commands.pop(slot)) {
        if (slot.generation != s.presentationGeneration.load(std::memory_order_acquire))
            continue;
        const PresentationCommand command =
            static_cast<PresentationCommand>(slot.command);
        const bool live = s.sessionV2 != nullptr
            && !s.sessionV2->hasResult();
        const bool takeoverActive = s.takeover.active();
        const bool mayHold = live && transportMayHoldV3(s);
        bool applied = false;
        switch (command) {
            case PresentationCommand::Pause:
                applied = mayHold;
                if (applied) s.pausedV3 = true;
                break;
            case PresentationCommand::Resume:
                applied = live && !takeoverActive && !s.navigation.active;
                if (applied) {
                    s.pausedV3 = false;
                    s.pendingStepsV3 = 0u;
                }
                break;
            case PresentationCommand::TogglePause:
                if (s.pausedV3) {
                    applied = live && !takeoverActive && !s.navigation.active;
                    if (applied) {
                        s.pausedV3 = false;
                        s.pendingStepsV3 = 0u;
                    }
                } else {
                    applied = mayHold;
                    if (applied) s.pausedV3 = true;
                }
                break;
            case PresentationCommand::Step:
                applied = mayHold && s.bootstrapCompleteV3;
                if (applied) {
                    s.pausedV3 = true;
                    if (s.pendingStepsV3 != 0xFFFFFFFFu)
                        ++s.pendingStepsV3;
                }
                break;
            case PresentationCommand::SetSpeedPermille:
                applied = live && !takeoverActive
                    && qreplay::ipc::v3::isSupportedPlaybackSpeedV3(
                        slot.argument);
                if (applied)
                    applyPlaybackSpeedV3(
                        s, static_cast<uint32_t>(slot.argument));
                break;
            case PresentationCommand::SpeedStepSlower:
            case PresentationCommand::SpeedStepFaster: {
                uint32_t next = s.speedPermilleV3;
                applied = live && !takeoverActive
                    && qreplay::ipc::v3::nextPlaybackSpeedV3(
                        s.speedPermilleV3,
                        command == PresentationCommand::SpeedStepFaster,
                        next);
                if (applied) applyPlaybackSpeedV3(s, next);
                break;
            }
            case PresentationCommand::TakeoverP1:
            case PresentationCommand::TakeoverP2: {
                const uint8_t lane = command == PresentationCommand::TakeoverP1
                    ? 0u : 1u;
                if (takeoverActive) {
                    applied = s.takeoverSnapshotReady
                        && s.takeover.requestSwitch(lane);
                } else if (live && s.takeoverAvailable && !s.navigation.active
                           && s.bootstrapCompleteV3
                           && !s.sessionV2->headless()
                           && s.sessionV2->transportMayHold()
                           && s.sessionV2->nextRecord() >= 2u
                           && s.sessionV2->nextRecord()
                               < s.sessionV2->endpoint().nextRecord) {
                    const uint32_t branch = s.sessionV2->nextRecord();
                    const qreplay::CanonicalTickBundleV2* source =
                        s.sessionV2->tickAt(branch);
                    applied = source != nullptr && s.takeover.begin(
                        branch, source->frame.sourceFrame, lane);
                    if (applied) {
                        s.takeoverSnapshotReady = false;
                        s.pausedV3 = false;
                        s.pendingStepsV3 = 0u;
                        s.speedPermilleV3 = qreplay::kSpeedNormal;
                        s.speedPermille = qreplay::kSpeedNormal;
                        s.speedCreditV3 = 0u;
                    }
                }
                break;
            }
            case PresentationCommand::TakeoverRetry:
                applied = takeoverActive && s.takeoverSnapshotReady
                    && s.takeover.requestRetry();
                break;
            case PresentationCommand::TakeoverExit:
                if (takeoverActive && !s.takeoverSnapshotReady) {
                    const uint8_t delay = s.takeover.delay();
                    s.takeover.reset();
                    (void)s.takeover.setDelay(delay);
                    applied = true;
                } else {
                    applied = takeoverActive && s.takeover.requestExit();
                }
                break;
            case PresentationCommand::TakeoverDelayDecrease:
                applied = live && s.takeoverAvailable;
                if (applied) s.takeover.decreaseDelay();
                break;
            case PresentationCommand::TakeoverDelayIncrease:
                applied = live && s.takeoverAvailable;
                if (applied) s.takeover.increaseDelay();
                break;
            case PresentationCommand::TakeoverSetDelay:
                applied = live && s.takeoverAvailable
                    && slot.argument <= qreplay::kReplayTakeoverMaximumDelay;
                if (applied)
                    (void)s.takeover.setDelay(
                        static_cast<uint8_t>(slot.argument));
                break;
            case PresentationCommand::ToggleCollisionDisplay:
                applied = s.active;
                if (applied)
                    s.collisionDisplayV3 = !s.collisionDisplayV3;
                break;
            case PresentationCommand::Exit:
                applied = live && s.sessionV2->requestUserExit();
                break;
            case PresentationCommand::Restart:
                applied = requestNavigationV3(s, 0u);
                break;
            case PresentationCommand::StepBack:
                applied = s.sessionV2 && requestNavigationV3(
                    s, qreplay::ReplayNavigation::previousFrame(
                        s.sessionV2->nextRecord()));
                break;
            case PresentationCommand::SeekFrame:
                applied = requestNavigationV3(s, slot.argument);
                break;
            case PresentationCommand::SeekMatch:
            case PresentationCommand::SeekRound:
            case PresentationCommand::NextMatch:
            case PresentationCommand::PreviousMatch:
                applied = false;
                break;
            case PresentationCommand::None:
            default:
                applied = false;
                break;
        }
        if (applied) {
            s.transportStatusDirtyV3 = true;
        } else {
            s.commandDrops.fetch_add(1u, std::memory_order_relaxed);
            QLOG("[replay.v3] local presentation command rejected kind=%u "
                 "argument=%llu phase=%u",
                 slot.command,
                 static_cast<unsigned long long>(slot.argument),
                 static_cast<unsigned>(
                     s.sessionV2 ? s.sessionV2->phase()
                                 : qreplay::ReplayPlaybackPhaseV2::Bound));
        }
    }
}

const char* validateLegacyReplayTestLaunch(const qipc::IpcConfig& cfg) {
    if (cfg.isHost != 0u || cfg.localPlayer != 0u)
        return "replay has no player identity";
    if (cfg.hasSocket != 0u || cfg.hasSpectatorSocket != 0u
        || cfg.serveSpectators != 0u)
        return "replay cannot own a socket or serve spectators";
    if (cfg.cookieLow != 0u || cfg.cookieHigh != 0u
        || cfg.spectatorCookieLow != 0u
        || cfg.spectatorCookieHigh != 0u)
        return "replay carries no session cookie";
    if (cfg.peerAddrBE != 0u || cfg.peerPortBE != 0u)
        return "replay has no peer endpoint";
    if (cfg.trainingEnabled != 0u || cfg.recordEnabled != 0u
        || cfg.probeMode != 0u)
        return "replay cannot own training, recording, or probe state";
    if (cfg.replayChannelGeneration == 0u
        || cfg.replayBrokerNonce == 0u)
        return "replay requires a broker channel generation and nonce";
    return nullptr;
}

void resetPlaybackSessionV3(RuntimeState& s, uint16_t presentationFlags) {
    s.sessionV2.reset();
    s.takeover.reset();
    s.takeoverAvailable = false;
    s.takeoverSnapshotReady = false;
    s.prebufferInstalledV3 = false;
    s.bootstrapCompleteV3 = false;
    s.presentationFlagsV3 = presentationFlags;
    s.resultReadyAtMsV3 = 0u;
    s.winScreenFinishedV3 = false;
    s.winScreenFailedV3 = false;
    s.nextGamePendingV3 = false;
    s.nextGameV3 = {};
    s.navigation.reset(s.headlessV3
        || (presentationFlags & qipc::kReplayAutoStart) != 0u);
    s.pausedV3 = false;
    s.pendingStepsV3 = 0u;
    s.speedCreditV3 = replaySpeedInitialCreditV3(s.speedPermilleV3);
    s.transportStatusDirtyV3 = true;
    s.terminalPresentationSerialV3 = 0u;
    s.terminalPresentationWaitSlotsV3 = 0u;
    s.resultQueuedV3 = false;
    s.byeQueuedV3 = false;
    s.byeQueuedAtMsV3 = 0u;
    s.lastStatusAtMsV3 = 0u;
}

bool startPlayback(const qipc::IpcConfig& cfg,
                   std::unique_ptr<ByteChannel> broker,
                   const TimelineSourceOptions& sourceOptions,
                   bool inlineWorker,
                   const qreplay::SimulationContractV2* testContract = nullptr) {
    RuntimeState& s = rt();
    if (s.active) return false;

    if (cfg.mode != qipc::MODE_REPLAY) {
        QLOG("[replay.play] refused: mode=%u is not MODE_REPLAY", cfg.mode);
        return false;
    }
    const char* launchFailure = inlineWorker
        && cfg.replayIpcSchemaVersion == kReplayIpcSchemaVersion
        ? validateLegacyReplayTestLaunch(cfg)
        : qipc::validateReplayLaunch(cfg);
    if (launchFailure) {
        QLOG("[replay.play] refused: %s", launchFailure);
        return false;
    }
    if (!qreplay::isSupportedSpeedPermille(cfg.replaySpeedPermille)) {
        QLOG("[replay.play] refused: unsupported speed permille=%u",
             cfg.replaySpeedPermille);
        return false;
    }

    ReplayIpcOptions options{};
    if (!buildOptions(cfg, options, true, false)) {
        QLOG("[replay.play] refused: replay IPC schema mismatch (caster=%u "
             "runtime=%u)",
             cfg.replayIpcSchemaVersion, kReplayIpcSchemaVersion);
        return false;
    }
    if (inlineWorker && testContract)
        options.playbackRuntimeContract = *testContract;

    std::unique_ptr<ByteChannel> channel = std::move(broker);
    if (!channel) {
        wchar_t name[128];
        qipc::ipcReplayBrokerPipeName(name, 128, options.gamePid,
                                      options.replayGeneration);
        channel = makeNamedPipeChannel(name, options.writeTimeoutMs);
    }
    if (!channel) return false;

    const bool ok = inlineWorker
        ? s.worker.startForTest(options, std::move(channel), nullptr, nullptr)
        : s.worker.start(options, std::move(channel), nullptr, nullptr);
    if (!ok) {
        QLOG("[replay.play] refused: replay IPC worker did not start");
        return false;
    }

    s.playbackV3 = options.playbackV3;
    s.headlessV3 = options.playbackHeadless;
    if (!s.playbackV3) {
        if (!s.source.start(s.worker, cfg.replayManifestDigest,
                            sourceOptions)) {
            s.worker.stop();
            return false;
        }

        qreplay::PlaybackTuning tuning{};
        s.controller.reset(new (std::nothrow) qreplay::PlaybackController(
            s.source.timeline(), tuning));
        if (!s.controller) {
            s.source.stop();
            s.worker.stop();
            return false;
        }
        s.controller->beginBootstrap();
        if (cfg.replaySpeedPermille != qreplay::kSpeedNormal)
            s.controller->submit(qreplay::PlaybackCommand::SpeedSet,
                                 cfg.replaySpeedPermille);
    }

    s.manifestDigest = cfg.replayManifestDigest;
    s.matchOrdinal = cfg.replayMatchOrdinal;
    s.speedPermille = cfg.replaySpeedPermille;
    s.speedPermilleV3 = cfg.replaySpeedPermille;
    s.inlineWorker = inlineWorker;
    s.halted = false;
    s.complete = false;
    s.exitRequested = false;
    s.buffering = false;
    s.haveExpectedFrame = false;
    s.haveObservedPost = false;
    s.latchValid = false;
    s.latchedLaneMask = 0u;
    s.latched.fill(0u);
    ++s.inputDisplayGeneration;
    qinput::inputDisplay().reset(s.inputDisplayGeneration);
    s.fastForwardRemaining = 0u;
    s.seekActive = false;
    s.ticksExecuted = 0u;
    s.hiddenTicks = 0u;
    s.presentedSlots = 0u;
    s.underruns = 0u;
    s.framesSinceLog = 0u;
    s.logNextFrame = true;
    s.halt = ReplayHaltDiagnostic{};
    s.deferred = DeferredLog::None;
    s.lastSlotMicros = nowMicros();
    s.commands.reset();
    s.presentationGeneration.fetch_add(1u, std::memory_order_release);
    s.deviceCalls.store(0u, std::memory_order_release);
    s.firstDeviceCallSite = nullptr;
    resetPlaybackSessionV3(s, cfg.replayPresentationFlags);
    s.displayFlagsAppliedV3 = false;
    s.collisionDisplayV3 =
        (cfg.replayPresentationFlags & qipc::kReplayShowHitboxes) != 0u;
    s.lastCommandIdV3 = 0u;
    s.active = true;

    QLOG("[replay.play] armed manifest=%016llX match=%u speed=%u schema=%u "
         "generation=%u",
         static_cast<unsigned long long>(cfg.replayManifestDigest),
         cfg.replayMatchOrdinal, cfg.replaySpeedPermille,
         options.schemaVersion, cfg.replayChannelGeneration);
    return true;
}

constexpr uint64_t kMaxExecutableHashBytes = 256ull * 1024ull * 1024ull;

bool ownExecutableSha256(std::array<uint8_t, 32>& out) {
    static bool computed = false;
    static bool ok = false;
    static std::array<uint8_t, 32> digest{};
    if (!computed) {
        computed = true;
        wchar_t path[MAX_PATH * 2];
        const DWORD length = GetModuleFileNameW(
            nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
        if (length != 0u && length < (sizeof(path) / sizeof(path[0]))) {
            const HANDLE file = CreateFileW(
                path, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                qreplay::Sha256 sha;
                std::vector<uint8_t> buffer(64u * 1024u);
                uint64_t total = 0u;
                bool failed = false;
                for (;;) {
                    DWORD got = 0u;
                    if (!ReadFile(file, buffer.data(),
                                  static_cast<DWORD>(buffer.size()), &got,
                                  nullptr)) {
                        failed = true;
                        break;
                    }
                    if (got == 0u) break;
                    total += got;
                    if (total > kMaxExecutableHashBytes) {
                        failed = true;
                        break;
                    }
                    sha.update(buffer.data(), got);
                }
                CloseHandle(file);
                if (!failed && total != 0u) {
                    sha.finish(digest);
                    ok = true;
                }
            }
        }
        if (!ok) {
            QLOG("[replay.record] executable digest unavailable; the manifest "
                 "states an empty executable identity rather than a guess");
        }
    }
    out = digest;
    return ok;
}

int64_t nowUnixSeconds() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32)
                         | static_cast<uint64_t>(ft.dwLowDateTime);
    return static_cast<int64_t>(ticks / 10000000ull) - 11644473600ll;
}

int32_t localTimezoneOffsetMinutes() {
    TIME_ZONE_INFORMATION zone{};
    const DWORD id = GetTimeZoneInformation(&zone);
    if (id == TIME_ZONE_ID_INVALID) return 0;
    const LONG bias = zone.Bias
        + (id == TIME_ZONE_ID_DAYLIGHT ? zone.DaylightBias
                                       : zone.StandardBias);
    long offset = -static_cast<long>(bias);
    if (offset < -1440) offset = -1440;
    if (offset > 1440) offset = 1440;
    return static_cast<int32_t>(offset);
}

uint64_t splitmix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

void makeSessionUuid(const qipc::IpcConfig& cfg, uint64_t& low,
                     uint64_t& high) {
    static uint64_t counter = 0u;
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t wall = (static_cast<uint64_t>(ft.dwHighDateTime) << 32)
                        | static_cast<uint64_t>(ft.dwLowDateTime);
    const uint64_t seed = static_cast<uint64_t>(GetCurrentProcessId())
        ^ (static_cast<uint64_t>(cfg.sessionId) << 32)
        ^ static_cast<uint64_t>(cfg.recordChannelGeneration)
        ^ static_cast<uint64_t>(qpc.QuadPart)
        ^ wall ^ (++counter * 0xD1B54A32D192ED03ull);
    low = splitmix64(seed);
    high = splitmix64(low ^ wall ^ 0x0F1E2D3C4B5A6978ull);
}

std::string canonicalLabel(const char* text, size_t capacity) {
    if (text == nullptr) return std::string();
    size_t length = 0u;
    while (length < capacity && text[length] != '\0') ++length;
    if (length == 0u || length > qreplay::kMaxLabelBytes) return std::string();
    const std::string candidate(text, length);
    if (!qreplay::isCanonicalReplayText(candidate)) return std::string();
    return candidate;
}

bool buildRecorderManifest(const qipc::IpcConfig& cfg,
                           qreplay::ReplayPrefix& prefix,
                           qreplay::SessionManifest& manifest) {
    prefix = qreplay::ReplayPrefix{};
    manifest = qreplay::SessionManifest{};

    ownExecutableSha256(manifest.contract.executableSha256);
    manifest.contract.runtimeSimulationId = qipc::kRuntimeCompatibilityId;
    manifest.contract.characterCatalogId =
        qreplay::CharacterCatalog::forSeBuild().catalogId();
    manifest.contract.fpuControlWord = 0x027Fu;
    manifest.contract.mxcsr = 0x1F80u;

    const bool network = cfg.mode == qipc::MODE_NETPLAY
                      || cfg.mode == qipc::MODE_SPECTATOR;
    manifest.bootstrapKind = network
        ? qreplay::BootstrapKind::NetworkVsDirectBootV1
        : qreplay::BootstrapKind::OfflineVsDirectBootV1;

    const uint8_t vsRow = network ? qipc::kVsRow1v1 : cfg.vsRow;
    if (vsRow != qipc::kVsRow1v1 && vsRow != qipc::kVsRow1vCpu) return false;
    const bool cpuOpponent = (vsRow == qipc::kVsRow1vCpu);

    manifest.vsRow = vsRow;
    manifest.cpuMask = cpuOpponent ? 0x02u : 0x00u;
    manifest.cpuContract = cpuOpponent
        ? qreplay::CpuContract::NativeAiDeterministicV1
        : qreplay::CpuContract::None;
    manifest.slotToSourceRoute = {{0u, 1u, 2u, 3u}};
    manifest.rosterEpoch = 0u;

    std::string laneNames[qreplay::kSourceLanes];
    const std::string localLabel = canonicalLabel(cfg.localName,
                                                  sizeof(cfg.localName));
    const std::string remoteLabel = canonicalLabel(cfg.remoteName,
                                                   sizeof(cfg.remoteName));
    const size_t localLane = (cfg.localPlayer == 2u) ? 1u : 0u;
    laneNames[localLane] = localLabel;
    if (network) laneNames[localLane ^ 1u] = remoteLabel;

    const size_t activeLanes = cpuOpponent ? 1u : 2u;
    for (size_t lane = 0; lane < qreplay::kSourceLanes; ++lane) {
        manifest.lanes[lane].active = lane < activeLanes;
        manifest.lanes[lane].name =
            manifest.lanes[lane].active ? laneNames[lane] : std::string();
    }
    for (size_t slot = 0; slot < qreplay::kFighterSlots; ++slot) {
        qreplay::FighterSlotDescriptor& descriptor = manifest.slots[slot];
        if (slot >= 2u) {
            descriptor.ownership = qreplay::SlotOwnership::Inactive;
            descriptor.identity.nativeCharacterId =
                qreplay::kInactiveCharacterId;
            continue;
        }
        descriptor.ownership = (cpuOpponent && slot == 1u)
            ? qreplay::SlotOwnership::NativeCpu
            : qreplay::SlotOwnership::RecordedHuman;
        descriptor.side = static_cast<uint8_t>(slot % 2u);
        descriptor.identity.nativeCharacterId = qreplay::kInactiveCharacterId;
        descriptor.selectorProofPresent = false;
    }

    qreplay::DisplayMetadata& display = manifest.display;
    display.recordedUnixSeconds = nowUnixSeconds();
    if (display.recordedUnixSeconds < 0) display.recordedUnixSeconds = 0;
    display.timezoneOffsetMinutes = localTimezoneOffsetMinutes();
    switch (cfg.mode) {
        case qipc::MODE_NETPLAY:
            display.sourceKind = qreplay::ReplaySourceKind::NetplayPlayer;
            break;
        case qipc::MODE_SPECTATOR:
            display.sourceKind = qreplay::ReplaySourceKind::SpectatorSave;
            break;
        default:
            display.sourceKind = qreplay::ReplaySourceKind::OfflineVsPlayer;
            break;
    }
    if (network) {
        display.networkMetadataPresent = true;
        const uint16_t localDelay = cfg.delay;
        const uint16_t remoteDelay = cfg.remoteDelay;
        display.playerOneDelay = (localLane == 0u) ? localDelay : remoteDelay;
        display.playerTwoDelay = (localLane == 0u) ? remoteDelay : localDelay;
        display.rollbackWindow = cfg.rollback;
        if (cfg.prelaunchRouteSamples != 0u) {
            display.averageRttMicros = cfg.prelaunchRouteP50Us;
            display.lossPermille = cfg.prelaunchRouteLossPerMille > 1000u
                ? 1000u
                : cfg.prelaunchRouteLossPerMille;
        }
    }

    prefix.containerMajor = qreplay::kContainerVersionMajor;
    prefix.containerMinor = qreplay::kContainerVersionMinor;
    prefix.minimumReaderMajor = qreplay::kContainerReaderMajor;
    prefix.prefixBytes = static_cast<uint16_t>(qreplay::kPrefixBytes);
    prefix.featureFlags = cpuOpponent ? qreplay::kFeatureCpuFighters : 0u;
    makeSessionUuid(cfg, prefix.sessionUuidLow, prefix.sessionUuidHigh);
    return true;
}

void prepareRecorderManifest(RecorderState& r, const qipc::IpcConfig& cfg) {
    r.manifestReady = false;
    r.manifestPublished = false;
    r.manifestFailure = "not built";
    r.manifestMessage = qreplay::RecorderMessage{};

    qreplay::ReplayPrefix prefix{};
    qreplay::SessionManifest manifest{};
    if (!buildRecorderManifest(cfg, prefix, manifest)) {
        r.manifestFailure = "this launch shape has no honest manifest";
        return;
    }

    std::vector<uint8_t> encoded;
    encoded.reserve(qreplay::kMaxRecorderManifestBytes);
    if (!qreplay::encodeSessionManifest(manifest, encoded)) {
        r.manifestFailure = "the manifest failed its own encoder";
        return;
    }
    if (encoded.empty()
        || encoded.size() > qreplay::kMaxRecorderManifestBytes) {
        r.manifestFailure = "the manifest exceeds the publication carrier";
        QLOG("[replay.record] manifest is %u bytes, carrier holds %u",
             static_cast<unsigned>(encoded.size()),
             static_cast<unsigned>(qreplay::kMaxRecorderManifestBytes));
        return;
    }

    r.manifestMessage.kind = qreplay::RecorderMessageKind::Manifest;
    r.manifestMessage.manifest.prefix = prefix;
    r.manifestMessage.manifest.bytes = static_cast<uint32_t>(encoded.size());
    std::memcpy(r.manifestMessage.manifest.encoded.data(), encoded.data(),
                encoded.size());
    r.manifestReady = true;
    r.manifestFailure = "";
    uint8_t activeLaneMask = 0u;
    for (size_t lane = 0; lane < qreplay::kSourceLanes; ++lane) {
        if (manifest.lanes[lane].active)
            activeLaneMask |= static_cast<uint8_t>(1u << lane);
    }
    bool exeDigestKnown = false;
    for (uint8_t byte : manifest.contract.executableSha256)
        if (byte != 0u) exeDigestKnown = true;
    QLOG("[replay.record] manifest prepared bytes=%u session=%016llX%016llX "
         "bootstrap=%u vsRow=%u cpuMask=%02X lanes=%02X source=%u "
         "exeDigest=%s",
         static_cast<unsigned>(encoded.size()),
         static_cast<unsigned long long>(prefix.sessionUuidHigh),
         static_cast<unsigned long long>(prefix.sessionUuidLow),
         static_cast<unsigned>(manifest.bootstrapKind), manifest.vsRow,
         manifest.cpuMask, activeLaneMask,
         static_cast<unsigned>(manifest.display.sourceKind),
         exeDigestKnown ? "present" : "absent");
}

bool prepareRecorderV2LaunchState(
    RecorderState& r, const qipc::IpcConfig& cfg) {
    std::array<uint8_t, 32> contentRoot{};
    std::memcpy(
        contentRoot.data(), cfg.replayContentManifestRoot,
        contentRoot.size());
    r.contractV2 = qreplay::makeSeSimulationContractV2(
        qipc::kRuntimeCompatibilityId,
        contractExecutableSha256(), contentRoot);
    if (!qreplay::isValidSimulationContractV2(r.contractV2))
        return false;

    r.lanesV2 = {};
    const bool network = cfg.mode == qipc::MODE_NETPLAY
                      || cfg.mode == qipc::MODE_SPECTATOR;
    const size_t localLane = cfg.localPlayer == 2u ? 1u : 0u;
    std::array<std::string, qreplay::kSourceLanes> labels{};
    labels[localLane] =
        canonicalLabel(cfg.localName, sizeof(cfg.localName));
    if (network) {
        labels[localLane ^ 1u] =
            canonicalLabel(cfg.remoteName, sizeof(cfg.remoteName));
    }
    for (size_t lane = 0u; lane < qreplay::kSourceLanes; ++lane) {
        r.lanesV2[lane].active = lane < 2u;
        if (r.lanesV2[lane].active
            && !qreplay::setReplayTextV2(
                r.lanesV2[lane].name, labels[lane])) {
            return false;
        }
    }

    r.displayV2 = qreplay::DisplayMetadataV2{};
    r.displayV2.recordedUnixSeconds = nowUnixSeconds();
    if (r.displayV2.recordedUnixSeconds < 0)
        r.displayV2.recordedUnixSeconds = 0;
    r.displayV2.timezoneOffsetMinutes =
        localTimezoneOffsetMinutes();
    r.displayV2.flags =
        qreplay::DisplayMetadataSeriesV2;
    switch (cfg.mode) {
        case qipc::MODE_NETPLAY:
            r.displayV2.sourceKind =
                qreplay::ReplaySourceKindV2::NetplayPlayer;
            break;
        case qipc::MODE_SPECTATOR:
            r.displayV2.sourceKind =
                qreplay::ReplaySourceKindV2::SpectatorSave;
            break;
        default:
            r.displayV2.sourceKind =
                qreplay::ReplaySourceKindV2::OfflineVsPlayer;
            break;
    }
    if (network) {
        r.displayV2.flags |=
            qreplay::DisplayMetadataNetworkV2;
        const uint16_t localDelay = cfg.delay;
        const uint16_t remoteDelay = cfg.remoteDelay;
        r.displayV2.playerOneDelay =
            localLane == 0u ? localDelay : remoteDelay;
        r.displayV2.playerTwoDelay =
            localLane == 0u ? remoteDelay : localDelay;
        r.displayV2.rollbackWindow = cfg.rollback;
        if (cfg.prelaunchRouteSamples != 0u) {
            r.displayV2.averageRttMicros =
                cfg.prelaunchRouteP50Us;
            r.displayV2.jitterMicros =
                cfg.prelaunchRouteJitterUs;
            r.displayV2.lossPermille =
                (std::min)(
                    static_cast<uint32_t>(
                        cfg.prelaunchRouteLossPerMille),
                    1000u);
        }
    }

    makeSessionUuid(
        cfg, r.seriesUuidLow, r.seriesUuidHigh);
    if (r.seriesUuidLow == 0u && r.seriesUuidHigh == 0u)
        r.seriesUuidLow = 1u;
    r.displayV2.seriesUuidLow = r.seriesUuidLow;
    r.displayV2.seriesUuidHigh = r.seriesUuidHigh;
    r.nextGameGeneration = 1u;
    r.nextGameOrdinal = 0u;
    r.stagedIdentity = qreplay::GameIdentityV2{};
    r.stagedStart = qreplay::ConfirmedGameStartV2{};
    r.stagedLaunchSourceFrame = 0u;
    r.activeIdentity = qreplay::GameIdentityV2{};
    r.lastV2Fault = qreplay::ReplayRecordingFaultV2::None;
    r.epochTracker.reset();
    return true;
}

bool startRecorder(const qipc::IpcConfig& cfg,
                   std::unique_ptr<ByteChannel> channelIn, size_t queueCapacity,
                   bool inlineWorker, const bool useV3) {
    RecorderState& r = rec();
    if (r.active) return false;
    if (cfg.mode == qipc::MODE_REPLAY) return false;
    if (!cfg.recordEnabled) return false;
    if (const char* why = qipc::validateRecorderLaunch(cfg)) {
        QLOG("[replay.record] generation=%u source=- session=- match=- "
             "state=refused(%s)", cfg.recordChannelGeneration, why);
        return false;
    }

    ReplayIpcOptions options{};
    if (!buildOptions(cfg, options, false, true))
        return false;
    options.recordingV3 = useV3;
    options.recorderNonce = cfg.recordChannelNonce;
    options.runtimeCompatibilityId =
        qipc::kRuntimeCompatibilityId;

    std::unique_ptr<ByteChannel> channel = std::move(channelIn);
    if (!channel) {
        wchar_t name[128];
        qipc::ipcRecorderPipeName(name, 128, options.gamePid,
                                  options.recordGeneration);
        channel = makeNamedPipeChannel(name, options.writeTimeoutMs);
    }
    if (!channel) return false;

    r.v3 = useV3;
    if (useV3) {
        r.queueV2.reset(new (std::nothrow)
            qreplay::ReplayPublisherQueueV2(queueCapacity));
        if (!r.queueV2 || !r.queueV2->valid()) {
            r.queueV2.reset();
            return false;
        }
        r.queueV2->beginGeneration(
            cfg.recordChannelGeneration);
        r.coordinatorV2.reset(new (std::nothrow)
            qreplay::ReplayRecordingCoordinatorV2(
                *r.queueV2));
        if (!r.coordinatorV2
            || !prepareRecorderV2LaunchState(r, cfg)) {
            r.coordinatorV2.reset();
            r.queueV2.reset();
            return false;
        }
        r.coordinatorV2->reset(true);
    } else {
        r.queue.reset(new (std::nothrow)
                          qreplay::ReplayRecorderQueue(queueCapacity));
        if (!r.queue) return false;
        r.queue->beginGeneration(cfg.recordChannelGeneration);
    }

    r.worker.reset(new (std::nothrow) ReplayIpcWorker());
    if (!r.worker) {
        r.coordinatorV2.reset();
        r.queueV2.reset();
        r.queue.reset();
        return false;
    }

    const bool ok = inlineWorker
        ? r.worker->startForTest(options, nullptr, std::move(channel),
                                 r.queue.get(), r.queueV2.get())
        : r.worker->start(
            options, nullptr, std::move(channel),
            r.queue.get(), r.queueV2.get());
    if (!ok) {
        r.worker.reset();
        r.coordinatorV2.reset();
        r.queueV2.reset();
        r.queue.reset();
        QLOG("[replay.record] generation=%u source=%u session=- match=- "
             "state=worker-start-failed",
             cfg.recordChannelGeneration, options.recordSourceKind);
        return false;
    }

    r.generation = cfg.recordChannelGeneration;
    r.sourceKind = options.recordSourceKind;
    r.published = 0u;
    r.lastLoggedPublished = 0u;
    r.ownWorker = true;
    r.active = true;
    r.sessionStartPublished = false;
    r.matchesStarted = 0u;
    r.matchesEnded = 0u;

    if (!useV3)
        prepareRecorderManifest(r, cfg);

    QLOG("[replay.record] generation=%u source=%u session=%016llX match=%u "
         "state=armed protocol=%s manifest=%s",
         r.generation, r.sourceKind,
         static_cast<unsigned long long>(cfg.replayManifestDigest),
         cfg.replayMatchOrdinal,
         useV3 ? "wire3/q99r2" : "wire2/q99r1",
         useV3 ? "per-game-at-boundary"
               : (r.manifestReady ? "ready" : r.manifestFailure));
    return true;
}

void submitPlaybackResultIfReadyV3(RuntimeState& s);
void submitPlaybackStatusV3(RuntimeState& s, bool force);
void servicePlaybackV3(RuntimeState& s);
uint8_t playbackPhaseByteV3(const RuntimeState& s);

class TransportStatusPublisher {
public:
    void publish(const ReplayTransportStatusV2& source) {
        ReplayTransportStatusV2 snapshot = source;
        snapshot.structBytes =
            static_cast<uint32_t>(sizeof(ReplayTransportStatusV2));
        snapshot.apiVersion = kReplayTransportApiVersion;

        writerIndex_ ^= 1u;
        Buffer& buffer = buffers_[writerIndex_];
        uint32_t sequence = buffer.sequence.load(std::memory_order_relaxed);
        if (sequence & 1u) ++sequence;
        buffer.sequence.store(sequence + 1u, std::memory_order_seq_cst);

        uint32_t words[kWordCount] = {};
        std::memcpy(words, &snapshot, sizeof(snapshot));
        for (size_t i = 0; i < kWordCount; ++i)
            buffer.words[i].store(words[i], std::memory_order_seq_cst);

        buffer.sequence.store(sequence + 2u, std::memory_order_seq_cst);
        publishedIndex_.store(writerIndex_, std::memory_order_seq_cst);
    }

    bool read(ReplayTransportStatusV2& out) const {
        for (uint32_t attempt = 0; attempt < 8u; ++attempt) {
            const uint32_t index =
                publishedIndex_.load(std::memory_order_seq_cst) & 1u;
            const Buffer& buffer = buffers_[index];
            const uint32_t before =
                buffer.sequence.load(std::memory_order_seq_cst);
            if (before & 1u) continue;

            uint32_t words[kWordCount] = {};
            for (size_t i = 0; i < kWordCount; ++i)
                words[i] = buffer.words[i].load(std::memory_order_seq_cst);

            const uint32_t after =
                buffer.sequence.load(std::memory_order_seq_cst);
            const uint32_t finalIndex =
                publishedIndex_.load(std::memory_order_seq_cst) & 1u;
            if (before != after || (after & 1u) || index != finalIndex)
                continue;

            ReplayTransportStatusV2 candidate{};
            std::memcpy(&candidate, words, sizeof(candidate));
            if (candidate.apiVersion != kReplayTransportApiVersion
                || candidate.structBytes != sizeof(candidate)) {
                return false;
            }
            out = candidate;
            return true;
        }
        return false;
    }

private:
    static constexpr size_t kWordCount =
        sizeof(ReplayTransportStatusV2) / sizeof(uint32_t);
    static_assert(sizeof(ReplayTransportStatusV2) % sizeof(uint32_t) == 0,
                  "the transport status ABI must be a u32 word sequence");
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "transport publication requires lock-free u32 atomics");

    struct Buffer {
        std::atomic<uint32_t> sequence{0u};
        std::array<std::atomic<uint32_t>, kWordCount> words{};
    };

    Buffer buffers_[2];
    std::atomic<uint32_t> publishedIndex_{0u};
    uint32_t writerIndex_ = 0u;
};

TransportStatusPublisher& transportPublisher() {
    static TransportStatusPublisher publisher;
    return publisher;
}

void publishTransportStatusV3(const RuntimeState& s) {
    ReplayTransportStatusV2 status{};
    status.generation =
        s.presentationGeneration.load(std::memory_order_acquire);
    status.active = s.active ? 1u : 0u;
    status.paused = s.active
        && (transportHoldingV3(s)
            || (s.takeover.active() && !s.takeover.simulating())) ? 1u : 0u;
    status.mayHold = s.active && transportMayHoldV3(s) ? 1u : 0u;
    status.speedPermille = s.speedPermilleV3;
    status.pendingSteps = s.pendingStepsV3;
    status.phase = playbackPhaseByteV3(s);
    const uint64_t drops = s.commandDrops.load(std::memory_order_relaxed);
    status.droppedCommands = drops > 0xFFFFFFFFull
        ? 0xFFFFFFFFu : static_cast<uint32_t>(drops);
    if (s.sessionV2) {
        status.nextRecord = s.sessionV2->nextRecord();
        status.endpointRecord = s.sessionV2->endpoint().nextRecord;
    }
    status.takeoverAvailable = s.active && s.takeoverAvailable ? 1u : 0u;
    status.takeoverPhase = static_cast<uint32_t>(s.takeover.phase());
    status.takeoverLane = s.takeover.active()
        ? static_cast<uint32_t>(s.takeover.selectedLane()) + 1u : 0u;
    status.takeoverDelay = s.takeover.delay();
    status.takeoverCountdown = s.takeover.countdown();
    status.takeoverBranchRecord = s.takeover.branchReplayFrame();
    status.takeoverElapsed = s.takeover.elapsedFrames();
    status.takeoverBeyondEndpoint = s.sessionV2 && s.takeover.active()
        && s.takeover.currentReplayFrame()
            >= s.sessionV2->endpoint().nextRecord ? 1u : 0u;
    status.takeoverFault = static_cast<uint32_t>(s.takeover.fault());
    transportPublisher().publish(status);
}

int __stdcall transportStatusThunkV2(ReplayTransportStatusV2* out,
                                     uint32_t structBytes, void*) {
    if (!out || structBytes < sizeof(ReplayTransportStatusV2)) return 0;
    ReplayTransportStatusV2 status{};
    if (!transportPublisher().read(status)) return 0;
    *out = status;
    return 1;
}

int __stdcall transportSubmitThunkV2(uint32_t generation, uint32_t command,
                                     uint64_t argument, void*) {
    return replaySubmitPresentationCommand(generation, command, argument)
        ? 1 : 0;
}

RegisterReplayTransportProviderFnV2 transportRegisterExport() {
    HMODULE wrapper = GetModuleHandleW(L"ddraw.dll");
    if (!wrapper) return nullptr;
    return reinterpret_cast<RegisterReplayTransportProviderFnV2>(
        reinterpret_cast<void*>(
            GetProcAddress(wrapper,
                           kRegisterReplayTransportProviderExport)));
}

void tryRegisterTransportProviderV3(RuntimeState& s) {
    if (!s.playbackV3 || s.transportProviderRegistered) return;
    const uint64_t now = GetTickCount64();
    if (s.transportProviderRetryMsV3 != 0u
        && now - s.transportProviderRetryMsV3 < 500u) {
        return;
    }
    s.transportProviderRetryMsV3 = now;
    const RegisterReplayTransportProviderFnV2 registerProvider =
        transportRegisterExport();
    if (!registerProvider) return;
    registerProvider(kReplayTransportApiVersion, &transportStatusThunkV2,
                     &transportSubmitThunkV2, nullptr);
    s.transportProviderRegistered = true;
    QLOG("[replay.transport] provider registered api=%u status=%zu",
         kReplayTransportApiVersion, sizeof(ReplayTransportStatusV2));
}

void releaseTransportProviderV3(RuntimeState& s) {
    if (!s.transportProviderRegistered) return;
    s.transportProviderRegistered = false;
    s.transportProviderRetryMsV3 = 0u;
    const RegisterReplayTransportProviderFnV2 registerProvider =
        transportRegisterExport();
    if (!registerProvider) return;
    registerProvider(kReplayTransportApiVersion, nullptr, nullptr, nullptr);
}

void tryRegisterInputDisplayProvider(RuntimeState& s) {
    if (!qinput::inputDisplayTryRegisterOverlay()) return;
    QLOG("[replay.play] input display provider registered api=%u "
         "snapshot=%zu lanes=%u generation=%u",
         qinput::kInputDisplayApiVersion,
         sizeof(qinput::InputDisplaySnapshotV1),
         qinput::kInputDisplayLaneCount, s.inputDisplayGeneration);
}

// A .q99r stores compact masks (buttons 0..3, directions 8..11); the display ABI carries logical masks (directions 16..19).
static_assert(qreplay::kSourceLanes
                  == static_cast<size_t>(qinput::kInputDisplayLaneCount),
              "the display ABI carries one lane per replay source lane, and the "
              "lane index IS the hkInputRead player index in both");

void publishInputDisplayLatch(RuntimeState& s) {
    qinput::InputDisplayStep display{};
    display.mode = qinput::InputDisplayMode::ReplayPlayback;
    display.generation = s.inputDisplayGeneration;
    display.stepSerial = s.latchTick;
    display.drivenLane = qinput::kInputDisplayNoLane;
    for (size_t lane = 0; lane < qreplay::kSourceLanes; ++lane) {
        if ((s.latchedLaneMask & (1u << lane)) == 0u) continue;
        display.laneActiveMask |= 1u << static_cast<uint32_t>(lane);
        display.laneLogicalMask[lane] = qinput::logicalFromCompact(
            static_cast<uint16_t>(s.latched[lane]
                                  & qinput::kCanonicalFightMask));
    }
    if (display.laneActiveMask == 0u) return;
    (void)qinput::inputDisplay().publishStep(display);
}

}

const char* haltReasonText(HaltReason reason) {
    switch (reason) {
        case HaltReason::None: return "none";
        case HaltReason::LaunchRejected: return "launch-rejected";
        case HaltReason::ManifestMismatch: return "manifest-mismatch";
        case HaltReason::ExpectedFrameMismatch: return "expected-frame";
        case HaltReason::LaneShapeMismatch: return "lane-shape";
        case HaltReason::InvalidActionMask: return "invalid-action-mask";
        case HaltReason::DigestBreak: return "digest-break";
        case HaltReason::PreStateMismatch: return "pre-state";
        case HaltReason::PostStateMismatch: return "post-state";
        case HaltReason::TerminalProofMismatch: return "terminal-proof";
        case HaltReason::ChannelFault: return "channel-fault";
        case HaltReason::SeekFailed: return "seek-failed";
        case HaltReason::CallerReportedMismatch: return "caller-region";
        case HaltReason::DeviceCallObserved: return "device-call";
    }
    return "unknown";
}

bool startReplayRuntime(const qipc::IpcConfig& cfg) {
    return startPlayback(cfg, nullptr, TimelineSourceOptions{}, false);
}

bool startReplayRuntimeForTest(const ReplayRuntimeTestConfig& config) {
    return startPlayback(config.ipc, adoptChannel(config.brokerChannel),
                         config.source, config.inlineWorker,
                         config.playbackRuntimeContract);
}

void stopReplayRuntime() {
    RuntimeState& s = rt();
    if (!s.active) return;
    s.active = false;
    // Must not reorder: publish the inactive snapshot, de-register, then destroy the session.
    publishTransportStatusV3(s);
    releaseTransportProviderV3(s);
    s.sessionV2.reset();
    s.takeover.reset();
    s.takeoverAvailable = false;
    s.takeoverSnapshotReady = false;
    s.controller.reset();
    s.source.stop();
    s.worker.stop();
    s.latchValid = false;
    s.latchedLaneMask = 0u;
    s.latched.fill(0u);
    qinput::inputDisplay().publishIdle();
    (void)qinput::inputDisplayReleaseOverlay();
    s.presentationGeneration.fetch_add(1u, std::memory_order_release);
    QLOG("[replay.play] stopped ticks=%llu presented=%llu underruns=%llu "
         "deviceCalls=%llu halted=%u",
         static_cast<unsigned long long>(s.ticksExecuted),
         static_cast<unsigned long long>(s.presentedSlots),
         static_cast<unsigned long long>(s.underruns),
         static_cast<unsigned long long>(
             s.deviceCalls.load(std::memory_order_relaxed)),
         s.halted ? 1u : 0u);
}

bool replayRuntimeActive() { return rt().active; }

bool replayCollisionDisplayRequested() {
    const RuntimeState& s = rt();
    return s.active && s.collisionDisplayV3;
}

void replaySetSimulationHooks(const SimulationHooks& hooks) {
    RuntimeState& s = rt();
    s.hooks = hooks;
    s.source.setSimulationHooks(hooks);
}

void replaySetRegionDetail(const char* text) {
    RuntimeState& s = rt();
    if (!text) { s.regionDetail[0] = '\0'; return; }
    size_t i = 0;
    for (; text[i] && i + 1u < sizeof(s.regionDetail); ++i)
        s.regionDetail[i] = text[i];
    s.regionDetail[i] = '\0';
}

bool replayAddMatchStart(uint32_t ordinal, uint32_t firstTimelineFrame,
                         uint32_t lastTimelineFrame) {
    RuntimeState& s = rt();
    if (!s.active) return false;
    const uint32_t last = (lastTimelineFrame >= firstTimelineFrame)
        ? lastTimelineFrame
        : firstTimelineFrame;
    if (s.controller)
        s.controller->addMatchMarker(ordinal, firstTimelineFrame, last);
    return s.source.addMatchStart(ordinal, firstTimelineFrame);
}

bool replayRecordingActive() {
    RecorderState& r = rec();
    return r.coordinatorV2
        && r.coordinatorV2->state()
               == qreplay::ReplayGameRecordStateV2::Recording;
}

bool replayRecordPalette(const qreplay::ReplayPaletteV2& palette) {
    RecorderState& r = rec();
    if (!r.coordinatorV2) return false;
    return r.coordinatorV2->publishPalette(palette);
}

bool replayStartReady() {
    RuntimeState& s = rt();
    if (s.playbackV3)
        return s.active && s.sessionV2 != nullptr;
    return s.active && s.source.timelineStarted();
}

const qreplay::DeterministicStartDescriptor& replayStartDescriptor() {
    return rt().source.startDescriptor();
}

bool replayPlaybackV3() {
    const RuntimeState& s = rt();
    return s.active && s.playbackV3;
}

bool replayHeadlessV3() {
    const RuntimeState& s = rt();
    return s.active && s.playbackV3 && s.headlessV3;
}

bool replayChannelReadyV3() {
    RuntimeState& s = rt();
    return s.active && s.playbackV3
        && s.worker.playbackPrebufferReadyV3()
        && s.worker.playbackFaultV3() == SourceFault::None;
}

bool replayChannelFailedV3() {
    RuntimeState& s = rt();
    return s.active && s.playbackV3
        && s.worker.playbackFaultV3() != SourceFault::None;
}

bool replayGameStartV2(qreplay::ConfirmedGameStartV2& out) {
    RuntimeState& s = rt();
    if (!s.active || !s.playbackV3 || !s.sessionV2) return false;
    out = s.sessionV2->start();
    return true;
}

qreplay::ReplayVerificationModeV2 replayVerificationModeV2() {
    RuntimeState& s = rt();
    if (!s.active || !s.playbackV3 || !s.sessionV2)
        return qreplay::ReplayVerificationModeV2::ByteExact;
    return s.sessionV2->mode();
}

bool replayCompleteBootstrapV2(
    const qreplay::ReplayBootstrapObservationV2& observed) {
    RuntimeState& s = rt();
    if (!s.active || !s.playbackV3 || !s.sessionV2) return false;
    const bool ok = s.sessionV2->completeBootstrap(observed);
    s.bootstrapCompleteV3 = ok;
    submitPlaybackStatusV3(s, true);
    return ok;
}

bool replayRewindRequested() {
    const RuntimeState& s = rt();
    return s.active && s.playbackV3 && s.navigation.restorePending;
}

bool replayNavigationActive() {
    const RuntimeState& s = rt();
    return s.active && s.playbackV3 && s.navigation.active;
}

uint32_t replayRewindTarget() { return rt().navigation.target; }

bool replayCaptureCheckpoint(qreplay::ReplayPlaybackSessionV2::Checkpoint& out) {
    const RuntimeState& s = rt();
    return s.active && s.playbackV3 && !s.headlessV3 && s.bootstrapCompleteV3
        && !s.takeover.active() && !s.navigation.restorePending && s.sessionV2
        && s.sessionV2->captureCheckpoint(out);
}

bool replayWinScreenPendingV3() {
    const RuntimeState& s = rt();
    if (!s.active || !s.playbackV3 || s.headlessV3 || s.winScreenFinishedV3
        || !s.sessionV2) return false;
    const auto* result = s.sessionV2->result();
    return result && qreplay::replayCompletedGame(*result);
}

void replayCompleteWinScreenV3(bool succeeded) {
    RuntimeState& s = rt();
    if (!replayWinScreenPendingV3()) return;
    s.winScreenFinishedV3 = true;
    s.winScreenFailedV3 = !succeeded;
    s.resultReadyAtMsV3 = GetTickCount64();
    QLOG("[replay.presentation] win screen %s", succeeded ? "finished" : "FAILED");
}

bool replayNextGamePendingV3() {
    const RuntimeState& s = rt();
    return s.active && s.playbackV3 && s.nextGamePendingV3
        && !s.exitRequested && !s.halted;
}

bool replayBeginNextGameV3() {
    RuntimeState& s = rt();
    if (!replayNextGamePendingV3()) return false;
    const auto next = s.nextGameV3;
    if (!s.worker.submitPlaybackNextGameAckV3(next)) return false;

    s.presentationGeneration.fetch_add(1u, std::memory_order_release);
    resetPlaybackSessionV3(s, next.presentationFlags | qipc::kReplayAutoStart);
    s.complete = false;
    s.halted = false;
    s.buffering = true;
    s.haveExpectedFrame = false;
    s.expectedFrame = 0u;
    s.haveObservedPost = false;
    s.lastObservedPost = 0u;
    s.latchValid = false;
    s.latchedLaneMask = 0u;
    s.latched.fill(0u);
    ++s.inputDisplayGeneration;
    qinput::inputDisplay().reset(s.inputDisplayGeneration);
    s.fastForwardRemaining = 0u;
    s.seekActive = false;
    s.seek = {};
    s.seekStartMs = 0u;
    s.seekFrom = s.seekFastForwarded = 0u;
    s.ticksExecuted = s.hiddenTicks = s.presentedSlots = s.underruns = 0u;
    s.halt = {};
    s.deferred = DeferredLog::None;
    s.regionDetail[0] = '\0';
    s.framesSinceLog = 0u;
    s.logNextFrame = true;
    s.lastSlotMicros = nowMicros();
    s.lastTickStartMicros = 0u;
    QLOG("[replay.folder] next game accepted switch=%llu generation=%u",
         static_cast<unsigned long long>(next.switchId),
         next.game.gameGeneration);
    return true;
}

bool replayCompleteRewind(
    bool restored, const qreplay::ReplayBootstrapObservationV2& observed,
    const qreplay::ReplayPlaybackSessionV2::Checkpoint* checkpoint) {
    RuntimeState& s = rt();
    if (!replayRewindRequested() || !s.sessionV2) return false;
    if (!restored || !(checkpoint ? s.sessionV2->restoreCheckpoint(*checkpoint)
                                  : s.sessionV2->restart())) {
        (void)s.sessionV2->reportBrokerFailure();
        return false;
    }
    s.bootstrapCompleteV3 = false;
    s.latchValid = false;
    s.latchedLaneMask = 0u;
    s.latched.fill(0u);
    s.haveObservedPost = false;
    s.lastObservedPost = 0u;
    s.terminalPresentationSerialV3 = 0u;
    s.terminalPresentationWaitSlotsV3 = 0u;
    ++s.inputDisplayGeneration;
    qinput::inputDisplay().reset(s.inputDisplayGeneration);
    const bool ok = checkpoint != nullptr || replayCompleteBootstrapV2(observed);
    s.bootstrapCompleteV3 = ok;
    if (ok) s.navigation.restored();
    QLOG("[replay.seek] restored=%u target=%u", ok ? 1u : 0u,
         s.navigation.target);
    return ok;
}

bool replayPeekFrameV2(qreplay::ConfirmedGameFrameV2& out,
                       bool& finalFrame, bool& headless) {
    RuntimeState& s = rt();
    finalFrame = false;
    headless = false;
    if (!s.active || !s.playbackV3 || !s.sessionV2) return false;
    const qreplay::CanonicalTickBundleV2* tick = s.sessionV2->peek();
    if (!tick) return false;
    out = tick->frame;
    finalFrame = tick->frame.replayFrame + 1u
        == s.sessionV2->endpoint().nextRecord;
    headless = s.sessionV2->headless();
    return true;
}

bool replayBeginFrameV2(
    const qreplay::ReplayFrameBeginObservationV2& observed) {
    RuntimeState& s = rt();
    if (!s.active || !s.playbackV3 || !s.sessionV2) return false;
    const bool ok = s.sessionV2->beginFrame(observed);
    if (ok) {
        s.latched = s.sessionV2->latchedActions();
        s.latchedLaneMask = s.sessionV2->latchedActiveLaneMask();
        s.latchValid = true;
        ++s.latchTick;
        publishInputDisplayLatch(s);
        s.lastTickStartMicros = nowMicros();
        if (observed.finalPresentationSerial != 0u)
            s.terminalPresentationSerialV3 =
                observed.finalPresentationSerial;
    }
    return ok;
}

bool replayCommitFrameV2(
    const qreplay::ReplayFrameCommitObservationV2& observed) {
    RuntimeState& s = rt();
    if (!s.active || !s.playbackV3 || !s.sessionV2) return false;
    const bool ok = s.sessionV2->commitFrame(observed);
    s.latchValid = false;
    s.latchedLaneMask = 0u;
    s.latched.fill(0u);
    if (ok) {
        s.lastObservedPost = observed.postStateHash;
        s.haveObservedPost = true;
        ++s.ticksExecuted;
        if (s.sessionV2->phase()
            == qreplay::ReplayPlaybackPhaseV2::
                AwaitingFinalPresentation) {
            s.terminalPresentationWaitSlotsV3 = 0u;
        }
    }
    return ok;
}

bool replayTakeoverNeedsSnapshot() {
    const RuntimeState& s = rt();
    return s.active && s.playbackV3 && s.takeover.active()
        && !s.takeoverSnapshotReady
        && s.takeover.phase() == qreplay::ReplayTakeoverPhase::Countdown;
}

bool replayTakeoverRestorePending() {
    const RuntimeState& s = rt();
    return s.active && s.playbackV3 && s.takeoverSnapshotReady
        && s.takeover.phase()
            == qreplay::ReplayTakeoverPhase::RestorePending;
}

qreplay::ReplayTakeoverRestoreReason replayTakeoverRestoreReason() {
    const RuntimeState& s = rt();
    return replayTakeoverRestorePending()
        ? s.takeover.restoreReason()
        : qreplay::ReplayTakeoverRestoreReason::None;
}

bool replayTakeoverCompleteSnapshot(bool succeeded) {
    RuntimeState& s = rt();
    if (!replayTakeoverNeedsSnapshot()) return false;
    if (!succeeded) {
        s.takeover.reset();
        s.takeoverAvailable = false;
        s.takeoverSnapshotReady = false;
        s.transportStatusDirtyV3 = true;
        QLOG("[replay.takeover] activation refused: native snapshot unavailable");
        return false;
    }
    s.takeoverSnapshotReady = true;
    s.transportStatusDirtyV3 = true;
    QLOG("[replay.takeover] armed record=%u source=%u lane=P%u delay=%u",
         s.takeover.branchReplayFrame(), s.takeover.branchSourceFrame(),
         static_cast<unsigned>(s.takeover.selectedLane()) + 1u,
         static_cast<unsigned>(s.takeover.delay()));
    return true;
}

bool replayTakeoverCompleteRestore(bool succeeded) {
    RuntimeState& s = rt();
    if (!replayTakeoverRestorePending()) return false;
    const qreplay::ReplayTakeoverRestoreReason reason =
        s.takeover.restoreReason();
    const bool completed = s.takeover.completeRestore(succeeded);
    if (completed) {
        ++s.inputDisplayGeneration;
        qinput::inputDisplay().reset(s.inputDisplayGeneration);
        if (reason == qreplay::ReplayTakeoverRestoreReason::Exit)
            s.takeoverSnapshotReady = false;
    }
    s.transportStatusDirtyV3 = true;
    QLOG("[replay.takeover] restore reason=%u result=%s lane=P%u",
         static_cast<unsigned>(reason), succeeded ? "ok" : "FAILED",
         static_cast<unsigned>(s.takeover.selectedLane()) + 1u);
    return completed;
}

bool replayTakeoverBeginFrame(
        uint16_t compactLocalAction,
        qreplay::ReplayTakeoverResolvedFrame& out) {
    RuntimeState& s = rt();
    if (!s.active || !s.playbackV3 || !s.sessionV2
        || !s.takeoverSnapshotReady || !s.takeover.simulating()) {
        return false;
    }
    if (!s.takeover.capture(compactLocalAction)) {
        s.transportStatusDirtyV3 = true;
        return false;
    }
    const qreplay::CanonicalTickBundleV2* recorded = nullptr;
    if (s.takeover.currentReplayFrame()
        < s.sessionV2->endpoint().nextRecord) {
        recorded = s.sessionV2->tickAt(s.takeover.currentReplayFrame());
    }
    if (!s.takeover.resolve(recorded, s.takeoverFallbackLaneMask, out))
        return false;
    s.latched = out.actions;
    s.latchedLaneMask = out.activeLaneMask;
    s.latchValid = true;
    ++s.latchTick;
    publishInputDisplayLatch(s);
    s.lastTickStartMicros = nowMicros();
    return true;
}

bool replayTakeoverCommitFrame() {
    RuntimeState& s = rt();
    if (!s.active || !s.takeover.simulating() || !s.latchValid)
        return false;
    s.latchValid = false;
    s.latchedLaneMask = 0u;
    s.latched.fill(0u);
    const bool advanced = s.takeover.advance();
    s.transportStatusDirtyV3 = true;
    return advanced;
}

bool replayTakeoverFailBranch() {
    RuntimeState& s = rt();
    if (!s.active || !s.playbackV3 || !s.takeoverSnapshotReady)
        return false;
    const bool failed = s.takeover.failSimulation();
    if (failed) {
        s.latchValid = false;
        s.latchedLaneMask = 0u;
        s.latched.fill(0u);
        s.transportStatusDirtyV3 = true;
    }
    return failed;
}

bool replayTakeoverRequestSafetyExit() {
    RuntimeState& s = rt();
    if (!s.active || !s.playbackV3 || !s.takeoverSnapshotReady)
        return false;
    const bool requested = s.takeover.requestExit();
    if (requested) s.transportStatusDirtyV3 = true;
    return requested;
}

bool replayTakeoverActive() {
    const RuntimeState& s = rt();
    return s.active && s.playbackV3 && s.takeover.active();
}

qreplay::ReplayTakeoverPhase replayTakeoverPhase() {
    const RuntimeState& s = rt();
    return s.active && s.playbackV3
        ? s.takeover.phase() : qreplay::ReplayTakeoverPhase::Inactive;
}

qreplay::ReplayPresentationAckV2 replayAcknowledgeFinalPresentationV2(
    uint64_t presentationSerial) {
    RuntimeState& s = rt();
    if (!s.active || !s.playbackV3 || !s.sessionV2)
        return qreplay::ReplayPresentationAckV2::NotWaiting;
    const qreplay::ReplayPresentationAckV2 ack =
        s.sessionV2->acknowledgeFinalPresentation(presentationSerial);
    submitPlaybackStatusV3(s, true);
    return ack;
}

bool replayAwaitingFinalPresentationV2() {
    const RuntimeState& s = rt();
    return s.active && s.playbackV3 && s.sessionV2
        && s.sessionV2->phase()
            == qreplay::ReplayPlaybackPhaseV2::
                AwaitingFinalPresentation;
}

void replayFailPlaybackV2() {
    RuntimeState& s = rt();
    if (!s.active || !s.playbackV3 || !s.sessionV2) return;
    (void)s.sessionV2->reportBrokerFailure();
    submitPlaybackStatusV3(s, true);
}

bool replayNextFrame(qreplay::ReplayFrameV1& out) {
    RuntimeState& s = rt();
    if (!s.active || s.halted || s.complete) return false;

    s.source.pump();
    if (s.source.state() == SourceState::Faulted) {
        haltFromSource();
        return false;
    }
    if (!s.source.timelineStarted()) return false;

    qreplay::ReplayFrameV1 frame{};
    if (!s.source.peek(frame)) {
        ++s.underruns;
        if (!s.buffering) {
            s.buffering = true;
            if (s.deferred == DeferredLog::None)
                s.deferred = DeferredLog::Buffering;
        }
        return false;
    }
    s.buffering = false;

    if (s.haveExpectedFrame && frame.timelineFrame != s.expectedFrame) {
        haltWith(HaltReason::ExpectedFrameMismatch, frame.timelineFrame,
                 s.expectedFrame, frame.timelineFrame, &frame);
        return false;
    }

    const uint8_t expectedLanes = s.source.startDescriptor().activeLaneMask;
    if (frame.activeLaneMask == 0u
        || (frame.activeLaneMask & static_cast<uint8_t>(~expectedLanes)) != 0u) {
        haltWith(HaltReason::LaneShapeMismatch, frame.timelineFrame,
                 expectedLanes, frame.activeLaneMask, &frame);
        return false;
    }
    for (size_t lane = 0; lane < qreplay::kSourceLanes; ++lane) {
        const bool activeLane =
            (frame.activeLaneMask & (1u << lane)) != 0u;
        if (activeLane) {
            if (!qreplay::isValidActionMask(frame.actions[lane])) {
                haltWith(HaltReason::InvalidActionMask, frame.timelineFrame,
                         qreplay::kActionMask, frame.actions[lane], &frame);
                return false;
            }
        } else if (frame.actions[lane] != 0u) {
            haltWith(HaltReason::LaneShapeMismatch, frame.timelineFrame,
                     0u, frame.actions[lane], &frame);
            return false;
        }
    }

    uint64_t observedPre = 0u;
    if (s.hooks.currentStateHash) {
        observedPre = s.hooks.currentStateHash(s.hooks.user);
    } else if (s.haveObservedPost) {
        observedPre = s.lastObservedPost;
    } else {
        observedPre = s.source.startDescriptor().start.initialStateHash;
    }

    qreplay::ParseStatus why = qreplay::ParseStatus::Ok;
    if (!s.source.consume(observedPre, why)) {
        const qreplay::TimelineFault tf = s.source.timelineFault();
        HaltReason reason = HaltReason::PreStateMismatch;
        if (tf == qreplay::TimelineFault::DigestChainBreak)
            reason = HaltReason::DigestBreak;
        else if (tf == qreplay::TimelineFault::BadActionMask)
            reason = HaltReason::InvalidActionMask;
        else if (tf == qreplay::TimelineFault::BadLaneShape)
            reason = HaltReason::LaneShapeMismatch;
        haltWith(reason, frame.timelineFrame, frame.preStateHash, observedPre,
                 &frame);
        return false;
    }

    s.latched = frame.actions;
    s.latchedLaneMask = frame.activeLaneMask;
    s.latchValid = true;
    ++s.latchTick;
    publishInputDisplayLatch(s);

    s.expectedFrame = frame.timelineFrame + 1u;
    s.haveExpectedFrame = true;
    s.lastTickStartMicros = nowMicros();

    out = frame;
    return true;
}

uint16_t replayInjectedAction(int sourceLane) {
    RuntimeState& s = rt();
    if (!s.active || !s.latchValid) return 0u;
    if (sourceLane < 0
        || static_cast<size_t>(sourceLane) >= qreplay::kSourceLanes)
        return 0u;
    ++s.injectedReads;
    if ((s.latchedLaneMask & (1u << sourceLane)) == 0u) return 0u;
    return s.latched[static_cast<size_t>(sourceLane)];
}

void replayCommitFrame(const qreplay::ReplayFrameV1& applied,
                       uint64_t observedPostHash) {
    RuntimeState& s = rt();
    if (!s.active || s.halted) return;

    s.source.recordPostState(applied.timelineFrame, observedPostHash);
    if (applied.postStateHash != 0u
        && observedPostHash != applied.postStateHash) {
        haltWith(HaltReason::PostStateMismatch, applied.timelineFrame,
                 applied.postStateHash, observedPostHash, &applied);
        flushDeferredLogs();
        return;
    }

    s.lastObservedPost = observedPostHash;
    s.haveObservedPost = true;
    ++s.ticksExecuted;

    const uint64_t tickMicros =
        (s.lastTickStartMicros != 0u) ? (nowMicros() - s.lastTickStartMicros)
                                      : 0u;
    if (s.controller) s.controller->noteTickExecuted(tickMicros);

    const bool marker =
        applied.lifecycleAfter.matchState != applied.lifecycleBefore.matchState
        || applied.lifecycleAfter.roundState
               != applied.lifecycleBefore.roundState;
    s.source.maybeCaptureSnapshot(applied.timelineFrame + 1u, marker);

    const uint32_t cursor = s.source.consumeCursor();
    s.source.trimConsumedBelow(
        cursor > kSeekRetainFrames ? cursor - kSeekRetainFrames : 0u);

    if (s.fastForwardRemaining > 0u) {
        --s.fastForwardRemaining;
        ++s.seekFastForwarded;
        ++s.hiddenTicks;
        if (s.fastForwardRemaining == 0u && s.seekActive) {
            s.seekActive = false;
            if (s.controller) s.controller->resetDeadlineAfterPauseOrSeek();
            if (s.deferred == DeferredLog::None)
                s.deferred = DeferredLog::SeekComplete;
        }
    }

    ++s.framesSinceLog;
    if (s.logNextFrame || s.framesSinceLog >= kFrameLogInterval) {
        s.logNextFrame = false;
        s.framesSinceLog = 0u;
        QLOG("[replay.play] frame=%u preHash=%016llX digest=%016llX "
             "speed=%u hidden=%llu",
             applied.timelineFrame,
             static_cast<unsigned long long>(applied.preStateHash),
             static_cast<unsigned long long>(s.source.rollingDigest()),
             s.controller ? s.controller->speedPermille() : s.speedPermille,
             static_cast<unsigned long long>(s.hiddenTicks));
    }

    if (!s.complete && s.source.terminalKnown()
        && s.source.bufferedFrames() == 0u
        && s.source.consumeCursor()
               == s.source.terminal().nextRecord) {
        qreplay::ParseStatus why = qreplay::ParseStatus::Ok;
        if (s.source.verifyTerminal(why)) {
            s.complete = true;
            QLOG("[replay.play] complete frame=%u digest=%016llX "
                 "postTail=%016llX reason=%u",
                 s.source.terminal().nextRecord,
                 static_cast<unsigned long long>(
                     s.source.terminal().finalDigest),
                 static_cast<unsigned long long>(
                     s.source.terminal().postTailStateHash),
                 static_cast<unsigned>(s.source.terminal().reason));
        } else {
            haltWith(HaltReason::TerminalProofMismatch,
                     s.source.terminal().nextRecord,
                     s.source.terminal().postTailStateHash, observedPostHash,
                     &applied);
        }
    }

    flushDeferredLogs();
}

void replayReportMismatch(uint32_t frame, uint64_t expected, uint64_t actual) {
    RuntimeState& s = rt();
    haltWith(HaltReason::CallerReportedMismatch, frame, expected, actual,
             nullptr);
    flushDeferredLogs();
    (void)s;
}

ReplayTickPlan replayPlanOuterSlot() {
    RuntimeState& s = rt();
    ReplayTickPlan plan{};
    if (!s.active) {
        plan.present = true;
        plan.paused = true;
        return plan;
    }

    if (s.inlineWorker) s.worker.pumpOnceForTest();

    tryRegisterInputDisplayProvider(s);

    if (s.playbackV3) {
        applyDisplayFlagsV3(s);
        drainPresentationCommandsV3(s);
        servicePlaybackV3(s);
        publishTransportStatusV3(s);
        tryRegisterTransportProviderV3(s);
        if (!s.sessionV2 || !s.bootstrapCompleteV3
            || s.exitRequested || s.sessionV2->hasResult()) {
            plan.present = true;
            plan.paused = true;
            return plan;
        }
        if (s.sessionV2->phase()
            == qreplay::ReplayPlaybackPhaseV2::
                AwaitingFinalPresentation) {
            if (++s.terminalPresentationWaitSlotsV3 >= 300u) {
                (void)s.sessionV2->finalPresentationTimedOut();
                submitPlaybackResultIfReadyV3(s);
                submitPlaybackStatusV3(s, true);
            }
            plan.present = true;
            plan.paused = true;
            return plan;
        }
        if (s.sessionV2->phase()
            != qreplay::ReplayPlaybackPhaseV2::Ready) {
            plan.present = true;
            plan.paused = true;
            return plan;
        }

        if (s.navigation.active) {
            if (s.navigation.restorePending) {
                plan.present = true;
                plan.paused = true;
                return plan;
            }
            if (s.navigation.committed(s.sessionV2->nextRecord())) {
                s.pausedV3 = true;
                s.transportStatusDirtyV3 = true;
                plan.present = true;
                plan.paused = true;
                return plan;
            }
            const auto slot = s.navigation.plan(s.sessionV2->nextRecord());
            plan.hiddenTicks = slot.hiddenTicks;
            plan.present = slot.present;
            plan.paused = false;
            s.hiddenTicks += plan.hiddenTicks;
            if (plan.present) ++s.presentedSlots;
            return plan;
        }

        if (s.takeover.active()) {
            plan.takeover = true;
            plan.present = true;
            if (!s.takeoverSnapshotReady
                || s.takeover.phase()
                    == qreplay::ReplayTakeoverPhase::RestorePending
                || s.takeover.phase()
                    == qreplay::ReplayTakeoverPhase::Faulted) {
                plan.paused = true;
                return plan;
            }
            if (s.takeover.phase()
                    == qreplay::ReplayTakeoverPhase::Countdown) {
                const bool armed = s.takeover.advanceCountdown();
                s.transportStatusDirtyV3 = true;
                publishTransportStatusV3(s);
                if (!armed) {
                    plan.paused = true;
                    plan.takeoverCountdownHold = true;
                    return plan;
                }
            }
            plan.paused = false;
            ++s.presentedSlots;
            return plan;
        }

        const uint32_t remaining =
            s.sessionV2->endpoint().nextRecord
            - s.sessionV2->nextRecord();
        const uint32_t ticks = replayTransportTicksForSlotV3(
            s.pausedV3, s.pendingStepsV3, s.speedPermilleV3, s.speedCreditV3,
            remaining);
        if (ticks == 0u) {
            plan.present = true;
            plan.paused = true;
            plan.slowMotionHold = !s.pausedV3;
            return plan;
        }
        plan.hiddenTicks = ticks - 1u;
        plan.present = true;
        plan.paused = false;
        s.hiddenTicks += plan.hiddenTicks;
        ++s.presentedSlots;
        return plan;
    }

    drainPresentationCommands();
    s.source.pump();
    if (s.source.state() == SourceState::Faulted) haltFromSource();

    flushDeferredLogs();

    if (s.halted || s.complete || s.exitRequested) {
        plan.present = true;
        plan.paused = true;
        return plan;
    }

    if (s.fastForwardRemaining > 0u) {
        plan.hiddenTicks = (s.fastForwardRemaining < kSeekBatchTicks)
            ? s.fastForwardRemaining
            : kSeekBatchTicks;
        plan.present = false;
        plan.paused = false;
        s.lastSlotMicros = nowMicros();
        return plan;
    }

    qreplay::SeekRequest request{};
    if (s.controller && s.controller->takeSeekRequest(request)) {
        s.seekFrom = s.source.consumeCursor();
        s.seekStartMs = GetTickCount64();
        const SeekOutcome outcome = s.source.positionFor(request.targetFrame);
        if (!outcome.ok
            || !s.controller->completeSeek(outcome.checkpointFrame)) {
            s.controller->failSeek(qreplay::ParseStatus::Truncated);
            haltWith(HaltReason::SeekFailed, request.targetFrame, 0u, 0u,
                     nullptr);
            flushDeferredLogs();
            plan.present = true;
            plan.paused = true;
            return plan;
        }
        s.seek = outcome;
        s.seekActive = true;
        s.seekFastForwarded = 0u;
        s.haveExpectedFrame = !outcome.requestedRestream;
        s.expectedFrame = outcome.checkpointFrame;
        s.haveObservedPost = false;
        s.logNextFrame = true;
    }

    const uint64_t now = nowMicros();
    const uint64_t elapsed =
        (s.lastSlotMicros != 0u && now > s.lastSlotMicros)
            ? (now - s.lastSlotMicros)
            : 0u;
    s.lastSlotMicros = now;

    if (!s.controller) {
        plan.present = true;
        plan.paused = true;
        return plan;
    }

    const qreplay::OuterSlotPlan slot = s.controller->planOuterSlot(elapsed);
    plan.hiddenTicks = slot.hiddenTicks;
    plan.present = slot.present;
    plan.paused = slot.hold;

    s.hiddenTicks += slot.hiddenTicks;
    if (slot.present) ++s.presentedSlots;

    if (s.seekActive && !s.controller->seeking()
        && s.fastForwardRemaining == 0u) {
        s.seekActive = false;
        s.seekFastForwarded =
            (s.source.consumeCursor() > s.seek.checkpointFrame)
                ? (s.source.consumeCursor()
                   - s.seek.checkpointFrame)
                : 0u;
        if (s.deferred == DeferredLog::None)
            s.deferred = DeferredLog::SeekComplete;
    }

    return plan;
}

uint32_t replayPresentationGeneration() {
    return rt().presentationGeneration.load(std::memory_order_acquire);
}

bool replayTransportStatus(ReplayTransportStatusV2& out) {
    return transportPublisher().read(out);
}

bool replaySubmitPresentationCommand(uint32_t generation, uint32_t command,
                                     uint64_t argument) {
    RuntimeState& s = rt();
    if (!s.active) return false;
    if (generation != s.presentationGeneration.load(std::memory_order_acquire))
        return false;
    if (!isKnownPresentationCommand(command)) return false;
    PresentationSlot slot{};
    slot.generation = generation;
    slot.command = command;
    slot.argument = argument;
    if (!s.commands.push(slot)) {
        s.commandDrops.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    return true;
}

uint64_t replayDroppedPresentationCommands() {
    RuntimeState& s = rt();
    uint64_t total = s.commandDrops.load(std::memory_order_relaxed);
    if (s.controller) total += s.controller->droppedCommands();
    return total;
}

bool replayHalted() { return rt().halted; }

ReplayHaltDiagnostic replayHaltDiagnostic() { return rt().halt; }

bool replayExitRequested() { return rt().exitRequested; }

bool replayComplete() { return rt().complete; }

bool replayNextDueEvent(qreplay::ReplayEvent& out) {
    RuntimeState& s = rt();
    if (!s.active) return false;
    qreplay::ReplayEvent event{};
    if (!s.source.peekEvent(event)) return false;
    if (event.timelineFrame > s.source.consumeCursor()) return false;
    return s.source.consumeEvent(out);
}

void replayStatus(ReplayStatusSnapshot& out) {
    RuntimeState& s = rt();
    out = ReplayStatusSnapshot{};
    if (s.playbackV3) {
        out.active = s.active ? 1u : 0u;
        out.halted = s.sessionV2
            && s.sessionV2->phase()
                == qreplay::ReplayPlaybackPhaseV2::Faulted ? 1u : 0u;
        out.paused = transportHoldingV3(s) ? 1u : 0u;
        out.pendingSteps = s.pendingStepsV3;
        out.transportMayHold = transportMayHoldV3(s) ? 1u : 0u;
        out.buffering = s.sessionV2 ? 0u : 1u;
        out.seeking = s.navigation.active ? 1u : 0u;
        out.complete = s.sessionV2 && s.sessionV2->hasResult() ? 1u : 0u;
        out.speedPermille = s.speedPermilleV3;
        out.phase = playbackPhaseByteV3(s);
        out.lastPostStateHash = s.lastObservedPost;
        out.ticksExecuted = s.ticksExecuted;
        out.hiddenTicksExecuted = s.hiddenTicks;
        out.presentedSlots = s.presentedSlots;
        out.deviceCalls =
            s.deviceCalls.load(std::memory_order_relaxed);
        if (s.sessionV2) {
            const uint32_t end = s.sessionV2->endpoint().nextRecord;
            const uint32_t next = s.sessionV2->nextRecord();
            out.sealed = s.sessionV2->endpoint().kind
                    == qreplay::ipc::v3::PlaybackEndpointKind::RecordedGameEnd
                ? 1u : 0u;
            out.currentFrame = next;
            out.firstFrame = 0u;
            out.lastFrame = end == 0u ? 0u : end - 1u;
            out.bufferedFrames = end >= next ? end - next : 0u;
            out.rollingDigest = s.sessionV2->currentRollingDigest();
        }
        return;
    }
    out.active = s.active ? 1u : 0u;
    out.halted = s.halted ? 1u : 0u;
    out.buffering = s.buffering ? 1u : 0u;
    out.complete = s.complete ? 1u : 0u;
    out.seeking = (s.seekActive || s.fastForwardRemaining > 0u) ? 1u : 0u;
    out.sourceState = static_cast<uint8_t>(s.source.state());
    out.sealed = s.source.terminalKnown() ? 1u : 0u;
    out.currentFrame = s.source.consumeCursor();
    out.firstFrame = s.source.sessionInfo().firstTimelineFrame;
    out.lastFrame = s.source.sessionInfo().lastTimelineFrame;
    out.bufferedFrames = static_cast<uint32_t>(s.source.bufferedFrames());
    out.rollingDigest = s.source.rollingDigest();
    out.lastPostStateHash = s.lastObservedPost;
    out.matchOrdinal = s.source.sessionInfo().matchOrdinal;
    out.fastForwardRemaining = s.fastForwardRemaining;
    out.ticksExecuted = s.ticksExecuted;
    out.hiddenTicksExecuted = s.hiddenTicks;
    out.presentedSlots = s.presentedSlots;
    out.underruns = s.underruns;
    out.deviceCalls = s.deviceCalls.load(std::memory_order_relaxed);
    out.snapshotCaptures = s.source.snapshotCaptures();
    out.snapshotRestores = s.source.snapshotRestores();
    out.haltReason = static_cast<uint16_t>(s.halt.reason);
    if (s.controller) {
        const qreplay::PlaybackStatus status = s.controller->status();
        out.paused = status.paused ? 1u : 0u;
        out.speedPermille = status.speedPermille;
        out.phase = static_cast<uint8_t>(status.phase);
    } else {
        out.speedPermille = s.speedPermille;
    }
}

void replayNoteDeviceCall(const char* site) {
    RuntimeState& s = rt();
    const uint64_t before = s.deviceCalls.fetch_add(1u,
                                                    std::memory_order_relaxed);
    if (before == 0u) s.firstDeviceCallSite = site;
    if (s.active && s.playbackV3 && s.sessionV2) {
        (void)s.sessionV2->noteDeviceBoundaryCall();
        submitPlaybackStatusV3(s, true);
    }
}

uint64_t replayDeviceCallCount() {
    return rt().deviceCalls.load(std::memory_order_relaxed);
}

const char* replayFirstDeviceCallSite() {
    const char* site = rt().firstDeviceCallSite;
    return site ? site : "";
}

void replayResetDeviceCallCounter() {
    RuntimeState& s = rt();
    s.deviceCalls.store(0u, std::memory_order_release);
    s.firstDeviceCallSite = nullptr;
}

bool startReplayRecorder(const qipc::IpcConfig& cfg) {
    return startRecorder(cfg, nullptr, qreplay::kDefaultRecorderQueueCapacity,
                         false, true);
}

bool startReplayRecorderForTest(const ReplayRuntimeTestConfig& config) {
    return startRecorder(config.ipc, adoptChannel(config.recorderChannel),
                         config.recorderQueueCapacity, config.inlineWorker,
                         config.recorderV3);
}

void stopReplayRecorder() {
    RecorderState& r = rec();
    if (!r.active) return;
    qreplay::ReplayGameRecordStateV2 finalStateV2 =
        qreplay::ReplayGameRecordStateV2::Disabled;
    if (r.v3 && r.coordinatorV2) {
        const qreplay::ReplayGameRecordStateV2 state =
            r.coordinatorV2->state();
        finalStateV2 = state;
        const bool openGame =
            state == qreplay::ReplayGameRecordStateV2::WaitingForFrameZero
            || state == qreplay::ReplayGameRecordStateV2::Recording
            || state
                == qreplay::ReplayGameRecordStateV2::
                    AwaitFirstWinScreenTick;
        if (openGame) {
            (void)r.coordinatorV2->requestAbnormalEnd(
                qreplay::TerminalReason::ProcessExit,
                qreplay::ReplayCloseProvenanceV2::ProcessObserved);
        }
        if (r.worker)
            (void)r.worker->requestRecorderDrain(false);
    }
    r.active = false;
    if (r.worker) {
        r.worker->pumpOnceForTest();
        r.worker->stop();
    }
    QLOG("[replay.record] published=%llu confirmed=%llu written=%llu "
         "queueHighWater=%llu stateV2=%u faultV2=%s",
         static_cast<unsigned long long>(r.published),
         static_cast<unsigned long long>(r.published),
         static_cast<unsigned long long>(
             r.worker ? r.worker->stats().recorderRecordsSent : 0ull),
         static_cast<unsigned long long>(
             r.queue
                  ? r.queue->highWater()
                  : (r.queueV2
                        ? r.queueV2->highWater() : 0ull)),
         static_cast<unsigned>(finalStateV2),
         qreplay::replayRecordingFaultTextV2(r.lastV2Fault));
    r.worker.reset();
    r.queue.reset();
    r.coordinatorV2.reset();
    r.queueV2.reset();
    r.epochTracker.reset();
    r.v3 = false;
    r.manifestReady = false;
    r.manifestPublished = false;
    r.manifestFailure = "not built";
    r.manifestMessage = qreplay::RecorderMessage{};
    r.sessionStartPublished = false;
    r.matchesStarted = 0u;
    r.matchesEnded = 0u;
}

bool replayRecorderActive() { return rec().active; }

qreplay::ReplayGameRecordStateV2 replayRecordStateV2() {
    const RecorderState& r = rec();
    if (!r.active || !r.coordinatorV2)
        return qreplay::ReplayGameRecordStateV2::Disabled;
    return r.coordinatorV2->state();
}

bool replayRecorderV2Active() {
    const RecorderState& r = rec();
    return r.active && r.v3 && r.queueV2
        && r.coordinatorV2 && r.worker;
}

namespace {

void drainRecorderV2CommitAcks(RecorderState& r) {
    if (!r.v3 || !r.worker || !r.coordinatorV2)
        return;
    qreplay::GameCommitAckV2 ack{};
    while (r.worker->popGameCommitAck(ack)) {
        const qreplay::CommitAckResultV2 applied =
            r.coordinatorV2->onCasterCommitAck(ack);
        if (applied == qreplay::CommitAckResultV2::Conflict) {
            r.lastV2Fault =
                qreplay::ReplayRecordingFaultV2::CommitAckConflict;
        } else if (applied == qreplay::CommitAckResultV2::Stale) {
            r.lastV2Fault =
                qreplay::ReplayRecordingFaultV2::CommitAckStale;
        }
    }
}

bool buildFreshGameStartV2(
    RecorderState& r, const ReplayFreshGameCaptureV2& capture,
    qreplay::ConfirmedGameStartV2& out) {
    if (capture.layoutGeneration == 0u
        || capture.launchSourceFrame
            == (std::numeric_limits<uint32_t>::max)()
        || capture.bootstrapFingerprint == 0u
        || capture.fpu.x87ControlWord == 0u
        || capture.fpu.mxcsr == 0u
        || r.nextGameGeneration == 0u) {
        return false;
    }
    const qreplay::CharacterCatalog& catalog =
        qreplay::CharacterCatalog::forSeBuild();
    for (size_t slot = 0u; slot < qreplay::kFighterSlots; ++slot) {
        const qreplay::FighterIdentityV2& identity =
            capture.roster[slot];
        if (slot < 2u) {
            if (!catalog.isSelectable(identity.nativeCharacterId)
                || identity.paletteIndex
                    > qreplay::kMaxPaletteIndex
                || !catalog.verifySelectorProof(
                    capture.selectorProof[slot],
                    identity.nativeCharacterId)) {
                return false;
            }
        } else if (identity.nativeCharacterId
                       != qreplay::kInactiveCharacterId
                   || identity.paletteIndex != 0u) {
            return false;
        }
    }

    qreplay::ConfirmedGameStartV2 start{};
    qreplay::GameManifestV2& manifest = start.manifest;
    const uint32_t generation = r.nextGameGeneration;
    manifest.identity.gameGeneration = generation;
    manifest.identity.gameUuidLow = splitmix64(
        r.seriesUuidLow
        ^ (static_cast<uint64_t>(generation)
           * 0xD1B54A32D192ED03ull));
    manifest.identity.gameUuidHigh = splitmix64(
        r.seriesUuidHigh ^ manifest.identity.gameUuidLow
        ^ 0x94D049BB133111EBull);
    if (manifest.identity.gameUuidLow == 0u
        && manifest.identity.gameUuidHigh == 0u) {
        manifest.identity.gameUuidLow = generation;
    }
    manifest.contract = r.contractV2;
    manifest.bootstrapKind =
        qreplay::BootstrapKindV2::MatchActiveDirectBootV1;
    manifest.vsRow = qipc::kVsRow1v1;
    manifest.activeLaneMask = 0x03u;
    manifest.activeFighterMask = 0x03u;
    manifest.cpuMask = 0u;
    manifest.cpuContract = qreplay::CpuContract::None;
    manifest.slotToSourceRoute = {{0u, 1u, 2u, 3u}};
    manifest.lanes = r.lanesV2;
    for (size_t slot = 0u; slot < qreplay::kFighterSlots; ++slot) {
        qreplay::FighterSlotDescriptorV2& descriptor =
            manifest.slots[slot];
        descriptor.ownership = slot < 2u
            ? qreplay::SlotOwnership::RecordedHuman
            : qreplay::SlotOwnership::Inactive;
        descriptor.side = static_cast<uint8_t>(slot & 1u);
        descriptor.identity = capture.roster[slot];
        descriptor.selectorProofPresent = slot < 2u;
        descriptor.selectorProof = slot < 2u
            ? capture.selectorProof[slot]
            : qreplay::SelectorProofV2{};
    }
    manifest.routeEpoch = capture.layoutGeneration;
    manifest.rosterEpoch = capture.layoutGeneration;
    manifest.layoutGeneration = capture.layoutGeneration;
    manifest.stage = qreplay::StageIdentityV2{};
    manifest.display = r.displayV2;
    manifest.display.gameOrdinalInSeries =
        r.nextGameOrdinal;

    start.start.configuration = capture.configuration;
    start.start.rng = capture.rng;
    start.start.fpu = capture.fpu;
    start.start.nativeDisplay =
        qreplay::NativeDisplayStateV2{};
    start.start.initialPhase = capture.initialPhase;
    start.start.bootstrapFingerprint =
        capture.bootstrapFingerprint;
    start.start.frameZeroPreStateHash = 0u;
    start.start.initialRollingDigest = 0u;
    if (!qreplay::isValidStagedFreshGameStartV2(start))
        return false;
    out = start;
    return true;
}

using PlaybackPhaseV3 = qreplay::ipc::v3::PlaybackRuntimePhase;
using SessionPhaseV2 = qreplay::ReplayPlaybackPhaseV2;

PlaybackPhaseV3 playbackPhaseV3(const RuntimeState& s) {
    if (!s.sessionV2) return PlaybackPhaseV3::WaitingForStart;
    switch (s.sessionV2->phase()) {
        case SessionPhaseV2::Bound:
            return PlaybackPhaseV3::Bootstrapping;
        case SessionPhaseV2::Ready:
            if (transportHoldingV3(s) && s.pendingStepsV3 == 0u)
                return PlaybackPhaseV3::Paused;
            return s.bootstrapCompleteV3
                ? PlaybackPhaseV3::Playing : PlaybackPhaseV3::Ready;
        case SessionPhaseV2::FrameInFlight:
            return PlaybackPhaseV3::Playing;
        case SessionPhaseV2::TerminalArmed:
            return PlaybackPhaseV3::TerminalArmed;
        case SessionPhaseV2::AwaitingFinalPresentation:
            return PlaybackPhaseV3::AwaitingTerminalPresentation;
        case SessionPhaseV2::ResultReady:
            return PlaybackPhaseV3::Complete;
        case SessionPhaseV2::Faulted:
            return PlaybackPhaseV3::Failed;
    }
    return PlaybackPhaseV3::Failed;
}

uint8_t playbackPhaseByteV3(const RuntimeState& s) {
    return static_cast<uint8_t>(playbackPhaseV3(s));
}

bool installPlaybackPrebufferV3(RuntimeState& s) {
    if (s.sessionV2) return true;
    if (s.worker.playbackFaultV3() != SourceFault::None) {
        s.halted = true;
        s.exitRequested = true;
        QLOG_ISSUE("[replay.v3] prebuffer failed fault=%s",
             sourceFaultText(s.worker.playbackFaultV3()));
        return false;
    }
    std::unique_ptr<PlaybackPrebufferV3> prebuffer =
        s.worker.takePlaybackPrebufferV3();
    if (!prebuffer) return false;
    if (!prebuffer->timeline) {
        s.halted = true;
        s.exitRequested = true;
        QLOG("[replay.v3] prebuffer ownership transfer had no timeline");
        return false;
    }
    s.sessionV2.reset(new (std::nothrow) qreplay::ReplayPlaybackSessionV2(
        static_cast<qreplay::ConfirmedTimelineBufferV2&&>(
            *prebuffer->timeline), prebuffer->binding));
    if (!s.sessionV2) {
        s.halted = true;
        s.exitRequested = true;
        QLOG_ISSUE("[replay.v3] could not allocate playback session");
        return false;
    }
    qgame::paletteClearReplayHold();
    for (const auto& palette : prebuffer->palettes)
        qgame::paletteHoldFromReplay(palette);
    s.prebufferInstalledV3 = true;
    s.buffering = false;
    const uint32_t endpoint = s.sessionV2->endpoint().nextRecord;
    const uint64_t desiredCapacity = static_cast<uint64_t>(endpoint)
        + qreplay::kReplayTakeoverContinuationFrames;
    const uint32_t takeoverCapacity = static_cast<uint32_t>(
        (std::min)(desiredCapacity,
                   static_cast<uint64_t>(qreplay::kMaxTimelineFrames)));
    s.takeoverAvailable = !s.sessionV2->headless()
        && s.takeover.prepare(takeoverCapacity);
    s.takeoverSnapshotReady = false;
    s.takeoverFallbackLaneMask = 3u;
    if (endpoint != 0u) {
        const qreplay::CanonicalTickBundleV2* tail =
            s.sessionV2->tickAt(endpoint - 1u);
        if (tail != nullptr && tail->frame.core.activeLaneMask != 0u)
            s.takeoverFallbackLaneMask = tail->frame.core.activeLaneMask;
    }
    if (!s.takeoverAvailable && !s.sessionV2->headless()) {
        QLOG("[replay.takeover] unavailable: branch storage allocation failed "
             "capacity=%u", takeoverCapacity);
    }
    QLOG("[replay.v3] finite game ready frames=%u endpoint=%u headless=%u "
         "generation=%u verification=%s",
         s.sessionV2->endpoint().nextRecord,
         static_cast<unsigned>(s.sessionV2->endpoint().kind),
         s.sessionV2->headless() ? 1u : 0u,
         s.sessionV2->start().manifest.identity.gameGeneration,
         qreplay::replayVerificationModeTextV2(s.sessionV2->mode()));
    if (!s.sessionV2->byteExactStateProof()) {
        QLOG("[replay.v3] BEHAVIOUR-VERIFIED playback: %s  The per-frame "
             "deterministic STATE HASHES are not enforced for this file.  "
             "Everything else still is: the recorded input stream, the "
             "lifecycle tuple, the native dispatcher result, the rolling "
             "digest chain, event ordering, the frame count and the recorded "
             "terminal.",
             s.sessionV2->start().manifest.identity.gameGeneration > 1u
                 ? "this recording is a rematch, so it began on top of the "
                   "previous game's memory and a fresh boot cannot reproduce "
                   "that image byte for byte."
                 : "this recording was made under an earlier state-proof "
                   "schema, whose stored hashes this build cannot reproduce "
                   "bit for bit.");
    }
    return true;
}

qreplay::ipc::v3::PlaybackStatusPayload playbackStatusV3(
    const RuntimeState& s) {
    qreplay::ipc::v3::PlaybackStatusPayload status{};
    status.phase = playbackPhaseV3(s);
    status.paused = transportHoldingV3(s);
    status.seeking = s.navigation.active;
    status.buffering = !s.sessionV2;
    status.headless = s.headlessV3;
    status.terminalArmed = s.sessionV2
        && (s.sessionV2->phase() == SessionPhaseV2::TerminalArmed
            || s.sessionV2->phase()
                == SessionPhaseV2::AwaitingFinalPresentation);
    status.hasFailure = s.sessionV2
        && s.sessionV2->phase() == SessionPhaseV2::Faulted;
    status.speedPermille = s.speedPermilleV3;
    status.lastCommandId = s.lastCommandIdV3;
    status.droppedCommands =
        s.commandDrops.load(std::memory_order_relaxed);
    status.deviceCallCount =
        s.deviceCalls.load(std::memory_order_relaxed);
    status.presentationSerial = static_cast<uint32_t>(
        s.terminalPresentationSerialV3);
    status.terminalPresentationSerial = status.presentationSerial;
    if (s.sessionV2) {
        status.nextRecord = s.sessionV2->nextRecord();
        status.endpointNextRecord =
            s.sessionV2->endpoint().nextRecord;
        status.bufferedFrames = status.endpointNextRecord - status.nextRecord;
        status.rollingDigest =
            s.sessionV2->currentRollingDigest();
        status.postStateHash = s.lastObservedPost;
        if (const qreplay::ipc::v3::PlaybackResultPayload* result =
                s.sessionV2->result()) {
            status.failureReason = result->reason;
            status.mismatchFrame = result->mismatchFrame;
        }
    }
    return status;
}

void submitPlaybackStatusV3(RuntimeState& s, bool force) {
    const uint64_t now = GetTickCount64();
    const bool urgent = force || s.transportStatusDirtyV3;
    if (!urgent && s.lastStatusAtMsV3 != 0u
        && now - s.lastStatusAtMsV3 < 250u) {
        return;
    }
    if (s.worker.submitPlaybackStatusV3(playbackStatusV3(s))) {
        s.lastStatusAtMsV3 = now;
        s.transportStatusDirtyV3 = false;
    }
}

void submitPlaybackResultIfReadyV3(RuntimeState& s) {
    if (!s.sessionV2 || !s.sessionV2->hasResult()) return;
    s.complete = true;
    s.halted = s.sessionV2->phase() == SessionPhaseV2::Faulted;
    if (!s.resultQueuedV3) {
        const qreplay::ipc::v3::PlaybackResultPayload* result =
            s.sessionV2->result();
        if (!result) return;
        if (replayWinScreenPendingV3()) return;
        if (s.resultReadyAtMsV3 == 0u) s.resultReadyAtMsV3 = GetTickCount64();
        const bool completed = qreplay::replayCompletedGame(*result);
        if (completed && !s.headlessV3 && !s.winScreenFailedV3
            && !qreplay::replayFinalHoldElapsed(
                GetTickCount64(), s.resultReadyAtMsV3,
                (s.presentationFlagsV3 & qipc::kReplayHoldFinal) != 0u)) return;
        auto reply = *result;
        if (s.winScreenFailedV3)
            reply.reason = qreplay::ipc::v3::PlaybackResultReason::InternalFailure;
        using Reason = qreplay::ipc::v3::PlaybackResultReason;
        if (reply.reason != Reason::VerifiedCompleted
            && reply.reason != Reason::VerifiedAbnormalEnd
            && reply.reason != Reason::VerifiedUnsealedPrefix
            && reply.reason != Reason::VerifiedBehaviourOnly
            && reply.reason != Reason::UserExit)
            qoh99::Log::get().issue();
        reply.presentationFlags = currentDisplayFlagsV3(s);
        if (s.worker.submitPlaybackResultV3(reply)) {
            s.resultQueuedV3 = true;
            QLOG("[replay.v3] result queued reason=%u nextRecord=%u "
                 "fault=%u",
                 static_cast<unsigned>(reply.reason),
                 result->nextRecordConsumed,
                 static_cast<unsigned>(s.sessionV2->fault()));
        }
    }
}

void submitPlaybackCommandAckV3(
    RuntimeState& s,
    const qreplay::ipc::v3::PlaybackCommandPayload& command,
    qreplay::ipc::v3::PlaybackCommandAckResult result,
    uint64_t effectiveArgument = 0u,
    uint32_t detail = 0u) {
    qreplay::ipc::v3::PlaybackCommandAckPayload ack{};
    ack.commandId = command.commandId;
    ack.kind = command.kind;
    ack.result = result;
    ack.phase = playbackPhaseV3(s);
    ack.effectiveArgument = effectiveArgument;
    ack.nextRecord = s.sessionV2 ? s.sessionV2->nextRecord() : 0u;
    ack.detail = detail;
    if (!s.worker.submitPlaybackCommandAckV3(ack) && s.sessionV2)
        (void)s.sessionV2->reportBrokerFailure();
}

void servicePlaybackCommandsV3(RuntimeState& s) {
    qreplay::ipc::v3::PlaybackCommandPayload command{};
    while (s.worker.popPlaybackCommandV3(command)) {
        using Kind = qreplay::ipc::v3::PlaybackCommandKind;
        using Ack = qreplay::ipc::v3::PlaybackCommandAckResult;
        if (command.commandId <= s.lastCommandIdV3) {
            submitPlaybackCommandAckV3(s, command, Ack::StaleCommand);
            continue;
        }
        s.lastCommandIdV3 = command.commandId;
        if (!s.sessionV2) {
            submitPlaybackCommandAckV3(s, command, Ack::RejectedPhase);
            continue;
        }
        switch (command.kind) {
            case Kind::Pause:
                if (!transportMayHoldV3(s)) {
                    submitPlaybackCommandAckV3(
                        s, command, Ack::RejectedPhase);
                } else {
                    s.pausedV3 = true;
                    s.transportStatusDirtyV3 = true;
                    submitPlaybackCommandAckV3(
                        s, command, Ack::Applied, 0u);
                }
                break;
            case Kind::Resume:
                if (s.sessionV2->hasResult() || s.takeover.active()
                    || s.navigation.active) {
                    submitPlaybackCommandAckV3(
                        s, command, Ack::RejectedPhase);
                } else {
                    s.pausedV3 = false;
                    s.pendingStepsV3 = 0u;
                    s.transportStatusDirtyV3 = true;
                    submitPlaybackCommandAckV3(
                        s, command, Ack::Applied, 0u);
                }
                break;
            case Kind::Step:
                if (!s.bootstrapCompleteV3 || !transportMayHoldV3(s)) {
                    submitPlaybackCommandAckV3(
                        s, command, Ack::RejectedPhase);
                } else {
                    s.pausedV3 = true;
                    if (s.pendingStepsV3 != 0xFFFFFFFFu)
                        ++s.pendingStepsV3;
                    s.transportStatusDirtyV3 = true;
                    submitPlaybackCommandAckV3(
                        s, command, Ack::Queued, 0u);
                }
                break;
            case Kind::SpeedSet:
                if (!qreplay::ipc::v3::isSupportedPlaybackSpeedV3(
                        command.argument)) {
                    submitPlaybackCommandAckV3(
                        s, command, Ack::RejectedArgument);
                } else if (s.sessionV2->hasResult()
                           || s.takeover.active()) {
                    submitPlaybackCommandAckV3(
                        s, command, Ack::RejectedPhase);
                } else {
                    applyPlaybackSpeedV3(
                        s, static_cast<uint32_t>(command.argument));
                    submitPlaybackCommandAckV3(
                        s, command, Ack::Applied, command.argument);
                }
                break;
            case Kind::Restart:
            case Kind::SeekFrame: {
                const uint64_t target = command.kind == Kind::Restart
                    ? 0u : command.argument;
                const bool accepted = requestNavigationV3(s, target);
                submitPlaybackCommandAckV3(s, command,
                    accepted ? Ack::Queued : Ack::RejectedPhase, target);
                break;
            }
            case Kind::SeekRound:
                submitPlaybackCommandAckV3(
                    s, command, Ack::RejectedPhase);
                break;
            case Kind::Exit: {
                if (!s.sessionV2->hasResult())
                    (void)s.sessionV2->requestUserExit();
                submitPlaybackCommandAckV3(
                    s, command, Ack::Applied, 0u);
                if (s.resultQueuedV3
                    && s.sessionV2->acknowledgeResultDelivery()
                    && s.sessionV2->requestGracefulExit()) {
                    const qreplay::ipc::v3::PlaybackResultPayload* result =
                        s.sessionV2->result();
                    qreplay::ipc::v3::PlaybackByePayload bye{};
                    bye.reason = qreplay::replayWinScreenByeReason(
                        result ? result->reason
                               : qreplay::ipc::v3::PlaybackResultReason::InternalFailure,
                        s.sessionV2->fault() != qreplay::ReplayPlaybackFaultV2::None,
                        s.winScreenFailedV3);
                    bye.resultSent = true;
                    bye.nextRecord = s.sessionV2->nextRecord();
                    bye.lastCommandId = command.commandId;
                    if (s.worker.submitPlaybackByeV3(bye)) {
                        s.byeQueuedV3 = true;
                        s.byeQueuedAtMsV3 = GetTickCount64();
                    }
                }
                break;
            }
        }
    }
}

void servicePlaybackV3(RuntimeState& s) {
    (void)installPlaybackPrebufferV3(s);
    servicePlaybackCommandsV3(s);
    submitPlaybackResultIfReadyV3(s);
    qreplay::ipc::v3::PlaybackNextGamePayload next{};
    if (s.worker.popPlaybackNextGameV3(next)) {
        const auto* result = s.sessionV2 ? s.sessionV2->result() : nullptr;
        if (s.nextGamePendingV3 || !s.resultQueuedV3 || s.byeQueuedV3
            || s.winScreenFailedV3 || !result || !qreplay::replayCompletedGame(*result)) {
            s.halted = true;
            s.exitRequested = true;
            QLOG("[replay.folder] refused next game outside completed result boundary");
        } else {
            s.nextGameV3 = next;
            s.nextGamePendingV3 = true;
        }
    }
    submitPlaybackStatusV3(s, false);
    if (s.byeQueuedV3
        && GetTickCount64() - s.byeQueuedAtMsV3 >= 100u) {
        s.exitRequested = true;
    }
}

}

bool replayRecordStageFreshGameV2(
    const ReplayFreshGameCaptureV2& capture) {
    RecorderState& r = rec();
    if (!replayRecorderV2Active())
        return false;
    drainRecorderV2CommitAcks(r);
    if (r.epochTracker.failed()
        || r.coordinatorV2->state()
            == qreplay::ReplayGameRecordStateV2::Failed) {
        return false;
    }
    if (r.epochTracker.hasPendingLaunch()) {
        return capture.launchSourceFrame
                == r.stagedLaunchSourceFrame;
    }
    if (r.coordinatorV2->state()
        != qreplay::ReplayGameRecordStateV2::WaitingForFreshGame) {
        return false;
    }
    qreplay::ConfirmedGameStartV2 start{};
    if (!buildFreshGameStartV2(r, capture, start)) {
        r.lastV2Fault =
            qreplay::ReplayRecordingFaultV2::InvalidFreshGame;
        return false;
    }
    const qreplay::GameEpochTrackerResultV2 staged =
        r.epochTracker.stageLaunchBarrier(
            capture.launchSourceFrame, start);
    if (staged != qreplay::GameEpochTrackerResultV2::LaunchStaged
        && staged
            != qreplay::GameEpochTrackerResultV2::ExactDuplicate) {
        r.lastV2Fault =
            qreplay::ReplayRecordingFaultV2::InvalidFreshGame;
        return false;
    }
    if (staged == qreplay::GameEpochTrackerResultV2::LaunchStaged) {
        r.stagedIdentity = start.manifest.identity;
        r.stagedStart = start;
        r.stagedLaunchSourceFrame =
            capture.launchSourceFrame;
        ++r.nextGameGeneration;
        ++r.nextGameOrdinal;
    }
    return true;
}

bool replayRecordSetSourceLaneNamesV2(
    const std::array<std::string, 2>& names) {
    RecorderState& r = rec();
    if (!replayRecorderV2Active()) return false;
    if (r.epochTracker.hasPendingLaunch()
        || r.coordinatorV2->state()
               != qreplay::ReplayGameRecordStateV2::WaitingForFreshGame) {
        return false;
    }
    for (size_t lane = 0u; lane < 2u; ++lane) {
        if (!r.lanesV2[lane].active) continue;
        if (!qreplay::setReplayTextV2(r.lanesV2[lane].name, names[lane]))
            return false;
    }
    return true;
}

bool replayRecordConfirmedV2(
    const ReplayNativeFrameV2& source) {
    RecorderState& r = rec();
    if (!replayRecorderV2Active())
        return false;
    drainRecorderV2CommitAcks(r);

    if (r.epochTracker.hasPendingLaunch()) {
        if (source.sourceFrame <= r.stagedLaunchSourceFrame)
            return true;
        qreplay::FreshGameDescriptorV2 fresh{};
        const qreplay::GameEpochTrackerResultV2 observed =
            r.epochTracker.observeBeforeFrame(
                source.sourceFrame, source.core.before, fresh);
        if (observed
            == qreplay::GameEpochTrackerResultV2::Failed) {
            r.lastV2Fault =
                qreplay::ReplayRecordingFaultV2::InvalidFreshGame;
            return false;
        }
        if (observed
            == qreplay::GameEpochTrackerResultV2::EpochArmed) {
            const qreplay::ReplayPublishResultV2 armed =
                r.coordinatorV2->armFreshGame(fresh);
            if (armed.code
                != qreplay::ReplayPublishCodeV2::Armed) {
                r.lastV2Fault = armed.fault;
                return false;
            }
            r.activeIdentity = fresh.start.manifest.identity;
            r.stagedIdentity = qreplay::GameIdentityV2{};
            r.stagedStart = qreplay::ConfirmedGameStartV2{};
            r.stagedLaunchSourceFrame = 0u;
        } else if (observed
                   == qreplay::GameEpochTrackerResultV2::Ignored) {
            return true;
        }
    }

    const qreplay::ReplayGameRecordStateV2 state =
        r.coordinatorV2->state();
    if (state
            == qreplay::ReplayGameRecordStateV2::WaitingForFreshGame
        || state == qreplay::ReplayGameRecordStateV2::Disabled) {
        return true;
    }
    if (!qreplay::isValidGameIdentityV2(r.activeIdentity))
        return false;

    qreplay::ProvenGameFrameV2 proven{};
    proven.sourceFrame = source.sourceFrame;
    proven.gameEpoch =
        r.activeIdentity.gameGeneration;
    proven.game = r.activeIdentity;
    proven.core = source.core;
    proven.roundTransition = source.roundTransition;
    proven.activeFighterMask = source.activeFighterMask;
    proven.roster = source.roster;
    proven.stage = source.stage;
    const qreplay::ReplayPublishResultV2 published =
        r.coordinatorV2->acceptConfirmedFrame(proven);
    if (published.code == qreplay::ReplayPublishCodeV2::Failed) {
        r.lastV2Fault = published.fault;
        if (state != qreplay::ReplayGameRecordStateV2::Failed) {
            QLOG_ISSUE("[replay.record-v2] recording disabled source=%u state=%u "
                 "fault=%s lifecycle=%u/%d->%u/%d roundFlags=%u "
                 "roundOrdinal=%u timeline=%s",
                 source.sourceFrame, static_cast<unsigned>(state),
                 qreplay::replayRecordingFaultTextV2(published.fault),
                 source.core.before.scene, source.core.before.matchSub,
                 source.core.after.scene, source.core.after.matchSub,
                 source.roundTransition.flags,
                 source.roundTransition.roundOrdinal,
                 qreplay::confirmedTimelineFaultTextV2(
                     r.coordinatorV2->lastTimelineFault()));
            if (published.fault
                    == qreplay::ReplayRecordingFaultV2::TimelineValidationFailure) {
                const qreplay::GameLifecycleV2& expected =
                    r.coordinatorV2->timelineExpectedLifecycle();
                const qreplay::GameLifecycleV2& observed =
                    r.coordinatorV2->timelineObservedLifecycle();
                QLOG("[replay.record-v2] continuity expected="
                     "[%u,%u,%d,%u,%u,%u,%u,%u/%u,%d,%u/%u/%u] "
                     "observed=[%u,%u,%d,%u,%u,%u,%u,%u/%u,%d,%u/%u/%u] "
                     "proof=%016llX/%016llX",
                     expected.scene, expected.requestedScene,
                     expected.matchSub, expected.sceneThreeState,
                     expected.roundState, expected.roundCounter,
                     expected.resultCounter, expected.roundTallies[0],
                     expected.roundTallies[1], expected.winnerSlot,
                     expected.routeEpoch, expected.rosterEpoch,
                     expected.layoutGeneration,
                     observed.scene, observed.requestedScene,
                     observed.matchSub, observed.sceneThreeState,
                     observed.roundState, observed.roundCounter,
                     observed.resultCounter, observed.roundTallies[0],
                     observed.roundTallies[1], observed.winnerSlot,
                     observed.routeEpoch, observed.rosterEpoch,
                     observed.layoutGeneration,
                     static_cast<unsigned long long>(
                         r.coordinatorV2->timelineExpectedStateProof()),
                     static_cast<unsigned long long>(
                         r.coordinatorV2->timelineObservedStateProof()));
            }
            if (published.fault
                    == qreplay::ReplayRecordingFaultV2::InvalidRoundTransition) {
                const qreplay::RoundTransitionProofV2& proof =
                    source.roundTransition;
                QLOG("[replay.record-v2] round-proof site=%u flags=%u "
                     "ordinal=%u winner=%d mask=%u tallies=%u/%u->%u/%u "
                     "roundState=%u->%u roundCounter=%u->%u "
                     "resultCounter=%u->%u lifecycleWinner=%d->%d",
                     proof.transitionSiteId, proof.flags,
                     proof.roundOrdinal, proof.winnerSlot,
                     proof.winningSideMask, proof.talliesBefore[0],
                     proof.talliesBefore[1], proof.talliesAfter[0],
                     proof.talliesAfter[1], proof.roundStateBefore,
                     proof.roundStateAfter, proof.roundCounterBefore,
                     proof.roundCounterAfter, proof.resultCounterBefore,
                     proof.resultCounterAfter,
                     source.core.before.winnerSlot,
                     source.core.after.winnerSlot);
            }
        }
        return false;
    }
    if (published.code
        == qreplay::ReplayPublishCodeV2::TerminalPublished) {
        r.activeIdentity = qreplay::GameIdentityV2{};
    }
    if (published.code == qreplay::ReplayPublishCodeV2::Published
        || published.code
            == qreplay::ReplayPublishCodeV2::TerminalPublished) {
        ++r.published;
    }
    return true;
}

bool replayAbandonOpenGameV2(
    const qreplay::TerminalReason reason,
    const qreplay::ReplayCloseProvenanceV2 provenance) {
    RecorderState& r = rec();
    if (!replayRecorderV2Active()) return false;
    const bool hadPendingLaunch = r.epochTracker.hasPendingLaunch();
    if (hadPendingLaunch) {
        r.epochTracker.reset();
        r.stagedIdentity = qreplay::GameIdentityV2{};
        r.stagedStart = qreplay::ConfirmedGameStartV2{};
        r.stagedLaunchSourceFrame = 0u;
        QLOG("[replay.record-v2] abandoned a staged launch barrier that no "
             "record had armed yet");
    }
    const bool ended = replayRecordAbnormalEndV2(reason, provenance);
    return ended || hadPendingLaunch;
}

bool replayRecordAbnormalEndV2(
    const qreplay::TerminalReason reason,
    const qreplay::ReplayCloseProvenanceV2 provenance) {
    RecorderState& r = rec();
    if (!replayRecorderV2Active())
        return false;
    drainRecorderV2CommitAcks(r);
    const qreplay::ReplayPublishResultV2 ended =
        r.coordinatorV2->requestAbnormalEnd(
            reason, provenance);
    if (ended.code == qreplay::ReplayPublishCodeV2::Failed) {
        r.lastV2Fault = ended.fault;
        return false;
    }
    if (ended.code
            == qreplay::ReplayPublishCodeV2::TerminalPublished
        || ended.code == qreplay::ReplayPublishCodeV2::NoFile) {
        r.activeIdentity = qreplay::GameIdentityV2{};
    }
    return true;
}

bool replayRecorderManifestPublished() { return rec().manifestPublished; }

bool replayRecordManifest() {
    RecorderState& r = rec();
    if (!r.active || !r.queue) return false;
    if (r.manifestPublished) return true;
    if (r.queue->disabled()) return false;

    if (!r.manifestReady) {
        QLOG("[replay.record] generation=%u source=%u state=disabled(manifest "
             "unavailable: %s) - the match is unaffected",
             r.generation, r.sourceKind,
             r.manifestFailure != nullptr ? r.manifestFailure : "?");
        disableRecordingGeneration(
            r, static_cast<uint16_t>(
                   qreplay::TerminalReason::RecordingFailure));
        return false;
    }

    if (!r.queue->publishFor(r.generation, r.manifestMessage)) {
        QLOG("[replay.record] generation=%u source=%u state=disabled(manifest "
             "could not be published) - the match is unaffected",
             r.generation, r.sourceKind);
        if (r.worker) r.worker->noteRecorderDrop();
        disableRecordingGeneration(
            r, static_cast<uint16_t>(
                   qreplay::TerminalReason::RecordingFailure));
        return false;
    }
    r.manifestPublished = true;
    QLOG("[replay.record] generation=%u source=%u session=%016llX%016llX "
         "state=manifest-published bytes=%u",
         r.generation, r.sourceKind,
         static_cast<unsigned long long>(
             r.manifestMessage.manifest.prefix.sessionUuidHigh),
         static_cast<unsigned long long>(
             r.manifestMessage.manifest.prefix.sessionUuidLow),
         r.manifestMessage.manifest.bytes);
    return true;
}

bool replayRecorderSessionStartPublished() {
    return rec().sessionStartPublished;
}

bool replayRecordSessionStart(const qreplay::DeterministicStart& start) {
    RecorderState& r = rec();
    if (!r.active || !r.queue) return false;
    if (r.sessionStartPublished) return true;
    if (r.queue->disabled()) return false;
    if (!r.manifestPublished) {
        QLOG("[replay.record] generation=%u source=%u state=disabled(session "
             "start before the manifest) - the match is unaffected",
             r.generation, r.sourceKind);
        disableRecordingGeneration(
            r, static_cast<uint16_t>(
                   qreplay::TerminalReason::RecordingFailure));
        return false;
    }

    qreplay::RecorderMessage message{};
    message.kind = qreplay::RecorderMessageKind::SessionStart;
    message.matchStart.start = start;
    if (!r.queue->publishFor(r.generation, message)) {
        QLOG("[replay.record] generation=%u source=%u state=disabled(session "
             "start could not be published) - the match is unaffected",
             r.generation, r.sourceKind);
        if (r.worker) r.worker->noteRecorderDrop();
        disableRecordingGeneration(
            r, static_cast<uint16_t>(
                   qreplay::TerminalReason::RecordingFailure));
        return false;
    }
    r.sessionStartPublished = true;
    QLOG("[replay.record] generation=%u source=%u state=session-start-published "
         "firstFrame=%u phase=%08X seeds=[%08X,%08X,%08X] bootstrap=%016llX "
         "config=[timer=%u ai=%u life=%u roundsMinusOne=%u] state=%016llX "
         "digest=%016llX",
         r.generation, r.sourceKind, start.firstTimelineFrame,
         start.initialPhase, start.seedGameplay, start.seedCosmetic,
         start.selector,
         static_cast<unsigned long long>(start.bootstrapHash),
         start.timerMode, start.aiDifficulty, start.lifeMultiplier,
         start.roundsToWinMinusOne,
         static_cast<unsigned long long>(start.initialStateHash),
         static_cast<unsigned long long>(start.initialDigest));
    return true;
}

static bool publishMatchRecord(RecorderState& r,
                               const qreplay::RecorderMessage& message,
                               const char* what) {
    if (!r.queue->publishFor(r.generation, message)) {
        QLOG("[replay.record] generation=%u source=%u state=disabled(%s could "
             "not be published) - the match is unaffected",
             r.generation, r.sourceKind, what ? what : "?");
        if (r.worker) {
            r.worker->noteRecorderDrop();
            r.worker->disableRecording(static_cast<uint16_t>(
                qreplay::TerminalReason::RecorderOverflow));
        }
        return false;
    }
    return true;
}

bool replayRecordMatchStart(const qreplay::MatchStartRecord& record) {
    RecorderState& r = rec();
    if (!r.active || !r.queue) return false;
    if (r.queue->disabled()) return false;
    if (!r.sessionStartPublished) {
        QLOG("[replay.record] generation=%u source=%u state=disabled(match "
             "start before the session start) - the match is unaffected",
             r.generation, r.sourceKind);
        disableRecordingGeneration(
            r, static_cast<uint16_t>(
                   qreplay::TerminalReason::RecordingFailure));
        return false;
    }
    qreplay::RecorderMessage message{};
    message.kind = qreplay::RecorderMessageKind::MatchStart;
    message.matchStart = record;
    if (!publishMatchRecord(r, message, "match start")) return false;
    ++r.matchesStarted;
    QLOG("[replay.record] generation=%u match=%u ordinal=%u state=match-start "
         "firstFrame=%u independent=%u dependsOn=%u vsRow=%u cpuMask=%02X "
         "roster=[%u,%u,%u,%u] rosterEpoch=%u",
         r.generation, record.matchId, record.ordinal,
         record.firstTimelineFrame, record.independentBootstrap ? 1u : 0u,
         record.dependsOnMatchId, static_cast<unsigned>(record.vsRow),
         static_cast<unsigned>(record.cpuMask),
         static_cast<unsigned>(record.slots[0].identity.nativeCharacterId),
         static_cast<unsigned>(record.slots[1].identity.nativeCharacterId),
         static_cast<unsigned>(record.slots[2].identity.nativeCharacterId),
         static_cast<unsigned>(record.slots[3].identity.nativeCharacterId),
         record.rosterEpoch);
    return true;
}

bool replayRecordMatchEnd(const qreplay::MatchEndRecord& record) {
    RecorderState& r = rec();
    if (!r.active || !r.queue) return false;
    if (r.queue->disabled()) return false;
    if (!r.sessionStartPublished) {
        QLOG("[replay.record] generation=%u source=%u state=disabled(match end "
             "before the session start) - the match is unaffected",
             r.generation, r.sourceKind);
        disableRecordingGeneration(
            r, static_cast<uint16_t>(
                   qreplay::TerminalReason::RecordingFailure));
        return false;
    }
    qreplay::RecorderMessage message{};
    message.kind = qreplay::RecorderMessageKind::MatchEnd;
    message.matchEnd = record;
    if (!publishMatchRecord(r, message, "match end")) return false;
    ++r.matchesEnded;
    QLOG("[replay.record] generation=%u match=%u state=match-end lastFrame=%u "
         "reason=%u winners=%02X rounds=[%u,%u] postTail=%016llX "
         "digest=%016llX",
         r.generation, record.matchId, record.lastTimelineFrame,
         static_cast<unsigned>(record.reason),
         static_cast<unsigned>(record.winningSideMask),
         static_cast<unsigned>(record.roundsSideZero),
         static_cast<unsigned>(record.roundsSideOne),
         static_cast<unsigned long long>(record.postTailStateHash),
         static_cast<unsigned long long>(record.rollingDigest));
    return true;
}

bool replayRecordConfirmed(const qreplay::ReplayFrameV1& frame) {
    RecorderState& r = rec();
    if (!r.active || !r.queue) return false;
    if (r.queue->disabled()) return false;

    qreplay::RecorderMessage message{};
    message.kind = qreplay::RecorderMessageKind::Frame;
    message.frame = frame;
    if (!r.queue->publishFor(r.generation, message)) {
        if (r.worker) {
            r.worker->noteRecorderDrop();
            r.worker->disableRecording(static_cast<uint16_t>(
                qreplay::TerminalReason::RecorderOverflow));
        }
        return false;
    }
    ++r.published;
    return true;
}

bool replayRecordEvent(const qreplay::ReplayEvent& event) {
    RecorderState& r = rec();
    if (!r.active || !r.queue) return false;
    if (r.queue->disabled()) return false;
    qreplay::RecorderMessage message{};
    message.kind = qreplay::RecorderMessageKind::Event;
    message.event = event;
    if (!r.queue->publishFor(r.generation, message)) {
        if (r.worker) {
            r.worker->noteRecorderDrop();
            r.worker->disableRecording(static_cast<uint16_t>(
                qreplay::TerminalReason::RecorderOverflow));
        }
        return false;
    }
    return true;
}

void replayRecordTerminal(uint16_t reason, uint32_t lastTimelineFrame,
                          uint64_t postTailStateHash, uint64_t rollingDigest) {
    RecorderState& r = rec();
    if (!r.active || !r.queue) return;
    qreplay::RecorderMessage message{};
    message.kind = qreplay::RecorderMessageKind::Terminal;
    message.sessionEnd.reason =
        static_cast<qreplay::TerminalReason>(reason);
    message.sessionEnd.lastTimelineFrame = lastTimelineFrame;
    message.sessionEnd.postTailStateHash = postTailStateHash;
    message.sessionEnd.rollingDigest = rollingDigest;
    (void)r.queue->publishFor(r.generation, message);
}

ReplayRecorderStats replayRecorderStats() {
    RecorderState& r = rec();
    ReplayRecorderStats out{};
    out.active = r.active ? 1u : 0u;
    out.generation = r.generation;
    out.published = r.published;
    out.manifestPublished = r.manifestPublished ? 1u : 0u;
    out.manifestBytes = r.manifestReady ? r.manifestMessage.manifest.bytes : 0u;
    out.sessionStartPublished = r.sessionStartPublished ? 1u : 0u;
    out.matchesStarted = r.matchesStarted;
    out.matchesEnded = r.matchesEnded;
    if (r.queue) {
        out.disabled = r.queue->disabled() ? 1u : 0u;
        out.dropped = r.queue->droppedCount();
        out.queueHighWater = r.queue->highWater();
    } else if (r.queueV2) {
        out.disabled = r.queueV2->disabled() ? 1u : 0u;
        out.dropped =
            r.queueV2->ordinaryRejects()
            + r.queueV2->reservedEndRejects()
            + r.queueV2->staleRejects();
        out.queueHighWater = r.queueV2->highWater();
    }
    if (r.worker) {
        const ReplayIpcStats stats = r.worker->stats();
        out.written = stats.recorderRecordsSent;
        out.disableReason = stats.recordingDisableReason;
        if (stats.recordingDisabled) out.disabled = 1u;
    }
    if (r.active && r.published >= r.lastLoggedPublished + kRecordLogInterval) {
        r.lastLoggedPublished = r.published;
        QLOG("[replay.record] published=%llu confirmed=%llu written=%llu "
             "queueHighWater=%llu",
             static_cast<unsigned long long>(out.published),
             static_cast<unsigned long long>(out.published - out.dropped),
             static_cast<unsigned long long>(out.written),
             static_cast<unsigned long long>(out.queueHighWater));
    }
    return out;
}

void pumpWorkerForTest() {
    RuntimeState& s = rt();
    if (s.active) s.worker.pumpOnceForTest();
    RecorderState& r = rec();
    if (r.active && r.worker) r.worker->pumpOnceForTest();
}

ReplayIpcWorker* replayWorkerForTest() {
    RuntimeState& s = rt();
    if (s.active) return &s.worker;
    RecorderState& r = rec();
    return r.worker.get();
}

}
