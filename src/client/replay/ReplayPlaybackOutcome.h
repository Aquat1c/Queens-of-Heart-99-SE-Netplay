#pragma once

#include "ReplayBrokerV2.h"

namespace qclient::replay {

inline bool replayClosedBeforeResultV2(
    bool interactive, bool cleanNativeExit,
    const ReplayBrokerRunResultV2& result) {
    return interactive && cleanNativeExit && !result.hasPlaybackResult
        && (result.state == ReplayBrokerStateV2::MissingResult
            || result.state == ReplayBrokerStateV2::ProcessExitedWithoutResult);
}

inline const char* replayPlaybackCompletionTextV2(
    qreplay::ipc::v3::PlaybackResultReason reason) {
    using Reason = qreplay::ipc::v3::PlaybackResultReason;
    switch (reason) {
        case Reason::CompletedWithDivergence:
            return "Playback finished with differences from the recording.";
        case Reason::VerifiedBehaviourOnly:
            return "verified complete (behaviour-verified: the byte-exact "
                   "state proof does not apply to this recording) - the "
                   "recorded inputs, lifecycle, dispatcher results, digest "
                   "chain and terminal all reproduced exactly";
        case Reason::VerifiedCompleted:
        case Reason::VerifiedAbnormalEnd:
            return "verified complete (byte-exact: every recorded pre/post "
                   "state hash reproduced bit for bit)";
        default:
            return "Playback finished.";
    }
}

}
