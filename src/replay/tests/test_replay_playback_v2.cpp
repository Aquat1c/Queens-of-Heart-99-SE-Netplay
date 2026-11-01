#include "TestHarness.h"

#include "replay/ReplayIntegrity.h"
#include "replay/ReplayNavigation.h"
#include "replay/ReplayPlaybackSessionV2.h"
#include "replay/ReplaySimulationContractV2.h"
#include "replay/ReplayWinScreen.h"
#include "replay/StageCatalog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using namespace qreplay;
namespace wire3 = qreplay::ipc::v3;

struct PlaybackGame {
    ConfirmedGameStartV2 start{};
    std::vector<CanonicalTickBundleV2> ticks;
    ConfirmedGameEndV2 end{};
};

SimulationContractV2 simulationContract() {
    SimulationContractV2 contract{};
    for (size_t i = 0u; i < contract.executableSha256.size(); ++i)
        contract.executableSha256[i] =
            static_cast<uint8_t>(i + 1u);
    for (size_t i = 0u; i < contract.contentManifestRoot.size(); ++i)
        contract.contentManifestRoot[i] =
            static_cast<uint8_t>(0x80u + i);
    contract.nativeBuildIdentity = 0x1111222233334444ull;
    contract.runtimeSimulationId = 0x5555666677778888ull;
    contract.stateRegionSchemaFingerprint = 0x0102030405060708ull;
    contract.stateHashProjectionId = 0x1112131415161718ull;
    contract.stateHashAlgorithmId = kReplayStateHashAlgorithmIdV2;
    contract.bootstrapSchemaId = 0x3132333435363738ull;
    contract.eventSchemaId = 0x4142434445464748ull;
    contract.characterCatalogId = 0x5152535455565758ull;
    contract.terminalResultSchemaId = 0x6162636465666768ull;
    contract.compactActionDomainVersion = 1u;
    return contract;
}

ConfirmedGameEventV2 event(
    GameEventKindV2 kind,
    GameEventPhaseV2 phase,
    uint32_t replayFrame,
    uint32_t sequence,
    uint64_t stateProof) {
    ConfirmedGameEventV2 value{};
    value.kind = kind;
    value.phase = phase;
    value.replayFrame = replayFrame;
    value.eventSequence = sequence;
    value.stateProof = stateProof;
    if (kind == GameEventKindV2::RoundStarted) {
        value.transitionSiteId = 100u;
        value.roundOrdinal = 1u;
    } else if (kind == GameEventKindV2::RoundEnded
               || kind == GameEventKindV2::GameResult) {
        value.transitionSiteId =
            kind == GameEventKindV2::RoundEnded ? 200u : 201u;
        value.roundOrdinal = 2u;
        value.winningSideMask = 0x01u;
        value.roundTallies = {{2u, 1u}};
        value.winnerSlot = 0;
    }
    return value;
}

CanonicalTickBundleV2 tick(
    uint32_t sourceFrame,
    uint32_t replayFrame,
    const GameLifecycleV2& before,
    const GameLifecycleV2& after,
    uint64_t preStateHash,
    uint64_t postStateHash) {
    CanonicalTickBundleV2 value{};
    value.frame.sourceFrame = sourceFrame;
    value.frame.replayFrame = replayFrame;
    value.frame.core.inputDomain = InputDomain::LogicalSourceV1;
    value.frame.core.activeLaneMask = 0x0Fu;
    value.frame.core.actions = {{
        static_cast<uint16_t>(0x0001u | (replayFrame << 8)),
        static_cast<uint16_t>(0x0002u | (replayFrame << 8)),
        static_cast<uint16_t>(0x0104u | replayFrame),
        static_cast<uint16_t>(0x0208u | replayFrame),
    }};
    value.frame.core.before = before;
    value.frame.core.after = after;
    value.frame.core.dispatcherResult = 1;
    value.frame.core.preStateHash = preStateHash;
    value.frame.core.postStateHash = postStateHash;
    return value;
}

PlaybackGame completedFourLaneGame(
    bool validStageIdentity = true) {
    PlaybackGame game{};
    GameManifestV2& manifest = game.start.manifest;
    manifest.identity.gameUuidLow = 0x0123456789ABCDEFull;
    manifest.identity.gameUuidHigh = 0xFEDCBA9876543210ull;
    manifest.identity.gameGeneration = 1u;
    manifest.contract = simulationContract();
    manifest.vsRow = 5u;
    manifest.activeLaneMask = 0x0Fu;
    manifest.activeFighterMask = 0x0Fu;
    manifest.cpuMask = 0u;
    manifest.cpuContract = CpuContract::None;
    manifest.slotToSourceRoute = {{0u, 1u, 2u, 3u}};
    for (size_t i = 0u; i < kSourceLanes; ++i) {
        manifest.lanes[i].active = true;
        manifest.slots[i].ownership = SlotOwnership::RecordedHuman;
        manifest.slots[i].side = static_cast<uint8_t>(i & 1u);
        manifest.slots[i].identity.nativeCharacterId =
            static_cast<uint8_t>(i);
        manifest.slots[i].identity.paletteIndex =
            static_cast<uint8_t>(i);
    }
    manifest.routeEpoch = 10u;
    manifest.rosterEpoch = 11u;
    manifest.layoutGeneration = 12u;
    (void)StageCatalogV2::forSeBuild().makeIdentity(
        3u, manifest.stage);
    if (!validStageIdentity)
        manifest.stage.catalogEntryId ^= 1u;
    manifest.display.recordedUnixSeconds = 1785456000;
    manifest.display.sourceKind = ReplaySourceKindV2::OfflineVsPlayer;

    DeterministicGameStartV2& start = game.start.start;
    start.configuration.timerMode = 2u;
    start.configuration.aiDifficulty = 4u;
    start.configuration.lifeMultiplier = 0u;
    start.configuration.roundsToWinMinusOne = 1u;
    start.rng.gameplay = 0x11111111u;
    start.rng.cosmetic = 0x22222222u;
    start.rng.selector = 0x33333333u;
    start.fpu.x87ControlWord = 0x027Fu;
    start.fpu.mxcsr = 0x1F80u;
    start.initialPhase = 0u;
    start.bootstrapFingerprint = 0x1234567890ABCDEFull;
    start.frameZeroPreStateHash = 0x1000u;
    start.initialRollingDigest = initialReplayDigestV2(game.start);

    uint64_t rolling = start.initialRollingDigest;
    uint32_t eventSequence = 0u;

    GameLifecycleV2 zeroBefore{};
    zeroBefore.scene = 2u;
    zeroBefore.requestedScene = 2u;
    zeroBefore.matchSub = 0;
    zeroBefore.winnerSlot = -1;
    zeroBefore.routeEpoch = manifest.routeEpoch;
    zeroBefore.rosterEpoch = manifest.rosterEpoch;
    zeroBefore.layoutGeneration = manifest.layoutGeneration;
    GameLifecycleV2 zeroAfter = zeroBefore;
    zeroAfter.matchSub = 1;
    CanonicalTickBundleV2 zero = tick(
        500u, 0u, zeroBefore, zeroAfter, 0x1000u, 0x2000u);
    zero.preEvents.count = 2u;
    zero.preEvents.events[0] = event(
        GameEventKindV2::GameStarted,
        GameEventPhaseV2::PreFrame,
        0u, eventSequence++, zero.frame.core.preStateHash);
    zero.preEvents.events[0].rollingDigest =
        advanceReplayDigestV2(rolling, zero.preEvents.events[0]);
    rolling = zero.preEvents.events[0].rollingDigest;
    zero.preEvents.events[1] = event(
        GameEventKindV2::RoundStarted,
        GameEventPhaseV2::PreFrame,
        0u, eventSequence++, zero.frame.core.preStateHash);
    zero.preEvents.events[1].rollingDigest =
        advanceReplayDigestV2(rolling, zero.preEvents.events[1]);
    rolling = zero.preEvents.events[1].rollingDigest;
    zero.frame.rollingDigest =
        advanceReplayDigestV2(rolling, zero.frame);
    rolling = zero.frame.rollingDigest;
    game.ticks.push_back(zero);

    GameLifecycleV2 transitionAfter = zeroAfter;
    transitionAfter.matchSub = 2;
    transitionAfter.roundCounter = 5u;
    transitionAfter.resultCounter = 0u;
    transitionAfter.winnerSlot = 0;
    transitionAfter.roundTallies = {{2u, 1u}};
    CanonicalTickBundleV2 transition = tick(
        501u, 1u, zeroAfter, transitionAfter, 0x2000u, 0x3000u);
    transition.frame.rollingDigest =
        advanceReplayDigestV2(rolling, transition.frame);
    rolling = transition.frame.rollingDigest;
    transition.postEvents.count = 1u;
    transition.postEvents.events[0] = event(
        GameEventKindV2::RoundEnded,
        GameEventPhaseV2::PostFrame,
        1u, eventSequence++, transition.frame.core.postStateHash);
    transition.postEvents.events[0].rollingDigest =
        advanceReplayDigestV2(
            rolling, transition.postEvents.events[0]);
    rolling = transition.postEvents.events[0].rollingDigest;
    game.ticks.push_back(transition);

    GameLifecycleV2 winscreenAfter = transitionAfter;
    winscreenAfter.resultCounter = 1u;
    CanonicalTickBundleV2 winscreen = tick(
        502u, 2u, transitionAfter, winscreenAfter, 0x3000u, 0x4000u);
    winscreen.frame.rollingDigest =
        advanceReplayDigestV2(rolling, winscreen.frame);
    rolling = winscreen.frame.rollingDigest;
    winscreen.postEvents.count = 1u;
    winscreen.postEvents.events[0] = event(
        GameEventKindV2::GameResult,
        GameEventPhaseV2::PostFrame,
        2u, eventSequence++, winscreen.frame.core.postStateHash);
    winscreen.postEvents.events[0].rollingDigest =
        advanceReplayDigestV2(
            rolling, winscreen.postEvents.events[0]);
    rolling = winscreen.postEvents.events[0].rollingDigest;
    game.ticks.push_back(winscreen);

    game.end.reason = TerminalReason::Completed;
    game.end.nextRecord =
        static_cast<uint32_t>(game.ticks.size());
    game.end.winningSideMask = 0x01u;
    game.end.roundTallies = {{2u, 1u}};
    game.end.winnerSlot = 0;
    game.end.postTailStateHash = 0x4000u;
    game.end.finalRollingDigest = rolling;

    CanonicalGameDigestV2 canonical;
    if (!canonical.begin(game.start)) return PlaybackGame{};
    for (const CanonicalTickBundleV2& bundle : game.ticks) {
        for (size_t i = 0u; i < bundle.preEvents.count; ++i)
            if (!canonical.append(bundle.preEvents.events[i]))
                return PlaybackGame{};
        if (!canonical.append(bundle.frame)) return PlaybackGame{};
        for (size_t i = 0u; i < bundle.postEvents.count; ++i)
            if (!canonical.append(bundle.postEvents.events[i]))
                return PlaybackGame{};
    }
    if (!canonical.finish(game.end, game.end.canonicalGameDigest))
        return PlaybackGame{};
    return game;
}

