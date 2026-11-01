#include "TestHarness.h"

#include "audio/QohBgmBoundaryGuard.h"

#include <vector>

using namespace qaudio;

namespace {

struct Executed {
    uint32_t frame;
    uint8_t trackId;
};

}

QNP_TEST(BgmGuardClassifiesOwnedCallsites) {
    QNP_CHECK(bgmOwnedCallsite(kBgmOwnedCallsiteA));
    QNP_CHECK(bgmOwnedCallsite(kBgmOwnedCallsiteB));
    QNP_CHECK(!bgmOwnedCallsite(0x400000u));
    QNP_CHECK(!bgmOwnedCallsite(0));
}

QNP_TEST(BgmGuardUnknownCallsiteFailsClosed) {
    QohBgmBoundaryGuard guard;
    AudioTelemetry telemetry{};
    guard.reset();
    QNP_CHECK(!guard.capture(1, 10, 0x123456u, 0, 3, 0, telemetry));
    QNP_CHECK(guard.failedClosed());
    QNP_CHECK(telemetry.bgmUnexpectedOwnedCallsite == 1);
}

QNP_TEST(BgmGuardCommitConfirmDrainLifecycle) {
    QohBgmBoundaryGuard guard;
    AudioTelemetry telemetry{};
    guard.reset();
    QNP_REQUIRE(guard.capture(1, 10, kBgmOwnedCallsiteA, 0, 3, 0, telemetry));
    guard.abortFrame(10, telemetry);
    QNP_CHECK(guard.pendingCount() == 0);
    QNP_CHECK(telemetry.bgmCommandDiscarded == 1);

    QNP_REQUIRE(guard.capture(1, 11, kBgmOwnedCallsiteA, 0, 3, 0, telemetry));
    QNP_REQUIRE(guard.capture(1, 13, kBgmOwnedCallsiteB, 0, 5, 0, telemetry));
    guard.commitFrame(11, telemetry);
    guard.commitFrame(13, telemetry);
    QNP_CHECK(telemetry.bgmCommandCommitted == 2);

    std::vector<Executed> executed;
    size_t drained = guard.drainConfirmed(
        12,
        [&executed](const PendingBgmCommand& command) {
            executed.push_back({command.frame, command.trackId});
        },
        telemetry);
    QNP_CHECK(drained == 1);
    QNP_REQUIRE(executed.size() == 1);
    QNP_CHECK(executed[0].frame == 11 && executed[0].trackId == 3);
    QNP_CHECK(guard.pendingCount() == 1);

    drained = guard.drainConfirmed(
        20,
        [&executed](const PendingBgmCommand& command) {
            executed.push_back({command.frame, command.trackId});
        },
        telemetry);
    QNP_CHECK(drained == 1);
    QNP_CHECK(executed.size() == 2);
    QNP_CHECK(guard.pendingCount() == 0);
}

QNP_TEST(BgmGuardPreservesDistinctSameTrackCommands) {
    QohBgmBoundaryGuard guard;
    AudioTelemetry telemetry{};
    guard.reset();
    QNP_REQUIRE(guard.capture(1, 10, kBgmOwnedCallsiteA, 0, 7, 0, telemetry));
    QNP_REQUIRE(guard.capture(1, 15, kBgmOwnedCallsiteA, 0, 7, 1, telemetry));
    guard.commitFrame(10, telemetry);
    guard.commitFrame(15, telemetry);
    size_t executions = 0;
    guard.drainConfirmed(
        20,
        [&executions](const PendingBgmCommand&) { ++executions; },
        telemetry);
    QNP_CHECK(executions == 2);
    QNP_CHECK(telemetry.bgmSameTrackCommandPreserved == 1);

    QNP_REQUIRE(guard.capture(1, 20, kBgmOwnedCallsiteB, 0, kBgmStopTrack, 0,
                              telemetry));
    QNP_REQUIRE(guard.capture(1, 24, kBgmOwnedCallsiteA, 0, 7, 1, telemetry));
    QNP_CHECK(guard.pendingCount() == 2);
}

