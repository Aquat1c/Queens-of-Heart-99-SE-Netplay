#include "TestHarness.h"
#include "replay/ReplaySimulationContractV2.h"

#include "client/replay/ReplayBrokerV2.h"
#include "client/replay/ReplayPlaybackOutcome.h"

#include "replay/CharacterCatalog.h"
#include "replay/ReplayIntegrity.h"
#include "replay/ReplayWinScreen.h"
#include "replay/StageCatalog.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace qreplay;
using namespace qclient::replay;
namespace wire3 = qreplay::ipc::v3;

std::array<uint8_t, 32> digest(uint8_t seed) {
    std::array<uint8_t, 32> out{};
    for (size_t i = 0u; i < out.size(); ++i)
        out[i] = static_cast<uint8_t>(seed + i);
    return out;
}

qnetplay::compat::CompatibilityIdentity compatibilityIdentity() {
    qnetplay::compat::CompatibilityIdentity identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = 10u;
    (void)qnetplay::compat::setProfileId(identity, "qoh99-se1126");
    const std::array<uint8_t, 32> executable = digest(0x11u);
    const std::array<uint8_t, 32> simulation = digest(0x61u);
    const std::array<uint8_t, 32> validation = digest(0x91u);
    std::copy(
        executable.begin(), executable.end(),
        identity.runtimeBuild.executableSha256);
    identity.runtimeBuild.executableSize = 704512u;
    identity.runtimeBuild.peTimestamp = 0x3A19A8A9u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    std::copy(
        simulation.begin(), simulation.end(),
        identity.simulationContent.simulationRootSha256);
    std::copy(
        validation.begin(), validation.end(),
        identity.simulationContent.validationRootSha256);
    identity.simulationContent.entryCount = 68u;
    identity.simulationContent.requiredEntryCount = 68u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.stateSchemaId = 0x1010101010101010ull;
    identity.modSimulationId = 0x2020202020202020ull;
    identity.timingProfileId = 0x3030303030303030ull;
    return identity;
}

bool verifiedReason(wire3::PlaybackResultReason reason) {
    return reason == wire3::PlaybackResultReason::VerifiedCompleted
        || reason == wire3::PlaybackResultReason::CompletedWithDivergence
        || reason == wire3::PlaybackResultReason::VerifiedAbnormalEnd
        || reason == wire3::PlaybackResultReason::VerifiedUnsealedPrefix
        || reason
            == wire3::PlaybackResultReason::VerifiedBehaviourOnly;
}

ConfirmedGameEventV2 event(
    GameEventKindV2 kind, GameEventPhaseV2 phase,
    uint32_t frame, uint32_t sequence, uint64_t proof) {
    ConfirmedGameEventV2 value{};
    value.kind = kind;
    value.phase = phase;
    value.replayFrame = frame;
    value.eventSequence = sequence;
    value.stateProof = proof;
    if (kind == GameEventKindV2::RoundStarted) {
        value.transitionSiteId = 0x100u;
        value.roundOrdinal = 1u;
    } else if (kind == GameEventKindV2::RoundEnded
               || kind == GameEventKindV2::GameResult) {
        value.transitionSiteId =
            kind == GameEventKindV2::RoundEnded ? 0x200u : 0x201u;
        value.roundOrdinal = 2u;
        value.winningSideMask = 1u;
        value.roundTallies = {{2u, 1u}};
        value.winnerSlot = 0;
    }
    return value;
}

CanonicalTickBundleV2 tick(
    uint32_t sourceFrame, uint32_t replayFrame,
    const GameLifecycleV2& before,
    const GameLifecycleV2& after,
    uint64_t preHash, uint64_t postHash) {
    CanonicalTickBundleV2 value{};
    value.frame.sourceFrame = sourceFrame;
    value.frame.replayFrame = replayFrame;
    value.frame.core.inputDomain = InputDomain::LogicalSourceV1;
    value.frame.core.activeLaneMask = 0x03u;
    value.frame.core.actions[0] =
        static_cast<uint16_t>(replayFrame & 1u ? 0x0100u : 0u);
    value.frame.core.actions[1] =
        static_cast<uint16_t>(replayFrame & 1u ? 0u : 0x0001u);
    value.frame.core.before = before;
    value.frame.core.after = after;
    value.frame.core.dispatcherResult = 1;
    value.frame.core.preStateHash = preHash;
    value.frame.core.postStateHash = postHash;
    return value;
}