bool fillTimeline(
    ConfirmedTimelineBufferV2& timeline,
    const PlaybackGame& game) {
    ParseStatus why = ParseStatus::Ok;
    if (!timeline.begin(game.start, why)) return false;
    for (const CanonicalTickBundleV2& bundle : game.ticks)
        if (!timeline.append(bundle, why)) return false;
    return timeline.seal(game.end, why);
}

ReplayPlaybackBindingV2 bindingFor(
    const PlaybackGame& game, bool headless) {
    ReplayPlaybackBindingV2 binding{};
    binding.endpoint.kind =
        wire3::PlaybackEndpointKind::RecordedGameEnd;
    binding.endpoint.hasRecordedGameEnd = true;
    binding.endpoint.nextRecord = game.end.nextRecord;
    binding.endpoint.postTailStateHash =
        game.end.postTailStateHash;
    binding.endpoint.finalRollingDigest =
        game.end.finalRollingDigest;
    binding.endpoint.validatedSourceByteCount = 0x12345u;
    binding.endpoint.validatedSourceRangeDigest.fill(0x33u);
    binding.endpoint.recordedGameEnd = game.end;
    binding.runtimeContract = game.start.manifest.contract;
    binding.headless = headless;
    binding.hasFileDigest = true;
    binding.fileDigest.fill(0x44u);
    return binding;
}

ReplayBootstrapObservationV2 bootstrapOf(
    const PlaybackGame& game) {
    ReplayBootstrapObservationV2 observed{};
    observed.initialPhase = game.start.start.initialPhase;
    observed.bootstrapFingerprint =
        game.start.start.bootstrapFingerprint;
    observed.frameZeroPreStateHash =
        game.start.start.frameZeroPreStateHash;
    observed.frameZeroLifecycle =
        game.ticks.front().frame.core.before;
    return observed;
}

ReplayFrameBeginObservationV2 beginOf(
    const CanonicalTickBundleV2& bundle,
    uint64_t finalPresentationSerial = 0u) {
    ReplayFrameBeginObservationV2 observed{};
    observed.replayFrame = bundle.frame.replayFrame;
    observed.preStateHash = bundle.frame.core.preStateHash;
    observed.lifecycleBefore = bundle.frame.core.before;
    observed.activeLaneMask = bundle.frame.core.activeLaneMask;
    observed.finalPresentationSerial = finalPresentationSerial;
    return observed;
}

ReplayFrameCommitObservationV2 commitOf(
    const CanonicalTickBundleV2& bundle) {
    ReplayFrameCommitObservationV2 observed{};
    observed.replayFrame = bundle.frame.replayFrame;
    observed.postStateHash = bundle.frame.core.postStateHash;
    observed.lifecycleAfter = bundle.frame.core.after;
    observed.dispatcherResult =
        bundle.frame.core.dispatcherResult;
    return observed;
}

}

QNP_TEST(ReplayRestartRequiresBootstrapAndReplaysTheOriginalProof) {
    const PlaybackGame game = completedFourLaneGame();
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(std::move(timeline), bindingFor(game, false));
    QNP_CHECK(!session.restart());
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
    QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[0], 0u)));
    QNP_CHECK(!session.restart());
    QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[0])));
    QNP_REQUIRE(session.restart());
    QNP_CHECK(session.nextRecord() == 0u);
    QNP_CHECK(session.currentRollingDigest() == 0u);
    QNP_CHECK(session.peek() == nullptr);
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
    for (size_t i = 0; i < game.ticks.size(); ++i) {
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[i], i + 1u)));
        QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[i])));
    }
    QNP_CHECK(session.acknowledgeFinalPresentation(game.ticks.size())
              == ReplayPresentationAckV2::Accepted);
    QNP_REQUIRE(session.result() != nullptr);
    QNP_CHECK(session.result()->reason == wire3::PlaybackResultReason::VerifiedCompleted);
    QNP_CHECK(!session.restart());
}

QNP_TEST(ReplayCheckpointReplaysTheSameVerifiedSuffixAndDigest) {
    const PlaybackGame game = completedFourLaneGame();
    for (bool headless : {false, true}) {
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(std::move(timeline), bindingFor(game, headless));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        ReplayPlaybackSessionV2::Checkpoint initial;
        QNP_REQUIRE(session.captureCheckpoint(initial));
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[0])));
        QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[0])));
        ReplayPlaybackSessionV2::Checkpoint afterZero;
        QNP_REQUIRE(session.captureCheckpoint(afterZero));
        const uint64_t prefixDigest = session.currentRollingDigest();
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[1])));
        QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[1])));
        const uint64_t suffixDigest = session.currentRollingDigest();

        QNP_REQUIRE(session.restoreCheckpoint(afterZero));
        QNP_CHECK(session.nextRecord() == 1u);
        QNP_CHECK(session.currentRollingDigest() == prefixDigest);
        QNP_CHECK(session.peek() == session.tickAt(1u));
        QNP_CHECK(session.inFlightReplayFrame() == kNoReplayMismatchFrameV2);
        QNP_CHECK(session.latchedActiveLaneMask() == 0u);
        QNP_CHECK((session.latchedActions() == std::array<uint16_t, kSourceLanes>{}));
        QNP_CHECK(session.byteExactStateProof());
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[1])));
        QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[1])));
        QNP_CHECK(session.currentRollingDigest() == suffixDigest);

        QNP_REQUIRE(session.restoreCheckpoint(initial));
        QNP_CHECK(session.nextRecord() == 0u);
        QNP_CHECK(session.currentRollingDigest() == 0u);
        for (const auto& bundle : game.ticks) {
            QNP_REQUIRE(session.beginFrame(beginOf(bundle, 99u)));
            QNP_REQUIRE(session.commitFrame(commitOf(bundle)));
        }
        if (!headless) {
            QNP_CHECK(session.acknowledgeFinalPresentation(98u)
                      == ReplayPresentationAckV2::WrongSerial);
            QNP_CHECK(session.acknowledgeFinalPresentation(99u)
                      == ReplayPresentationAckV2::Accepted);
        }
        QNP_REQUIRE(session.result() != nullptr);
        QNP_CHECK(session.result()->reason == wire3::PlaybackResultReason::VerifiedCompleted);
        QNP_CHECK(session.result()->nextRecordConsumed == game.end.nextRecord);
        QNP_CHECK(session.result()->finalRollingDigest == game.end.finalRollingDigest);
        QNP_CHECK(session.result()->canonicalGameDigest == game.end.canonicalGameDigest);
        QNP_CHECK(session.result()->deviceCallCount == 0u);
    }
}

QNP_TEST(ReplayCheckpointRejectsDefaultOtherSessionAndReplacedTimeline) {
    const PlaybackGame game = completedFourLaneGame();
    ConfirmedTimelineBufferV2 firstTimeline(game.ticks.size());
    ConfirmedTimelineBufferV2 secondTimeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(firstTimeline, game));
    QNP_REQUIRE(fillTimeline(secondTimeline, game));
    ReplayPlaybackSessionV2 first(std::move(firstTimeline), bindingFor(game, true));
    ReplayPlaybackSessionV2 second(std::move(secondTimeline), bindingFor(game, true));
    QNP_REQUIRE(first.completeBootstrap(bootstrapOf(game)));
    QNP_REQUIRE(second.completeBootstrap(bootstrapOf(game)));
    ReplayPlaybackSessionV2::Checkpoint checkpoint;
    QNP_CHECK(!first.restoreCheckpoint(checkpoint));
    QNP_REQUIRE(first.captureCheckpoint(checkpoint));
    QNP_CHECK(!second.restoreCheckpoint(checkpoint));
    QNP_CHECK(second.nextRecord() == 0u);
    QNP_CHECK(!second.hasResult());
    first = std::move(second);
    QNP_CHECK(!first.restoreCheckpoint(checkpoint));
    QNP_CHECK(first.nextRecord() == 0u);
    QNP_CHECK(!first.hasResult());
}

QNP_TEST(ReplayCheckpointRefusesBootstrapInFlightAndTerminalStates) {
    const PlaybackGame game = completedFourLaneGame();
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(std::move(timeline), bindingFor(game, false));
    ReplayPlaybackSessionV2::Checkpoint checkpoint;
    QNP_CHECK(!session.captureCheckpoint(checkpoint));
    QNP_CHECK(!session.restoreCheckpoint(checkpoint));
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
    QNP_REQUIRE(session.captureCheckpoint(checkpoint));
    QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[0])));
    QNP_CHECK(!session.captureCheckpoint(checkpoint));
    QNP_CHECK(!session.restoreCheckpoint(checkpoint));
    QNP_CHECK(session.nextRecord() == 1u);
    QNP_CHECK(session.latchedActions() == game.ticks[0].frame.core.actions);
    QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[0])));
    QNP_REQUIRE(session.restoreCheckpoint(checkpoint));
    for (const auto& bundle : game.ticks) {
        QNP_REQUIRE(session.beginFrame(beginOf(bundle, 42u)));
        QNP_CHECK(!session.captureCheckpoint(checkpoint));
        QNP_CHECK(!session.restoreCheckpoint(checkpoint));
        QNP_REQUIRE(session.commitFrame(commitOf(bundle)));
    }
    QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::AwaitingFinalPresentation);
    QNP_CHECK(!session.captureCheckpoint(checkpoint));
    QNP_CHECK(!session.restoreCheckpoint(checkpoint));
    QNP_REQUIRE(session.acknowledgeFinalPresentation(42u) == ReplayPresentationAckV2::Accepted);
    QNP_CHECK(!session.captureCheckpoint(checkpoint));
    QNP_CHECK(!session.restoreCheckpoint(checkpoint));
    QNP_CHECK(session.result()->reason == wire3::PlaybackResultReason::VerifiedCompleted);
}

QNP_TEST(ReplayCheckpointCannotUndoADeviceFaultOrItsCounter) {
    const PlaybackGame game = completedFourLaneGame();
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(std::move(timeline), bindingFor(game, true));
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
    ReplayPlaybackSessionV2::Checkpoint checkpoint;
    QNP_REQUIRE(session.captureCheckpoint(checkpoint));
    QNP_REQUIRE(session.noteDeviceBoundaryCall(7u));
    QNP_CHECK(!session.captureCheckpoint(checkpoint));
    QNP_CHECK(!session.restoreCheckpoint(checkpoint));
    QNP_CHECK(session.deviceCallCount() == 7u);
    QNP_CHECK(session.fault() == ReplayPlaybackFaultV2::DeviceIsolationViolation);
    QNP_REQUIRE(session.result() != nullptr);
    QNP_CHECK(session.result()->deviceCallCount == 7u);
}

