#pragma once

#include "ReplayTimeline.h"
#include "ReplayTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qreplay {

enum class PlaybackPhase : uint8_t {
    Preflight,
    Bootstrapping,
    Buffering,
    CatchingUp,
    Live,
    SealedCatchUp,
    Complete,
    Reconnecting,
    Rejected,
    Failed,
};

const char* playbackPhaseText(PlaybackPhase phase);

enum class PlaybackCommand : uint8_t {
    None = 0,
    Pause,
    Resume,
    Step,
    SpeedSet,
    SeekMatch,
    SeekRound,
    Restart,
    NextMatch,
    PreviousMatch,
    Exit,
};

constexpr uint32_t kSpeedQuarter = 250u;
constexpr uint32_t kSpeedHalf = 500u;
constexpr uint32_t kSpeedNormal = 1000u;
constexpr uint32_t kSpeedDouble = 2000u;
constexpr uint32_t kSpeedQuadruple = 4000u;
constexpr uint32_t kSpeedOctuple = 8000u;

constexpr bool isSupportedSpeedPermille(uint32_t permille) {
    return permille == kSpeedQuarter || permille == kSpeedHalf
        || permille == kSpeedNormal || permille == kSpeedDouble
        || permille == kSpeedQuadruple || permille == kSpeedOctuple;
}

struct PlaybackStatus {
    PlaybackPhase phase = PlaybackPhase::Preflight;
    uint32_t currentFrame = 0u;
    uint32_t lastVerifiedFrame = 0u;
    uint32_t matchOrdinal = 0u;
    uint32_t matchCount = 0u;
    uint32_t speedPermille = kSpeedNormal;
    bool paused = false;
    bool seeking = false;
    bool buffering = false;
    size_t bufferedFrames = 0u;
    uint64_t rollingDigest = 0u;
    ParseStatus failure = ParseStatus::Ok;
};

struct OuterSlotPlan {
    uint32_t hiddenTicks = 0u;
    bool present = false;
    bool hold = false;

    uint32_t totalTicks() const {
        return hiddenTicks + ((present && !hold) ? 1u : 0u);
    }
};

struct PlaybackTuning {
    // QOH's nominal presentation period: 1 / 60.6 Hz = 16501.65 us.
    uint32_t nominalTickMicros = 16501u;
    uint32_t slotBudgetMicros = 12000u;
    uint32_t maxTicksPerSlot = 16u;
    uint32_t maxSlotElapsedMicros = 66004u;
    uint32_t initialTickCostMicros = 1200u;
    uint32_t commandQueueDepth = 32u;
    uint32_t maxPendingSteps = 8u;
};

struct SeekRequest {
    bool active = false;
    uint32_t targetFrame = 0u;
    uint32_t matchOrdinal = 0u;
};

struct PlaybackMatchMarker {
    uint32_t ordinal = 0u;
    uint32_t firstTimelineFrame = 0u;
    uint32_t lastTimelineFrame = 0u;
};

struct PlaybackRoundMarker {
    uint32_t matchOrdinal = 0u;
    uint32_t roundOrdinal = 0u;
    uint32_t firstTimelineFrame = 0u;
};

constexpr size_t kMaxPlaybackRoundMarkers = 16384u;

class PlaybackController {
public:
    explicit PlaybackController(ConfirmedTimelineBuffer& timeline,
                                const PlaybackTuning& tuning = PlaybackTuning{});

    void submit(PlaybackCommand command, uint64_t argument);
    uint64_t droppedCommands() const;

    OuterSlotPlan planOuterSlot(uint64_t elapsedMicrosSinceLastSlot);

    void noteTickExecuted(uint64_t tickMicros);

    bool slotBudgetRemaining() const;

    void resetDeadlineAfterPauseOrSeek();

    PlaybackStatus status() const;
    PlaybackPhase phase() const;
    uint32_t speedPermille() const { return speedPermille_; }
    bool paused() const { return paused_; }
    bool seeking() const { return seekActive_ || fastForward_; }
    bool exitRequested() const { return exitRequested_; }
    ParseStatus failure() const { return failure_; }
    TimelineFault lastFault() const { return lastFault_; }

    void beginBootstrap();
    void beginPlayback();
    void noteReconnecting();
    void noteRejected(ParseStatus why);
    void noteFailure(ParseStatus why);
    void noteFailure(ParseStatus why, TimelineFault fault);
    void clearFailure();

    bool addMatchMarker(uint32_t ordinal, uint32_t firstFrame,
                        uint32_t lastFrame);
    bool addRoundMarker(uint32_t matchOrdinal, uint32_t roundOrdinal,
                        uint32_t firstFrame);
    void clearMarkers();
    uint32_t matchCount() const;
    uint32_t currentMatchOrdinal() const;

    bool takeSeekRequest(SeekRequest& out);
    bool completeSeek(uint32_t positionedFrame);
    void failSeek(ParseStatus why);

private:
    struct CommandSlot {
        PlaybackCommand command = PlaybackCommand::None;
        uint64_t argument = 0u;
    };

    void applyPendingCommands();
    void applyCommand(PlaybackCommand command, uint64_t argument);
    void requestSeekToFrame(uint32_t frame, uint32_t matchOrdinal);
    bool matchFirstFrame(uint32_t ordinal, uint32_t& frame) const;
    uint32_t budgetTickCap() const;
    uint64_t creditThreshold() const;

    ConfirmedTimelineBuffer& timeline_;
    PlaybackTuning tuning_{};

    std::vector<CommandSlot> commands_;
    std::atomic<uint32_t> commandHead_{0u};
    std::atomic<uint32_t> commandTail_{0u};
    std::atomic<uint64_t> commandDrops_{0u};

    std::vector<PlaybackMatchMarker> matches_;
    std::vector<PlaybackRoundMarker> rounds_;

    uint64_t credit_ = 0u;
    bool deadlineReset_ = true;

    uint32_t speedPermille_ = kSpeedNormal;
    bool paused_ = false;
    uint32_t pendingSteps_ = 0u;
    bool exitRequested_ = false;

    bool seekActive_ = false;
    bool seekTaken_ = false;
    bool fastForward_ = false;
    uint32_t seekTargetFrame_ = 0u;
    uint32_t seekMatchOrdinal_ = 0u;

    uint32_t avgTickMicros_ = 1u;
    uint64_t slotMicrosUsed_ = 0u;
    uint32_t slotTicksAllowed_ = 0u;
    uint32_t slotTicksPlanned_ = 0u;

    PlaybackPhase hostPhase_ = PlaybackPhase::Preflight;
    ParseStatus failure_ = ParseStatus::Ok;
    TimelineFault lastFault_ = TimelineFault::None;
};

}