ReplayPreflightInputV2 sealedGame(
    uint64_t hashAlgorithm = qreplay::kReplayStateHashAlgorithmIdCurrent,
    uint8_t contentRootSeed = 0x80u) {
    ReplayPreflightInputV2 input{};
    ConfirmedGameStartV2& game = input.start;
    game.manifest.identity.gameUuidLow = 0x1020304050607080ull;
    game.manifest.identity.gameUuidHigh = 0x8070605040302010ull;
    game.manifest.identity.gameGeneration = 1u;

    SimulationContractV2& contract = game.manifest.contract;
    contract.executableSha256 = digest(1u);
    contract.nativeBuildIdentity = 0x1111111111111111ull;
    contract.runtimeSimulationId = 0x2222222222222222ull;
    contract.stateRegionSchemaFingerprint = 0x3333333333333333ull;
    contract.stateHashProjectionId = 0x4444444444444444ull;
    contract.stateHashAlgorithmId = hashAlgorithm;
    contract.contentManifestRoot = digest(contentRootSeed);
    contract.bootstrapSchemaId = 0x6666666666666666ull;
    contract.eventSchemaId = 0x7777777777777777ull;
    contract.characterCatalogId =
        CharacterCatalog::forSeBuild().catalogId();
    contract.terminalResultSchemaId = 0x8888888888888888ull;
    contract.compactActionDomainVersion = 1u;

    GameManifestV2& manifest = game.manifest;
    manifest.vsRow = 1u;
    manifest.activeLaneMask = 0x03u;
    manifest.activeFighterMask = 0x03u;
    manifest.slotToSourceRoute = {{0u, 1u, 2u, 3u}};
    manifest.lanes[0].active = true;
    manifest.lanes[1].active = true;
    (void)setReplayTextV2(manifest.lanes[0].name, "P1");
    (void)setReplayTextV2(manifest.lanes[1].name, "P2");

    const CharacterCatalog& characters =
        CharacterCatalog::forSeBuild();
    for (size_t slot = 0u; slot < kFighterSlots; ++slot)
        manifest.slots[slot].side =
            static_cast<uint8_t>(slot & 1u);
    for (size_t slot = 0u; slot < 2u; ++slot) {
        const uint8_t cell = static_cast<uint8_t>(slot);
        const uint8_t character = characters.baseIdForCell(cell);
        manifest.slots[slot].ownership =
            SlotOwnership::RecordedHuman;
        manifest.slots[slot].identity.nativeCharacterId = character;
        manifest.slots[slot].identity.paletteIndex =
            static_cast<uint8_t>(slot);
        manifest.slots[slot].selectorProofPresent = true;
        manifest.slots[slot].selectorProof.cursorCell = cell;
        manifest.slots[slot].selectorProof.alternateFlag = 0u;
        manifest.slots[slot].selectorProof.confirmState = 4u;
    }
    manifest.routeEpoch = 3u;
    manifest.rosterEpoch = 4u;
    manifest.layoutGeneration = 5u;
    if (!StageCatalogV2::forSeBuild().makeIdentity(
            3u, manifest.stage)) {
        return input;
    }
    manifest.display.recordedUnixSeconds = 1785456000;
    manifest.display.sourceKind = ReplaySourceKindV2::NetplayPlayer;
    (void)setReplayTextV2(manifest.display.title, "Broker V2");

    game.start.configuration.timerMode = 2u;
    game.start.configuration.roundsToWinMinusOne = 1u;
    game.start.rng.gameplay = 1u;
    game.start.rng.cosmetic = 2u;
    game.start.rng.selector = 3u;
    game.start.fpu.x87ControlWord = 0x027Fu;
    game.start.fpu.mxcsr = 0x1F80u;
    game.start.bootstrapFingerprint = 0xABCDEF1234567890ull;
    game.start.frameZeroPreStateHash = 0x1000u;
    game.start.initialRollingDigest = initialReplayDigestV2(game);

    GameLifecycleV2 before{};
    before.scene = 2u;
    before.requestedScene = 2u;
    before.matchSub = 0;
    before.winnerSlot = -1;
    before.routeEpoch = manifest.routeEpoch;
    before.rosterEpoch = manifest.rosterEpoch;
    before.layoutGeneration = manifest.layoutGeneration;
    GameLifecycleV2 active = before;
    active.matchSub = 1;
    GameLifecycleV2 winscreen = active;
    winscreen.matchSub = 2;
    winscreen.roundCounter = 5u;
    winscreen.resultCounter = 0u;
    winscreen.winnerSlot = 0;
    winscreen.roundTallies = {{2u, 1u}};
    GameLifecycleV2 presented = winscreen;
    presented.resultCounter = 1u;

    uint64_t rolling = game.start.initialRollingDigest;
    uint32_t sequence = 0u;
    CanonicalTickBundleV2 zero =
        tick(100u, 0u, before, active, 0x1000u, 0x2000u);
    zero.preEvents.count = 2u;
    zero.preEvents.events[0] = event(
        GameEventKindV2::GameStarted,
        GameEventPhaseV2::PreFrame, 0u, sequence++,
        zero.frame.core.preStateHash);
    zero.preEvents.events[0].rollingDigest =
        advanceReplayDigestV2(rolling, zero.preEvents.events[0]);
    rolling = zero.preEvents.events[0].rollingDigest;
    zero.preEvents.events[1] = event(
        GameEventKindV2::RoundStarted,
        GameEventPhaseV2::PreFrame, 0u, sequence++,
        zero.frame.core.preStateHash);
    zero.preEvents.events[1].rollingDigest =
        advanceReplayDigestV2(rolling, zero.preEvents.events[1]);
    rolling = zero.preEvents.events[1].rollingDigest;
    zero.frame.rollingDigest =
        advanceReplayDigestV2(rolling, zero.frame);
    rolling = zero.frame.rollingDigest;
    input.ticks.push_back(zero);

    CanonicalTickBundleV2 transition =
        tick(101u, 1u, active, winscreen, 0x2000u, 0x3000u);
    transition.frame.rollingDigest =
        advanceReplayDigestV2(rolling, transition.frame);
    rolling = transition.frame.rollingDigest;
    transition.postEvents.count = 1u;
    transition.postEvents.events[0] = event(
        GameEventKindV2::RoundEnded,
        GameEventPhaseV2::PostFrame, 1u, sequence++,
        transition.frame.core.postStateHash);
    transition.postEvents.events[0].rollingDigest =
        advanceReplayDigestV2(
            rolling, transition.postEvents.events[0]);
    rolling = transition.postEvents.events[0].rollingDigest;
    input.ticks.push_back(transition);

    CanonicalTickBundleV2 final =
        tick(102u, 2u, winscreen, presented, 0x3000u, 0x4000u);
    final.frame.rollingDigest =
        advanceReplayDigestV2(rolling, final.frame);
    rolling = final.frame.rollingDigest;
    final.postEvents.count = 1u;
    final.postEvents.events[0] = event(
        GameEventKindV2::GameResult,
        GameEventPhaseV2::PostFrame, 2u, sequence++,
        final.frame.core.postStateHash);
    final.postEvents.events[0].rollingDigest =
        advanceReplayDigestV2(
            rolling, final.postEvents.events[0]);
    rolling = final.postEvents.events[0].rollingDigest;
    input.ticks.push_back(final);

    ConfirmedGameEndV2 end{};
    end.reason = TerminalReason::Completed;
    end.nextRecord = 3u;
    end.winningSideMask = 1u;
    end.roundTallies = {{2u, 1u}};
    end.winnerSlot = 0;
    end.postTailStateHash = 0x4000u;
    end.finalRollingDigest = rolling;
    CanonicalGameDigestV2 canonical;
    if (!canonical.begin(game)) return input;
    for (const CanonicalTickBundleV2& one : input.ticks) {
        for (size_t i = 0u; i < one.preEvents.count; ++i)
            if (!canonical.append(one.preEvents.events[i])) return input;
        if (!canonical.append(one.frame)) return input;
        for (size_t i = 0u; i < one.postEvents.count; ++i)
            if (!canonical.append(one.postEvents.events[i])) return input;
    }
    if (!canonical.finish(end, end.canonicalGameDigest)) return input;

    ReplaySourceProofV2& source = input.source;
    source.classification = ReplaySourceClassificationV2::SealedFile;
    source.fileIdentity.low = 0x12345678u;
    source.fileIdentity.high = 0x87654321u;
    source.structurallyValid = true;
    source.exactlyOneIndependentGame = true;
    source.independentStartVerified = true;
    source.completePostFrameBoundary = true;
    source.hasGameEnd = true;
    source.hasFooter = true;
    source.footerVerified = true;
    source.sourceRangeDigestVerified = true;
    source.hasFileDigest = true;
    source.fileDigestVerified = true;
    source.featureFlags = kFeatureCompatibilityIdentityV3;
    source.validatedSourceByteCount = 4096u;
    source.validatedSourceRangeDigest = digest(0x20u);
    source.fileDigest = digest(0x40u);

    input.endpoint.kind =
        wire3::PlaybackEndpointKind::RecordedGameEnd;
    input.endpoint.hasRecordedGameEnd = true;
    input.endpoint.nextRecord = end.nextRecord;
    input.endpoint.postTailStateHash = end.postTailStateHash;
    input.endpoint.finalRollingDigest = end.finalRollingDigest;
    input.endpoint.validatedSourceByteCount =
        source.validatedSourceByteCount;
    input.endpoint.validatedSourceRangeDigest =
        source.validatedSourceRangeDigest;
    input.endpoint.recordedGameEnd = end;
    input.compatibilityIdentity.status =
        ReplayCompatibilityIdentityStatusV3::Attested;
    input.compatibilityIdentity.identity = compatibilityIdentity();
    return input;
}

ReplayRuntimeIdentityV2 runtimeFor(
    const ReplayPreflightInputV2& input) {
    ReplayRuntimeIdentityV2 runtime{};
    runtime.known = true;
    runtime.contract = input.start.manifest.contract;
    runtime.compatibilityIdentity = input.compatibilityIdentity.identity;
    return runtime;
}

wire3::PlaybackResultPayload verifiedResult(
    const ReplayPreflightInputV2& input,
    const ReplayRuntimeIdentityV2& runtime,
    bool headless) {
    wire3::PlaybackResultPayload result{};
    result.reason = wire3::PlaybackResultReason::VerifiedCompleted;
    result.endpointKind = input.endpoint.kind;
    result.hasFileDigest = true;
    result.hasCanonicalGameDigest = true;
    result.hasRecordedTerminal = true;
    result.headless = headless;
    result.fileDigest = input.source.fileDigest;
    result.canonicalGameDigest =
        input.endpoint.recordedGameEnd.canonicalGameDigest;
    result.validatedSourceByteCount =
        input.source.validatedSourceByteCount;
    result.validatedSourceRangeDigest =
        input.source.validatedSourceRangeDigest;
    result.runtimeContract = runtime.contract;
    result.nextRecordConsumed = input.endpoint.nextRecord;
    result.finalRollingDigest = input.endpoint.finalRollingDigest;
    result.postTailStateHash = input.endpoint.postTailStateHash;
    result.recordedTerminal = static_cast<uint16_t>(
        input.endpoint.recordedGameEnd.reason);
    return result;
}

struct MemoryState {
    std::deque<std::vector<uint8_t>> inbound;
    std::vector<wire3::Frame> outbound;
    std::function<void(const wire3::Frame&, MemoryState&)> onWrite;
    std::function<ReplayBrokerIoStatusV2(const wire3::Frame&)> writeFailure;
    unsigned pollPaceMs = 0u;
    std::function<void(MemoryState&)> onPoll;
    bool peerClosed = false;
    bool brokerClosed = false;
};

class MemoryConnection final : public IReplayBrokerConnectionV2 {
public:
    explicit MemoryConnection(std::shared_ptr<MemoryState> state)
        : state_(static_cast<std::shared_ptr<MemoryState>&&>(state)) {}