QNP_TEST(ReplayCheckpointRestoresTheFullDivergencePrefix) {
    const PlaybackGame game = completedFourLaneGame();
    for (bool preservePrefix : {false, true}) {
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(std::move(timeline), bindingFor(game, true));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        ReplayPlaybackSessionV2::Checkpoint initial;
        QNP_REQUIRE(session.captureCheckpoint(initial));
        ReplayPlaybackSessionV2::Checkpoint checkpoint;
        QNP_REQUIRE(session.captureCheckpoint(checkpoint));
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[0])));
        auto wrong = commitOf(game.ticks[0]);
        wrong.postStateHash ^= 1u;
        wrong.dispatcherResult ^= 1;
        QNP_REQUIRE(session.commitFrame(wrong));
        QNP_CHECK(session.divergenceCount() == 2u);
        if (preservePrefix) QNP_REQUIRE(session.captureCheckpoint(checkpoint));
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[1])));
        wrong = commitOf(game.ticks[1]);
        wrong.postStateHash ^= 1u;
        QNP_REQUIRE(session.commitFrame(wrong));
        QNP_CHECK(session.divergenceCount() == 3u);

        if (preservePrefix) {
            QNP_REQUIRE(session.restoreCheckpoint(initial));
            QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[0])));
            QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[0])));
            auto differentFirstDivergence = beginOf(game.ticks[1]);
            differentFirstDivergence.lifecycleBefore.scene ^= 1u;
            QNP_REQUIRE(session.beginFrame(differentFirstDivergence));
            QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[1])));
            QNP_CHECK(session.divergenceCount() == 1u);
            QNP_CHECK(session.firstDivergenceFrame() == 1u);
        }

        QNP_REQUIRE(session.restoreCheckpoint(checkpoint));
        QNP_CHECK(session.divergenceCount() == (preservePrefix ? 2u : 0u));
        QNP_CHECK(session.firstDivergenceFrame()
                  == (preservePrefix ? 0u : kNoReplayMismatchFrameV2));
        while (const auto* bundle = session.peek()) {
            QNP_REQUIRE(session.beginFrame(beginOf(*bundle)));
            auto observed = commitOf(*bundle);
            if (bundle->frame.replayFrame == 2u) observed.postStateHash ^= 2u;
            QNP_REQUIRE(session.commitFrame(observed));
        }
        QNP_REQUIRE(session.result() != nullptr);
        QNP_CHECK(session.divergenceCount() == (preservePrefix ? 3u : 1u));
        QNP_CHECK(session.result()->reason == wire3::PlaybackResultReason::CompletedWithDivergence);
        QNP_CHECK(session.result()->mismatchFrame == (preservePrefix ? 0u : 2u));
        QNP_CHECK(session.result()->mismatchRegion
                  == static_cast<uint32_t>(ReplayPlaybackProofRegionV2::FramePostState));
        const uint64_t expected = game.ticks[preservePrefix ? 0u : 2u].frame.core.postStateHash;
        QNP_CHECK(session.result()->expectedValue == expected);
        QNP_CHECK(session.result()->actualValue == (expected ^ (preservePrefix ? 1u : 2u)));
    }
}

QNP_TEST(ReplayCheckpointRestorationStillEnforcesFrameOrderAndLaneMask) {
    const PlaybackGame game = completedFourLaneGame();
    for (bool wrongFrame : {false, true}) {
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(std::move(timeline), bindingFor(game, true));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[0])));
        QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[0])));
        ReplayPlaybackSessionV2::Checkpoint checkpoint;
        QNP_REQUIRE(session.captureCheckpoint(checkpoint));
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[1])));
        QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[1])));
        QNP_REQUIRE(session.restoreCheckpoint(checkpoint));
        auto wrong = beginOf(game.ticks[1]);
        if (wrongFrame) wrong.replayFrame = 0u;
        else wrong.activeLaneMask ^= 1u;
        QNP_CHECK(!session.beginFrame(wrong));
        QNP_CHECK(session.nextRecord() == 1u);
        QNP_CHECK(session.fault() == (wrongFrame
            ? ReplayPlaybackFaultV2::FrameNumberMismatch
            : ReplayPlaybackFaultV2::FrameLaneMaskMismatch));
        QNP_CHECK(!session.restoreCheckpoint(checkpoint));
    }
}

QNP_TEST(StageCatalogV2IsExactAndStrict) {
    const StageCatalogV2& catalog = StageCatalogV2::forSeBuild();
    QNP_CHECK(catalog.entryCount() == 28u);
    QNP_CHECK(catalog.catalogId() == 0x301DE5FF726FBA85ull);

    for (uint32_t logical = 0u; logical < 28u; ++logical) {
        const StageCatalogEntryV2* entry =
            catalog.findByLogicalStageIndex(logical);
        QNP_REQUIRE(entry != nullptr);
        QNP_CHECK(entry->logicalStageIndex == logical);
        QNP_CHECK(entry->nativeStageId == logical + 1u);
        QNP_CHECK(entry->catalogEntryId != 0u);
        QNP_CHECK(catalog.findByNativeStageId(logical + 1u) == entry);

        uint32_t native = 0xAAAAAAAAu;
        uint32_t inverted = 0xBBBBBBBBu;
        QNP_CHECK(catalog.nativeStageIdForLogicalStageIndex(
            logical, native));
        QNP_CHECK(native == logical + 1u);
        QNP_CHECK(catalog.logicalStageIndexForNativeStageId(
            native, inverted));
        QNP_CHECK(inverted == logical);

        StageIdentityV2 identity{};
        QNP_REQUIRE(catalog.makeIdentity(logical, identity));
        QNP_CHECK(catalog.validateIdentity(identity));

        StageIdentityV2 tampered = identity;
        tampered.verified = false;
        QNP_CHECK(!catalog.validateIdentity(tampered));
        tampered = identity;
        ++tampered.catalogEntryId;
        QNP_CHECK(!catalog.validateIdentity(tampered));
    }
    QNP_CHECK(catalog.entryAt(0u).catalogEntryId
              == 0x7D97322396220844ull);
    QNP_CHECK(catalog.entryAt(1u).catalogEntryId
              == 0x3E8C6F338DD3F3D6ull);
    QNP_CHECK(catalog.entryAt(27u).catalogEntryId
              == 0xC4A11D0DF8A3B032ull);

    uint32_t untouched = 0xCAFEBABEu;
    QNP_CHECK(!catalog.nativeStageIdForLogicalStageIndex(
        28u, untouched));
    QNP_CHECK(untouched == 0xCAFEBABEu);
    QNP_CHECK(!catalog.logicalStageIndexForNativeStageId(
        0u, untouched));
    QNP_CHECK(untouched == 0xCAFEBABEu);
    QNP_CHECK(!catalog.logicalStageIndexForNativeStageId(
        29u, untouched));
    QNP_CHECK(catalog.entryAt(28u).nativeStageId == 0u);

    StageIdentityV2 unknown{};
    unknown.verified = true;
    unknown.nativeStageId = 29u;
    unknown.catalogEntryId = 1u;
    QNP_CHECK(!catalog.validateIdentity(unknown));
}

QNP_TEST(ReplayPlaybackV2WrongPreDoesNotConsume) {
    PlaybackGame game = completedFourLaneGame();
    QNP_REQUIRE(game.ticks.size() == 3u);
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, true));
    QNP_REQUIRE(session.phase() == ReplayPlaybackPhaseV2::Bound);
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));

    const CanonicalTickBundleV2* first = session.peek();
    QNP_REQUIRE(first != nullptr);
    QNP_CHECK(session.peek() == first);
    ReplayFrameBeginObservationV2 wrong = beginOf(*first);
    wrong.preStateHash ^= 1u;
    QNP_CHECK(session.beginFrame(wrong));
    QNP_CHECK(session.divergenceCount() == 1u);
    QNP_CHECK(session.phase() != ReplayPlaybackPhaseV2::Faulted);
    QNP_CHECK(session.fault() == ReplayPlaybackFaultV2::None);
    QNP_CHECK(session.result() == nullptr);
    QNP_CHECK(session.firstDivergenceFrame()
              == first->frame.replayFrame);
    QNP_CHECK(session.nextRecord() == 1u);
}

QNP_TEST(ReplayPlaybackV2WrongLaneMaskFaultsAndConsumesNothing) {
    PlaybackGame game = completedFourLaneGame();
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, true));
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));

    const CanonicalTickBundleV2* first = session.peek();
    QNP_REQUIRE(first != nullptr);
    ReplayFrameBeginObservationV2 wrong = beginOf(*first);
    wrong.activeLaneMask = static_cast<uint8_t>(wrong.activeLaneMask ^ 1u);
    QNP_CHECK(!session.beginFrame(wrong));
    QNP_CHECK(session.nextRecord() == 0u);
    QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::Faulted);
    QNP_CHECK(session.fault()
              == ReplayPlaybackFaultV2::FrameLaneMaskMismatch);
    QNP_CHECK(session.divergenceCount() == 0u);
    const wire3::PlaybackResultPayload* result = session.result();
    QNP_REQUIRE(result != nullptr);
    QNP_CHECK(result->nextRecordConsumed == 0u);
}

QNP_TEST(ReplayPlaybackV2LatchesAllFourLogicalLanes) {
    PlaybackGame game = completedFourLaneGame();
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, true));
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));

    const CanonicalTickBundleV2* first = session.peek();
    QNP_REQUIRE(first != nullptr);
    const std::array<uint16_t, kSourceLanes> expected =
        first->frame.core.actions;
    QNP_REQUIRE(session.beginFrame(beginOf(*first)));
    QNP_CHECK(session.nextRecord() == 1u);
    QNP_CHECK(session.latchedActiveLaneMask() == 0x0Fu);
    QNP_CHECK(session.latchedActions() == expected);
    QNP_CHECK(session.inFlightReplayFrame() == 0u);
    QNP_CHECK(session.phase()
              == ReplayPlaybackPhaseV2::FrameInFlight);
    QNP_REQUIRE(session.commitFrame(commitOf(*first)));
    QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::Ready);
    QNP_CHECK(session.latchedActiveLaneMask() == 0u);
    const std::array<uint16_t, kSourceLanes> cleared{};
    QNP_CHECK(session.latchedActions() == cleared);
}

