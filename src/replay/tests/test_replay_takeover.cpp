#include "replay/ReplayTakeoverController.h"
#include "TestHarness.h"

using namespace qreplay;

namespace {

CanonicalTickBundleV2 tick(uint32_t frame, uint16_t p1, uint16_t p2) {
    CanonicalTickBundleV2 value{};
    value.frame.replayFrame = frame;
    value.frame.sourceFrame = 100u + frame;
    value.frame.core.activeLaneMask = 3u;
    value.frame.core.actions[0] = p1;
    value.frame.core.actions[1] = p2;
    return value;
}

}

QNP_TEST(replay_takeover_countdown_and_recorded_fallback) {
    ReplayTakeoverController takeover;
    QNP_REQUIRE(takeover.prepare(128u));
    QNP_REQUIRE(takeover.begin(7u, 107u, 0u));
    QNP_CHECK(takeover.phase() == ReplayTakeoverPhase::Countdown);
    for (uint32_t i = 1u; i < kReplayTakeoverCountdownFrames; ++i)
        QNP_CHECK(!takeover.advanceCountdown());
    QNP_CHECK(takeover.advanceCountdown());

    QNP_REQUIRE(takeover.capture(0x0101u));
    const CanonicalTickBundleV2 recorded = tick(7u, 0x0002u, 0x0004u);
    ReplayTakeoverResolvedFrame resolved{};
    QNP_REQUIRE(takeover.resolve(&recorded, 3u, resolved));
    QNP_CHECK(resolved.actions[0] == 0x0101u);
    QNP_CHECK(resolved.actions[1] == 0x0004u);
    QNP_CHECK(resolved.sourceFrame == 107u);
}

QNP_TEST(replay_takeover_delay_gap_fill_and_endpoint_neutral) {
    ReplayTakeoverController takeover;
    QNP_REQUIRE(takeover.prepare(64u));
    QNP_REQUIRE(takeover.begin(10u, 200u, 1u));
    for (uint32_t i = 0u; i < kReplayTakeoverCountdownFrames; ++i)
        (void)takeover.advanceCountdown();
    QNP_REQUIRE(takeover.setDelay(2u));
    QNP_REQUIRE(takeover.capture(0x0202u));

    ReplayTakeoverResolvedFrame resolved{};
    QNP_REQUIRE(takeover.resolve(nullptr, 3u, resolved));
    QNP_CHECK(resolved.actions[1] == 0u);
    QNP_CHECK(resolved.beyondEndpoint);
    QNP_REQUIRE(takeover.advance());
    QNP_REQUIRE(takeover.capture(0x0404u));
    QNP_REQUIRE(takeover.advance());
    QNP_REQUIRE(takeover.resolve(nullptr, 3u, resolved));
    QNP_CHECK(resolved.actions[1] == 0x0202u);
    QNP_REQUIRE(takeover.advance());
    QNP_REQUIRE(takeover.resolve(nullptr, 3u, resolved));
    QNP_CHECK(resolved.actions[1] == 0x0404u);
}

QNP_TEST(replay_takeover_delay_decrease_does_not_rewrite_earlier_frames) {
    ReplayTakeoverController takeover;
    QNP_REQUIRE(takeover.prepare(64u));
    QNP_REQUIRE(takeover.begin(10u, 20u, 0u));
    for (uint32_t i = 0u; i < kReplayTakeoverCountdownFrames; ++i)
        (void)takeover.advanceCountdown();

    QNP_REQUIRE(takeover.setDelay(3u));
    QNP_REQUIRE(takeover.capture(0x0001u));
    QNP_REQUIRE(takeover.setDelay(0u));
    QNP_REQUIRE(takeover.capture(0x0002u));

    CanonicalTickBundleV2 recorded{};
    recorded.frame.core.activeLaneMask = 3u;
    recorded.frame.core.actions[0] = 0x0004u;
    ReplayTakeoverResolvedFrame resolved{};
    QNP_REQUIRE(takeover.resolve(&recorded, 3u, resolved));
    QNP_CHECK(resolved.actions[0] == 0x0004u);
}