    ReplayBrokerIoStatusV2 readSome(
        std::vector<uint8_t>& out, uint32_t) override {
        if (state_->inbound.empty() && !state_->peerClosed) {
            if (state_->pollPaceMs != 0u) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(state_->pollPaceMs));
            }
            if (state_->onPoll) state_->onPoll(*state_);
        }
        if (!state_->inbound.empty()) {
            out = static_cast<std::vector<uint8_t>&&>(
                state_->inbound.front());
            state_->inbound.pop_front();
            return ReplayBrokerIoStatusV2::Ok;
        }
        return state_->peerClosed
            ? ReplayBrokerIoStatusV2::Closed
            : ReplayBrokerIoStatusV2::Timeout;
    }
    ReplayBrokerIoStatusV2 writeAll(
        const uint8_t* data, size_t bytes, uint32_t) override {
        wire3::Frame frame{};
        size_t consumed = 0u;
        if (wire3::decodeFrame(data, bytes, frame, consumed)
                != wire3::FrameStatus::Ok
            || consumed != bytes) {
            return ReplayBrokerIoStatusV2::Error;
        }
        if (state_->writeFailure) {
            const auto failure = state_->writeFailure(frame);
            if (failure != ReplayBrokerIoStatusV2::Ok) return failure;
        }
        state_->outbound.push_back(frame);
        if (state_->onWrite) state_->onWrite(frame, *state_);
        return ReplayBrokerIoStatusV2::Ok;
    }
    void close() override { state_->brokerClosed = true; }

private:
    std::shared_ptr<MemoryState> state_;
};

class MemoryListener final : public IReplayBrokerListenerV2 {
public:
    explicit MemoryListener(std::shared_ptr<MemoryState> state)
        : state_(static_cast<std::shared_ptr<MemoryState>&&>(state)) {}
    bool create(
        const std::wstring& pipeName, std::string&) override {
        listening_ = !pipeName.empty();
        return listening_;
    }
    bool isListening() const override { return listening_; }
    ReplayBrokerIoStatusV2 accept(
        std::unique_ptr<IReplayBrokerConnectionV2>& out,
        uint32_t) override {
        if (!listening_ || accepted_)
            return ReplayBrokerIoStatusV2::Closed;
        accepted_ = true;
        out.reset(new MemoryConnection(state_));
        return ReplayBrokerIoStatusV2::Ok;
    }
    void cancel() override { listening_ = false; }

private:
    std::shared_ptr<MemoryState> state_;
    bool listening_ = false;
    bool accepted_ = false;
};

class ExitingProcess final : public IReplayProcessMonitorV2 {
public:
    bool hasExited(uint32_t& exitCode) override {
        if (!earlyExit_) return false;
        exitCode = exitCode_;
        return true;
    }
    ReplayProcessWaitStatusV2 waitForExit(
        uint32_t, uint32_t& exitCode) override {
        ++waitCalls_;
        if (!exitOnWait_) return ReplayProcessWaitStatusV2::Timeout;
        exitCode = exitCode_;
        return ReplayProcessWaitStatusV2::Exited;
    }
    bool earlyExit_ = false;
    bool exitOnWait_ = true;
    uint32_t exitCode_ = 0u;
    unsigned waitCalls_ = 0u;
};

struct PeerScript {
    ReplayBrokerLaunchV2 launch{};
    GameIdentityV2 game{};
    ReplayRuntimeIdentityV2 runtime{};
    wire3::PlaybackResultPayload result{};
    uint32_t nextSequence = 1u;
    bool earlyResult = false;
    bool prebufferStatus = false;
    bool missingResult = false;
    bool waitForUserCommand = false;
    bool endpointSeen = false;
    bool winScreenFailed = false;
    wire3::PlaybackResultReason sessionReason =
        wire3::PlaybackResultReason::VerifiedCompleted;

    void enqueue(
        MemoryState& state, wire3::MessageType type,
        const std::vector<uint8_t>& payload,
        uint64_t nonceOverride = 0u,
        uint32_t generationOverride = 0u,
        const GameIdentityV2* gameOverride = nullptr) {
        wire3::Frame frame{};
        frame.channel = wire3::Channel::Playback;
        frame.type = type;
        frame.launchGeneration = generationOverride != 0u
            ? generationOverride : launch.launchGeneration;
        frame.game = gameOverride != nullptr ? *gameOverride : game;
        frame.launchNonce = nonceOverride != 0u
            ? nonceOverride : launch.launchNonce;
        frame.sequence = nextSequence++;
        frame.payload = payload;
        std::vector<uint8_t> bytes;
        QNP_REQUIRE(wire3::encodeFrame(frame, bytes));
        state.inbound.push_back(bytes);
    }

    void hello(
        MemoryState& state, uint64_t nonceOverride = 0u,
        uint32_t generationOverride = 0u,
        const GameIdentityV2* gameOverride = nullptr) {
        wire3::PlaybackHelloPayload hello{};
        hello.gamePid = launch.gamePid;
        hello.headless = launch.headless;
        hello.runtimeCompatibilityId =
            runtime.contract.runtimeSimulationId;
        hello.stateHashAlgorithmId =
            runtime.contract.stateHashAlgorithmId;
        hello.bootstrapSchemaId =
            runtime.contract.bootstrapSchemaId;
        std::vector<uint8_t> payload;
        QNP_REQUIRE(wire3::encodePlaybackHello(hello, payload));
        enqueue(
            state, wire3::MessageType::PlaybackHello, payload,
            nonceOverride, generationOverride, gameOverride);
    }

    void enqueueResult(MemoryState& state) {
        std::vector<uint8_t> payload;
        QNP_REQUIRE(wire3::encodePlaybackResult(result, payload));
        enqueue(state, wire3::MessageType::PlaybackResult, payload);
    }

    void onBrokerFrame(
        const wire3::Frame& frame, MemoryState& state) {
        if (prebufferStatus
            && frame.type == wire3::MessageType::PlaybackGameStart) {
            wire3::PlaybackStatusPayload status{};
            status.phase =
                wire3::PlaybackRuntimePhase::WaitingForStart;
            status.headless = launch.headless;
            std::vector<uint8_t> payload;
            QNP_REQUIRE(wire3::encodePlaybackStatus(status, payload));
            enqueue(
                state, wire3::MessageType::PlaybackStatus, payload);
            prebufferStatus = false;
        }
        if (earlyResult
            && frame.type == wire3::MessageType::PlaybackGameStart) {
            enqueueResult(state);
            earlyResult = false;
            return;
        }
        if (frame.type == wire3::MessageType::PlaybackEndpoint) {
            endpointSeen = true;
            if (missingResult) {
                state.peerClosed = true;
            } else if (!waitForUserCommand) {
                enqueueResult(state);
            }
            return;
        }
        if (frame.type != wire3::MessageType::PlaybackCommand)
            return;
        wire3::PlaybackCommandPayload command{};
        QNP_REQUIRE(wire3::decodePlaybackCommand(
            frame.payload.data(), frame.payload.size(), command));
        wire3::PlaybackCommandAckPayload ack{};
        ack.commandId = command.commandId;
        ack.kind = command.kind;
        ack.result = wire3::PlaybackCommandAckResult::Applied;
        ack.phase = command.kind == wire3::PlaybackCommandKind::Exit
            ? wire3::PlaybackRuntimePhase::Complete
            : wire3::PlaybackRuntimePhase::Playing;
        ack.effectiveArgument = command.argument;
        ack.nextRecord = command.kind == wire3::PlaybackCommandKind::Exit
            ? result.nextRecordConsumed : 0u;
        std::vector<uint8_t> payload;
        QNP_REQUIRE(wire3::encodePlaybackCommandAck(ack, payload));
        enqueue(
            state, wire3::MessageType::PlaybackCommandAck, payload);

        if (command.kind != wire3::PlaybackCommandKind::Exit) {
            if (waitForUserCommand && endpointSeen) {
                waitForUserCommand = false;
                enqueueResult(state);
            }
            return;
        }
        wire3::PlaybackByePayload bye{};
        bye.reason = winScreenFailed
            ? replayWinScreenByeReason(sessionReason, false, true)
            : verifiedReason(result.reason)
            ? wire3::PlaybackByeReason::NormalAfterResult
            : wire3::PlaybackByeReason::FailureAfterResult;
        bye.resultSent = true;
        bye.nextRecord = result.nextRecordConsumed;
        bye.lastCommandId = command.commandId;
        payload.clear();
        QNP_REQUIRE(wire3::encodePlaybackBye(bye, payload));
        enqueue(state, wire3::MessageType::PlaybackBye, payload);
    }
};

struct BrokerFixture {
    ReplayPreflightInputV2 input = sealedGame();
    ReplayRuntimeIdentityV2 runtime = runtimeFor(input);
    ReplayBrokerLaunchV2 launch{};
    PeerScript peer{};
    std::shared_ptr<MemoryState> memory =
        std::make_shared<MemoryState>();

