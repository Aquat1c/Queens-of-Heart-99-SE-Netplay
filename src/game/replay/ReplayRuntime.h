#pragma once

#include "game/replay/ReplayTimelineSource.h"
#include "ipc/IpcConfig.h"
#include "replay/ReplayPlaybackController.h"
#include "replay/ReplayRecordingCoordinator.h"
#include "replay/ReplayTakeoverController.h"
#include "replay/ReplayTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace qgame::replay {

enum class HaltReason : uint16_t {
    None = 0,
    LaunchRejected,
    ManifestMismatch,
    ExpectedFrameMismatch,
    LaneShapeMismatch,
    InvalidActionMask,
    DigestBreak,
    PreStateMismatch,
    PostStateMismatch,
    TerminalProofMismatch,
    ChannelFault,
    SeekFailed,
    CallerReportedMismatch,
    DeviceCallObserved,
};

const char* haltReasonText(HaltReason reason);

struct ReplayHaltDiagnostic {
    HaltReason reason = HaltReason::None;
    uint32_t replayFrame = 0u;
    uint32_t matchId = 0u;
    uint32_t matchOrdinal = 0u;
    uint64_t sessionUuidLow = 0u;
    uint64_t sessionUuidHigh = 0u;
    uint64_t manifestDigest = 0u;
    uint64_t expectedStateHash = 0u;
    uint64_t actualStateHash = 0u;
    uint64_t expectedDigest = 0u;
    uint64_t actualDigest = 0u;
    qreplay::LifecycleTag lifecycleBefore{};
    qreplay::LifecycleTag lifecycleAfter{};
    uint32_t matchSubstate = 0u;
    std::array<uint16_t, qreplay::kSourceLanes> inputMasks{};
    uint8_t activeLaneMask = 0u;
    uint8_t expectedLaneMask = 0u;
    uint64_t simulationContractId = 0u;
    uint64_t contentDigest = 0u;
    uint16_t sourceFault = 0u;
    uint16_t timelineStatus = 0u;
    char regionDetail[96]{};
};

struct ReplayStatusSnapshot {
    uint8_t active = 0u;
    uint8_t halted = 0u;
    uint8_t paused = 0u;
    uint8_t buffering = 0u;
    uint8_t sealed = 0u;
    uint8_t complete = 0u;
    uint8_t seeking = 0u;
    uint8_t sourceState = 0u;
    uint8_t phase = 0u;
    uint32_t currentFrame = 0u;
    uint32_t firstFrame = 0u;
    uint32_t lastFrame = 0u;
    uint32_t bufferedFrames = 0u;
    uint32_t speedPermille = 0u;
    uint32_t matchOrdinal = 0u;
    uint32_t fastForwardRemaining = 0u;
    uint64_t rollingDigest = 0u;
    uint64_t lastPostStateHash = 0u;
    uint64_t ticksExecuted = 0u;
    uint64_t hiddenTicksExecuted = 0u;
    uint64_t presentedSlots = 0u;
    uint64_t underruns = 0u;
    uint64_t deviceCalls = 0u;
    uint64_t snapshotCaptures = 0u;
    uint64_t snapshotRestores = 0u;
    uint16_t haltReason = 0u;
    uint32_t pendingSteps = 0u;
    uint8_t transportMayHold = 0u;
};

enum class PresentationCommand : uint32_t {
    None = 0,
    Pause = 1,
    Resume = 2,
    TogglePause = 3,
    Step = 4,
    SetSpeedPermille = 5,
    SeekFrame = 6,
    SeekMatch = 7,
    SeekRound = 8,
    Restart = 9,
    NextMatch = 10,
    PreviousMatch = 11,
    Exit = 12,
    SpeedStepSlower = 13,
    SpeedStepFaster = 14,
    TakeoverExit = 15,
    TakeoverP1 = 16,
    TakeoverP2 = 17,
    TakeoverRetry = 18,
    TakeoverDelayDecrease = 19,
    TakeoverDelayIncrease = 20,
    TakeoverSetDelay = 21,
    ToggleCollisionDisplay = 22,
    StepBack = 23,
};