QNP_TEST(ReplayPlaybackV2FinalFrameRunsExactlyOnce) {
    PlaybackGame game = completedFourLaneGame();
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, false));
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));

    for (size_t i = 0u; i + 1u < game.ticks.size(); ++i) {
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[i])));
        QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[i])));
    }

    constexpr uint64_t kFinalSerial = 77u;
    const CanonicalTickBundleV2& final = game.ticks.back();
    QNP_REQUIRE(session.beginFrame(beginOf(final, kFinalSerial)));
    QNP_CHECK(session.phase()
              == ReplayPlaybackPhaseV2::TerminalArmed);
    QNP_CHECK(session.nextRecord() == game.end.nextRecord);
    QNP_CHECK(!session.beginFrame(beginOf(final, kFinalSerial)));
    QNP_REQUIRE(session.commitFrame(commitOf(final)));
    QNP_CHECK(session.phase()
              == ReplayPlaybackPhaseV2::AwaitingFinalPresentation);
    QNP_CHECK(session.peek() == nullptr);
    QNP_CHECK(session.acknowledgeFinalPresentation(kFinalSerial)
              == ReplayPresentationAckV2::Accepted);
    QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::ResultReady);
    QNP_CHECK(session.acknowledgeFinalPresentation(kFinalSerial)
              == ReplayPresentationAckV2::AlreadyCompleted);
    QNP_CHECK(!session.beginFrame(beginOf(final, kFinalSerial)));

    const wire3::PlaybackResultPayload* result = session.result();
    QNP_REQUIRE(result != nullptr);
    QNP_CHECK(result->reason
              == wire3::PlaybackResultReason::VerifiedCompleted);
    QNP_CHECK(result->nextRecordConsumed == game.end.nextRecord);
}

QNP_TEST(ReplayPlaybackV2WrongAckCannotCompleteAndMissingAckTimesOut) {
    PlaybackGame game = completedFourLaneGame();
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, false));
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));

    for (size_t i = 0u; i < game.ticks.size(); ++i) {
        const uint64_t serial =
            i + 1u == game.ticks.size() ? 9001u : 0u;
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[i], serial)));
        QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[i])));
    }
    QNP_CHECK(session.phase()
              == ReplayPlaybackPhaseV2::AwaitingFinalPresentation);
    QNP_CHECK(session.acknowledgeFinalPresentation(9000u)
              == ReplayPresentationAckV2::WrongSerial);
    QNP_CHECK(!session.hasResult());
    QNP_CHECK(session.nextRecord() == game.end.nextRecord);

    QNP_REQUIRE(session.finalPresentationTimedOut());
    QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::Faulted);
    QNP_CHECK(session.fault()
              == ReplayPlaybackFaultV2::PresentationAckTimeout);
    const wire3::PlaybackResultPayload* result = session.result();
    QNP_REQUIRE(result != nullptr);
    QNP_CHECK(result->reason
              == wire3::PlaybackResultReason::PresentationAckTimeout);
    QNP_CHECK(result->nextRecordConsumed == game.end.nextRecord);
    QNP_CHECK(result->expectedValue == 9001u);
    QNP_CHECK(result->actualValue == 0u);
    QNP_CHECK(!session.finalPresentationTimedOut());
}

QNP_TEST(ReplayPlaybackV2DeviceBoundaryFailsClosed) {
    PlaybackGame game = completedFourLaneGame();
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, true));
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));

    QNP_REQUIRE(session.noteDeviceBoundaryCall());
    QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::Faulted);
    QNP_CHECK(session.fault()
              == ReplayPlaybackFaultV2::DeviceIsolationViolation);
    QNP_CHECK(session.deviceCallCount() == 1u);
    QNP_CHECK(session.nextRecord() == 0u);
    const wire3::PlaybackResultPayload* result = session.result();
    QNP_REQUIRE(result != nullptr);
    QNP_CHECK(result->reason
              == wire3::PlaybackResultReason::DeviceIsolationViolation);
    QNP_CHECK(result->deviceCallCount == 1u);
    QNP_CHECK(result->nextRecordConsumed == 0u);
    QNP_CHECK(!session.noteDeviceBoundaryCall());
    QNP_CHECK(!session.beginFrame(beginOf(game.ticks.front())));
}

QNP_TEST(ReplayPlaybackV2ResultExactlyBindsEndpointAndExitOrder) {
    PlaybackGame game = completedFourLaneGame();
    const ReplayPlaybackBindingV2 binding = bindingFor(game, true);
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), binding);
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));

    for (const CanonicalTickBundleV2& bundle : game.ticks) {
        QNP_REQUIRE(session.beginFrame(beginOf(bundle)));
        QNP_REQUIRE(session.commitFrame(commitOf(bundle)));
    }

    QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::ResultReady);
    QNP_CHECK(session.exitState()
              == ReplayPlaybackExitStateV2::ResultPending);
    const wire3::PlaybackResultPayload* result = session.result();
    QNP_REQUIRE(result != nullptr);
    QNP_CHECK(result->reason
              == wire3::PlaybackResultReason::VerifiedCompleted);
    QNP_CHECK(result->endpointKind == binding.endpoint.kind);
    QNP_CHECK(result->headless);
    QNP_CHECK(result->hasFileDigest);
    QNP_CHECK(result->fileDigest == binding.fileDigest);
    QNP_CHECK(result->hasCanonicalGameDigest);
    QNP_CHECK(result->canonicalGameDigest
              == game.end.canonicalGameDigest);
    QNP_CHECK(result->validatedSourceByteCount
              == binding.endpoint.validatedSourceByteCount);
    QNP_CHECK(result->validatedSourceRangeDigest
              == binding.endpoint.validatedSourceRangeDigest);
    QNP_CHECK(result->runtimeContract == binding.runtimeContract);
    QNP_CHECK(result->nextRecordConsumed == game.end.nextRecord);
    QNP_CHECK(result->finalRollingDigest
              == binding.endpoint.finalRollingDigest);
    QNP_CHECK(result->postTailStateHash
              == binding.endpoint.postTailStateHash);
    QNP_CHECK(result->hasRecordedTerminal);
    QNP_CHECK(result->recordedTerminal
              == static_cast<uint16_t>(TerminalReason::Completed));
    QNP_CHECK(result->deviceCallCount == 0u);
    QNP_CHECK(result->mismatchFrame == kNoReplayMismatchFrameV2);

    std::vector<uint8_t> encoded;
    QNP_CHECK(wire3::encodePlaybackResult(*result, encoded));
    QNP_CHECK(encoded.size() == wire3::kPlaybackResultPayloadBytes);

    QNP_CHECK(!session.requestGracefulExit());
    QNP_REQUIRE(session.acknowledgeResultDelivery());
    QNP_CHECK(session.exitState()
              == ReplayPlaybackExitStateV2::ResultAcknowledged);
    QNP_REQUIRE(session.requestGracefulExit());
    QNP_CHECK(session.exitState()
              == ReplayPlaybackExitStateV2::ExitRequested);
    QNP_CHECK(!session.acknowledgeResultDelivery());
    QNP_CHECK(!session.requestGracefulExit());
}

QNP_TEST(ReplayPlaybackV2RejectsEndpointBeforeReady) {
    PlaybackGame game = completedFourLaneGame();
    ReplayPlaybackBindingV2 binding = bindingFor(game, true);
    ++binding.endpoint.nextRecord;
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), binding);
    QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::Faulted);
    QNP_CHECK(session.fault()
              == ReplayPlaybackFaultV2::EndpointMismatch);
    QNP_CHECK(!session.completeBootstrap(bootstrapOf(game)));
    QNP_CHECK(session.nextRecord() == 0u);
    const wire3::PlaybackResultPayload* result = session.result();
    QNP_REQUIRE(result != nullptr);
    QNP_CHECK(result->reason
              == wire3::PlaybackResultReason::TerminalMismatch);
}

QNP_TEST(ReplayPlaybackV2RejectsUnknownStageCatalogEntryBeforeReady) {
    PlaybackGame game = completedFourLaneGame(false);
    QNP_REQUIRE(game.ticks.size() == 3u);
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, true));
    QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::Faulted);
    QNP_CHECK(session.fault()
              == ReplayPlaybackFaultV2::StageIdentityMismatch);
    QNP_CHECK(session.nextRecord() == 0u);
    const wire3::PlaybackResultPayload* result = session.result();
    QNP_REQUIRE(result != nullptr);
    QNP_CHECK(result->reason
              == wire3::PlaybackResultReason::Unsupported);
    QNP_CHECK(result->mismatchRegion
              == static_cast<uint32_t>(
                  ReplayPlaybackProofRegionV2::StageIdentity));
}

QNP_TEST(ReplayPlaybackV2FirstGameIsByteExactAndStillRejectsHashDrift) {
    PlaybackGame game = completedFourLaneGame();
    QNP_CHECK(game.start.manifest.identity.gameGeneration == 1u);
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, true));
    QNP_CHECK(session.mode()
              == ReplayVerificationModeV2::ByteExact);
    QNP_CHECK(session.byteExactStateProof());

    ReplayBootstrapObservationV2 drifted = bootstrapOf(game);
    drifted.frameZeroPreStateHash ^= 1u;
    QNP_CHECK(session.completeBootstrap(drifted));
    QNP_CHECK(session.divergenceCount() == 1u);
    QNP_CHECK(session.fault() == ReplayPlaybackFaultV2::None);
    QNP_CHECK(session.phase() != ReplayPlaybackPhaseV2::Faulted);

    for (const auto& tick : game.ticks) {
        QNP_REQUIRE(session.beginFrame(beginOf(tick)));
        QNP_REQUIRE(session.commitFrame(commitOf(tick)));
    }
    const wire3::PlaybackResultPayload* result = session.result();
    QNP_REQUIRE(result != nullptr);
    QNP_CHECK(result->reason
              == wire3::PlaybackResultReason::CompletedWithDivergence);
    QNP_CHECK(!wire3::isVerifiedPlaybackResultReason(result->reason));
}

QNP_TEST(ReplayPlaybackV2FirstGamePostHashDriftIsReportedAndPlaysOn) {
    PlaybackGame game = completedFourLaneGame();
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, true));
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
    QNP_REQUIRE(session.beginFrame(beginOf(game.ticks.front())));
    ReplayFrameCommitObservationV2 commit = commitOf(game.ticks.front());
    commit.postStateHash ^= 1u;
    QNP_CHECK(session.commitFrame(commit));
    QNP_CHECK(session.divergenceCount() == 1u);
    QNP_CHECK(session.firstDivergenceFrame()
              == game.ticks.front().frame.replayFrame);
    QNP_CHECK(session.fault() == ReplayPlaybackFaultV2::None);
    QNP_CHECK(session.phase() != ReplayPlaybackPhaseV2::Faulted);

    for (size_t index = 1u; index < game.ticks.size(); ++index) {
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[index])));
        QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[index])));
    }
    const wire3::PlaybackResultPayload* result = session.result();
    QNP_REQUIRE(result != nullptr);
    QNP_CHECK(result->reason
              == wire3::PlaybackResultReason::CompletedWithDivergence);
    QNP_CHECK(!wire3::isVerifiedPlaybackResultReason(result->reason));
    QNP_CHECK(wire3::isPlayedThroughPlaybackResultReason(result->reason));
    QNP_CHECK(result->mismatchFrame
              == game.ticks.front().frame.replayFrame);
}