    explicit BrokerFixture(uint32_t gameGeneration = 1u) {
        input.start.manifest.identity.gameGeneration = gameGeneration;
        launch.gamePid = 4242u;
        launch.launchGeneration = 77u;
        launch.launchNonce = 0xA1B2C3D4E5F60718ull;
        launch.connectTimeoutMs = 10u;
        launch.helloTimeoutMs = 10u;
        launch.ioProgressTimeoutMs = 10u;
        launch.resultTimeoutMs = 20u;
        launch.drainTimeoutMs = 20u;
        launch.processExitTimeoutMs = 10u;
        peer.launch = launch;
        peer.game = input.start.manifest.identity;
        peer.runtime = runtime;
        peer.result = verifiedResult(input, runtime, launch.headless);
        memory->onWrite = [this](
            const wire3::Frame& frame, MemoryState& state) {
            peer.onBrokerFrame(frame, state);
        };
    }

    bool arm(ReplayBrokerV2& broker, ReplayBrokerNextGameProviderV2 provider = {}) {
        const ReplayPreflightResultV2 prepared =
            broker.prepare(input, runtime);
        if (!prepared.ok()) return false;
        if (provider && !broker.setNextGameProvider(std::move(provider))) return false;
        if (!broker.listenBeforeInjection(
                launch,
                std::unique_ptr<IReplayBrokerListenerV2>(
                    new MemoryListener(memory)))) {
            return false;
        }
        if (!broker.readyForInjection()) return false;
        return broker.markInjectionStarted();
    }
};

}

QNP_TEST(replay_v2_broker_requests_next_file_only_after_a_completed_result) {
    BrokerFixture fixture;
    ReplayBrokerV2 broker;
    QNP_REQUIRE(broker.prepare(fixture.input, fixture.runtime).ok());
    unsigned requests = 0u;
    QNP_REQUIRE(broker.setNextGameProvider(
        [&](const wire3::PlaybackResultPayload& result) {
            ++requests;
            QNP_CHECK(result.nextRecordConsumed == fixture.input.endpoint.nextRecord);
            QNP_CHECK(broker.status().resultReceived);
            return ReplayBrokerNextGameV2{};
        }));
    QNP_REQUIRE(broker.listenBeforeInjection(fixture.launch,
        std::unique_ptr<IReplayBrokerListenerV2>(new MemoryListener(fixture.memory))));
    QNP_REQUIRE(broker.markInjectionStarted());
    fixture.peer.hello(*fixture.memory);
    ExitingProcess process;
    QNP_CHECK(broker.serve(process).success);
    QNP_CHECK(requests == 1u);
    QNP_CHECK(process.waitCalls_ == 1u);
}

QNP_TEST(replay_v2_broker_drains_current_game_when_the_next_file_is_refused) {
    BrokerFixture fixture;
    ReplayBrokerV2 broker;
    QNP_REQUIRE(broker.prepare(fixture.input, fixture.runtime).ok());
    QNP_REQUIRE(broker.setNextGameProvider(
        [](const wire3::PlaybackResultPayload&) {
            ReplayBrokerNextGameV2 next{};
            next.error = "Game 2 failed its source digest preflight";
            return next;
        }));
    QNP_REQUIRE(broker.listenBeforeInjection(fixture.launch,
        std::unique_ptr<IReplayBrokerListenerV2>(new MemoryListener(fixture.memory))));
    QNP_REQUIRE(broker.markInjectionStarted());
    fixture.peer.hello(*fixture.memory);
    ExitingProcess process;
    const auto result = broker.serve(process);
    QNP_CHECK(!result.success);
    QNP_CHECK(result.state == ReplayBrokerStateV2::RuntimeFailure);
    QNP_CHECK(result.detail.find("source digest preflight") != std::string::npos);
    QNP_CHECK(result.hasPlaybackResult);
    QNP_CHECK(result.processExited);
    QNP_CHECK(result.nextGameRefused);
    QNP_CHECK(process.waitCalls_ == 1u);
}

QNP_TEST(replay_v2_broker_plays_two_independently_verified_files_in_one_process) {
    for (bool repeatedIdentity : {false, true}) {
        BrokerFixture fixture;
        if (repeatedIdentity)
            fixture.peer.result.reason = wire3::PlaybackResultReason::CompletedWithDivergence;
        auto second = fixture.input;
        if (!repeatedIdentity) {
            second.start.manifest.identity.gameUuidLow += 1u;
            second.start.manifest.identity.gameGeneration = 2u;
        }
        second.source.fileDigest = digest(0x47u);
        auto secondResult = verifiedResult(second, fixture.runtime, false);
        if (!repeatedIdentity)
            secondResult.reason = wire3::PlaybackResultReason::VerifiedBehaviourOnly;
        unsigned requests = 0u;
        unsigned switches = 0u;
        unsigned endpoints = 0u;
        unsigned exits = 0u;
        uint32_t lastOutboundSequence = 0u;
        const auto firstGame = fixture.peer.game;
        fixture.memory->onWrite = [&](const wire3::Frame& frame, MemoryState& state) {
            QNP_CHECK(frame.sequence == ++lastOutboundSequence);
            QNP_CHECK(frame.launchGeneration == fixture.launch.launchGeneration);
            QNP_CHECK(frame.launchNonce == fixture.launch.launchNonce);
            if (frame.type == wire3::MessageType::PlaybackEndpoint) ++endpoints;
            if (frame.type == wire3::MessageType::PlaybackNextGame) {
                QNP_CHECK(endpoints == 1u);
                QNP_CHECK(frame.game.gameUuidLow == firstGame.gameUuidLow);
                wire3::PlaybackNextGamePayload next{};
                QNP_REQUIRE(wire3::decodePlaybackNextGame(
                    frame.payload.data(), frame.payload.size(), next));
                QNP_CHECK(next.switchId == ++switches);
                QNP_CHECK(next.game.gameUuidLow == second.start.manifest.identity.gameUuidLow);
                QNP_CHECK(next.frameCount == second.ticks.size());
                QNP_CHECK(next.presentationFlags == 7u);
                QNP_CHECK(next.validatedSourceRangeDigest == second.source.validatedSourceRangeDigest);
                wire3::PlaybackStatusPayload status{};
                status.phase = wire3::PlaybackRuntimePhase::Complete;
                status.nextRecord = fixture.input.endpoint.nextRecord;
                status.endpointNextRecord = fixture.input.endpoint.nextRecord;
                std::vector<uint8_t> payload;
                QNP_REQUIRE(wire3::encodePlaybackStatus(status, payload));
                fixture.peer.enqueue(state, wire3::MessageType::PlaybackStatus, payload);
                fixture.peer.enqueue(state, wire3::MessageType::PlaybackNextGameAck, frame.payload);
                fixture.peer.game = next.game;
                fixture.peer.result = secondResult;
                fixture.peer.endpointSeen = false;
                return;
            }
            if (frame.type == wire3::MessageType::PlaybackCommand) {
                ++exits;
                QNP_CHECK(endpoints == 2u);
            }
            fixture.peer.onBrokerFrame(frame, state);
        };
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker, [&](const wire3::PlaybackResultPayload& accepted) {
            ReplayBrokerNextGameV2 next{};
            ++requests;
            if (requests == 1u) {
                QNP_CHECK(accepted.fileDigest == fixture.input.source.fileDigest);
                QNP_CHECK(prepareReplayV2(second, fixture.runtime, next.replay).ok());
                next.presentationFlags = 6u;
            } else {
                QNP_CHECK(accepted.fileDigest == second.source.fileDigest);
            }
            return next;
        }));
        fixture.peer.hello(*fixture.memory);
        ExitingProcess process;
        const auto result = broker.serve(process);
        QNP_CHECK(result.success);
        QNP_CHECK(result.playbackResult.fileDigest == second.source.fileDigest);
        QNP_CHECK(requests == 2u);
        QNP_CHECK(switches == 1u);
        QNP_CHECK(endpoints == 2u);
        QNP_CHECK(exits == 1u);
        QNP_CHECK(process.waitCalls_ == 1u);
    }
}

