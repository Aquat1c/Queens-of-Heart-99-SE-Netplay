#include "TestHarness.h"

#include "replay/GameEpochTracker.h"
#include "replay/ReplayCodec.h"
#include "replay/ReplayRecordingCoordinator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace qreplay;

struct CapturingPublisher final : ReplayRecordPublisherV2 {
    bool failNextOrdinary = false;
    bool failReserved = false;
    std::vector<PublisherRecordV2> records;

    bool tryPublishOrdinary(
        const PublisherRecordV2& record) noexcept override {
        if (failNextOrdinary) {
            failNextOrdinary = false;
            return false;
        }
        records.push_back(record);
        return true;
    }

    bool publishEndGameReserved(
        const PublisherRecordV2& record) noexcept override {
        if (failReserved) return false;
        records.push_back(record);
        return true;
    }
};

StageIdentityV2 appliedStage() {
    StageIdentityV2 stage{};
    stage.nativeStageId = 4u;
    stage.verified = true;
    stage.catalogEntryId = 0x53544147452D3034ull;
    return stage;
}

void fillContract(SimulationContractV2& contract) {
    for (size_t i = 0u; i < contract.executableSha256.size(); ++i)
        contract.executableSha256[i] =
            static_cast<uint8_t>(i + 1u);
    for (size_t i = 0u; i < contract.contentManifestRoot.size(); ++i)
        contract.contentManifestRoot[i] =
            static_cast<uint8_t>(0x80u + i);
    contract.nativeBuildIdentity = 0x0102030405060708ull;
    contract.runtimeSimulationId = 0x1112131415161718ull;
    contract.stateRegionSchemaFingerprint =
        0x2122232425262728ull;
    contract.stateHashProjectionId = 0x3132333435363738ull;
    contract.stateHashAlgorithmId = 0x4142434445464748ull;
    contract.bootstrapSchemaId = 0x5152535455565758ull;
    contract.eventSchemaId = 0x6162636465666768ull;
    contract.characterCatalogId = 0x7172737475767778ull;
    contract.terminalResultSchemaId =
        0x8182838485868788ull;
    contract.compactActionDomainVersion = 1u;
}

FreshGameDescriptorV2 descriptor(
    const uint32_t generation = 1u) {
    FreshGameDescriptorV2 fresh{};
    fresh.gameEpoch = generation;
    fresh.launchSourceFrame = 90u;

    ConfirmedGameStartV2& start = fresh.start;
    start.manifest.identity.gameUuidLow =
        0xAAAABBBBCCCC0000ull + generation;
    start.manifest.identity.gameUuidHigh =
        0x1111222233330000ull + generation;
    start.manifest.identity.gameGeneration = generation;
    fillContract(start.manifest.contract);
    start.manifest.vsRow = 1u;
    start.manifest.activeLaneMask = 0x03u;
    start.manifest.activeFighterMask = 0x03u;
    start.manifest.cpuMask = 0u;
    start.manifest.cpuContract = CpuContract::None;
    start.manifest.slotToSourceRoute = {{0u, 1u, 2u, 3u}};
    start.manifest.lanes[0].active = true;
    start.manifest.lanes[1].active = true;
    (void)setReplayTextV2(
        start.manifest.lanes[0].name, "Local");
    (void)setReplayTextV2(
        start.manifest.lanes[1].name, "Remote");
    start.manifest.slots[0].ownership =
        SlotOwnership::RecordedHuman;
    start.manifest.slots[0].side = 0u;
    start.manifest.slots[0].identity.nativeCharacterId = 1u;
    start.manifest.slots[0].identity.paletteIndex = 2u;
    start.manifest.slots[1].ownership =
        SlotOwnership::RecordedHuman;
    start.manifest.slots[1].side = 1u;
    start.manifest.slots[1].identity.nativeCharacterId = 8u;
    start.manifest.slots[1].identity.paletteIndex = 3u;
    start.manifest.slots[2].side = 0u;
    start.manifest.slots[3].side = 1u;
    start.manifest.routeEpoch = 10u;
    start.manifest.rosterEpoch = 11u;
    start.manifest.layoutGeneration = 12u;
    start.manifest.stage = StageIdentityV2{};
    start.manifest.display.sourceKind =
        ReplaySourceKindV2::NetplayPlayer;

    start.start.configuration.timerMode = 2u;
    start.start.configuration.roundsToWinMinusOne = 1u;
    start.start.rng.gameplay = 0x12345678u;
    start.start.rng.cosmetic = 0x23456789u;
    start.start.rng.selector = 0x3456789Au;
    start.start.fpu.x87ControlWord = 0x027Fu;
    start.start.fpu.mxcsr = 0x1F80u;
    start.start.bootstrapFingerprint =
        0xB00757A900000001ull;
    start.start.frameZeroPreStateHash = 0u;
    start.start.initialRollingDigest = 0u;
    return fresh;
}