QNP_TEST(ReplayPlaybackV2RematchIsBehaviourVerifiedEndToEnd) {
    PlaybackGame game = completedFourLaneGame();
    game.start.manifest.identity.gameGeneration = 4u;
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, true));
    QNP_CHECK(session.mode()
              == ReplayVerificationModeV2::BehaviourVerified);
    QNP_CHECK(!session.byteExactStateProof());

    ReplayBootstrapObservationV2 bootstrap = bootstrapOf(game);
    bootstrap.frameZeroPreStateHash ^= 0xA5A5A5A5A5A5A5A5ull;
    QNP_REQUIRE(session.completeBootstrap(bootstrap));

    for (size_t i = 0u; i < game.ticks.size(); ++i) {
        ReplayFrameBeginObservationV2 begin = beginOf(game.ticks[i]);
        begin.preStateHash ^= 0x5A5A5A5A5A5A5A5Aull;
        QNP_REQUIRE(session.beginFrame(begin));
        ReplayFrameCommitObservationV2 commit = commitOf(game.ticks[i]);
        commit.postStateHash ^= 0x5A5A5A5A5A5A5A5Aull;
        QNP_REQUIRE(session.commitFrame(commit));
    }
    QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::ResultReady);
    const wire3::PlaybackResultPayload* result = session.result();
    QNP_REQUIRE(result != nullptr);
    QNP_CHECK(result->reason
              == wire3::PlaybackResultReason::VerifiedBehaviourOnly);
    QNP_CHECK(result->nextRecordConsumed == game.end.nextRecord);
    QNP_CHECK(result->hasRecordedTerminal);
    QNP_CHECK(result->recordedTerminal
              == static_cast<uint16_t>(TerminalReason::Completed));
    QNP_CHECK(result->canonicalGameDigest
              == game.end.canonicalGameDigest);
    QNP_CHECK(result->deviceCallCount == 0u);
    std::vector<uint8_t> encoded;
    QNP_CHECK(wire3::encodePlaybackResult(*result, encoded));
    QNP_CHECK(encoded.size() == wire3::kPlaybackResultPayloadBytes);
    wire3::PlaybackResultPayload decoded{};
    QNP_CHECK(wire3::decodePlaybackResult(
        encoded.data(), encoded.size(), decoded));
    QNP_CHECK(decoded.reason
              == wire3::PlaybackResultReason::VerifiedBehaviourOnly);
}

QNP_TEST(ReplayPlaybackV2BehaviourVerifiedStillEnforcesEverythingElse) {
    {
        PlaybackGame game = completedFourLaneGame();
        game.start.manifest.identity.gameGeneration = 2u;
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, true));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        ReplayFrameBeginObservationV2 begin = beginOf(game.ticks.front());
        begin.preStateHash ^= 1u;
        begin.lifecycleBefore.roundCounter += 1u;
        QNP_CHECK(session.beginFrame(begin));
        QNP_CHECK(session.divergenceCount() == 1u);
        QNP_CHECK(session.fault() == ReplayPlaybackFaultV2::None);
        QNP_CHECK(session.nextRecord() == 1u);
    }
    {
        PlaybackGame game = completedFourLaneGame();
        game.start.manifest.identity.gameGeneration = 2u;
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, true));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks.front())));
        ReplayFrameCommitObservationV2 commit =
            commitOf(game.ticks.front());
        commit.postStateHash ^= 1u;
        commit.lifecycleAfter.winnerSlot = 1;
        QNP_CHECK(session.commitFrame(commit));
        QNP_CHECK(session.divergenceCount() == 1u);
        QNP_CHECK(session.fault() == ReplayPlaybackFaultV2::None);
    }
    {
        PlaybackGame game = completedFourLaneGame();
        game.start.manifest.identity.gameGeneration = 2u;
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, true));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks.front())));
        ReplayFrameCommitObservationV2 commit =
            commitOf(game.ticks.front());
        commit.dispatcherResult += 1;
        QNP_CHECK(session.commitFrame(commit));
        QNP_CHECK(session.divergenceCount() == 1u);
        QNP_CHECK(session.fault() == ReplayPlaybackFaultV2::None);
    }
    {
        PlaybackGame game = completedFourLaneGame();
        game.start.manifest.identity.gameGeneration = 2u;
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, true));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        ReplayFrameBeginObservationV2 begin = beginOf(game.ticks[1]);
        QNP_CHECK(!session.beginFrame(begin));
        QNP_CHECK(session.fault()
                  == ReplayPlaybackFaultV2::FrameNumberMismatch);
    }
    {
        PlaybackGame game = completedFourLaneGame();
        game.start.manifest.identity.gameGeneration = 2u;
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, true));
        QNP_REQUIRE(!session.byteExactStateProof());
        ReplayBootstrapObservationV2 bootstrap = bootstrapOf(game);
        bootstrap.bootstrapFingerprint ^= 1u;
        QNP_CHECK(session.completeBootstrap(bootstrap));
        QNP_CHECK(session.divergenceCount() == 0u);
        QNP_CHECK(session.fault() == ReplayPlaybackFaultV2::None);
    }
    {
        PlaybackGame game = completedFourLaneGame();
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, true));
        QNP_REQUIRE(session.byteExactStateProof());
        ReplayBootstrapObservationV2 bootstrap = bootstrapOf(game);
        bootstrap.bootstrapFingerprint ^= 1u;
        QNP_CHECK(session.completeBootstrap(bootstrap));
        QNP_CHECK(session.divergenceCount() == 1u);
    }
}

QNP_TEST(ReplayPlaybackV2TransportMayHoldOnlyBetweenFrames) {
    PlaybackGame game = completedFourLaneGame();
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, false));

    QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::Bound);
    QNP_CHECK(!session.transportMayHold());

    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
    QNP_CHECK(session.transportMayHold());

    QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[0])));
    QNP_CHECK(session.phase()
              == ReplayPlaybackPhaseV2::FrameInFlight);
    QNP_CHECK(!session.transportMayHold());
    QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[0])));
    QNP_CHECK(session.transportMayHold());

    QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[1])));
    QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[1])));

    constexpr uint64_t kFinalSerial = 4242u;
    const CanonicalTickBundleV2& last = game.ticks.back();
    QNP_REQUIRE(session.beginFrame(beginOf(last, kFinalSerial)));
    QNP_CHECK(session.phase()
              == ReplayPlaybackPhaseV2::TerminalArmed);
    QNP_CHECK(!session.transportMayHold());
    QNP_REQUIRE(session.commitFrame(commitOf(last)));
    QNP_CHECK(session.phase()
              == ReplayPlaybackPhaseV2::AwaitingFinalPresentation);
    QNP_CHECK(!session.transportMayHold());

    QNP_CHECK(session.acknowledgeFinalPresentation(kFinalSerial)
              == ReplayPresentationAckV2::Accepted);
    QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::ResultReady);
    QNP_CHECK(!session.transportMayHold());

    PlaybackGame other = completedFourLaneGame();
    ConfirmedTimelineBufferV2 otherTimeline(other.ticks.size());
    QNP_REQUIRE(fillTimeline(otherTimeline, other));
    ReplayPlaybackSessionV2 faulted(
        std::move(otherTimeline), bindingFor(other, true));
    QNP_REQUIRE(faulted.completeBootstrap(bootstrapOf(other)));
    QNP_CHECK(faulted.transportMayHold());
    QNP_REQUIRE(faulted.noteDeviceBoundaryCall());
    QNP_CHECK(faulted.phase() == ReplayPlaybackPhaseV2::Faulted);
    QNP_CHECK(!faulted.transportMayHold());
    QNP_CHECK(faulted.peek() == nullptr);
}

QNP_TEST(ReplayPlaybackV2TransportPacingCannotChangeTheProof) {
    struct PacingModel {
        bool paused = false;
        uint32_t pendingSteps = 0u;
        uint32_t speedPermille = 1000u;
        uint32_t speedCredit = 0u;

        void setSpeed(uint32_t permille) {
            speedPermille = permille;
            speedCredit = permille != 0u && permille < 1000u
                ? 1000u - permille : 0u;
        }

        uint32_t ticksThisSlot(uint32_t remaining) {
            if (paused) {
                if (pendingSteps == 0u || remaining == 0u) return 0u;
                --pendingSteps;
                return 1u;
            }
            uint32_t permille = speedPermille == 0u ? 1000u : speedPermille;
            speedCredit += permille;
            const uint32_t ticks = speedCredit / 1000u;
            speedCredit %= 1000u;
            return ticks > remaining ? remaining : ticks;
        }
    };

    constexpr uint64_t kFinalSerial = 5150u;

    std::vector<uint64_t> straightDigests;
    std::vector<uint8_t> straightResult;
    {
        PlaybackGame game = completedFourLaneGame();
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, false));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        for (size_t i = 0u; i < game.ticks.size(); ++i) {
            const uint64_t serial =
                i + 1u == game.ticks.size() ? kFinalSerial : 0u;
            QNP_REQUIRE(
                session.beginFrame(beginOf(game.ticks[i], serial)));
            QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[i])));
            straightDigests.push_back(session.currentRollingDigest());
        }
        QNP_CHECK(session.acknowledgeFinalPresentation(kFinalSerial)
                  == ReplayPresentationAckV2::Accepted);
        const wire3::PlaybackResultPayload* result = session.result();
        QNP_REQUIRE(result != nullptr);
        QNP_CHECK(result->reason
                  == wire3::PlaybackResultReason::VerifiedCompleted);
        QNP_REQUIRE(
            wire3::encodePlaybackResult(*result, straightResult));
    }
    QNP_CHECK(straightDigests.size() == 3u);

    std::vector<uint64_t> pacedDigests;
    std::vector<uint8_t> pacedResult;
    {
        PlaybackGame game = completedFourLaneGame();
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, false));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));

        PacingModel pacing{};
        const uint32_t endpointNextRecord = game.end.nextRecord;
        uint32_t heldSlots = 0u;
        for (uint32_t slot = 0u; slot != 64u; ++slot) {
            if (session.hasResult()
                || session.nextRecord() == endpointNextRecord) {
                break;
            }
            switch (slot) {
                case 0u:
                    pacing.paused = true;
                    break;
                case 2u:
                    pacing.paused = true;
                    ++pacing.pendingSteps;
                    break;
                case 3u:
                    pacing.setSpeed(8000u);
                    break;
                case 4u:
                    pacing.paused = false;
                    pacing.pendingSteps = 0u;
                    pacing.setSpeed(250u);
                    break;
                case 9u:
                    pacing.setSpeed(8000u);
                    break;
                default:
                    break;
            }

            const uint32_t before = session.nextRecord();
            const uint32_t ticks =
                pacing.ticksThisSlot(endpointNextRecord - before);
            if (ticks == 0u) {
                ++heldSlots;
                QNP_CHECK(session.transportMayHold());
                QNP_CHECK(session.nextRecord() == before);
                QNP_CHECK(session.phase()
                          == ReplayPlaybackPhaseV2::Ready);
                QNP_CHECK(!session.hasResult());
                continue;
            }
            for (uint32_t i = 0u; i != ticks; ++i) {
                const uint32_t record = session.nextRecord();
                const uint64_t serial =
                    record + 1u == endpointNextRecord ? kFinalSerial : 0u;
                QNP_REQUIRE(session.beginFrame(
                    beginOf(game.ticks[record], serial)));
                QNP_REQUIRE(session.commitFrame(
                    commitOf(game.ticks[record])));
                pacedDigests.push_back(
                    session.currentRollingDigest());
            }
        }
        QNP_CHECK(heldSlots >= 5u);
        QNP_CHECK(session.phase()
                  == ReplayPlaybackPhaseV2::AwaitingFinalPresentation);
        QNP_CHECK(session.acknowledgeFinalPresentation(kFinalSerial)
                  == ReplayPresentationAckV2::Accepted);
        const wire3::PlaybackResultPayload* result = session.result();
        QNP_REQUIRE(result != nullptr);
        QNP_REQUIRE(
            wire3::encodePlaybackResult(*result, pacedResult));
    }

    QNP_CHECK(!straightResult.empty());
    QNP_CHECK(pacedDigests == straightDigests);
    QNP_CHECK(pacedResult == straightResult);
}