constexpr bool isKnownPresentationCommand(uint32_t value) {
    return value >= static_cast<uint32_t>(PresentationCommand::Pause)
        && value <= static_cast<uint32_t>(
               PresentationCommand::StepBack);
}

constexpr size_t kPresentationQueueCapacity = 64u;

constexpr uint32_t kReplayTransportApiVersion = 2u;

// Pointer-free u32-word POD read from the render thread: it must never dereference anything the game thread owns.
struct ReplayTransportStatusV2 {
    uint32_t structBytes = 0u;
    uint32_t apiVersion = 0u;
    uint32_t generation = 0u;
    uint32_t active = 0u;
    uint32_t paused = 0u;
    uint32_t mayHold = 0u;
    uint32_t speedPermille = 0u;
    uint32_t pendingSteps = 0u;
    uint32_t nextRecord = 0u;
    uint32_t endpointRecord = 0u;
    uint32_t phase = 0u;
    uint32_t droppedCommands = 0u;
    uint32_t takeoverAvailable = 0u;
    uint32_t takeoverPhase = 0u;
    uint32_t takeoverLane = 0u;     // 0=none, 1=P1, 2=P2
    uint32_t takeoverDelay = 0u;
    uint32_t takeoverCountdown = 0u;
    uint32_t takeoverBranchRecord = 0u;
    uint32_t takeoverElapsed = 0u;
    uint32_t takeoverBeyondEndpoint = 0u;
    uint32_t takeoverFault = 0u;
};
static_assert(sizeof(ReplayTransportStatusV2) == 84u,
              "wrapper and runtime takeover status ABI must stay byte-exact");

using ReplayTransportStatusFnV2 =
    int(__stdcall*)(ReplayTransportStatusV2* out, uint32_t structBytes,
                    void* userData);

using ReplayTransportSubmitFnV2 =
    int(__stdcall*)(uint32_t generation, uint32_t command, uint64_t argument,
                    void* userData);

using RegisterReplayTransportProviderFnV2 =
    void(__stdcall*)(uint32_t apiVersion, ReplayTransportStatusFnV2 status,
                     ReplayTransportSubmitFnV2 submit, void* userData);

constexpr const char* kRegisterReplayTransportProviderExport =
    "EFZDX9_RegisterReplayTransportProvider";

bool replayTransportStatus(ReplayTransportStatusV2& out);

bool replayCollisionDisplayRequested();

bool startReplayRuntime(const qipc::IpcConfig& cfg);

void stopReplayRuntime();

bool replayRuntimeActive();

bool replayNextFrame(qreplay::ReplayFrameV1& out);

void replayCommitFrame(const qreplay::ReplayFrameV1& applied,
                       uint64_t observedPostHash);

uint16_t replayInjectedAction(int sourceLane);

struct ReplayTickPlan {
    uint32_t hiddenTicks = 0u;
    bool present = false;
    bool paused = false;
    bool slowMotionHold = false;
    bool takeoverCountdownHold = false;
    bool takeover = false;
};

inline uint32_t replaySpeedTicksForSlotV3(uint32_t speedPermille,
                                          uint32_t& creditPermille) {
    if (speedPermille == 0u) speedPermille = qreplay::kSpeedNormal;
    creditPermille += speedPermille;
    const uint32_t ticks = creditPermille / qreplay::kSpeedNormal;
    creditPermille %= qreplay::kSpeedNormal;
    return ticks;
}

inline uint32_t replaySpeedInitialCreditV3(uint32_t speedPermille) {
    return speedPermille != 0u && speedPermille < qreplay::kSpeedNormal
        ? qreplay::kSpeedNormal - speedPermille
        : 0u;
}