GameLifecycleV2 lifecycle(
    const FreshGameDescriptorV2& fresh, const int32_t matchSub,
    const std::array<uint8_t, 2>& tallies = {},
    const int32_t winner = -1) {
    GameLifecycleV2 state{};
    state.scene = 2u;
    state.requestedScene = 2u;
    state.matchSub = matchSub;
    state.winnerSlot = winner;
    state.roundTallies = tallies;
    state.routeEpoch = fresh.start.manifest.routeEpoch;
    state.rosterEpoch = fresh.start.manifest.rosterEpoch;
    state.layoutGeneration =
        fresh.start.manifest.layoutGeneration;
    return state;
}

RoundTransitionProofV2 proofFor(
    const GameLifecycleV2& before,
    const GameLifecycleV2& after) {
    RoundTransitionProofV2 proof{};
    proof.talliesBefore = before.roundTallies;
    proof.talliesAfter = after.roundTallies;
    proof.roundStateBefore = before.roundState;
    proof.roundStateAfter = after.roundState;
    proof.roundCounterBefore = before.roundCounter;
    proof.roundCounterAfter = after.roundCounter;
    proof.resultCounterBefore = before.resultCounter;
    proof.resultCounterAfter = after.resultCounter;
    return proof;
}

ProvenGameFrameV2 frame(
    const FreshGameDescriptorV2& fresh,
    const uint32_t sourceFrame,
    const GameLifecycleV2& before,
    const GameLifecycleV2& after,
    const uint64_t preHash, const uint64_t postHash,
    const StageIdentityV2& stage = appliedStage()) {
    ProvenGameFrameV2 out{};
    out.sourceFrame = sourceFrame;
    out.gameEpoch = fresh.gameEpoch;
    out.game = fresh.start.manifest.identity;
    out.core.inputDomain = InputDomain::LogicalSourceV1;
    out.core.activeLaneMask = 0x03u;
    out.core.before = before;
    out.core.after = after;
    out.core.dispatcherResult = 1;
    out.core.preStateHash = preHash;
    out.core.postStateHash = postHash;
    out.roundTransition = proofFor(before, after);
    out.activeFighterMask = 0x03u;
    for (size_t i = 0u; i < kFighterSlots; ++i)
        out.roster[i] = fresh.start.manifest.slots[i].identity;
    out.stage = stage;
    return out;
}

void markRoundActive(
    ProvenGameFrameV2& source, const uint8_t ordinal,
    const uint32_t site) {
    source.roundTransition.flags = RoundBecameActiveV2;
    source.roundTransition.transitionSiteId = site;
    source.roundTransition.roundOrdinal = ordinal;
}