namespace {

constexpr uint64_t kTransportFinalSerialV3 = 0x51993921u;

struct TransportPacingV3 {
    bool paused = false;
    uint32_t pendingSteps = 0u;
    uint32_t speedPermille = 1000u;
    uint32_t speedCredit = 0u;

    void pause() { paused = true; }
    void resume() {
        paused = false;
        pendingSteps = 0u;
    }
    void step() {
        paused = true;
        if (pendingSteps != 0xFFFFFFFFu) ++pendingSteps;
    }
    void setSpeed(uint32_t permille) {
        speedPermille = permille;
        speedCredit = permille != 0u && permille < 1000u
            ? 1000u - permille : 0u;
    }

    uint32_t ticksThisSlot(uint32_t remaining) {
        if (paused) {
            if (pendingSteps == 0u || remaining == 0u) return 0u;
            --pendingSteps;
            return 1u;
        }
        uint32_t permille = speedPermille == 0u ? 1000u : speedPermille;
        speedCredit += permille;
        const uint32_t ticks = speedCredit / 1000u;
        speedCredit %= 1000u;
        return ticks > remaining ? remaining : ticks;
    }
};

struct FrameWitnessV3 {
    uint32_t replayFrame = 0u;
    uint32_t nextRecordAfter = 0u;
    uint8_t laneMaskInFlight = 0u;
    std::array<uint16_t, kSourceLanes> actionsInFlight{};
    uint64_t digestAfter = 0u;
};

bool operator==(const FrameWitnessV3& a, const FrameWitnessV3& b) {
    return a.replayFrame == b.replayFrame
        && a.nextRecordAfter == b.nextRecordAfter
        && a.laneMaskInFlight == b.laneMaskInFlight
        && a.actionsInFlight == b.actionsInFlight
        && a.digestAfter == b.digestAfter;
}

struct PlaybackWitnessV3 {
    std::vector<FrameWitnessV3> frames;
    std::vector<uint8_t> encodedResult;
    uint64_t deviceCalls = 0u;
    uint32_t heldSlots = 0u;
    bool byteExact = false;
    wire3::PlaybackResultReason reason =
        wire3::PlaybackResultReason::InternalFailure;
};

template <typename Script>
bool runPacedPlaybackV3(
    const PlaybackGame& game, Script script, PlaybackWitnessV3& out) {
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    if (!fillTimeline(timeline, game)) return false;
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, false));
    if (!session.completeBootstrap(bootstrapOf(game))) return false;
    out.byteExact = session.byteExactStateProof();

    const uint32_t endpointNextRecord = game.end.nextRecord;
    TransportPacingV3 pacing{};
    for (uint32_t slot = 0u; slot != 512u; ++slot) {
        if (session.hasResult()
            || session.nextRecord() == endpointNextRecord) {
            break;
        }
        script(slot, pacing);
        const uint32_t before = session.nextRecord();
        const uint32_t ticks =
            pacing.ticksThisSlot(endpointNextRecord - before);
        if (ticks == 0u) {
            ++out.heldSlots;
            if (!session.transportMayHold()) return false;
            if (session.nextRecord() != before) return false;
            if (session.phase() != ReplayPlaybackPhaseV2::Ready)
                return false;
            if (session.hasResult()) return false;
            continue;
        }
        for (uint32_t i = 0u; i != ticks; ++i) {
            const uint32_t record = session.nextRecord();
            const uint64_t serial = record + 1u == endpointNextRecord
                ? kTransportFinalSerialV3 : 0u;
            if (!session.beginFrame(beginOf(game.ticks[record], serial)))
                return false;
            FrameWitnessV3 witness{};
            witness.replayFrame = session.inFlightReplayFrame();
            witness.laneMaskInFlight = session.latchedActiveLaneMask();
            witness.actionsInFlight = session.latchedActions();
            if (!session.commitFrame(commitOf(game.ticks[record])))
                return false;
            witness.nextRecordAfter = session.nextRecord();
            witness.digestAfter = session.currentRollingDigest();
            out.frames.push_back(witness);
        }
    }
    if (session.acknowledgeFinalPresentation(kTransportFinalSerialV3)
        != ReplayPresentationAckV2::Accepted) {
        return false;
    }
    const wire3::PlaybackResultPayload* result = session.result();
    if (result == nullptr) return false;
    out.reason = result->reason;
    out.deviceCalls = session.deviceCallCount();
    return wire3::encodePlaybackResult(*result, out.encodedResult);
}

bool runStraightPlaybackV3(
    const PlaybackGame& game, PlaybackWitnessV3& out) {
    return runPacedPlaybackV3(
        game, [](uint32_t, TransportPacingV3&) {}, out);
}

}

QNP_TEST(ReplayPlaybackV2PausedSlotsAdvanceNoFrameAndSkipNoProof) {
    PlaybackGame game = completedFourLaneGame();
    QNP_REQUIRE(game.ticks.size() == 3u);

    {
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, false));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[0])));
        QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[0])));

        const uint32_t record = session.nextRecord();
        const uint64_t digest = session.currentRollingDigest();
        const uint64_t deviceCalls = session.deviceCallCount();
        const CanonicalTickBundleV2* offered = session.peek();
        QNP_REQUIRE(offered != nullptr);
        for (uint32_t held = 0u; held != 64u; ++held) {
            QNP_CHECK(session.transportMayHold());
            QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::Ready);
            QNP_CHECK(!session.hasResult());
            QNP_CHECK(session.nextRecord() == record);
            QNP_CHECK(session.currentRollingDigest() == digest);
            QNP_CHECK(session.deviceCallCount() == deviceCalls);
            QNP_CHECK(session.peek() == offered);
            QNP_CHECK(session.latchedActiveLaneMask() == 0u);
        }
        QNP_CHECK(offered->frame.replayFrame
                  == game.ticks[record].frame.replayFrame);
    }

    std::vector<uint8_t> unpausedFailure;
    std::vector<uint8_t> resumedFailure;
    for (int paused = 0; paused < 2; ++paused) {
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, false));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[0])));
        QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[0])));
        if (paused != 0) {
            for (uint32_t held = 0u; held != 16u; ++held)
                QNP_CHECK(session.transportMayHold());
        }
        ReplayFrameBeginObservationV2 wrong = beginOf(game.ticks[1]);
        wrong.activeLaneMask = static_cast<uint8_t>(wrong.activeLaneMask ^ 1u);
        QNP_CHECK(!session.beginFrame(wrong));
        QNP_CHECK(session.fault()
                  == ReplayPlaybackFaultV2::FrameLaneMaskMismatch);
        QNP_CHECK(session.nextRecord() == 1u);
        const wire3::PlaybackResultPayload* result = session.result();
        QNP_REQUIRE(result != nullptr);
        QNP_REQUIRE(wire3::encodePlaybackResult(
            *result, paused != 0 ? resumedFailure : unpausedFailure));
    }
    QNP_CHECK(!unpausedFailure.empty());
    QNP_CHECK(resumedFailure == unpausedFailure);

    PlaybackWitnessV3 straight{};
    QNP_REQUIRE(runStraightPlaybackV3(game, straight));
    QNP_REQUIRE(straight.frames.size() == 3u);

    PlaybackWitnessV3 held{};
    QNP_REQUIRE(runPacedPlaybackV3(
        game,
        [](uint32_t slot, TransportPacingV3& pacing) {
            if (slot == 0u) pacing.pause();
            else if (slot == 6u) pacing.resume();
            else if (slot == 8u) pacing.pause();
            else if (slot == 12u) pacing.resume();
        },
        held));
    QNP_CHECK(held.heldSlots == 10u);
    QNP_CHECK(held.frames == straight.frames);
    QNP_CHECK(held.encodedResult == straight.encodedResult);
    QNP_CHECK(held.deviceCalls == straight.deviceCalls);
    QNP_CHECK(held.reason
              == wire3::PlaybackResultReason::VerifiedCompleted);
}

QNP_TEST(ReplayPlaybackV2FrameStepAdvancesExactlyOneVerifiedFrame) {
    PlaybackGame game = completedFourLaneGame();
    PlaybackWitnessV3 straight{};
    QNP_REQUIRE(runStraightPlaybackV3(game, straight));
    QNP_REQUIRE(straight.frames.size() == 3u);

    PlaybackWitnessV3 stepped{};
    QNP_REQUIRE(runPacedPlaybackV3(
        game,
        [](uint32_t slot, TransportPacingV3& pacing) {
            if (slot == 0u) pacing.pause();
            else if (slot == 2u || slot == 5u || slot == 9u) pacing.step();
        },
        stepped));
    QNP_CHECK(stepped.frames == straight.frames);
    QNP_CHECK(stepped.encodedResult == straight.encodedResult);
    QNP_CHECK(stepped.heldSlots == 7u);

    {
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, false));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));

        TransportPacingV3 pacing{};
        pacing.step();
        pacing.step();
        QNP_CHECK(pacing.pendingSteps == 2u);
        QNP_CHECK(pacing.ticksThisSlot(3u) == 1u);
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[0])));
        QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[0])));
        QNP_CHECK(session.nextRecord() == 1u);
        QNP_CHECK(session.currentRollingDigest()
                  == straight.frames[0].digestAfter);
        QNP_CHECK(pacing.pendingSteps == 1u);
        QNP_CHECK(pacing.ticksThisSlot(2u) == 1u);
        QNP_CHECK(pacing.pendingSteps == 0u);
        QNP_CHECK(pacing.ticksThisSlot(2u) == 0u);

        pacing.step();
        QNP_CHECK(pacing.ticksThisSlot(2u) == 1u);
        QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[1])));
        ReplayFrameCommitObservationV2 wrong = commitOf(game.ticks[1]);
        wrong.postStateHash ^= 1u;
        QNP_CHECK(session.commitFrame(wrong));
        QNP_CHECK(session.divergenceCount() == 1u);
        QNP_CHECK(session.firstDivergenceFrame()
                  == game.ticks[1].frame.replayFrame);
    }

    {
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, false));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        for (size_t i = 0u; i + 1u < game.ticks.size(); ++i) {
            QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[i])));
            QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[i])));
        }
        QNP_CHECK(session.transportMayHold());
        const CanonicalTickBundleV2& last = game.ticks.back();
        QNP_REQUIRE(session.beginFrame(
            beginOf(last, kTransportFinalSerialV3)));
        QNP_REQUIRE(session.commitFrame(commitOf(last)));
        QNP_CHECK(session.phase()
                  == ReplayPlaybackPhaseV2::AwaitingFinalPresentation);
        QNP_CHECK(!session.transportMayHold());
        QNP_CHECK(session.peek() == nullptr);
    }
}

