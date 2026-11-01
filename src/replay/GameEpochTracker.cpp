#include "GameEpochTracker.h"

#include <limits>

namespace qreplay {

bool isValidStagedFreshGameStartV2(
    const ConfirmedGameStartV2& start) {
    if (!isValidDeterministicGameStartV2(start.start))
        return false;

    GameManifestV2 manifest = start.manifest;
    if (!manifest.stage.verified) {
        if (manifest.stage.nativeStageId != 0u
            || manifest.stage.catalogEntryId != 0u) {
            return false;
        }
        manifest.stage.nativeStageId = kMinNativeStageIdV2;
        manifest.stage.verified = true;
        manifest.stage.catalogEntryId = 1u;
    }
    return isValidGameManifestV2(manifest);
}

const char* gameEpochTrackerFaultTextV2(
    const GameEpochTrackerFaultV2 fault) {
    switch (fault) {
        case GameEpochTrackerFaultV2::None:
            return "none";
        case GameEpochTrackerFaultV2::InvalidDescriptor:
            return "invalid fresh-game descriptor";
        case GameEpochTrackerFaultV2::NonMonotonicGeneration:
            return "game generation is not monotonic";
        case GameEpochTrackerFaultV2::ConflictingLaunchBarrier:
            return "conflicting launch barrier";
        case GameEpochTrackerFaultV2::SourceFrameRegression:
            return "source frame did not advance after launch barrier";
        case GameEpochTrackerFaultV2::LaunchDidNotEnterInitialMatch:
            return "launch barrier did not enter initial scene-2 matchSub -1";
    }
    return "unknown";
}

void GameEpochTrackerV2::reset() {
    pendingStart_ = ConfirmedGameStartV2{};
    pendingSourceFrame_ = 0u;
    lastEpoch_ = 0u;
    pending_ = false;
    failed_ = false;
    lastFault_ = GameEpochTrackerFaultV2::None;
}

GameEpochTrackerResultV2 GameEpochTrackerV2::fail(
    const GameEpochTrackerFaultV2 fault) {
    pending_ = false;
    failed_ = true;
    lastFault_ = fault;
    return GameEpochTrackerResultV2::Failed;
}

GameEpochTrackerResultV2 GameEpochTrackerV2::stageLaunchBarrier(
    const uint32_t sourceFrame, const ConfirmedGameStartV2& start) {
    if (failed_) return GameEpochTrackerResultV2::Failed;
    if (sourceFrame == std::numeric_limits<uint32_t>::max()
        || !isValidStagedFreshGameStartV2(start)
        || start.start.frameZeroPreStateHash != 0u
        || start.start.initialRollingDigest != 0u) {
        return fail(GameEpochTrackerFaultV2::InvalidDescriptor);
    }

    if (pending_) {
        if (sourceFrame == pendingSourceFrame_
            && start == pendingStart_) {
            return GameEpochTrackerResultV2::ExactDuplicate;
        }
        return fail(GameEpochTrackerFaultV2::ConflictingLaunchBarrier);
    }

    const uint32_t generation =
        start.manifest.identity.gameGeneration;
    if (generation <= lastEpoch_) {
        return fail(GameEpochTrackerFaultV2::NonMonotonicGeneration);
    }

    pendingStart_ = start;
    pendingSourceFrame_ = sourceFrame;
    pending_ = true;
    lastFault_ = GameEpochTrackerFaultV2::None;
    return GameEpochTrackerResultV2::LaunchStaged;
}

GameEpochTrackerResultV2 GameEpochTrackerV2::observeBeforeFrame(
    const uint32_t sourceFrame, const GameLifecycleV2& before,
    FreshGameDescriptorV2& out) {
    if (failed_) return GameEpochTrackerResultV2::Failed;
    if (!pending_) return GameEpochTrackerResultV2::Ignored;
    if (sourceFrame <= pendingSourceFrame_) {
        return fail(GameEpochTrackerFaultV2::SourceFrameRegression);
    }

    if (before.scene == 2u && before.matchSub == -1) {
        FreshGameDescriptorV2 armed{};
        armed.gameEpoch =
            pendingStart_.manifest.identity.gameGeneration;
        armed.launchSourceFrame = pendingSourceFrame_;
        armed.start = pendingStart_;
        lastEpoch_ = armed.gameEpoch;
        pending_ = false;
        pendingStart_ = ConfirmedGameStartV2{};
        pendingSourceFrame_ = 0u;
        out = armed;
        lastFault_ = GameEpochTrackerFaultV2::None;
        return GameEpochTrackerResultV2::EpochArmed;
    }

    if (before.scene == 3u)
        return GameEpochTrackerResultV2::Ignored;
    return fail(
        GameEpochTrackerFaultV2::LaunchDidNotEnterInitialMatch);
}

}