QNP_TEST(replay_v2_broker_rejects_wrong_switch_ack_and_stale_next_file_result) {
    for (unsigned fault : {0u, 1u, 2u}) {
        BrokerFixture fixture;
        auto second = fixture.input;
        second.source.fileDigest = digest(0x54u);
        unsigned requests = 0u;
        unsigned endpoints = 0u;
        fixture.memory->onWrite = [&](const wire3::Frame& frame, MemoryState& state) {
            if (frame.type == wire3::MessageType::PlaybackEndpoint) ++endpoints;
            if (frame.type == wire3::MessageType::PlaybackNextGame) {
                if (fault == 2u) return;
                auto payload = frame.payload;
                if (fault == 0u) {
                    wire3::PlaybackNextGamePayload next{};
                    QNP_REQUIRE(wire3::decodePlaybackNextGame(payload.data(), payload.size(), next));
                    ++next.switchId;
                    payload.clear();
                    QNP_REQUIRE(wire3::encodePlaybackNextGame(next, payload));
                }
                fixture.peer.enqueue(state, wire3::MessageType::PlaybackNextGameAck, payload);
                return;
            }
            fixture.peer.onBrokerFrame(frame, state);
        };
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker, [&](const wire3::PlaybackResultPayload&) {
            ++requests;
            ReplayBrokerNextGameV2 next{};
            QNP_CHECK(prepareReplayV2(second, fixture.runtime, next.replay).ok());
            return next;
        }));
        fixture.peer.hello(*fixture.memory);
        ExitingProcess process;
        const auto result = broker.serve(process);
        QNP_CHECK(!result.success);
        QNP_CHECK(result.state == (fault == 2u
            ? ReplayBrokerStateV2::TimedOut : ReplayBrokerStateV2::ProtocolFailure));
        QNP_CHECK(requests == 1u);
        QNP_CHECK(endpoints == (fault == 1u ? 2u : 1u));
        QNP_CHECK(process.waitCalls_ == 0u);
    }
}

QNP_TEST(replay_v2_broker_never_advances_a_failed_game) {
    BrokerFixture fixture;
    fixture.peer.result.reason = wire3::PlaybackResultReason::BootstrapMismatch;
    unsigned requests = 0u;
    ReplayBrokerV2 broker;
    QNP_REQUIRE(fixture.arm(broker, [&](const wire3::PlaybackResultPayload&) {
        ++requests;
        return ReplayBrokerNextGameV2{};
    }));
    fixture.peer.hello(*fixture.memory);
    ExitingProcess process;
    const auto result = broker.serve(process);
    QNP_CHECK(!result.success);
    QNP_CHECK(requests == 0u);
}

QNP_TEST(replay_v2_broker_distinguishes_closed_window_from_write_failure_during_handoff) {
    for (unsigned phase : {0u, 1u, 2u}) {
        for (auto failure : {ReplayBrokerIoStatusV2::Closed, ReplayBrokerIoStatusV2::Error}) {
            BrokerFixture fixture;
            unsigned prebuffers = 0u;
            fixture.memory->writeFailure = [&](const wire3::Frame& frame) {
                if (frame.type == wire3::MessageType::PlaybackSessionInfo) {
                    ++prebuffers;
                    if ((phase == 0u && prebuffers == 1u)
                        || (phase == 2u && prebuffers == 2u)) return failure;
                }
                if (phase == 1u && frame.type == wire3::MessageType::PlaybackNextGame)
                    return failure;
                return ReplayBrokerIoStatusV2::Ok;
            };
            fixture.memory->onWrite = [&](const wire3::Frame& frame, MemoryState& state) {
                if (frame.type == wire3::MessageType::PlaybackNextGame) {
                    fixture.peer.enqueue(state, wire3::MessageType::PlaybackNextGameAck, frame.payload);
                    return;
                }
                fixture.peer.onBrokerFrame(frame, state);
            };
            ReplayBrokerV2 broker;
            QNP_REQUIRE(fixture.arm(broker, [&](const wire3::PlaybackResultPayload&) {
                ReplayBrokerNextGameV2 next{};
                QNP_CHECK(prepareReplayV2(fixture.input, fixture.runtime, next.replay).ok());
                return next;
            }));
            fixture.peer.hello(*fixture.memory);
            ExitingProcess process;
            const auto result = broker.serve(process);
            QNP_CHECK(!result.success);
            QNP_CHECK(result.state == (failure == ReplayBrokerIoStatusV2::Closed
                ? ReplayBrokerStateV2::MissingResult : ReplayBrokerStateV2::IoFailure));
            QNP_CHECK(replayClosedBeforeResultV2(true, true, result)
                == (failure == ReplayBrokerIoStatusV2::Closed));
            QNP_CHECK(!replayClosedBeforeResultV2(false, true, result));
            QNP_CHECK(!replayClosedBeforeResultV2(true, false, result));
        }
    }
}

QNP_TEST(replay_v2_broker_keeps_local_contract_and_per_file_schema_mode_on_handoff) {
    for (bool wrongProofMode : {false, true}) {
        BrokerFixture fixture;
        fixture.input.compatibilityIdentity.identity.stateSchemaId =
            qnetplay::compat::kStateSchemaCurrent;
        fixture.runtime.compatibilityIdentity = fixture.input.compatibilityIdentity.identity;
        fixture.runtime.contract.contentManifestRoot = digest(0x90u);
        auto second = sealedGame(qreplay::kReplayStateHashAlgorithmIdV2, 0x82u);
        second.compatibilityIdentity.identity.stateSchemaId = qnetplay::compat::kStateSchemaV4;
        auto secondResult = verifiedResult(second, fixture.runtime, false);
        secondResult.runtimeContract.contentManifestRoot = second.start.manifest.contract.contentManifestRoot;
        if (!wrongProofMode)
            secondResult.reason = wire3::PlaybackResultReason::VerifiedBehaviourOnly;
        unsigned requests = 0u;
        fixture.memory->onWrite = [&](const wire3::Frame& frame, MemoryState& state) {
            if (frame.type == wire3::MessageType::PlaybackNextGame) {
                wire3::PlaybackNextGamePayload next{};
                QNP_REQUIRE(wire3::decodePlaybackNextGame(frame.payload.data(), frame.payload.size(), next));
                auto expected = fixture.runtime.contract;
                expected.contentManifestRoot = second.start.manifest.contract.contentManifestRoot;
                QNP_CHECK(next.runtimeContract == expected);
                QNP_CHECK(next.runtimeContract.stateHashAlgorithmId != second.start.manifest.contract.stateHashAlgorithmId);
                QNP_CHECK(next.supersededStateSchema);
                fixture.peer.enqueue(state, wire3::MessageType::PlaybackNextGameAck, frame.payload);
                fixture.peer.game = next.game;
                fixture.peer.result = secondResult;
                return;
            }
            fixture.peer.onBrokerFrame(frame, state);
        };
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker, [&](const wire3::PlaybackResultPayload&) {
            ReplayBrokerNextGameV2 next{};
            if (++requests == 1u) {
                const auto prepared = prepareReplayV2(second, fixture.runtime, next.replay);
                QNP_CHECK(prepared.ok());
                QNP_CHECK(prepared.compatibility.supersededStateSchema);
            }
            return next;
        }));
        fixture.peer.hello(*fixture.memory);
        ExitingProcess process;
        const auto result = broker.serve(process);
        QNP_CHECK(result.success == !wrongProofMode);
        QNP_CHECK(result.state == (wrongProofMode ? ReplayBrokerStateV2::ProtocolFailure
                                                 : ReplayBrokerStateV2::Complete));
    }
}

QNP_TEST(replay_v2_preflight_is_exact_one_game_and_honest_prefix) {
    ReplayPreflightInputV2 input = sealedGame();
    const ReplayRuntimeIdentityV2 runtime = runtimeFor(input);
    std::unique_ptr<PreparedReplayV2> prepared;
    ReplayPreflightResultV2 result =
        prepareReplayV2(input, runtime, prepared);
    QNP_REQUIRE(result.ok());
    QNP_REQUIRE(prepared != nullptr);
    QNP_CHECK(prepared->frameCount() == 3u);
    QNP_CHECK(prepared->eventCount() == 4u);
    QNP_CHECK(prepared->tick(0u).frame.replayFrame == 0u);
    QNP_CHECK(prepared->sessionInfo().hasFileDigest);

    ReplayPreflightInputV2 prefix = input;
    prefix.source.classification =
        ReplaySourceClassificationV2::UnsealedValidatedPrefix;
    prefix.source.sourceIsPartFile = true;
    prefix.source.hasGameEnd = false;
    prefix.source.hasFooter = false;
    prefix.source.footerVerified = false;
    prefix.source.hasFileDigest = false;
    prefix.source.fileDigestVerified = false;
    prefix.source.fileDigest.fill(0u);
    prefix.endpoint.kind =
        wire3::PlaybackEndpointKind::UnsealedValidatedPrefix;
    prefix.endpoint.hasRecordedGameEnd = false;
    prefix.endpoint.recordedGameEnd = ConfirmedGameEndV2{};
    std::unique_ptr<PreparedReplayV2> preparedPrefix;
    result = prepareReplayV2(prefix, runtime, preparedPrefix);
    QNP_CHECK(!result.ok());
    QNP_CHECK(
        result.status
        == ReplayPreflightStatusV2::SourceClassificationMismatch);
    QNP_CHECK(preparedPrefix == nullptr);
}