QNP_TEST(ReplayPlaybackV2SpeedChangesAlterNoFrameContentOrderOrDigest) {
    PlaybackGame game = completedFourLaneGame();
    PlaybackWitnessV3 straight{};
    QNP_REQUIRE(runStraightPlaybackV3(game, straight));
    QNP_REQUIRE(straight.frames.size() == 3u);
    for (size_t i = 0u; i < straight.frames.size(); ++i) {
        QNP_CHECK(straight.frames[i].replayFrame
                  == static_cast<uint32_t>(i));
        QNP_CHECK(straight.frames[i].nextRecordAfter
                  == static_cast<uint32_t>(i) + 1u);
    }

    for (const uint32_t rung : wire3::kPlaybackSpeedLadderV3) {
        PlaybackWitnessV3 paced{};
        QNP_REQUIRE(runPacedPlaybackV3(
            game,
            [rung](uint32_t slot, TransportPacingV3& pacing) {
                if (slot == 0u) pacing.setSpeed(rung);
            },
            paced));
        QNP_CHECK(paced.frames == straight.frames);
        QNP_CHECK(paced.encodedResult == straight.encodedResult);
        QNP_CHECK(paced.deviceCalls == straight.deviceCalls);
        QNP_CHECK(paced.byteExact == straight.byteExact);
        QNP_CHECK(paced.reason == straight.reason);
        QNP_CHECK((paced.heldSlots != 0u) == (rung < 1000u));
    }

    PlaybackWitnessV3 shifting{};
    QNP_REQUIRE(runPacedPlaybackV3(
        game,
        [](uint32_t slot, TransportPacingV3& pacing) {
            if (slot == 0u) pacing.setSpeed(250u);
            else if (slot == 2u) pacing.pause();
            else if (slot == 3u) pacing.setSpeed(8000u);
            else if (slot == 5u) pacing.resume();
            else if (slot == 6u) pacing.setSpeed(500u);
        },
        shifting));
    QNP_CHECK(shifting.frames == straight.frames);
    QNP_CHECK(shifting.encodedResult == straight.encodedResult);
}

QNP_TEST(ReplayPlaybackV2TransportIsRefusedOnceTheProofConcluded) {
    PlaybackGame game = completedFourLaneGame();

    {
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, false));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        for (size_t i = 0u; i < game.ticks.size(); ++i) {
            const uint64_t serial = i + 1u == game.ticks.size()
                ? kTransportFinalSerialV3 : 0u;
            QNP_REQUIRE(session.beginFrame(beginOf(game.ticks[i], serial)));
            QNP_REQUIRE(session.commitFrame(commitOf(game.ticks[i])));
        }
        QNP_CHECK(session.acknowledgeFinalPresentation(
            kTransportFinalSerialV3) == ReplayPresentationAckV2::Accepted);
        QNP_REQUIRE(session.phase() == ReplayPlaybackPhaseV2::ResultReady);
        QNP_REQUIRE(session.hasResult());

        std::vector<uint8_t> before;
        QNP_REQUIRE(wire3::encodePlaybackResult(*session.result(), before));

        QNP_CHECK(!session.transportMayHold());
        QNP_CHECK(session.peek() == nullptr);
        QNP_CHECK(!session.beginFrame(beginOf(game.ticks.back())));
        QNP_CHECK(!session.beginFrame(beginOf(game.ticks.front())));
        QNP_CHECK(session.nextRecord() == game.end.nextRecord);

        QNP_CHECK(session.acknowledgeResultDelivery());
        QNP_CHECK(session.exitState()
                  == ReplayPlaybackExitStateV2::ResultAcknowledged);
        QNP_CHECK(!session.transportMayHold());
        QNP_CHECK(session.requestGracefulExit());
        QNP_CHECK(session.exitState()
                  == ReplayPlaybackExitStateV2::ExitRequested);
        QNP_CHECK(!session.transportMayHold());

        std::vector<uint8_t> after;
        QNP_REQUIRE(wire3::encodePlaybackResult(*session.result(), after));
        QNP_CHECK(after == before);
    }

    {
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, false));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        QNP_CHECK(session.transportMayHold());
        QNP_CHECK(session.requestUserExit());
        QNP_CHECK(session.hasResult());
        QNP_CHECK(!session.transportMayHold());
        QNP_CHECK(session.peek() == nullptr);
    }
    {
        ConfirmedTimelineBufferV2 timeline(game.ticks.size());
        QNP_REQUIRE(fillTimeline(timeline, game));
        ReplayPlaybackSessionV2 session(
            std::move(timeline), bindingFor(game, false));
        QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
        QNP_CHECK(session.reportBrokerFailure());
        QNP_CHECK(session.phase() == ReplayPlaybackPhaseV2::Faulted);
        QNP_CHECK(!session.transportMayHold());
        QNP_CHECK(session.peek() == nullptr);
    }
}

QNP_TEST(ReplayPlaybackV2ByteExactProofIsBitIdenticalUnderTransport) {
    PlaybackGame game = completedFourLaneGame();
    QNP_REQUIRE(game.start.manifest.identity.gameGeneration == 1u);

    PlaybackWitnessV3 straight{};
    QNP_REQUIRE(runStraightPlaybackV3(game, straight));
    QNP_CHECK(straight.byteExact);
    QNP_CHECK(straight.heldSlots == 0u);
    QNP_REQUIRE(straight.frames.size() == 3u);
    QNP_CHECK(straight.reason
              == wire3::PlaybackResultReason::VerifiedCompleted);

    {
        FrameWitnessV3 tampered = straight.frames[0];
        tampered.digestAfter ^= 1u;
        QNP_CHECK(!(tampered == straight.frames[0]));
        tampered = straight.frames[0];
        tampered.actionsInFlight[2] ^= 1u;
        QNP_CHECK(!(tampered == straight.frames[0]));
        tampered = straight.frames[0];
        tampered.laneMaskInFlight ^= 1u;
        QNP_CHECK(!(tampered == straight.frames[0]));
    }

    PlaybackWitnessV3 hostile{};
    QNP_REQUIRE(runPacedPlaybackV3(
        game,
        [](uint32_t slot, TransportPacingV3& pacing) {
            switch (slot) {
                case 0u: pacing.pause(); break;
                case 3u: pacing.step(); break;
                case 4u: pacing.setSpeed(8000u); break;
                case 6u: pacing.step(); break;
                case 7u: pacing.setSpeed(250u); break;
                case 8u: pacing.resume(); break;
                case 14u: pacing.setSpeed(2000u); break;
                case 16u: pacing.pause(); break;
                case 20u: pacing.resume(); break;
                case 21u: pacing.setSpeed(8000u); break;
                default: break;
            }
        },
        hostile));
    QNP_CHECK(hostile.heldSlots == 6u);
    QNP_CHECK(hostile.frames.size() == 3u);
    QNP_CHECK(hostile.byteExact);
    QNP_CHECK(hostile.frames.size() == straight.frames.size());
    QNP_CHECK(hostile.frames == straight.frames);
    QNP_CHECK(hostile.encodedResult == straight.encodedResult);
    QNP_CHECK(hostile.deviceCalls == straight.deviceCalls);
    QNP_CHECK(hostile.reason == straight.reason);

    PlaybackGame rematch = completedFourLaneGame();
    rematch.start.manifest.identity.gameGeneration = 2u;
    PlaybackWitnessV3 rematchStraight{};
    QNP_REQUIRE(runStraightPlaybackV3(rematch, rematchStraight));
    QNP_CHECK(!rematchStraight.byteExact);
    QNP_CHECK(rematchStraight.reason
              == wire3::PlaybackResultReason::VerifiedBehaviourOnly);
    PlaybackWitnessV3 rematchPaced{};
    QNP_REQUIRE(runPacedPlaybackV3(
        rematch,
        [](uint32_t slot, TransportPacingV3& pacing) {
            if (slot == 0u) pacing.pause();
            else if (slot == 4u) pacing.step();
            else if (slot == 7u) pacing.resume();
            else if (slot == 8u) pacing.setSpeed(250u);
        },
        rematchPaced));
    QNP_CHECK(rematchPaced.frames == rematchStraight.frames);
    QNP_CHECK(rematchPaced.encodedResult == rematchStraight.encodedResult);
}

QNP_TEST(ReplayPlaybackV2EveryResultReasonSurvivesTheWire) {
    PlaybackGame game = completedFourLaneGame();
    ConfirmedTimelineBufferV2 timeline(game.ticks.size());
    QNP_REQUIRE(fillTimeline(timeline, game));
    ReplayPlaybackSessionV2 session(
        std::move(timeline), bindingFor(game, true));
    QNP_REQUIRE(session.completeBootstrap(bootstrapOf(game)));
    for (const auto& tick : game.ticks) {
        QNP_REQUIRE(session.beginFrame(beginOf(tick)));
        QNP_REQUIRE(session.commitFrame(commitOf(tick)));
    }
    const wire3::PlaybackResultPayload* produced = session.result();
    QNP_REQUIRE(produced != nullptr);

    std::vector<uint8_t> baseline;
    QNP_REQUIRE(wire3::encodePlaybackResult(*produced, baseline));

    wire3::PlaybackResultPayload diverged = *produced;
    diverged.reason =
        wire3::PlaybackResultReason::CompletedWithDivergence;
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(wire3::encodePlaybackResult(diverged, encoded));
    wire3::PlaybackResultPayload decoded{};
    QNP_REQUIRE(wire3::decodePlaybackResult(
        encoded.data(), encoded.size(), decoded));
    QNP_CHECK(decoded.reason
              == wire3::PlaybackResultReason::CompletedWithDivergence);

    wire3::PlaybackResultPayload unknown = *produced;
    unknown.reason = static_cast<wire3::PlaybackResultReason>(17u);
    std::vector<uint8_t> rejected;
    QNP_CHECK(!wire3::encodePlaybackResult(unknown, rejected));

    for (uint16_t flags : std::array<uint16_t, 5>{{ 0u, 4u, 8u, 16u, 28u }}) {
        wire3::PlaybackResultPayload choices = *produced;
        choices.presentationFlags = flags;
        encoded.clear();
        QNP_REQUIRE(wire3::encodePlaybackResult(choices, encoded));
        QNP_REQUIRE(wire3::decodePlaybackResult(
            encoded.data(), encoded.size(), decoded));
        QNP_CHECK(decoded.presentationFlags == flags);
        encoded[274u] |= 1u;
        QNP_CHECK(!wire3::decodePlaybackResult(
            encoded.data(), encoded.size(), decoded));
    }
    for (uint16_t flags : std::array<uint16_t, 4>{{ 1u, 2u, 32u, 0x8000u }}) {
        wire3::PlaybackResultPayload choices = *produced;
        choices.presentationFlags = flags;
        rejected.clear();
        QNP_CHECK(!wire3::encodePlaybackResult(choices, rejected));
    }
}

