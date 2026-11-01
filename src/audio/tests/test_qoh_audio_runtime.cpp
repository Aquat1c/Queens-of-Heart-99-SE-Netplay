#include "TestHarness.h"

#include "audio/QohAudioRuntime.h"

#include <memory>

using namespace qaudio;

namespace {

int playBaseOk(int sample, int) {
    return sample;
}

int seekPlayCalls = 0;
int seekLoadCalls = 0;
int seekBgmCalls = 0;
int seekPlay(int sample, int) { ++seekPlayCalls; return sample; }
int seekLoad(int, const char*) { ++seekLoadCalls; return 1; }
int seekBgm(int, int) { ++seekBgmCalls; return 1; }

}

QNP_TEST(ReplaySeekMutedAudioDrainsLongHistoryWithoutSilencingVisiblePlayback) {
    auto runtime = std::make_unique<QohAudioRuntime>();
    NativeAudioOps ops{};
    ops.load = &seekLoad;
    ops.playBase = &seekPlay;
    ops.bgm = &seekBgm;
    runtime->configure(ops);
    runtime->resetSession();
    seekPlayCalls = seekLoadCalls = seekBgmCalls = 0;
    runtime->catalog().observeLoad(2, "seek.wav", true, WaveFacts{});
    const AssetKey asset = runtime->catalog().assetOf(2);
    runtime->setDisposition(AudioTickDisposition::SpectatorConfirmedCatchup);
    for (uint32_t frame = 0u; frame < 96u; ++frame) {
        QNP_REQUIRE(runtime->beginTick(1u, 1u, frame));
        if (frame == 0u)
            runtime->captureLoad(2, "seek.wav", true, asset, 0u);
        runtime->capturePlayBase(2, 0u, 0u);
        QNP_REQUIRE(runtime->captureBgm(7u, 0u, kBgmOwnedCallsiteA));
        QNP_REQUIRE(runtime->commitTick());
        if ((frame + 1u) % 16u == 0u) runtime->flushPendingMuted(frame);
    }
    QNP_CHECK(!runtime->failedClosed());
    QNP_CHECK(runtime->telemetry().journalOverflow == 0u);
    QNP_CHECK(runtime->catalog().assetOf(2) == asset);
    QNP_CHECK(runtime->bgm().pendingCount() == 0u);
    QNP_CHECK(seekLoadCalls == 1);
    QNP_CHECK(seekPlayCalls == 0);
    QNP_CHECK(seekBgmCalls == 0);
    QNP_CHECK(!runtime->sink().muted());

    runtime->setDisposition(AudioTickDisposition::SpectatorConfirmedVisible);
    QNP_REQUIRE(runtime->beginTick(1u, 1u, 96u));
    runtime->capturePlayBase(2, 0u, 0u);
    QNP_REQUIRE(runtime->captureBgm(8u, 0u, kBgmOwnedCallsiteA));
    QNP_REQUIRE(runtime->commitTick());
    runtime->flushPending(96u);
    QNP_CHECK(seekPlayCalls == 1);
    QNP_CHECK(seekBgmCalls == 1);
    runtime->setMuted(true);
    runtime->flushPendingMuted(97u);
    QNP_CHECK(runtime->sink().muted());
}

QNP_TEST(ReplayRewindAudioBaselineSurvivesTakeoverAndRejectsNewSession) {
    auto runtime = std::make_unique<QohAudioRuntime>();
    runtime->resetSession();
    runtime->catalog().observeLoad(2, "start.wav", true, WaveFacts{});
    const AssetKey start = runtime->catalog().assetOf(2);
    QNP_REQUIRE(runtime->captureReplayStart());
    runtime->catalog().observeLoad(2, "later.wav", true, WaveFacts{});
    const AssetKey later = runtime->catalog().assetOf(2);
    QNP_REQUIRE(runtime->beginReplayTakeover());
    runtime->catalog().observeLoad(2, "branch.wav", true, WaveFacts{});
    QNP_REQUIRE(runtime->restoreAfterReplayTakeover());
    QNP_CHECK(runtime->catalog().assetOf(2) == later);
    runtime->endReplayTakeover();
    QNP_REQUIRE(runtime->restoreReplayStart());
    QNP_CHECK(runtime->catalog().assetOf(2) == start);
    runtime->resetSession();
    QNP_CHECK(!runtime->restoreReplayStart());
}