void markRoundOutcome(
    ProvenGameFrameV2& source, const uint8_t ordinal,
    const uint32_t site, const int32_t winner) {
    source.roundTransition.flags = RoundOutcomeCommittedV2;
    source.roundTransition.transitionSiteId = site;
    source.roundTransition.roundOrdinal = ordinal;
    source.roundTransition.winnerSlot = winner;
    source.roundTransition.winningSideMask =
        static_cast<uint8_t>(1u << static_cast<uint32_t>(winner));
}

void markFinalResult(
    ProvenGameFrameV2& source, const uint8_t ordinal,
    const uint32_t site, const int32_t winner) {
    source.roundTransition.flags = FinalGameResultCommittedV2;
    source.roundTransition.transitionSiteId = site;
    source.roundTransition.roundOrdinal = ordinal;
    source.roundTransition.winnerSlot = winner;
    source.roundTransition.winningSideMask =
        static_cast<uint8_t>(1u << static_cast<uint32_t>(winner));
}

struct ArmedCapture {
    FreshGameDescriptorV2 fresh;
    GameLifecycleV2 active;
    uint64_t tailHash = 0u;
    uint32_t nextSource = 0u;
};

ProvenGameFrameV2 chainedFrame(
    ArmedCapture& capture, const GameLifecycleV2& before,
    const GameLifecycleV2& after) {
    const uint64_t preHash = capture.tailHash;
    ++capture.tailHash;
    return frame(
        capture.fresh, capture.nextSource++, before, after,
        preHash, capture.tailHash);
}

ArmedCapture publishFrameZero(
    ReplayRecordingCoordinatorV2& recorder,
    CapturingPublisher& publisher,
    const uint32_t generation = 1u) {
    const size_t recordsBefore = publisher.records.size();
    ArmedCapture capture{};
    capture.fresh = descriptor(generation);
    QNP_CHECK(
        recorder.armFreshGame(capture.fresh).code
        == ReplayPublishCodeV2::Armed);

    GameLifecycleV2 setupBefore =
        lifecycle(capture.fresh, -1);
    GameLifecycleV2 setupAfter =
        lifecycle(capture.fresh, 0);
    ProvenGameFrameV2 setup = frame(
        capture.fresh, 100u, setupBefore, setupAfter,
        0x1000u, 0x2000u, StageIdentityV2{});
    markRoundActive(setup, 1u, 0x101u);
    QNP_CHECK(
        recorder.acceptConfirmedFrame(setup).code
        == ReplayPublishCodeV2::Armed);

    capture.active = setupAfter;
    ProvenGameFrameV2 zero = frame(
        capture.fresh, 101u, capture.active, capture.active,
        0x2800u, 0x3000u);
    zero.core.actions[0] = 0x0101u;
    zero.core.actions[1] = 0x0802u;
    QNP_CHECK(
        recorder.acceptConfirmedFrame(zero).code
        == ReplayPublishCodeV2::Published);
    QNP_CHECK(
        publisher.records.size() == recordsBefore + 1u);
    capture.tailHash = 0x3000u;
    capture.nextSource = 102u;
    return capture;
}