QNP_TEST(ReplayNavigationStartupPresentsOneFrameBeforePausing) {
    ReplayNavigation navigation;
    navigation.reset(false);
    QNP_CHECK(navigation.active);
    QNP_CHECK(!navigation.restorePending);
    const auto first = navigation.plan(0u);
    QNP_CHECK(first.hiddenTicks == 0u);
    QNP_CHECK(first.present);
    QNP_CHECK(!navigation.committed(0u));
    QNP_CHECK(navigation.committed(1u));
    QNP_CHECK(!navigation.active);
    QNP_CHECK(!navigation.plan(1u).present);
    QNP_CHECK(navigation.plan(1u).hiddenTicks == 0u);

    navigation.reset(true);
    QNP_CHECK(!navigation.active);
    QNP_CHECK(!navigation.plan(0u).present);
}

QNP_TEST(ReplayNavigationSeeksInBoundedBatchesAndPresentsOnlyTheTarget) {
    ReplayNavigation navigation;
    QNP_REQUIRE(navigation.request(100u, 200u));
    navigation.restored();
    uint32_t nextRecord = 0u;
    for (unsigned batch = 0u; batch < 6u; ++batch) {
        const auto slot = navigation.plan(nextRecord);
        QNP_CHECK(slot.hiddenTicks == 16u);
        QNP_CHECK(!slot.present);
        nextRecord += slot.hiddenTicks;
        QNP_CHECK(!navigation.committed(nextRecord));
    }
    const auto last = navigation.plan(nextRecord);
    QNP_CHECK(last.hiddenTicks == 4u);
    QNP_CHECK(last.present);
    QNP_CHECK(navigation.committed(101u));
    QNP_CHECK(!navigation.committed(101u));

    QNP_REQUIRE(navigation.request(16u, 17u));
    navigation.restored();
    QNP_CHECK(navigation.plan(0u).hiddenTicks == 16u);
    QNP_CHECK(!navigation.plan(0u).present);
    QNP_CHECK(!navigation.committed(16u));
    QNP_CHECK(navigation.plan(16u).hiddenTicks == 0u);
    QNP_CHECK(navigation.plan(16u).present);
    QNP_CHECK(navigation.committed(17u));
}

QNP_TEST(ReplayNavigationRestartAndStepBackClampAtTheBeginning) {
    QNP_CHECK(ReplayNavigation::previousFrame(0u) == 0u);
    QNP_CHECK(ReplayNavigation::previousFrame(1u) == 0u);
    QNP_CHECK(ReplayNavigation::previousFrame(2u) == 0u);
    QNP_CHECK(ReplayNavigation::previousFrame(3u) == 1u);
    QNP_CHECK(ReplayNavigation::previousFrame(40u) == 38u);

    ReplayNavigation navigation;
    QNP_REQUIRE(navigation.request(38u, 50u));
    navigation.restored();
    QNP_REQUIRE(navigation.request(0u, 50u));
    QNP_CHECK(navigation.target == 0u);
    QNP_CHECK(navigation.restorePending);
    navigation.restored();
    QNP_CHECK(navigation.plan(0u).hiddenTicks == 0u);
    QNP_CHECK(navigation.plan(0u).present);
    QNP_CHECK(navigation.committed(1u));
}

QNP_TEST(ReplayNavigationRefusesInvalidEndpointsWithoutChangingTheRequest) {
    ReplayNavigation navigation;
    QNP_CHECK(!navigation.request(0u, 0u));
    QNP_CHECK(!navigation.active);
    QNP_REQUIRE(navigation.request(3u, 4u));
    QNP_CHECK(!navigation.request(4u, 4u));
    QNP_CHECK(!navigation.request(UINT32_MAX, 4u));
    QNP_CHECK(navigation.active);
    QNP_CHECK(navigation.restorePending);
    QNP_CHECK(navigation.target == 3u);
    navigation.restored();
    QNP_CHECK(navigation.plan(4u).hiddenTicks == 0u);
    QNP_CHECK(!navigation.plan(4u).present);

    QNP_REQUIRE(navigation.request(UINT32_MAX - 1u, UINT32_MAX));
    navigation.restored();
    QNP_CHECK(navigation.plan(0u).hiddenTicks == 16u);
    QNP_CHECK(!navigation.plan(0u).present);
}

QNP_TEST(ReplayNavigationCannotTickOrFinishBeforeRestore) {
    ReplayNavigation navigation;
    QNP_REQUIRE(navigation.request(2u, 3u));
    QNP_CHECK(navigation.plan(0u).hiddenTicks == 0u);
    QNP_CHECK(!navigation.plan(0u).present);
    QNP_CHECK(!navigation.committed(3u));
    QNP_CHECK(navigation.active);
    navigation.restored();
    QNP_CHECK(!navigation.restorePending);
    QNP_CHECK(navigation.plan(0u).hiddenTicks == 2u);
    QNP_CHECK(navigation.plan(0u).present);
    navigation.reset(false);
    QNP_CHECK(navigation.target == 0u);
    QNP_CHECK(!navigation.restorePending);
}

QNP_TEST(ReplayWinScreenFinishesTheBannerWithoutAdvancingOtherScenes) {
    using Action = ReplayWinScreenAction;
    QNP_CHECK(replayWinScreenAction(2u, 2, 1u, 0u, false, 0u) == Action::Advance);
    QNP_CHECK(replayWinScreenAction(2u, 2, 2u, 60u, true, 599u) == Action::Advance);
    QNP_CHECK(replayWinScreenAction(2u, 2, 1u, 60u, false, 600u) == Action::Fail);
    QNP_CHECK(replayWinScreenAction(2u, 2, 2u, 61u, false, 600u) == Action::Fail);
    QNP_CHECK(replayWinScreenAction(2u, 2, 0u, 0u, false, 0u) == Action::Fail);
    QNP_CHECK(replayWinScreenAction(2u, 2, 3u, 61u, true, 0u) == Action::Fail);
    QNP_CHECK(replayWinScreenAction(1u, 2, 1u, 61u, true, 0u) == Action::Fail);
    QNP_CHECK(replayWinScreenAction(3u, 2, 1u, 61u, true, 0u) == Action::Fail);
    QNP_CHECK(replayWinScreenAction(2u, -1, 1u, 0u, false, 0u) == Action::Fail);
    QNP_CHECK(replayWinScreenAction(2u, 1, 1u, 0u, false, 0u) == Action::Fail);
    QNP_CHECK(replayWinScreenAction(2u, 3, 1u, 61u, true, 0u) == Action::Fail);
    QNP_CHECK(replayWinScreenAction(2u, 5, 0u, 61u, true, 0u) == Action::Fail);
    QNP_CHECK(replayWinScreenAction(2u, 5, 1u, 0u, false, 0u) == Action::Hold);
    QNP_CHECK(replayWinScreenAction(2u, 5, 2u, 0u, false, 600u) == Action::Hold);
    QNP_CHECK(replayWinScreenAction(2u, 2, 1u, 61u, false, 61u) == Action::Advance);
    QNP_CHECK(replayWinScreenAction(2u, 2, 1u, 61u, true, 61u) == Action::Hold);
    QNP_CHECK(replayWinScreenAction(2u, 2, 2u, 61u, true, 600u) == Action::Hold);
}

QNP_TEST(ReplayCompletedGameAllowsDivergenceWithoutTreatingCancellationAsCompletion) {
    wire3::PlaybackResultPayload result{};
    result.hasRecordedTerminal = true;
    result.recordedTerminal = static_cast<uint16_t>(TerminalReason::Completed);
    for (auto reason : {wire3::PlaybackResultReason::VerifiedCompleted,
                        wire3::PlaybackResultReason::VerifiedBehaviourOnly,
                        wire3::PlaybackResultReason::CompletedWithDivergence}) {
        result.reason = reason;
        QNP_CHECK(replayCompletedGame(result));
        result.hasRecordedTerminal = false;
        QNP_CHECK(!replayCompletedGame(result));
        result.hasRecordedTerminal = true;
    }
    result.recordedTerminal = static_cast<uint16_t>(TerminalReason::ProcessExit);
    QNP_CHECK(!replayCompletedGame(result));
    result.recordedTerminal = static_cast<uint16_t>(TerminalReason::Completed);
    for (auto reason : {wire3::PlaybackResultReason::UserExit,
                        wire3::PlaybackResultReason::BrokerFailure,
                        wire3::PlaybackResultReason::InternalFailure}) {
        result.reason = reason;
        QNP_CHECK(!replayCompletedGame(result));
    }
}

QNP_TEST(ReplayWinScreenFinalHoldUsesOneSecondOfElapsedTime) {
    QNP_CHECK(!replayFinalHoldElapsed(5000u, 5000u, true));
    QNP_CHECK(!replayFinalHoldElapsed(5999u, 5000u, true));
    QNP_CHECK(replayFinalHoldElapsed(6000u, 5000u, true));
    QNP_CHECK(replayFinalHoldElapsed(6001u, 5000u, true));
    QNP_CHECK(replayFinalHoldElapsed(5000u, 5000u, false));
    QNP_CHECK(!replayFinalHoldElapsed(4999u, 5000u, true));
}

QNP_TEST(ReplayHitboxesHideOnTheNativeResultTickAndReappearInTheNextCombat) {
    QNP_CHECK(replayHitboxPresentationAllowed(2u, 0));
    QNP_CHECK(!replayHitboxPresentationAllowed(2u, 1));
    QNP_CHECK(!replayHitboxPresentationAllowed(2u, 2));
    QNP_CHECK(!replayHitboxPresentationAllowed(2u, 5));
    QNP_CHECK(!replayHitboxPresentationAllowed(1u, 0));
    QNP_CHECK(!replayHitboxPresentationAllowed(3u, 0));
    QNP_CHECK(!replayHitboxPresentationAllowed(2u, -1));
    QNP_CHECK(replayHitboxPresentationAllowed(2u, 0));
}