QNP_TEST(replay_takeover_retry_and_switch_retain_both_branches) {
    ReplayTakeoverController takeover;
    QNP_REQUIRE(takeover.prepare(128u));
    QNP_REQUIRE(takeover.begin(3u, 103u, 0u));
    for (uint32_t i = 0u; i < kReplayTakeoverCountdownFrames; ++i)
        (void)takeover.advanceCountdown();
    QNP_REQUIRE(takeover.capture(0x0001u));
    QNP_REQUIRE(takeover.advance());

    QNP_REQUIRE(takeover.requestSwitch(1u));
    QNP_REQUIRE(takeover.completeRestore(true));
    for (uint32_t i = 0u; i < kReplayTakeoverCountdownFrames; ++i)
        (void)takeover.advanceCountdown();
    QNP_REQUIRE(takeover.capture(0x0002u));
    ReplayTakeoverResolvedFrame resolved{};
    const CanonicalTickBundleV2 recorded = tick(3u, 0u, 0u);
    QNP_REQUIRE(takeover.resolve(&recorded, 3u, resolved));
    QNP_CHECK(resolved.actions[0] == 0x0001u);
    QNP_CHECK(resolved.actions[1] == 0x0002u);

    QNP_REQUIRE(takeover.requestRetry());
    QNP_REQUIRE(takeover.completeRestore(true));
    QNP_CHECK(takeover.elapsedFrames() == 0u);
    QNP_CHECK(takeover.selectedLane() == 1u);
}

QNP_TEST(replay_takeover_exit_discards_branch_only_after_restore) {
    ReplayTakeoverController takeover;
    QNP_REQUIRE(takeover.prepare(16u));
    QNP_REQUIRE(takeover.begin(1u, 1u, 0u));
    QNP_REQUIRE(takeover.requestExit());
    QNP_CHECK(!takeover.completeRestore(false));
    QNP_CHECK(takeover.phase() == ReplayTakeoverPhase::Faulted);

    takeover.reset();
    QNP_REQUIRE(takeover.begin(1u, 1u, 0u));
    QNP_REQUIRE(takeover.requestExit());
    QNP_REQUIRE(takeover.completeRestore(true));
    QNP_CHECK(takeover.phase() == ReplayTakeoverPhase::Inactive);
}

QNP_TEST(replay_takeover_simulation_fault_keeps_exit_restore_available) {
    ReplayTakeoverController takeover;
    QNP_REQUIRE(takeover.prepare(16u));
    QNP_REQUIRE(takeover.begin(2u, 12u, 0u));
    for (uint32_t i = 0u; i < kReplayTakeoverCountdownFrames; ++i)
        (void)takeover.advanceCountdown();

    QNP_REQUIRE(takeover.failSimulation());
    QNP_CHECK(takeover.phase() == ReplayTakeoverPhase::Faulted);
    QNP_CHECK(takeover.fault() == ReplayTakeoverFault::SimulationFailed);
    QNP_CHECK(!takeover.requestRetry());
    QNP_REQUIRE(takeover.requestExit());
    QNP_CHECK(takeover.restoreReason()
              == ReplayTakeoverRestoreReason::Exit);
    QNP_REQUIRE(takeover.completeRestore(true));
    QNP_CHECK(takeover.phase() == ReplayTakeoverPhase::Inactive);
}

QNP_TEST(replay_takeover_delay_clamps_and_capacity_fails_closed) {
    ReplayTakeoverController takeover;
    QNP_REQUIRE(takeover.prepare(2u));
    QNP_REQUIRE(takeover.begin(0u, 0u, 0u));
    for (uint32_t i = 0u; i < 20u; ++i) takeover.increaseDelay();
    QNP_CHECK(takeover.delay() == 9u);
    for (uint32_t i = 0u; i < 20u; ++i) takeover.decreaseDelay();
    QNP_CHECK(takeover.delay() == 0u);
    for (uint32_t i = 0u; i < kReplayTakeoverCountdownFrames; ++i)
        (void)takeover.advanceCountdown();
    QNP_REQUIRE(takeover.capture(0u));
    QNP_REQUIRE(takeover.advance());
    QNP_CHECK(!takeover.advance());
    QNP_CHECK(takeover.fault() == ReplayTakeoverFault::CapacityExceeded);
}