void publishThroughFinalTransition(
    ReplayRecordingCoordinatorV2& recorder,
    ArmedCapture& capture) {
    const std::array<uint8_t, 2> oneZero{{1u, 0u}};
    const std::array<uint8_t, 2> twoZero{{2u, 0u}};

    GameLifecycleV2 firstOutcome =
        lifecycle(capture.fresh, 0, oneZero, -1);
    ProvenGameFrameV2 roundOneEnd =
        chainedFrame(capture, capture.active, firstOutcome);
    markRoundOutcome(roundOneEnd, 1u, 0x201u, 0);
    QNP_CHECK(
        recorder.acceptConfirmedFrame(roundOneEnd).code
        == ReplayPublishCodeV2::Published);

    GameLifecycleV2 nextSetup =
        lifecycle(capture.fresh, 0, oneZero, -1);
    nextSetup.roundCounter = 1u;
    ProvenGameFrameV2 outro =
        chainedFrame(capture, firstOutcome, nextSetup);
    markRoundActive(outro, 2u, 0x102u);
    QNP_CHECK(
        recorder.acceptConfirmedFrame(outro).code
        == ReplayPublishCodeV2::Published);

    GameLifecycleV2 secondActive =
        lifecycle(capture.fresh, 0, oneZero, -1);
    secondActive.roundCounter = 1u;
    ProvenGameFrameV2 setupTwo =
        chainedFrame(capture, nextSetup, secondActive);
    QNP_CHECK(
        recorder.acceptConfirmedFrame(setupTwo).code
        == ReplayPublishCodeV2::Published);

    ProvenGameFrameV2 firstRoundTwoFrame =
        chainedFrame(capture, secondActive, secondActive);
    firstRoundTwoFrame.core.actions[0] = 0x0001u;
    QNP_CHECK(
        recorder.acceptConfirmedFrame(firstRoundTwoFrame).code
        == ReplayPublishCodeV2::Published);

    GameLifecycleV2 secondOutcome =
        lifecycle(capture.fresh, 0, twoZero, -1);
    secondOutcome.roundCounter = 1u;
    ProvenGameFrameV2 roundTwoEnd =
        chainedFrame(capture, secondActive, secondOutcome);
    markRoundOutcome(roundTwoEnd, 2u, 0x202u, 0);
    QNP_CHECK(
        recorder.acceptConfirmedFrame(roundTwoEnd).code
        == ReplayPublishCodeV2::Published);

    GameLifecycleV2 resultBefore =
        lifecycle(capture.fresh, 1, twoZero, -1);
    resultBefore.roundCounter = 4u;
    ProvenGameFrameV2 resultIntro =
        chainedFrame(capture, secondOutcome, resultBefore);
    QNP_CHECK(
        recorder.acceptConfirmedFrame(resultIntro).code
        == ReplayPublishCodeV2::Published);

    GameLifecycleV2 finalAfter =
        lifecycle(capture.fresh, 2, twoZero, 0);
    finalAfter.roundCounter = 5u;
    finalAfter.resultCounter = 0u;
    ProvenGameFrameV2 finalTransition =
        chainedFrame(capture, resultBefore, finalAfter);
    markFinalResult(finalTransition, 2u, 0x301u, 0);
    QNP_CHECK(
        recorder.acceptConfirmedFrame(finalTransition).code
        == ReplayPublishCodeV2::Published);
    QNP_CHECK(
        recorder.state()
        == ReplayGameRecordStateV2::AwaitFirstWinScreenTick);
    capture.active = finalAfter;
}

}

QNP_TEST(GameEpochTrackerRequiresFreshLaunchBarrier) {
    FreshGameDescriptorV2 seed = descriptor(3u);
    QNP_CHECK(!isValidGameManifestV2(seed.start.manifest));
    QNP_CHECK(isValidStagedFreshGameStartV2(seed.start));
    GameEpochTrackerV2 tracker;
    tracker.reset();
    QNP_CHECK(
        tracker.stageLaunchBarrier(50u, seed.start)
        == GameEpochTrackerResultV2::LaunchStaged);

    FreshGameDescriptorV2 armed{};
    GameLifecycleV2 sceneThree{};
    sceneThree.scene = 3u;
    QNP_CHECK(
        tracker.observeBeforeFrame(51u, sceneThree, armed)
        == GameEpochTrackerResultV2::Ignored);
    GameLifecycleV2 initial{};
    initial.scene = 2u;
    initial.matchSub = -1;
    QNP_CHECK(
        tracker.observeBeforeFrame(52u, initial, armed)
        == GameEpochTrackerResultV2::EpochArmed);
    QNP_CHECK(armed.gameEpoch == 3u);
    QNP_CHECK(armed.launchSourceFrame == 50u);

    FreshGameDescriptorV2 untouched{};
    QNP_CHECK(
        tracker.observeBeforeFrame(80u, initial, untouched)
        == GameEpochTrackerResultV2::Ignored);
    QNP_CHECK(untouched.gameEpoch == 0u);
}