QNP_TEST(ReplayTakeoverAudioRestoreKeepsBaselineAndDropsBranchState) {
    auto runtime = std::make_unique<QohAudioRuntime>();
    NativeAudioOps ops{};
    ops.playBase = &playBaseOk;
    runtime->configure(ops);
    runtime->resetSession();

    const WaveFacts facts{};
    runtime->catalog().observeLoad(2, "baseline.wav", true, facts);
    const AssetKey baselineAsset = runtime->catalog().assetOf(2);
    const SinkResult baselineBgm = runtime->sink().playBase(0, 0u);
    const SinkResult baselineSfx = runtime->sink().playBase(2, 0u);
    QNP_REQUIRE(baselineBgm.ok);
    QNP_REQUIRE(baselineSfx.ok);
    QNP_REQUIRE(runtime->beginReplayTakeover());

    runtime->catalog().observeLoad(2, "branch.wav", true, facts);
    QNP_CHECK(!(runtime->catalog().assetOf(2) == baselineAsset));
    const SinkResult branchBgm = runtime->sink().playBase(0, 0u);
    QNP_REQUIRE(branchBgm.ok);
    runtime->setDisposition(AudioTickDisposition::ForwardVisible);
    QNP_REQUIRE(runtime->beginTick(1u, 1u, 10u));
    QNP_REQUIRE(runtime->captureBgm(
        7u, 0u, kBgmOwnedCallsiteA));
    QNP_REQUIRE(runtime->commitTick());
    QNP_CHECK(runtime->telemetry().bgmCommandQueued == 1u);

    QNP_REQUIRE(runtime->restoreAfterReplayTakeover());
    QNP_CHECK(runtime->catalog().assetOf(2) == baselineAsset);
    QNP_CHECK(runtime->telemetry().bgmCommandQueued == 0u);
    QNP_CHECK(runtime->disposition()
              == AudioTickDisposition::ImmediateOffline);
    QNP_CHECK(runtime->sink().currentOwner(
                  {AudioEndpoint::Type::Base, 0u}) == baselineBgm.token);
    QNP_CHECK(!runtime->sink().currentOwner(
                   {AudioEndpoint::Type::Base, 2u}).valid());

    QNP_REQUIRE(runtime->restoreAfterReplayTakeover());
    runtime->endReplayTakeover();
    QNP_CHECK(!runtime->restoreAfterReplayTakeover());
}