inline uint32_t replayTransportTicksForSlotV3(bool paused,
                                              uint32_t& pendingSteps,
                                              uint32_t speedPermille,
                                              uint32_t& creditPermille,
                                              uint32_t remaining) {
    if (paused) {
        if (pendingSteps == 0u || remaining == 0u) return 0u;
        --pendingSteps;
        return 1u;
    }
    const uint32_t ticks =
        replaySpeedTicksForSlotV3(speedPermille, creditPermille);
    return ticks > remaining ? remaining : ticks;
}

ReplayTickPlan replayPlanOuterSlot();

void replayReportMismatch(uint32_t frame, uint64_t expected, uint64_t actual);

void replaySetSimulationHooks(const SimulationHooks& hooks);

void replaySetRegionDetail(const char* text);

bool replayAddMatchStart(uint32_t ordinal, uint32_t firstTimelineFrame,
                         uint32_t lastTimelineFrame = 0u);

// Bounded and nonblocking. NEVER call this from InputRuntime.
uint32_t replayPresentationGeneration();
bool replaySubmitPresentationCommand(uint32_t generation, uint32_t command,
                                     uint64_t argument);
uint64_t replayDroppedPresentationCommands();

bool replayHalted();
ReplayHaltDiagnostic replayHaltDiagnostic();
void replayStatus(ReplayStatusSnapshot& out);
bool replayExitRequested();
bool replayComplete();

bool replayRecordingActive();

bool replayRecordPalette(const qreplay::ReplayPaletteV2& palette);

bool replayStartReady();
const qreplay::DeterministicStartDescriptor& replayStartDescriptor();

bool replayPlaybackV3();
bool replayHeadlessV3();
bool replayChannelReadyV3();
bool replayChannelFailedV3();
bool replayGameStartV2(qreplay::ConfirmedGameStartV2& out);
qreplay::ReplayVerificationModeV2 replayVerificationModeV2();
bool replayCompleteBootstrapV2(
    const qreplay::ReplayBootstrapObservationV2& observed);
bool replayRewindRequested();
uint32_t replayRewindTarget();
bool replayCaptureCheckpoint(qreplay::ReplayPlaybackSessionV2::Checkpoint& out);
bool replayNavigationActive();
bool replayWinScreenPendingV3();
void replayCompleteWinScreenV3(bool succeeded);
bool replayNextGamePendingV3();
bool replayBeginNextGameV3();
bool replayCompleteRewind(
    bool restored, const qreplay::ReplayBootstrapObservationV2& observed,
    const qreplay::ReplayPlaybackSessionV2::Checkpoint* checkpoint = nullptr);
bool replayPeekFrameV2(qreplay::ConfirmedGameFrameV2& out,
                       bool& finalFrame, bool& headless);
bool replayBeginFrameV2(
    const qreplay::ReplayFrameBeginObservationV2& observed);
bool replayCommitFrameV2(
    const qreplay::ReplayFrameCommitObservationV2& observed);

bool replayTakeoverNeedsSnapshot();
bool replayTakeoverRestorePending();
qreplay::ReplayTakeoverRestoreReason replayTakeoverRestoreReason();
bool replayTakeoverCompleteSnapshot(bool succeeded);
bool replayTakeoverCompleteRestore(bool succeeded);
bool replayTakeoverBeginFrame(uint16_t compactLocalAction,
                              qreplay::ReplayTakeoverResolvedFrame& out);
bool replayTakeoverCommitFrame();
bool replayTakeoverFailBranch();
bool replayTakeoverRequestSafetyExit();
bool replayTakeoverActive();
qreplay::ReplayTakeoverPhase replayTakeoverPhase();
qreplay::ReplayPresentationAckV2 replayAcknowledgeFinalPresentationV2(
    uint64_t presentationSerial);
bool replayAwaitingFinalPresentationV2();
void replayFailPlaybackV2();

bool replayNextDueEvent(qreplay::ReplayEvent& out);

void replayNoteDeviceCall(const char* site);
uint64_t replayDeviceCallCount();
const char* replayFirstDeviceCallSite();
void replayResetDeviceCallCounter();

