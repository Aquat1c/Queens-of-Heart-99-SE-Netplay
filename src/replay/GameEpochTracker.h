#pragma once

#include "ReplayTypes.h"

#include <cstdint>

namespace qreplay {

struct FreshGameDescriptorV2 {
    uint32_t gameEpoch = 0u;
    uint32_t launchSourceFrame = 0u;
    ConfirmedGameStartV2 start{};
};

bool isValidStagedFreshGameStartV2(
    const ConfirmedGameStartV2& start);

enum class GameEpochTrackerFaultV2 : uint8_t {
    None = 0,
    InvalidDescriptor,
    NonMonotonicGeneration,
    ConflictingLaunchBarrier,
    SourceFrameRegression,
    LaunchDidNotEnterInitialMatch,
};

enum class GameEpochTrackerResultV2 : uint8_t {
    Ignored = 0,
    LaunchStaged,
    ExactDuplicate,
    EpochArmed,
    Failed,
};

const char* gameEpochTrackerFaultTextV2(GameEpochTrackerFaultV2 fault);

class GameEpochTrackerV2 {
public:
    void reset();

    GameEpochTrackerResultV2 stageLaunchBarrier(
        uint32_t sourceFrame, const ConfirmedGameStartV2& start);

    GameEpochTrackerResultV2 observeBeforeFrame(
        uint32_t sourceFrame, const GameLifecycleV2& before,
        FreshGameDescriptorV2& out);

    bool hasPendingLaunch() const { return pending_; }
    bool failed() const { return failed_; }
    uint32_t lastGameEpoch() const { return lastEpoch_; }
    GameEpochTrackerFaultV2 lastFault() const { return lastFault_; }

private:
    GameEpochTrackerResultV2 fail(GameEpochTrackerFaultV2 fault);

    ConfirmedGameStartV2 pendingStart_{};
    uint32_t pendingSourceFrame_ = 0u;
    uint32_t lastEpoch_ = 0u;
    bool pending_ = false;
    bool failed_ = false;
    GameEpochTrackerFaultV2 lastFault_ = GameEpochTrackerFaultV2::None;
};

}