QNP_TEST(replay_v2_preflight_rejects_unattested_or_wrong_content_before_broker) {
    ReplayPreflightInputV2 input = sealedGame();
    ReplayRuntimeIdentityV2 runtime = runtimeFor(input);
    std::unique_ptr<PreparedReplayV2> prepared;

    input.compatibilityIdentity =
        makeLegacyUnattestedReplayCompatibilityIdentityV3();
    ReplayPreflightResultV2 result =
        prepareReplayV2(input, runtime, prepared);
    QNP_CHECK(!result.ok());
    QNP_CHECK(result.status
              == ReplayPreflightStatusV2::CompatibilityIdentityUnattested);
    QNP_CHECK(prepared == nullptr);

    input = sealedGame();
    runtime = runtimeFor(input);
    runtime.compatibilityIdentity.simulationContent.simulationRootSha256[0]
        ^= 1u;
    result = prepareReplayV2(input, runtime, prepared);
    QNP_CHECK(!result.ok());
    QNP_CHECK(result.status
              == ReplayPreflightStatusV2::CompatibilityIdentityMismatch);
    QNP_CHECK(result.compatibility.reason
              == ReplayCompatibilityReasonV2::CompleteCompatibilityIdentity);
    QNP_CHECK(result.technicalDetail.find("simulation-content root mismatch")
              != std::string::npos);
    QNP_CHECK(prepared == nullptr);
}

QNP_TEST(replay_v2_verified_identity_forgives_content_root_drift_only) {
    ReplayPreflightInputV2 input = sealedGame();
    ReplayRuntimeIdentityV2 runtime = runtimeFor(input);

    runtime.contract.contentManifestRoot[0] ^= 1u;
    const ReplayCompatibilityDecisionV2 drifted =
        evaluateReplayCompatibilityV2(input.start, runtime,
                                      &input.compatibilityIdentity);
    QNP_CHECK(drifted.state == ReplayCompatibilityStateV2::Exact);
    QNP_CHECK(drifted.watchable);
    QNP_CHECK(drifted.detail.find("unrelated local files")
              != std::string::npos);

    const ReplayCompatibilityDecisionV2 strict =
        evaluateReplayCompatibilityV2(input.start, runtime);
    QNP_CHECK(strict.state == ReplayCompatibilityStateV2::ContentMismatch);
    QNP_CHECK(strict.reason == ReplayCompatibilityReasonV2::ContentManifest);
    const qreplay::ReplayCompatibilityIdentityEnvelopeV3 legacy =
        makeLegacyUnattestedReplayCompatibilityIdentityV3();
    QNP_CHECK(evaluateReplayCompatibilityV2(input.start, runtime, &legacy)
                  .state
              == ReplayCompatibilityStateV2::ContentMismatch);

    ReplayRuntimeIdentityV2 wrongContent = runtime;
    wrongContent.compatibilityIdentity.simulationContent
        .simulationRootSha256[0] ^= 1u;
    QNP_CHECK(evaluateReplayCompatibilityV2(input.start, wrongContent,
                                            &input.compatibilityIdentity)
                  .state
              == ReplayCompatibilityStateV2::ContentMismatch);

    ReplayRuntimeIdentityV2 wrongSchema = runtime;
    wrongSchema.contract.eventSchemaId ^= 1u;
    const ReplayCompatibilityDecisionV2 schema =
        evaluateReplayCompatibilityV2(input.start, wrongSchema,
                                      &input.compatibilityIdentity);
    QNP_CHECK(schema.state == ReplayCompatibilityStateV2::Unsupported);
    QNP_CHECK(schema.reason == ReplayCompatibilityReasonV2::EventSchema);

    input = sealedGame();
    ReplayRuntimeIdentityV2 driftedRuntime = runtimeFor(input);
    driftedRuntime.contract.contentManifestRoot[0] ^= 1u;
    std::unique_ptr<PreparedReplayV2> prepared;
    const ReplayPreflightResultV2 preflight =
        prepareReplayV2(input, driftedRuntime, prepared);
    QNP_CHECK(preflight.ok());
    QNP_CHECK(prepared != nullptr);
    QNP_CHECK(preflight.compatibility.state
              == ReplayCompatibilityStateV2::Exact);
}

QNP_TEST(replay_v2_compatibility_rejects_contract_topology_and_stage_drift) {
    ReplayPreflightInputV2 input = sealedGame();
    ReplayRuntimeIdentityV2 runtime = runtimeFor(input);
    QNP_CHECK(
        evaluateReplayCompatibilityV2(input.start, runtime).state
        == ReplayCompatibilityStateV2::Exact);

    runtime.contract.contentManifestRoot[0] ^= 1u;
    ReplayCompatibilityDecisionV2 rejected =
        evaluateReplayCompatibilityV2(input.start, runtime);
    QNP_CHECK(
        rejected.state == ReplayCompatibilityStateV2::ContentMismatch);
    QNP_CHECK(
        rejected.reason
        == ReplayCompatibilityReasonV2::ContentManifest);

    runtime = runtimeFor(input);
    input.start.manifest.vsRow = 2u;
    rejected = evaluateReplayCompatibilityV2(input.start, runtime);
    QNP_CHECK(
        rejected.reason
        == ReplayCompatibilityReasonV2::UnsupportedTopology);

    input = sealedGame();
    runtime = runtimeFor(input);
    input.start.manifest.stage.catalogEntryId ^= 1u;
    rejected = evaluateReplayCompatibilityV2(input.start, runtime);
    QNP_CHECK(rejected.reason == ReplayCompatibilityReasonV2::StageIdentity);

    input = sealedGame();
    runtime = runtimeFor(input);
    input.ticks[0].frame.core.inputDomain =
        InputDomain::EffectiveFighterV1;
    std::unique_ptr<PreparedReplayV2> prepared;
    const ReplayPreflightResultV2 preflight =
        prepareReplayV2(input, runtime, prepared);
    QNP_CHECK(!preflight.ok());
    QNP_CHECK(
        preflight.compatibility.reason
        == ReplayCompatibilityReasonV2::UnsupportedInputDomain);
}

QNP_TEST(replay_v2_broker_requires_listener_before_injection_and_completes) {
    BrokerFixture fixture;
    fixture.peer.prebufferStatus = true;
    ReplayBrokerV2 broker;
    QNP_CHECK(!broker.markInjectionStarted());
    QNP_REQUIRE(fixture.arm(broker));
    fixture.peer.hello(*fixture.memory);
    ExitingProcess process;
    const ReplayBrokerRunResultV2 result = broker.serve(process);
    QNP_CHECK(result.success);
    QNP_CHECK(result.state == ReplayBrokerStateV2::Complete);
    QNP_CHECK(result.hasPlaybackResult);
    QNP_CHECK(result.processExited);
    QNP_CHECK(fixture.memory->brokerClosed);
    const ReplayBrokerStatusV2 status = broker.status();
    QNP_CHECK(status.sentFrames == 3u);
    QNP_CHECK(status.sentEvents == 4u);
    QNP_CHECK(status.receivedStatuses == 1u);
    QNP_CHECK(status.resultReceived);
    QNP_CHECK(status.byeReceived);
}

QNP_TEST(replay_v2_broker_preserves_win_screen_failure_through_result_exit_and_bye) {
    for (auto sessionReason : {wire3::PlaybackResultReason::VerifiedCompleted,
                              wire3::PlaybackResultReason::CompletedWithDivergence}) {
        BrokerFixture fixture;
        fixture.peer.sessionReason = sessionReason;
        fixture.peer.winScreenFailed = true;
        fixture.peer.result.reason = wire3::PlaybackResultReason::InternalFailure;
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker));
        fixture.peer.hello(*fixture.memory);
        ExitingProcess process;
        const auto result = broker.serve(process);
        QNP_CHECK(!result.success);
        QNP_CHECK(result.state == ReplayBrokerStateV2::RuntimeFailure);
        QNP_CHECK(result.hasPlaybackResult);
        QNP_CHECK(result.playbackResult.reason == wire3::PlaybackResultReason::InternalFailure);
        QNP_CHECK(result.processExited);
        QNP_CHECK(broker.status().byeReceived);
    }
    QNP_CHECK(replayWinScreenByeReason(wire3::PlaybackResultReason::UserExit,
                                      false, false)
              == wire3::PlaybackByeReason::UserExitAfterResult);
    QNP_CHECK(replayWinScreenByeReason(wire3::PlaybackResultReason::InternalFailure,
                                      true, false)
              == wire3::PlaybackByeReason::FailureAfterResult);
    QNP_CHECK(replayWinScreenByeReason(wire3::PlaybackResultReason::CompletedWithDivergence,
                                      false, false)
              == wire3::PlaybackByeReason::NormalAfterResult);
}