struct ReplayRecorderStats {
    uint8_t active = 0u;
    uint8_t disabled = 0u;
    uint8_t manifestPublished = 0u;
    uint8_t sessionStartPublished = 0u;
    uint16_t disableReason = 0u;
    uint32_t generation = 0u;
    uint32_t manifestBytes = 0u;
    uint32_t matchesStarted = 0u;
    uint32_t matchesEnded = 0u;
    uint64_t published = 0u;
    uint64_t dropped = 0u;
    uint64_t queueHighWater = 0u;
    uint64_t written = 0u;
};

struct ReplayFreshGameCaptureV2 {
    uint32_t launchSourceFrame = 0u;
    uint32_t layoutGeneration = 0u;
    qreplay::MatchConfigurationV2 configuration{};
    qreplay::RngStateV2 rng{};
    qreplay::FpuContractV2 fpu{};
    uint32_t initialPhase = 0u;
    uint64_t bootstrapFingerprint = 0u;
    std::array<
        qreplay::FighterIdentityV2,
        qreplay::kFighterSlots> roster{};
    std::array<
        qreplay::SelectorProofV2,
        qreplay::kFighterSlots> selectorProof{};
};

struct ReplayNativeFrameV2 {
    uint32_t sourceFrame = 0u;
    qreplay::ConfirmedGameFrameCoreV2 core{};
    qreplay::RoundTransitionProofV2 roundTransition{};
    uint8_t activeFighterMask = 0u;
    std::array<
        qreplay::FighterIdentityV2,
        qreplay::kFighterSlots> roster{};
    qreplay::StageIdentityV2 stage{};
};

bool startReplayRecorder(const qipc::IpcConfig& cfg);
void stopReplayRecorder();
bool replayRecorderActive();
bool replayRecorderV2Active();

qreplay::ReplayGameRecordStateV2 replayRecordStateV2();

bool replayRecordStageFreshGameV2(
    const ReplayFreshGameCaptureV2& capture);

bool replayRecordSetSourceLaneNamesV2(
    const std::array<std::string, 2>& names);

bool replayRecordConfirmedV2(const ReplayNativeFrameV2& source);

bool replayRecordAbnormalEndV2(
    qreplay::TerminalReason reason,
    qreplay::ReplayCloseProvenanceV2 provenance);

bool replayAbandonOpenGameV2(
    qreplay::TerminalReason reason,
    qreplay::ReplayCloseProvenanceV2 provenance);

bool replayRecordManifest();

bool replayRecorderManifestPublished();

bool replayRecordSessionStart(const qreplay::DeterministicStart& start);

bool replayRecorderSessionStartPublished();

bool replayRecordMatchStart(const qreplay::MatchStartRecord& record);
bool replayRecordMatchEnd(const qreplay::MatchEndRecord& record);

// A false return disables this recording generation; the caller must ignore it and continue the match unchanged.
bool replayRecordConfirmed(const qreplay::ReplayFrameV1& frame);
bool replayRecordEvent(const qreplay::ReplayEvent& event);
void replayRecordTerminal(uint16_t reason, uint32_t lastTimelineFrame,
                          uint64_t postTailStateHash, uint64_t rollingDigest);

ReplayRecorderStats replayRecorderStats();

struct ReplayRuntimeTestConfig {
    qipc::IpcConfig ipc{};
    TimelineSourceOptions source{};
    ByteChannel* brokerChannel = nullptr;
    ByteChannel* recorderChannel = nullptr;
    const qreplay::SimulationContractV2* playbackRuntimeContract = nullptr;
    size_t recorderQueueCapacity = 1024u;
    bool recorderV3 = false;
    bool inlineWorker = true;
};

bool startReplayRuntimeForTest(const ReplayRuntimeTestConfig& config);
bool startReplayRecorderForTest(const ReplayRuntimeTestConfig& config);
void pumpWorkerForTest();
ReplayIpcWorker* replayWorkerForTest();

}