QNP_TEST(BgmGuardSteadyStateSpamNeverOverflows) {
    QohBgmBoundaryGuard guard;
    AudioTelemetry telemetry{};
    guard.reset();
    for (uint32_t frame = 100; frame < 200; ++frame) {
        QNP_REQUIRE(guard.capture(1, frame, kBgmOwnedCallsiteA, 0, 33, 1,
                                  telemetry));
        guard.commitFrame(frame, telemetry);
    }
    QNP_CHECK(!guard.failedClosed());
    QNP_CHECK(guard.pendingCount() == 1);
    QNP_CHECK(telemetry.bgmCommandQueued == 1);
    QNP_CHECK(telemetry.bgmSteadyStateSuppressed == 99);
    QNP_CHECK(telemetry.bgmQueueOverflow == 0);

    size_t executions = 0;
    guard.drainConfirmed(
        200, [&executions](const PendingBgmCommand&) { ++executions; },
        telemetry);
    QNP_CHECK(executions == 1);
    QNP_REQUIRE(guard.capture(1, 210, kBgmOwnedCallsiteA, 0, 33, 1,
                              telemetry));
    QNP_CHECK(guard.pendingCount() == 0);
    QNP_CHECK(telemetry.bgmSteadyStateSuppressed == 100);
    QNP_REQUIRE(guard.capture(1, 212, kBgmOwnedCallsiteB, 0, kBgmStopTrack, 0,
                              telemetry));
    QNP_CHECK(guard.pendingCount() == 1);
}

QNP_TEST(BgmGuardCorrectionRemovesReplacedRange) {
    QohBgmBoundaryGuard guard;
    AudioTelemetry telemetry{};
    guard.reset();
    QNP_REQUIRE(guard.capture(1, 10, kBgmOwnedCallsiteA, 0, 3, 0, telemetry));
    QNP_REQUIRE(guard.capture(1, 14, kBgmOwnedCallsiteA, 0, 4, 0, telemetry));
    QNP_REQUIRE(guard.capture(1, 20, kBgmOwnedCallsiteB, 0, 5, 0, telemetry));
    guard.commitFrame(10, telemetry);
    guard.commitFrame(14, telemetry);
    guard.commitFrame(20, telemetry);
    guard.removeRange(12, 18, telemetry);
    QNP_CHECK(guard.pendingCount() == 2);
    QNP_CHECK(guard.pending(0)->frame == 10);
    QNP_CHECK(guard.pending(1)->frame == 20);
}

QNP_TEST(BgmGuardCapacityDerivedFromRollback) {
    QohBgmBoundaryGuard guard;
    AudioTelemetry telemetry{};
    guard.reset();
    for (uint32_t i = 0; i < kBgmQueueCapacity; ++i) {
        QNP_REQUIRE(guard.capture(1, 100 + i, kBgmOwnedCallsiteA,
                                  0, static_cast<uint8_t>(3 + (i & 1)), 0,
                                  telemetry));
    }
    QNP_CHECK(!guard.failedClosed());
    QNP_CHECK(!guard.capture(1, 200, kBgmOwnedCallsiteA, 0, 3, 0, telemetry));
    QNP_CHECK(guard.failedClosed());
    QNP_CHECK(telemetry.bgmQueueOverflow == 1);
}

QNP_TEST(BgmGuardStopTrackDrainsOnce) {
    QohBgmBoundaryGuard guard;
    AudioTelemetry telemetry{};
    guard.reset();
    QNP_REQUIRE(guard.capture(1, 30, kBgmOwnedCallsiteB, 0, kBgmStopTrack, 0,
                              telemetry));
    guard.commitFrame(30, telemetry);
    size_t executions = 0;
    guard.drainConfirmed(
        30, [&executions](const PendingBgmCommand&) { ++executions; },
        telemetry);
    guard.drainConfirmed(
        40, [&executions](const PendingBgmCommand&) { ++executions; },
        telemetry);
    QNP_CHECK(executions == 1);
}

QNP_TEST(BgmGuardSpectatorFoldKeepsLatestOnly) {
    QohBgmBoundaryGuard guard;
    AudioTelemetry telemetry{};
    guard.reset();
    QNP_REQUIRE(guard.capture(1, 10, kBgmOwnedCallsiteA, 0, 3, 0, telemetry));
    QNP_REQUIRE(guard.capture(1, 12, kBgmOwnedCallsiteA, 0, 4, 0, telemetry));
    QNP_REQUIRE(guard.capture(1, 14, kBgmOwnedCallsiteB, 0, 5, 0, telemetry));
    guard.commitFrame(10, telemetry);
    guard.commitFrame(12, telemetry);
    guard.commitFrame(14, telemetry);
    PendingBgmCommand latest{};
    QNP_REQUIRE(guard.foldForSpectator(14, latest));
    QNP_CHECK(latest.trackId == 5);
    QNP_CHECK(guard.pendingCount() == 0);
}