QNP_TEST(replay_v2_broker_has_finite_cancel_and_process_exit_states) {
    {
        BrokerFixture fixture;
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker));
        ReplayBrokerCancellationV2 cancellation;
        cancellation.request();
        ExitingProcess process;
        const ReplayBrokerRunResultV2 result =
            broker.serve(process, &cancellation);
        QNP_CHECK(!result.success);
        QNP_CHECK(result.state == ReplayBrokerStateV2::Cancelled);
    }
    {
        BrokerFixture fixture;
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker));
        ExitingProcess process;
        process.earlyExit_ = true;
        process.exitCode_ = 9u;
        const ReplayBrokerRunResultV2 result = broker.serve(process);
        QNP_CHECK(!result.success);
        QNP_CHECK(
            result.state
            == ReplayBrokerStateV2::ProcessExitedWithoutResult);
        QNP_CHECK(result.processExited);
        QNP_CHECK(result.processExitCode == 9u);
    }
}

QNP_TEST(replay_v2_broker_rejects_wrong_nonce_generation_and_uuid) {
    for (int which = 0; which < 3; ++which) {
        BrokerFixture fixture;
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker));
        GameIdentityV2 wrong = fixture.peer.game;
        wrong.gameUuidLow ^= 1u;
        fixture.peer.hello(
            *fixture.memory,
            which == 0 ? fixture.launch.launchNonce + 1u : 0u,
            which == 1 ? fixture.launch.launchGeneration + 1u : 0u,
            which == 2 ? &wrong : nullptr);
        ExitingProcess process;
        const ReplayBrokerRunResultV2 result = broker.serve(process);
        QNP_CHECK(!result.success);
        QNP_CHECK(result.state == ReplayBrokerStateV2::ProtocolFailure);
    }
}

QNP_TEST(replay_v2_broker_rejects_truncated_hello_and_early_result) {
    {
        BrokerFixture fixture;
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker));
        fixture.peer.hello(*fixture.memory);
        QNP_REQUIRE(!fixture.memory->inbound.empty());
        fixture.memory->inbound.front().pop_back();
        fixture.memory->peerClosed = true;
        ExitingProcess process;
        const ReplayBrokerRunResultV2 result = broker.serve(process);
        QNP_CHECK(!result.success);
        QNP_CHECK(
            result.state == ReplayBrokerStateV2::IoFailure
            || result.state == ReplayBrokerStateV2::MissingResult);
    }
    {
        BrokerFixture fixture;
        fixture.peer.earlyResult = true;
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker));
        fixture.peer.hello(*fixture.memory);
        ExitingProcess process;
        const ReplayBrokerRunResultV2 result = broker.serve(process);
        QNP_CHECK(!result.success);
        QNP_CHECK(result.state == ReplayBrokerStateV2::ProtocolFailure);
        QNP_CHECK(
            result.detail.find("before the finite endpoint")
            != std::string::npos);
    }
}

QNP_TEST(replay_v2_broker_reports_missing_result_and_terminal_mismatch) {
    {
        BrokerFixture fixture;
        fixture.peer.missingResult = true;
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker));
        fixture.peer.hello(*fixture.memory);
        ExitingProcess process;
        const ReplayBrokerRunResultV2 result = broker.serve(process);
        QNP_CHECK(!result.success);
        QNP_CHECK(result.state == ReplayBrokerStateV2::MissingResult);
        QNP_CHECK(!result.hasPlaybackResult);
    }
    {
        BrokerFixture fixture;
        fixture.peer.result.finalRollingDigest ^= 1u;
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker));
        fixture.peer.hello(*fixture.memory);
        ExitingProcess process;
        const ReplayBrokerRunResultV2 result = broker.serve(process);
        QNP_CHECK(!result.success);
        QNP_CHECK(result.state == ReplayBrokerStateV2::ProtocolFailure);
        QNP_CHECK(
            result.detail.find("endpoint") != std::string::npos);
    }
}

QNP_TEST(replay_v2_broker_matches_command_ack_before_result) {
    BrokerFixture fixture;
    fixture.peer.waitForUserCommand = true;
    ReplayBrokerV2 broker;
    const ReplayPreflightResultV2 prepared =
        broker.prepare(fixture.input, fixture.runtime);
    QNP_REQUIRE(prepared.ok());
    const uint64_t commandId = broker.enqueueCommand(
        wire3::PlaybackCommandKind::Pause, 0u);
    QNP_REQUIRE(commandId == 1u);
    QNP_REQUIRE(broker.listenBeforeInjection(
        fixture.launch,
        std::unique_ptr<IReplayBrokerListenerV2>(
            new MemoryListener(fixture.memory))));
    QNP_REQUIRE(broker.markInjectionStarted());
    fixture.peer.hello(*fixture.memory);
    ExitingProcess process;
    const ReplayBrokerRunResultV2 result = broker.serve(process);
    QNP_CHECK(result.success);
    QNP_CHECK(broker.status().acknowledgedCommands == 2u);
}

QNP_TEST(replay_v2_broker_second_fresh_process_resets_all_launch_binding) {
    ReplayBrokerV2 broker;
    {
        BrokerFixture first;
        QNP_REQUIRE(first.arm(broker));
        first.peer.hello(*first.memory);
        ExitingProcess process;
        QNP_REQUIRE(broker.serve(process).success);
    }
    QNP_REQUIRE(broker.reset());
    QNP_CHECK(broker.status().state == ReplayBrokerStateV2::Idle);

    BrokerFixture second;
    second.launch.gamePid = 5252u;
    second.launch.launchGeneration = 88u;
    second.launch.launchNonce = 0x8877665544332211ull;
    second.peer.launch = second.launch;
    QNP_REQUIRE(second.arm(broker));
    second.peer.hello(*second.memory);
    ExitingProcess process;
    const ReplayBrokerRunResultV2 result = broker.serve(process);
    QNP_CHECK(result.success);
    QNP_CHECK(
        broker.status().launchGeneration
        == second.launch.launchGeneration);
    QNP_CHECK(second.memory->outbound.front().sequence == 1u);
}

QNP_TEST(replay_v2_preflight_selects_the_verification_mode_per_file) {
    ReplayPreflightInputV2 first = sealedGame();
    const ReplayRuntimeIdentityV2 runtime = runtimeFor(first);
    QNP_CHECK(first.start.manifest.identity.gameGeneration == 1u);

    const ReplayCompatibilityDecisionV2 byteExact =
        evaluateReplayCompatibilityV2(first.start, runtime,
                                      &first.compatibilityIdentity);
    QNP_CHECK(byteExact.watchable);
    QNP_CHECK(byteExact.verificationMode
              == ReplayVerificationModeV2::ByteExact);
    QNP_CHECK(byteExact.detail.find("behaviour-verified")
              == std::string::npos);

    ReplayPreflightInputV2 rematch = sealedGame();
    rematch.start.manifest.identity.gameGeneration = 6u;
    const ReplayCompatibilityDecisionV2 behaviour =
        evaluateReplayCompatibilityV2(rematch.start, runtime,
                                      &rematch.compatibilityIdentity);
    QNP_CHECK(behaviour.watchable);
    QNP_CHECK(behaviour.state == ReplayCompatibilityStateV2::Exact);
    QNP_CHECK(behaviour.verificationMode
              == ReplayVerificationModeV2::BehaviourVerified);
    QNP_CHECK(behaviour.detail.find("behaviour-verified")
              != std::string::npos);

    std::unique_ptr<PreparedReplayV2> prepared;
    ReplayPreflightResultV2 result =
        prepareReplayV2(rematch, runtime, prepared);
    QNP_REQUIRE(result.ok());
    QNP_REQUIRE(prepared != nullptr);
    QNP_CHECK(prepared->verificationMode()
              == ReplayVerificationModeV2::BehaviourVerified);
    QNP_CHECK(result.compatibility.verificationMode
              == ReplayVerificationModeV2::BehaviourVerified);

    std::unique_ptr<PreparedReplayV2> preparedFirst;
    result = prepareReplayV2(first, runtime, preparedFirst);
    QNP_REQUIRE(result.ok());
    QNP_REQUIRE(preparedFirst != nullptr);
    QNP_CHECK(preparedFirst->verificationMode()
              == ReplayVerificationModeV2::ByteExact);
}

