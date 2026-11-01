#pragma once

#include "replay/ReplayIpcWire.h"
#include <cstdint>

namespace qreplay {

inline bool replayCompletedGame(const ipc::v3::PlaybackResultPayload& result) {
    return result.hasRecordedTerminal
        && result.recordedTerminal == static_cast<uint16_t>(TerminalReason::Completed)
        && (result.reason == ipc::v3::PlaybackResultReason::VerifiedCompleted
            || result.reason == ipc::v3::PlaybackResultReason::VerifiedBehaviourOnly
            || result.reason == ipc::v3::PlaybackResultReason::CompletedWithDivergence);
}

inline ipc::v3::PlaybackByeReason replayWinScreenByeReason(
    ipc::v3::PlaybackResultReason sessionReason, bool sessionFailed,
    bool winScreenFailed) {
    if (winScreenFailed)
        return ipc::v3::PlaybackByeReason::FailureAfterResult;
    return sessionReason == ipc::v3::PlaybackResultReason::UserExit
        ? ipc::v3::PlaybackByeReason::UserExitAfterResult
        : sessionFailed ? ipc::v3::PlaybackByeReason::FailureAfterResult
                        : ipc::v3::PlaybackByeReason::NormalAfterResult;
}

enum class ReplayWinScreenAction { Advance, Hold, Fail };

constexpr bool replayHitboxPresentationAllowed(uint32_t scene, int32_t matchSub) {
    return scene == 2u && matchSub == 0;
}

constexpr ReplayWinScreenAction replayWinScreenAction(
    uint32_t scene, int32_t matchSub, uint32_t formationRow,
    uint32_t resultCounter, bool bannerSettled, uint32_t ticks) {
    if (scene != 2u || (formationRow != 1u && formationRow != 2u))
        return ReplayWinScreenAction::Fail;
    if (matchSub == 5 || (matchSub == 2 && resultCounter >= 61u && bannerSettled))
        return ReplayWinScreenAction::Hold;
    if (matchSub == 2 && ticks < 600u)
        return ReplayWinScreenAction::Advance;
    return ReplayWinScreenAction::Fail;
}

constexpr bool replayFinalHoldElapsed(
    uint64_t now, uint64_t start, bool holdFinal) {
    return !holdFinal || (now >= start && now - start >= 1000u);
}

}