QNP_TEST(ReplayCoordinatorBuildsFrameZeroWithAppliedStage) {
    CapturingPublisher publisher;
    ReplayRecordingCoordinatorV2 recorder(publisher);
    recorder.reset(true);
    (void)publishFrameZero(recorder, publisher);

    QNP_REQUIRE(publisher.records.size() == 1u);
    const PublisherRecordV2& begin = publisher.records[0];
    QNP_CHECK(begin.kind == PublisherRecordKindV2::BeginGame);
    QNP_CHECK(begin.begin.start.manifest.stage == appliedStage());
    QNP_CHECK(
        begin.begin.start.start.frameZeroPreStateHash == 0x2800u);
    QNP_CHECK(
        begin.begin.start.start.initialRollingDigest != 0u);
    QNP_CHECK(begin.begin.frameZero.frame.replayFrame == 0u);
    QNP_CHECK(begin.begin.frameZero.frame.sourceFrame == 101u);
    QNP_CHECK(
        begin.begin.frameZero.frame.core.actions[0] == 0x0101u);
    QNP_CHECK(
        begin.begin.frameZero.frame.core.actions[1] == 0x0802u);
    QNP_CHECK(begin.begin.frameZero.preEvents.count == 2u);
    QNP_CHECK(
        begin.begin.frameZero.preEvents.events[0].kind
        == GameEventKindV2::GameStarted);
    QNP_CHECK(
        begin.begin.frameZero.preEvents.events[1].kind
        == GameEventKindV2::RoundStarted);
}

QNP_TEST(ReplayCoordinatorPublishesOneCompleteGameAndCommitAck) {
    CapturingPublisher publisher;
    ReplayRecordingCoordinatorV2 recorder(publisher);
    recorder.reset(true);
    ArmedCapture capture =
        publishFrameZero(recorder, publisher, 4u);
    publishThroughFinalTransition(recorder, capture);

    GameLifecycleV2 winscreenAfter = capture.active;
    winscreenAfter.resultCounter = 1u;
    ProvenGameFrameV2 winscreen = frame(
        capture.fresh, capture.nextSource,
        capture.active, winscreenAfter,
        capture.tailHash, capture.tailHash + 1u);
    winscreen.core.actions[1] = 0x0002u;
    QNP_CHECK(
        recorder.acceptConfirmedFrame(winscreen).code
        == ReplayPublishCodeV2::TerminalPublished);
    QNP_CHECK(
        recorder.state()
        == ReplayGameRecordStateV2::WaitingForFreshGame);
    QNP_CHECK(recorder.pendingCommitCount() == 1u);

    QNP_REQUIRE(!publisher.records.empty());
    const PublisherRecordV2& terminal =
        publisher.records.back();
    QNP_CHECK(terminal.kind == PublisherRecordKindV2::EndGame);
    QNP_CHECK(terminal.end.containsFinalFrame);
    QNP_CHECK(
        terminal.end.finalFrame.frame.sourceFrame
        == capture.nextSource);
    QNP_CHECK(
        terminal.end.finalFrame.frame.core.actions[1] == 0x0002u);
    QNP_CHECK(terminal.end.finalFrame.postEvents.count == 1u);
    QNP_CHECK(
        terminal.end.finalFrame.postEvents.events[0].kind
        == GameEventKindV2::GameResult);
    QNP_CHECK(
        terminal.end.end.reason == TerminalReason::Completed);
    QNP_CHECK(terminal.end.end.nextRecord == 9u);

    bool sawRoundTwoStart = false;
    size_t roundEnds = 0u;
    for (const PublisherRecordV2& record : publisher.records) {
        const CanonicalTickBundleV2* tick = nullptr;
        if (record.kind == PublisherRecordKindV2::BeginGame)
            tick = &record.begin.frameZero;
        else if (record.kind == PublisherRecordKindV2::Tick)
            tick = &record.tick;
        else if (record.end.containsFinalFrame)
            tick = &record.end.finalFrame;
        if (tick == nullptr) continue;
        for (size_t i = 0u; i < tick->preEvents.count; ++i) {
            const ConfirmedGameEventV2& event =
                tick->preEvents.events[i];
            if (event.kind == GameEventKindV2::RoundStarted
                && event.roundOrdinal == 2u) {
                sawRoundTwoStart = true;
                QNP_CHECK(
                    event.replayFrame
                    == tick->frame.replayFrame);
            }
        }
        for (size_t i = 0u; i < tick->postEvents.count; ++i)
            if (tick->postEvents.events[i].kind
                == GameEventKindV2::RoundEnded) {
                ++roundEnds;
            }
    }
    QNP_CHECK(sawRoundTwoStart);
    QNP_CHECK(roundEnds == 2u);

    GameCommitAckV2 ack{};
    ack.identity = capture.fresh.start.manifest.identity;
    ack.status = GameCommitStatusV2::Committed;
    ack.nextRecord = terminal.end.end.nextRecord;
    ack.canonicalGameDigest =
        terminal.end.end.canonicalGameDigest;
    ack.fileDigest[0] = 0xA5u;
    QNP_CHECK(
        recorder.onCasterCommitAck(ack)
        == CommitAckResultV2::Applied);
    QNP_CHECK(recorder.pendingCommitCount() == 0u);
    QNP_CHECK(
        recorder.onCasterCommitAck(ack)
        == CommitAckResultV2::ExactDuplicate);
    ack.fileDigest[0] ^= 0xFFu;
    QNP_CHECK(
        recorder.onCasterCommitAck(ack)
        == CommitAckResultV2::Conflict);
}