QNP_TEST(ReplayAudioCheckpointRestoresAnArbitraryBoundaryWithoutReplacingBaselines) {
    auto runtime = std::make_unique<QohAudioRuntime>();
    auto checkpoint = std::make_unique<QohAudioRuntime::ReplayCheckpoint>();
    NativeAudioOps ops{};
    ops.playBase = &seekPlay;
    ops.bgm = &seekBgm;
    runtime->configure(ops);
    runtime->resetSession();
    runtime->catalog().observeLoad(2, "checkpoint.wav", true, WaveFacts{});
    runtime->setDisposition(AudioTickDisposition::SpectatorConfirmedVisible);
    const uint32_t frames[] = {0u, 120u, 121u, 140u};
    const uint8_t tracks[] = {3u, 7u, 8u, 9u};
    for (size_t i = 0u; i < 4u; ++i) {
        QNP_REQUIRE(runtime->beginTick(2u, 4u, frames[i]));
        QNP_REQUIRE(runtime->captureBgm(tracks[i], 0u, kBgmOwnedCallsiteA));
        runtime->capturePlayBase(2, 0u, 0u);
        QNP_REQUIRE(runtime->commitTick());
        runtime->flushPending(frames[i]);
        if (i == 0u) QNP_REQUIRE(runtime->captureReplayStart());
        if (i == 1u) QNP_REQUIRE(runtime->captureReplayCheckpoint(*checkpoint));
        if (i == 2u) QNP_REQUIRE(runtime->beginReplayTakeover());
    }
    runtime->setMuted(true);
    for (size_t retry = 0u; retry < 2u; ++retry) {
        QNP_REQUIRE(runtime->restoreReplayCheckpoint(*checkpoint));
        QNP_CHECK(runtime->sink().muted());
        QNP_CHECK(runtime->telemetry().bgmCommandQueued == 2u);
        QNP_CHECK(!runtime->sink().currentOwner(
            {AudioEndpoint::Type::Base, 2u}).valid());
        QNP_REQUIRE(runtime->beginTick(2u, 4u, 121u));
        QNP_REQUIRE(runtime->captureBgm(7u, 0u, kBgmOwnedCallsiteA));
        QNP_REQUIRE(runtime->commitTick());
        runtime->flushPendingMuted(121u);
        QNP_CHECK(runtime->telemetry().bgmCommandQueued == 2u);
        QNP_CHECK(runtime->telemetry().bgmSteadyStateSuppressed == 1u);
    }
    QNP_REQUIRE(runtime->restoreAfterReplayTakeover());
    QNP_CHECK(runtime->telemetry().bgmCommandQueued == 3u);
    QNP_REQUIRE(runtime->restoreReplayStart());
    QNP_CHECK(runtime->telemetry().bgmCommandQueued == 1u);
    QNP_REQUIRE(runtime->restoreReplayCheckpoint(*checkpoint));
    QNP_CHECK(runtime->telemetry().bgmCommandQueued == 2u);
}

QNP_TEST(ReplayAudioCheckpointCaptureRequiresAFlushedIdleBoundary) {
    auto runtime = std::make_unique<QohAudioRuntime>();
    auto checkpoint = std::make_unique<QohAudioRuntime::ReplayCheckpoint>();
    runtime->resetSession();
    runtime->setDisposition(AudioTickDisposition::SpectatorConfirmedVisible);
    QNP_REQUIRE(runtime->captureReplayCheckpoint(*checkpoint));
    QNP_REQUIRE(runtime->beginTick(1u, 1u, 40u));
    QNP_CHECK(!runtime->captureReplayCheckpoint(*checkpoint));
    runtime->abortTick();
    QNP_REQUIRE(runtime->beginCorrection(40u, 41u));
    QNP_CHECK(!runtime->captureReplayCheckpoint(*checkpoint));
    runtime->abortCorrection();
    QNP_REQUIRE(runtime->beginTick(1u, 1u, 40u));
    QNP_REQUIRE(runtime->captureBgm(7u, 0u, kBgmOwnedCallsiteA));
    QNP_REQUIRE(runtime->commitTick());
    QNP_CHECK(!runtime->captureReplayCheckpoint(*checkpoint));
    runtime->flushPendingMuted(40u);
    QNP_REQUIRE(runtime->restoreReplayCheckpoint(*checkpoint));
    QNP_CHECK(runtime->telemetry().bgmCommandQueued == 0u);
    QNP_CHECK(runtime->bgm().pendingCount() == 0u);
    QNP_REQUIRE(runtime->captureReplayCheckpoint(*checkpoint));
}

