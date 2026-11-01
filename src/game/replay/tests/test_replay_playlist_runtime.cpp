#include "netplay/tests/TestHarness.h"
#include "game/replay/ReplayRuntime.h"
#include "game/PaletteBridge.h"
#include "replay/ReplayWinScreen.h"
#include "ReplayIpcTestFixture.h"

namespace {
using namespace qgame::replay;
using namespace qreplay;
using namespace replay_ipc_test;
namespace wire3 = qreplay::ipc::v3;

struct CompletedGame : Game {
    ReplayPaletteV2 palette{};
    std::array<ConfirmedGameFrameV2, 3> frames{};
    std::array<ConfirmedGameEventV2, 4> allEvents{};
    explicit CompletedGame(uint32_t generation) : Game(generation) {
        frames[0] = frame;
        allEvents[0] = events[0]; allEvents[1] = events[1];
        uint64_t digest = frame.rollingDigest;
        for (uint32_t i = 1u; i < frames.size(); ++i) {
            auto& f = frames[i];
            f = frames[i - 1u];
            f.sourceFrame = f.replayFrame = i;
            f.core.before = frames[i - 1u].core.after;
            f.core.after = f.core.before;
            f.core.after.matchSub = 2;
            f.core.after.roundCounter = 5u;
            f.core.after.resultCounter = i == 2u ? 1u : 0u;
            f.core.after.winnerSlot = 0;
            f.core.after.roundTallies = {{2u, 1u}};
            f.core.preStateHash = frames[i - 1u].core.postStateHash;
            f.core.postStateHash = f.core.preStateHash + 100u;
            f.rollingDigest = advanceReplayDigestV2(digest, f);
            digest = f.rollingDigest;
            auto& e = allEvents[i + 1u];
            e.kind = i == 1u ? GameEventKindV2::RoundEnded : GameEventKindV2::GameResult;
            e.phase = GameEventPhaseV2::PostFrame;
            e.replayFrame = i; e.eventSequence = i + 1u;
            e.transitionSiteId = i + 2u; e.roundOrdinal = 2u;
            e.stateProof = f.core.postStateHash;
            e.winningSideMask = 1u; e.roundTallies = {{2u, 1u}}; e.winnerSlot = 0;
            e.rollingDigest = advanceReplayDigestV2(digest, e);
            digest = e.rollingDigest;
        }
        auto& end = info.endpoint.recordedGameEnd;
        end.reason = TerminalReason::Completed;
        end.nextRecord = info.frameCount = info.endpoint.nextRecord = 3u;
        info.eventCount = 4u;
        end.winningSideMask = 1u; end.roundTallies = {{2u, 1u}}; end.winnerSlot = 0;
        end.postTailStateHash = info.endpoint.postTailStateHash = 400u;
        end.finalRollingDigest = info.endpoint.finalRollingDigest = digest;
        CanonicalGameDigestV2 canonical;
        canonical.begin(start);
        canonical.append(allEvents[0]); canonical.append(allEvents[1]);
        for (size_t i = 0u; i < frames.size(); ++i) {
            canonical.append(frames[i]);
            if (i != 0u) canonical.append(allEvents[i + 1u]);
        }
        QNP_CHECK(canonical.finish(end, end.canonicalGameDigest));
        info.endpoint.kind = wire3::PlaybackEndpointKind::RecordedGameEnd;
        info.endpoint.hasRecordedGameEnd = true;
        info.hasFileDigest = info.hasCanonicalGameDigest = true;
        info.fileDigest.fill(8u);
        info.canonicalGameDigest = end.canonicalGameDigest;
    }

    bool send(MemoryChannel& channel, uint32_t& seq) const {
        std::vector<uint8_t> p;
        const auto& id = start.manifest.identity;
        if (!wire3::encodePlaybackSessionInfo(info, p)
            || !feed(channel, seq, id, wire3::MessageType::PlaybackSessionInfo, p)) return false;
        p.clear();
        if (!wire3::encodePlaybackGameStart(start, p)
            || !feed(channel, seq, id, wire3::MessageType::PlaybackGameStart, p)) return false;
        if (palette.present) {
            p.clear();
            if (!wire3::encodePlaybackPalette(palette, p)
                || !feed(channel, seq, id, wire3::MessageType::PlaybackPalette, p)) return false;
        }
        p.clear();
        if (!wire3::encodePlaybackFrames(frames.data(), frames.size(), p)
            || !feed(channel, seq, id, wire3::MessageType::PlaybackFrames, p, 0u, 3u)) return false;
        p.clear();
        if (!wire3::encodePlaybackEvents(allEvents.data(), allEvents.size(), p)
            || !feed(channel, seq, id, wire3::MessageType::PlaybackEvents, p, 0u, 4u)) return false;
        p.clear();
        return wire3::encodePlaybackEndpoint(info.endpoint, p)
            && feed(channel, seq, id, wire3::MessageType::PlaybackEndpoint, p, 3u, 4u);
    }