QNP_TEST(ReplayCoordinatorWrongWinScreenSealsPriorTailAsFailure) {
    CapturingPublisher publisher;
    ReplayRecordingCoordinatorV2 recorder(publisher);
    recorder.reset(true);
    ArmedCapture capture =
        publishFrameZero(recorder, publisher, 5u);
    publishThroughFinalTransition(recorder, capture);

    GameLifecycleV2 wrongBefore = capture.active;
    wrongBefore.resultCounter = 1u;
    ProvenGameFrameV2 wrong = frame(
        capture.fresh, capture.nextSource,
        wrongBefore, wrongBefore,
        capture.tailHash, capture.tailHash + 1u);
    ReplayPublishResultV2 published =
        recorder.acceptConfirmedFrame(wrong);
    QNP_CHECK(published.code == ReplayPublishCodeV2::Failed);
    QNP_CHECK(
        published.fault
        == ReplayRecordingFaultV2::FinalWinScreenPredicateMismatch);
    QNP_CHECK(
        recorder.state() == ReplayGameRecordStateV2::Failed);

    QNP_REQUIRE(!publisher.records.empty());
    const PublisherRecordV2& terminal =
        publisher.records.back();
    QNP_CHECK(terminal.kind == PublisherRecordKindV2::EndGame);
    QNP_CHECK(!terminal.end.containsFinalFrame);
    QNP_CHECK(
        terminal.end.end.reason
        == TerminalReason::RecordingFailure);
    QNP_CHECK(terminal.end.end.nextRecord == 8u);
    QNP_CHECK(
        terminal.end.end.postTailStateHash == capture.tailHash);
}