QNP_TEST(ReplayAudioCheckpointRejectsStaleSessionDeviceAndResourceBindings) {
    auto runtime = std::make_unique<QohAudioRuntime>();
    auto checkpoint = std::make_unique<QohAudioRuntime::ReplayCheckpoint>();
    runtime->resetSession();
    QNP_CHECK(!runtime->restoreReplayCheckpoint(*checkpoint));
    runtime->catalog().observeLoad(2, "before.wav", true, WaveFacts{});
    QNP_REQUIRE(runtime->captureReplayCheckpoint(*checkpoint));
    runtime->catalog().observeLoad(2, "after.wav", true, WaveFacts{});
    const AssetKey after = runtime->catalog().assetOf(2);
    QNP_CHECK(!runtime->restoreReplayCheckpoint(*checkpoint));
    QNP_CHECK(runtime->catalog().assetOf(2) == after);

    QNP_REQUIRE(runtime->captureReplayCheckpoint(*checkpoint));
    runtime->onDeviceTeardown();
    const uint32_t device = runtime->sink().deviceGeneration();
    QNP_CHECK(!runtime->restoreReplayCheckpoint(*checkpoint));
    QNP_CHECK(runtime->sink().deviceGeneration() == device);
    QNP_REQUIRE(runtime->captureReplayCheckpoint(*checkpoint));
    runtime->resetSession();
    QNP_CHECK(!runtime->restoreReplayCheckpoint(*checkpoint));

    QNP_REQUIRE(runtime->captureReplayCheckpoint(*checkpoint));
    runtime->setDisposition(AudioTickDisposition::SpectatorConfirmedVisible);
    QNP_REQUIRE(runtime->beginTick(1u, 1u, 10u));
    runtime->captureUnload(2, 0u);
    QNP_REQUIRE(runtime->commitTick());
    runtime->flushPendingMuted(10u);
    QNP_CHECK(!runtime->restoreReplayCheckpoint(*checkpoint));
}

QNP_TEST(ReplayAudioCheckpointContinuesLongMutedHistoryAndThenVisibleAudio) {
    auto runtime = std::make_unique<QohAudioRuntime>();
    auto checkpoint = std::make_unique<QohAudioRuntime::ReplayCheckpoint>();
    NativeAudioOps ops{};
    ops.playBase = &seekPlay;
    ops.bgm = &seekBgm;
    runtime->configure(ops);
    runtime->resetSession();
    runtime->catalog().observeLoad(2, "seek.wav", true, WaveFacts{});
    runtime->setDisposition(AudioTickDisposition::SpectatorConfirmedVisible);
    QNP_REQUIRE(runtime->beginTick(1u, 1u, 137u));
    QNP_REQUIRE(runtime->captureBgm(7u, 0u, kBgmOwnedCallsiteA));
    QNP_REQUIRE(runtime->commitTick());
    runtime->flushPending(137u);
    QNP_REQUIRE(runtime->captureReplayCheckpoint(*checkpoint));
    seekPlayCalls = seekBgmCalls = 0;
    for (size_t retry = 0u; retry < 2u; ++retry) {
        QNP_REQUIRE(runtime->restoreReplayCheckpoint(*checkpoint));
        runtime->setDisposition(AudioTickDisposition::SpectatorConfirmedCatchup);
        for (uint32_t frame = 138u; frame < 234u; ++frame) {
            QNP_REQUIRE(runtime->beginTick(1u, 1u, frame));
            runtime->capturePlayBase(2, 0u, 0u);
            QNP_REQUIRE(runtime->captureBgm(
                static_cast<uint8_t>(7u + frame % 2u), 0u, kBgmOwnedCallsiteA));
            QNP_REQUIRE(runtime->commitTick());
            if ((frame - 137u) % 16u == 0u) runtime->flushPendingMuted(frame);
        }
        QNP_CHECK(!runtime->failedClosed());
        QNP_CHECK(runtime->telemetry().journalOverflow == 0u);
        QNP_CHECK(runtime->bgm().pendingCount() == 0u);
        QNP_CHECK(seekPlayCalls == 0);
        QNP_CHECK(seekBgmCalls == 0);
        QNP_CHECK(!runtime->sink().muted());
    }
    runtime->setDisposition(AudioTickDisposition::SpectatorConfirmedVisible);
    QNP_REQUIRE(runtime->beginTick(1u, 1u, 234u));
    runtime->capturePlayBase(2, 0u, 0u);
    QNP_REQUIRE(runtime->captureBgm(9u, 0u, kBgmOwnedCallsiteA));
    QNP_REQUIRE(runtime->commitTick());
    runtime->flushPending(234u);
    QNP_CHECK(seekPlayCalls == 1);
    QNP_CHECK(seekBgmCalls == 1);
}