    bool bootstrap() const {
        ReplayBootstrapObservationV2 observed{};
        observed.initialPhase = start.start.initialPhase;
        observed.bootstrapFingerprint = start.start.bootstrapFingerprint;
        observed.frameZeroPreStateHash = start.start.frameZeroPreStateHash;
        observed.frameZeroLifecycle = frames[0].core.before;
        return replayCompleteBootstrapV2(observed);
    }
    bool tick(size_t i) const {
        const auto& f = frames[i];
        ReplayFrameBeginObservationV2 begin{};
        begin.replayFrame = f.replayFrame; begin.preStateHash = f.core.preStateHash;
        begin.lifecycleBefore = f.core.before; begin.activeLaneMask = f.core.activeLaneMask;
        begin.finalPresentationSerial = i + 1u;
        ReplayFrameCommitObservationV2 commit{};
        commit.replayFrame = f.replayFrame; commit.postStateHash = f.core.postStateHash;
        commit.lifecycleAfter = f.core.after; commit.dispatcherResult = f.core.dispatcherResult;
        return replayBeginFrameV2(begin) && replayCommitFrameV2(commit);
    }
};

struct RuntimeGuard { ~RuntimeGuard() { stopReplayRuntime(); } };
}

QNP_TEST(replay_playlist_runtime_replaces_session_and_pauses_only_first) {
    CompletedGame first(1u), second(2u);
    first.palette.side = 1u;
    first.palette.present = 1u;
    std::memset(first.palette.table, 17, sizeof(first.palette.table));
    for (size_t variant = 0; variant < 4u; ++variant)
        std::memset(first.palette.table + variant * 64u, 0, 4u);
    auto* channel = new MemoryChannel();
    ReplayRuntimeTestConfig test{};
    test.brokerChannel = channel;
    test.playbackRuntimeContract = &first.start.manifest.contract;
    auto& c = test.ipc;
    c.mode = qipc::MODE_REPLAY;
    c.replayChannelGeneration = 5u; c.replayBrokerNonce = 6u;
    c.replayIpcSchemaVersion = qipc::kReplayPlaybackIpcSchemaVersion;
    c.replayFeatureBits = qipc::kReplayPlaybackRequiredFeatureBits;
    c.replayGameGeneration = first.start.manifest.identity.gameGeneration;
    c.replayGameUuidLow = first.start.manifest.identity.gameUuidLow;
    c.replayExpectedFrameCount = 3u; c.replayExpectedEventCount = 4u;
    c.replayValidatedSourceByteCount = first.info.validatedSourceByteCount;
    std::memcpy(c.replayValidatedSourceRangeDigest, first.info.validatedSourceRangeDigest.data(), 32u);
    std::memcpy(c.replayContentManifestRoot, first.start.manifest.contract.contentManifestRoot.data(), 32u);
    c.replayEndpointKind = qipc::kReplayEndpointRecordedGameEnd;
    c.replaySpeedPermille = 1000u;
    QNP_REQUIRE(startReplayRuntimeForTest(test));
    RuntimeGuard guard;
    uint32_t seq = 0u;
    QNP_REQUIRE(first.send(*channel, seq));
    for (int i = 0; i < 8; ++i) replayPlanOuterSlot();
    QNP_REQUIRE(first.bootstrap());
    QNP_CHECK(qgame::paletteReplayHold().held == 2u);
    QNP_CHECK(qgame::paletteReplayHold().slots[1].table[100] == 17u);
    QNP_REQUIRE(first.tick(0u));
    QNP_CHECK(replayPlanOuterSlot().paused);
    const uint32_t oldGeneration = replayPresentationGeneration();
    QNP_REQUIRE(replaySubmitPresentationCommand(oldGeneration,
        static_cast<uint32_t>(PresentationCommand::Resume), 0u));
    QNP_REQUIRE(replaySubmitPresentationCommand(oldGeneration,
        static_cast<uint32_t>(PresentationCommand::SetSpeedPermille), 2000u));
    QNP_REQUIRE(replaySubmitPresentationCommand(oldGeneration,
        static_cast<uint32_t>(PresentationCommand::ToggleCollisionDisplay), 0u));
    replayPlanOuterSlot();
    QNP_REQUIRE(first.tick(1u)); QNP_REQUIRE(first.tick(2u));
    QNP_REQUIRE(replayAcknowledgeFinalPresentationV2(3u) == ReplayPresentationAckV2::Accepted);
    QNP_REQUIRE(replayWinScreenPendingV3());
    replayCompleteWinScreenV3(true);
    for (int i = 0; i < 4; ++i) replayPlanOuterSlot();
    QNP_CHECK(replayComplete());
    bool completedResult = false;
    for (const auto& f : readOutput(*channel)) {
        if (f.type != wire3::MessageType::PlaybackResult) continue;
        wire3::PlaybackResultPayload result{};
        QNP_REQUIRE(wire3::decodePlaybackResult(f.payload.data(), f.payload.size(), result));
        completedResult = replayCompletedGame(result);
        if (!completedResult) std::printf("first result reason=%u region=%u\n",
            static_cast<unsigned>(result.reason), result.mismatchRegion);
    }
    QNP_REQUIRE(completedResult);
    auto next = second.next(1u);
    next.presentationFlags = qipc::kReplayAutoStart;
    std::vector<uint8_t> payload;
    QNP_REQUIRE(wire3::encodePlaybackNextGame(next, payload));
    QNP_REQUIRE(feed(*channel, seq, first.start.manifest.identity,
        wire3::MessageType::PlaybackNextGame, payload));
    for (int i = 0; i < 4; ++i) replayPlanOuterSlot();
    QNP_REQUIRE(replayNextGamePendingV3());
    QNP_REQUIRE(replaySubmitPresentationCommand(oldGeneration,
        static_cast<uint32_t>(PresentationCommand::Pause), 0u));
    QNP_REQUIRE(replayBeginNextGameV3());
    QNP_CHECK(!replayNextGamePendingV3());
    QNP_CHECK(!replayComplete());
    QNP_CHECK(!replayBeginNextGameV3());
    QNP_CHECK(replayPresentationGeneration() != oldGeneration);
    QNP_CHECK(!replaySubmitPresentationCommand(oldGeneration,
        static_cast<uint32_t>(PresentationCommand::Pause), 0u));
    pumpWorkerForTest();
    QNP_REQUIRE(second.send(*channel, seq));
    for (int i = 0; i < 8; ++i) replayPlanOuterSlot();
    QNP_REQUIRE(second.bootstrap());
    QNP_CHECK(qgame::paletteReplayHold().held == 0u);
    QNP_REQUIRE(second.tick(0u));
    QNP_CHECK(!replayPlanOuterSlot().paused);
    ReplayStatusSnapshot status{};
    replayStatus(status);
    QNP_CHECK(status.currentFrame == 1u && status.speedPermille == 2000u);
    QNP_CHECK(status.ticksExecuted == 1u && status.pendingSteps == 0u);
    QNP_CHECK(replayCollisionDisplayRequested());
    QNP_REQUIRE(second.tick(1u)); QNP_REQUIRE(second.tick(2u));
    QNP_REQUIRE(replayAcknowledgeFinalPresentationV2(3u) == ReplayPresentationAckV2::Accepted);
    QNP_REQUIRE(replayWinScreenPendingV3());
    replayCompleteWinScreenV3(true);
    for (int i = 0; i < 4; ++i) replayPlanOuterSlot();
    size_t results = 0u, acknowledgements = 0u;
    for (const auto& f : readOutput(*channel)) {
        if (f.type == wire3::MessageType::PlaybackResult) ++results;
        if (f.type == wire3::MessageType::PlaybackNextGameAck) ++acknowledgements;
    }
    QNP_CHECK(results == 2u && acknowledgements == 1u);
    QNP_CHECK(!replayExitRequested());
}