QNP_TEST(ReplayCoordinatorOverflowUsesReservedTerminalAtAcceptedTail) {
    CapturingPublisher publisher;
    ReplayRecordingCoordinatorV2 recorder(publisher);
    recorder.reset(true);
    ArmedCapture capture =
        publishFrameZero(recorder, publisher, 6u);

    ProvenGameFrameV2 next = frame(
        capture.fresh, capture.nextSource,
        capture.active, capture.active,
        capture.tailHash, capture.tailHash + 1u);
    publisher.failNextOrdinary = true;
    ReplayPublishResultV2 published =
        recorder.acceptConfirmedFrame(next);
    QNP_CHECK(published.code == ReplayPublishCodeV2::Failed);
    QNP_CHECK(
        published.fault
        == ReplayRecordingFaultV2::PublisherOverflow);
    QNP_CHECK(
        recorder.state() == ReplayGameRecordStateV2::Failed);
    QNP_CHECK(recorder.pendingCommitCount() == 1u);

    QNP_REQUIRE(publisher.records.size() == 2u);
    const PublisherRecordV2& terminal =
        publisher.records.back();
    QNP_CHECK(terminal.kind == PublisherRecordKindV2::EndGame);
    QNP_CHECK(!terminal.end.containsFinalFrame);
    QNP_CHECK(
        terminal.end.end.reason
        == TerminalReason::RecorderOverflow);
    QNP_CHECK(terminal.end.end.nextRecord == 1u);
    QNP_CHECK(
        terminal.end.end.postTailStateHash == capture.tailHash);
}

QNP_TEST(ReplayCoordinatorCreatesNoFileBeforeFrameZero) {
    CapturingPublisher publisher;
    ReplayRecordingCoordinatorV2 recorder(publisher);
    recorder.reset(true);
    FreshGameDescriptorV2 fresh = descriptor(7u);
    QNP_CHECK(
        recorder.armFreshGame(fresh).code
        == ReplayPublishCodeV2::Armed);
    QNP_CHECK(
        recorder.requestAbnormalEnd(
            TerminalReason::Disconnected,
            ReplayCloseProvenanceV2::PeerOrderly).code
        == ReplayPublishCodeV2::NoFile);
    QNP_CHECK(publisher.records.empty());
    QNP_CHECK(recorder.pendingCommitCount() == 0u);
    QNP_CHECK(
        recorder.state()
        == ReplayGameRecordStateV2::WaitingForFreshGame);
}

QNP_TEST(ReplayCoordinatorRejectsUnsupportedTopologyBeforeFile) {
    CapturingPublisher publisher;
    ReplayRecordingCoordinatorV2 recorder(publisher);
    recorder.reset(true);
    FreshGameDescriptorV2 team = descriptor(8u);
    team.start.manifest.vsRow = 2u;
    ReplayPublishResultV2 armed = recorder.armFreshGame(team);
    QNP_CHECK(armed.code == ReplayPublishCodeV2::Unsupported);
    QNP_CHECK(
        armed.fault
        == ReplayRecordingFaultV2::UnsupportedTopology);
    QNP_CHECK(publisher.records.empty());
    QNP_CHECK(
        recorder.state()
        == ReplayGameRecordStateV2::WaitingForFreshGame);
}

QNP_TEST(ReplayCoordinatorRequiresVerifiedStageOnFrameZero) {
    CapturingPublisher publisher;
    ReplayRecordingCoordinatorV2 recorder(publisher);
    recorder.reset(true);
    FreshGameDescriptorV2 fresh = descriptor(9u);
    QNP_CHECK(
        recorder.armFreshGame(fresh).code
        == ReplayPublishCodeV2::Armed);

    GameLifecycleV2 setupBefore = lifecycle(fresh, -1);
    GameLifecycleV2 setupAfter = lifecycle(fresh, 0);
    ProvenGameFrameV2 setup = frame(
        fresh, 100u, setupBefore, setupAfter,
        0x1000u, 0x2000u, StageIdentityV2{});
    markRoundActive(setup, 1u, 0x101u);
    QNP_CHECK(
        recorder.acceptConfirmedFrame(setup).code
        == ReplayPublishCodeV2::Armed);
    ProvenGameFrameV2 zero = frame(
        fresh, 101u, setupAfter, setupAfter,
        0x2000u, 0x3000u, StageIdentityV2{});
    ReplayPublishResultV2 accepted =
        recorder.acceptConfirmedFrame(zero);
    QNP_CHECK(accepted.code == ReplayPublishCodeV2::Failed);
    QNP_CHECK(publisher.records.empty());
}