QNP_TEST(replay_v2_broker_binds_the_verified_reason_to_the_selected_mode) {
    {
        BrokerFixture fixture(5u);
        fixture.peer.result.reason =
            wire3::PlaybackResultReason::VerifiedBehaviourOnly;
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker));
        fixture.peer.hello(*fixture.memory);
        ExitingProcess process;
        const ReplayBrokerRunResultV2 result = broker.serve(process);
        QNP_CHECK(result.success);
        QNP_CHECK(result.state == ReplayBrokerStateV2::Complete);
        QNP_REQUIRE(result.hasPlaybackResult);
        QNP_CHECK(result.playbackResult.reason
                  == wire3::PlaybackResultReason::VerifiedBehaviourOnly);
        QNP_CHECK(result.playbackResult.nextRecordConsumed
                  == fixture.input.endpoint.nextRecord);
    }
    {
        BrokerFixture fixture(5u);
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker));
        fixture.peer.hello(*fixture.memory);
        ExitingProcess process;
        const ReplayBrokerRunResultV2 result = broker.serve(process);
        QNP_CHECK(!result.success);
        QNP_CHECK(result.detail.find("byte-exact verified result")
                  != std::string::npos);
    }
    {
        BrokerFixture fixture(1u);
        fixture.peer.result.reason =
            wire3::PlaybackResultReason::VerifiedBehaviourOnly;
        ReplayBrokerV2 broker;
        QNP_REQUIRE(fixture.arm(broker));
        fixture.peer.hello(*fixture.memory);
        ExitingProcess process;
        const ReplayBrokerRunResultV2 result = broker.serve(process);
        QNP_CHECK(!result.success);
        QNP_CHECK(result.detail.find("behaviour-only result")
                  != std::string::npos);
    }
}

QNP_TEST(replay_v2_broker_keeps_waiting_while_a_held_runtime_still_reports) {
    BrokerFixture fixture;
    fixture.launch.connectTimeoutMs = 5000u;
    fixture.launch.helloTimeoutMs = 5000u;
    fixture.launch.ioProgressTimeoutMs = 5000u;
    fixture.launch.resultTimeoutMs = 120u;
    fixture.launch.drainTimeoutMs = 5000u;
    fixture.launch.processExitTimeoutMs = 5000u;
    fixture.peer.launch = fixture.launch;
    fixture.peer.waitForUserCommand = true;

    unsigned polls = 0u;
    unsigned statuses = 0u;
    bool sentResult = false;
    fixture.memory->pollPaceMs = 5u;
    fixture.memory->onPoll = [&](MemoryState& state) {
        if (!fixture.peer.endpointSeen || sentResult) return;
        if (++polls % 2u != 0u) return;
        if (statuses < 24u) {
            ++statuses;
            wire3::PlaybackStatusPayload status{};
            status.phase = wire3::PlaybackRuntimePhase::Paused;
            status.paused = true;
            status.headless = fixture.launch.headless;
            status.speedPermille = 1000u;
            std::vector<uint8_t> payload;
            QNP_REQUIRE(wire3::encodePlaybackStatus(status, payload));
            fixture.peer.enqueue(
                state, wire3::MessageType::PlaybackStatus, payload);
            return;
        }
        sentResult = true;
        fixture.peer.enqueueResult(state);
    };

    ReplayBrokerV2 broker;
    QNP_REQUIRE(fixture.arm(broker));
    fixture.peer.hello(*fixture.memory);
    ExitingProcess process;
    const auto started = std::chrono::steady_clock::now();
    const ReplayBrokerRunResultV2 result = broker.serve(process);
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();

    QNP_CHECK(result.success);
    QNP_CHECK(result.state == ReplayBrokerStateV2::Complete);
    QNP_CHECK(result.hasPlaybackResult);
    QNP_CHECK(broker.status().receivedStatuses == 24u);
    QNP_CHECK(elapsedMs > static_cast<long long>(
                  fixture.launch.resultTimeoutMs));

    BrokerFixture silent;
    silent.launch.connectTimeoutMs = 5000u;
    silent.launch.helloTimeoutMs = 5000u;
    silent.launch.ioProgressTimeoutMs = 5000u;
    silent.launch.resultTimeoutMs = 60u;
    silent.launch.drainTimeoutMs = 5000u;
    silent.launch.processExitTimeoutMs = 5000u;
    silent.peer.launch = silent.launch;
    silent.peer.waitForUserCommand = true;
    silent.memory->pollPaceMs = 5u;
    ReplayBrokerV2 silentBroker;
    QNP_REQUIRE(silent.arm(silentBroker));
    silent.peer.hello(*silent.memory);
    ExitingProcess silentProcess;
    const ReplayBrokerRunResultV2 silentResult =
        silentBroker.serve(silentProcess);
    QNP_CHECK(!silentResult.success);
    QNP_CHECK(silentResult.state == ReplayBrokerStateV2::TimedOut);
    QNP_CHECK(silentResult.detail.find("stopped reporting")
              != std::string::npos);
}

QNP_TEST(replay_v2_broker_refuses_a_transport_command_it_cannot_honour) {
    {
        BrokerFixture fixture;
        ReplayBrokerV2 broker;
        QNP_CHECK(broker.enqueueCommand(
            wire3::PlaybackCommandKind::Pause, 0u) == 0u);

        const ReplayPreflightResultV2 prepared =
            broker.prepare(fixture.input, fixture.runtime);
        QNP_REQUIRE(prepared.ok());

        QNP_CHECK(broker.enqueueCommand(
            wire3::PlaybackCommandKind::SpeedSet, 750u) == 0u);
        QNP_CHECK(broker.enqueueCommand(
            wire3::PlaybackCommandKind::SpeedSet, 0u) == 0u);
        QNP_CHECK(broker.enqueueCommand(
            wire3::PlaybackCommandKind::SpeedSet, 16000u) == 0u);
        QNP_CHECK(broker.enqueueCommand(
            wire3::PlaybackCommandKind::Pause, 1u) == 0u);
        QNP_CHECK(broker.enqueueCommand(
            wire3::PlaybackCommandKind::Exit, 0u) == 0u);

        QNP_CHECK(broker.enqueueCommand(
            wire3::PlaybackCommandKind::Pause, 0u) == 1u);
        QNP_CHECK(broker.enqueueCommand(
            wire3::PlaybackCommandKind::SpeedSet, 250u) == 2u);
        QNP_CHECK(broker.enqueueCommand(
            wire3::PlaybackCommandKind::Resume, 0u) == 3u);
    }

    BrokerFixture fixture;
    fixture.peer.waitForUserCommand = true;
    ReplayBrokerV2 broker;
    const ReplayPreflightResultV2 prepared =
        broker.prepare(fixture.input, fixture.runtime);
    QNP_REQUIRE(prepared.ok());
    QNP_REQUIRE(broker.enqueueCommand(
        wire3::PlaybackCommandKind::SpeedSet, 4000u) == 1u);
    QNP_REQUIRE(broker.listenBeforeInjection(
        fixture.launch,
        std::unique_ptr<IReplayBrokerListenerV2>(
            new MemoryListener(fixture.memory))));
    QNP_REQUIRE(broker.markInjectionStarted());
    fixture.peer.hello(*fixture.memory);
    ExitingProcess process;
    const ReplayBrokerRunResultV2 result = broker.serve(process);
    QNP_CHECK(result.success);
    QNP_CHECK(broker.status().resultReceived);
    QNP_CHECK(broker.status().acknowledgedCommands == 2u);

    QNP_CHECK(broker.enqueueCommand(
        wire3::PlaybackCommandKind::Pause, 0u) == 0u);
    QNP_CHECK(broker.enqueueCommand(
        wire3::PlaybackCommandKind::Step, 0u) == 0u);
    QNP_CHECK(broker.enqueueCommand(
        wire3::PlaybackCommandKind::SpeedSet, 2000u) == 0u);
}