QNP_TEST(ReplayCoordinatorAcceptsExactNativeRematchReset) {
    CapturingPublisher publisher;
    ReplayRecordingCoordinatorV2 recorder(publisher);
    recorder.reset(true);
    FreshGameDescriptorV2 fresh = descriptor(10u);
    QNP_CHECK(
        recorder.armFreshGame(fresh).code
        == ReplayPublishCodeV2::Armed);

    GameLifecycleV2 priorResult =
        lifecycle(fresh, -1, {{2u, 1u}}, -1);
    priorResult.roundCounter = 5u;
    GameLifecycleV2 resetActive = lifecycle(fresh, 0);
    ProvenGameFrameV2 reset = frame(
        fresh, 100u, priorResult, resetActive,
        0x1000u, 0x2000u, StageIdentityV2{});
    markRoundActive(reset, 1u, 0x101u);
    QNP_CHECK(
        recorder.acceptConfirmedFrame(reset).code
        == ReplayPublishCodeV2::Armed);

    ProvenGameFrameV2 zero = frame(
        fresh, 101u, resetActive, resetActive,
        0x2800u, 0x3000u);
    QNP_CHECK(
        recorder.acceptConfirmedFrame(zero).code
        == ReplayPublishCodeV2::Published);
}

QNP_TEST(ReplayCoordinatorNeverOverwritesUnresolvedCommitLedger) {
    CapturingPublisher publisher;
    ReplayRecordingCoordinatorV2 recorder(publisher);
    recorder.reset(true);

    for (uint32_t generation = 20u;
         generation < 20u + kMaxPendingReplayCommitsV2;
         ++generation) {
        (void)publishFrameZero(
            recorder, publisher, generation);
        QNP_CHECK(
            recorder.requestAbnormalEnd(
                TerminalReason::ProcessExit,
                ReplayCloseProvenanceV2::LocalOrderly).code
            == ReplayPublishCodeV2::TerminalPublished);
    }
    QNP_CHECK(
        recorder.pendingCommitCount()
        == kMaxPendingReplayCommitsV2);
    const size_t recordsBefore = publisher.records.size();

    FreshGameDescriptorV2 blocked = descriptor(40u);
    QNP_CHECK(
        recorder.armFreshGame(blocked).code
        == ReplayPublishCodeV2::Armed);
    GameLifecycleV2 setupBefore = lifecycle(blocked, -1);
    GameLifecycleV2 setupAfter = lifecycle(blocked, 0);
    ProvenGameFrameV2 setup = frame(
        blocked, 100u, setupBefore, setupAfter,
        0x1000u, 0x2000u, StageIdentityV2{});
    markRoundActive(setup, 1u, 0x101u);
    QNP_CHECK(
        recorder.acceptConfirmedFrame(setup).code
        == ReplayPublishCodeV2::Armed);
    ProvenGameFrameV2 zero = frame(
        blocked, 101u, setupAfter, setupAfter,
        0x2000u, 0x3000u);
    ReplayPublishResultV2 accepted =
        recorder.acceptConfirmedFrame(zero);
    QNP_CHECK(accepted.code == ReplayPublishCodeV2::Failed);
    QNP_CHECK(
        accepted.fault
        == ReplayRecordingFaultV2::CommitBacklog);
    QNP_CHECK(
        recorder.state() == ReplayGameRecordStateV2::Disabled);
    QNP_CHECK(publisher.records.size() == recordsBefore);
    QNP_CHECK(
        recorder.pendingCommitCount()
        == kMaxPendingReplayCommitsV2);
}
