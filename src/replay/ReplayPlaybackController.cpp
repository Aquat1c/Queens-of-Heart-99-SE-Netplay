#include "ReplayPlaybackController.h"

namespace qreplay {

namespace {

constexpr size_t kNoIndex = static_cast<size_t>(-1);

OuterSlotPlan holdPlan() {
    OuterSlotPlan plan{};
    plan.hiddenTicks = 0u;
    plan.present = true;
    plan.hold = true;
    return plan;
}

OuterSlotPlan idlePlan() {
    OuterSlotPlan plan{};
    plan.hiddenTicks = 0u;
    plan.present = false;
    plan.hold = false;
    return plan;
}

}

const char* playbackPhaseText(PlaybackPhase phase) {
    switch (phase) {
        case PlaybackPhase::Preflight: return "preflight";
        case PlaybackPhase::Bootstrapping: return "bootstrapping";
        case PlaybackPhase::Buffering: return "buffering";
        case PlaybackPhase::CatchingUp: return "catching up";
        case PlaybackPhase::Live: return "live";
        case PlaybackPhase::SealedCatchUp: return "sealed catch-up";
        case PlaybackPhase::Complete: return "complete";
        case PlaybackPhase::Reconnecting: return "reconnecting";
        case PlaybackPhase::Rejected: return "rejected";
        case PlaybackPhase::Failed: return "failed";
    }
    return "unknown";
}

PlaybackController::PlaybackController(ConfirmedTimelineBuffer& timeline,
                                       const PlaybackTuning& tuning)
    : timeline_(timeline), tuning_(tuning) {
    if (tuning_.nominalTickMicros == 0u) tuning_.nominalTickMicros = 16501u;
    if (tuning_.maxTicksPerSlot == 0u) tuning_.maxTicksPerSlot = 1u;
    if (tuning_.slotBudgetMicros == 0u) tuning_.slotBudgetMicros = 1u;
    if (tuning_.commandQueueDepth == 0u) tuning_.commandQueueDepth = 1u;
    if (tuning_.maxSlotElapsedMicros < tuning_.nominalTickMicros) {
        tuning_.maxSlotElapsedMicros = tuning_.nominalTickMicros;
    }
    avgTickMicros_ = tuning_.initialTickCostMicros == 0u
                         ? 1u
                         : tuning_.initialTickCostMicros;
    commands_.resize(tuning_.commandQueueDepth);
    credit_ = creditThreshold() - 1u;
    deadlineReset_ = true;
}

uint64_t PlaybackController::creditThreshold() const {
    return static_cast<uint64_t>(tuning_.nominalTickMicros) * 1000ull;
}

void PlaybackController::submit(PlaybackCommand command, uint64_t argument) {
    const uint32_t depth = static_cast<uint32_t>(commands_.size());
    const uint32_t head = commandHead_.load(std::memory_order_relaxed);
    const uint32_t tail = commandTail_.load(std::memory_order_acquire);
    if (head - tail >= depth) {
        commandDrops_.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    commands_[static_cast<size_t>(head % depth)] = CommandSlot{command, argument};
    commandHead_.store(head + 1u, std::memory_order_release);
}

uint64_t PlaybackController::droppedCommands() const {
    return commandDrops_.load(std::memory_order_relaxed);
}

void PlaybackController::applyPendingCommands() {
    const uint32_t depth = static_cast<uint32_t>(commands_.size());
    for (uint32_t guard = 0u; guard < depth; ++guard) {
        const uint32_t tail = commandTail_.load(std::memory_order_relaxed);
        const uint32_t head = commandHead_.load(std::memory_order_acquire);
        if (tail == head) break;
        const CommandSlot slot = commands_[static_cast<size_t>(tail % depth)];
        commandTail_.store(tail + 1u, std::memory_order_release);
        applyCommand(slot.command, slot.argument);
    }
}

void PlaybackController::applyCommand(PlaybackCommand command,
                                      uint64_t argument) {
    switch (command) {
        case PlaybackCommand::None:
            break;
        case PlaybackCommand::Pause:
            if (!paused_) {
                paused_ = true;
                pendingSteps_ = 0u;
                resetDeadlineAfterPauseOrSeek();
            }
            break;
        case PlaybackCommand::Resume:
            if (paused_) {
                paused_ = false;
                pendingSteps_ = 0u;
                resetDeadlineAfterPauseOrSeek();
            }
            break;
        case PlaybackCommand::Step:
            if (!paused_) {
                paused_ = true;
                resetDeadlineAfterPauseOrSeek();
            }
            if (pendingSteps_ < tuning_.maxPendingSteps) ++pendingSteps_;
            break;
        case PlaybackCommand::SpeedSet: {
            const uint32_t permille = static_cast<uint32_t>(argument);
            if (!isSupportedSpeedPermille(permille)) break;
            if (permille != speedPermille_) {
                speedPermille_ = permille;
                resetDeadlineAfterPauseOrSeek();
            }
            break;
        }
        case PlaybackCommand::SeekMatch: {
            uint32_t frame = 0u;
            const uint32_t ordinal = static_cast<uint32_t>(argument);
            if (matchFirstFrame(ordinal, frame)) requestSeekToFrame(frame, ordinal);
            break;
        }
        case PlaybackCommand::SeekRound: {
            const uint32_t match = currentMatchOrdinal();
            const uint32_t round = static_cast<uint32_t>(argument);
            for (size_t i = 0; i < rounds_.size(); ++i) {
                if (rounds_[i].matchOrdinal == match
                    && rounds_[i].roundOrdinal == round) {
                    requestSeekToFrame(rounds_[i].firstTimelineFrame, match);
                    break;
                }
            }
            break;
        }
        case PlaybackCommand::Restart: {
            if (argument == 1u) {
                const uint32_t ordinal =
                    matches_.empty() ? 0u : matches_.front().ordinal;
                requestSeekToFrame(timeline_.firstFrame(), ordinal);
                break;
            }
            uint32_t frame = 0u;
            const uint32_t ordinal = currentMatchOrdinal();
            if (matchFirstFrame(ordinal, frame)) {
                requestSeekToFrame(frame, ordinal);
            } else {
                requestSeekToFrame(timeline_.firstFrame(), ordinal);
            }
            break;
        }
        case PlaybackCommand::NextMatch:
        case PlaybackCommand::PreviousMatch: {
            if (matches_.empty()) break;
            const uint32_t current = currentMatchOrdinal();
            size_t index = kNoIndex;
            for (size_t i = 0; i < matches_.size(); ++i) {
                if (matches_[i].ordinal == current) { index = i; break; }
            }
            if (index == kNoIndex) index = 0u;
            if (command == PlaybackCommand::NextMatch) {
                if (index + 1u >= matches_.size()) break;
                ++index;
            } else {
                if (index == 0u) break;
                --index;
            }
            requestSeekToFrame(matches_[index].firstTimelineFrame,
                               matches_[index].ordinal);
            break;
        }
        case PlaybackCommand::Exit:
            exitRequested_ = true;
            break;
    }
}

bool PlaybackController::addMatchMarker(uint32_t ordinal, uint32_t firstFrame,
                                        uint32_t lastFrame) {
    if (matches_.size() >= kMaxMatchesPerSet) return false;
    if (!matches_.empty()) {
        const PlaybackMatchMarker& back = matches_.back();
        if (firstFrame < back.firstTimelineFrame) return false;
        if (ordinal <= back.ordinal) return false;
    }
    if (lastFrame < firstFrame) return false;
    matches_.push_back(PlaybackMatchMarker{ordinal, firstFrame, lastFrame});
    return true;
}

bool PlaybackController::addRoundMarker(uint32_t matchOrdinal,
                                        uint32_t roundOrdinal,
                                        uint32_t firstFrame) {
    if (rounds_.size() >= kMaxPlaybackRoundMarkers) return false;
    if (!rounds_.empty()
        && firstFrame < rounds_.back().firstTimelineFrame) {
        return false;
    }
    rounds_.push_back(
        PlaybackRoundMarker{matchOrdinal, roundOrdinal, firstFrame});
    return true;
}

void PlaybackController::clearMarkers() {
    matches_.clear();
    rounds_.clear();
}

uint32_t PlaybackController::matchCount() const {
    return static_cast<uint32_t>(matches_.size());
}

uint32_t PlaybackController::currentMatchOrdinal() const {
    const uint32_t frame = timeline_.consumeCursor();
    uint32_t ordinal = 0u;
    for (size_t i = 0; i < matches_.size(); ++i) {
        if (matches_[i].firstTimelineFrame <= frame) {
            ordinal = matches_[i].ordinal;
        } else {
            break;
        }
    }
    return ordinal;
}

bool PlaybackController::matchFirstFrame(uint32_t ordinal,
                                         uint32_t& frame) const {
    for (size_t i = 0; i < matches_.size(); ++i) {
        if (matches_[i].ordinal == ordinal) {
            frame = matches_[i].firstTimelineFrame;
            return true;
        }
    }
    return false;
}

void PlaybackController::requestSeekToFrame(uint32_t frame,
                                            uint32_t matchOrdinal) {
    seekActive_ = true;
    seekTaken_ = false;
    fastForward_ = false;
    seekTargetFrame_ = frame;
    seekMatchOrdinal_ = matchOrdinal;
    pendingSteps_ = 0u;
    resetDeadlineAfterPauseOrSeek();
}

bool PlaybackController::takeSeekRequest(SeekRequest& out) {
    if (!seekActive_) return false;
    out.active = true;
    out.targetFrame = seekTargetFrame_;
    out.matchOrdinal = seekMatchOrdinal_;
    seekActive_ = false;
    seekTaken_ = true;
    return true;
}

bool PlaybackController::completeSeek(uint32_t positionedFrame) {
    ParseStatus why = ParseStatus::Ok;
    if (!timeline_.rewindConsumeTo(positionedFrame, why)) {
        seekTaken_ = false;
        fastForward_ = false;
        noteFailure(why, timeline_.lastFault());
        return false;
    }
    seekTaken_ = false;
    fastForward_ = positionedFrame < seekTargetFrame_;
    resetDeadlineAfterPauseOrSeek();
    return true;
}

void PlaybackController::failSeek(ParseStatus why) {
    seekActive_ = false;
    seekTaken_ = false;
    fastForward_ = false;
    noteFailure(why);
}

uint32_t PlaybackController::budgetTickCap() const {
    const uint32_t cost = avgTickMicros_ == 0u ? 1u : avgTickMicros_;
    uint32_t cap = tuning_.slotBudgetMicros / cost;
    if (cap == 0u) cap = 1u;
    if (cap > tuning_.maxTicksPerSlot) cap = tuning_.maxTicksPerSlot;
    return cap;
}

void PlaybackController::noteTickExecuted(uint64_t tickMicros) {
    slotMicrosUsed_ += tickMicros;
    const uint64_t smoothed =
        (static_cast<uint64_t>(avgTickMicros_) * 3ull + tickMicros) / 4ull;
    avgTickMicros_ = smoothed == 0u ? 1u
                                    : static_cast<uint32_t>(
                                          smoothed > 0xFFFFFFFFull
                                              ? 0xFFFFFFFFull
                                              : smoothed);
}

bool PlaybackController::slotBudgetRemaining() const {
    return slotMicrosUsed_ < tuning_.slotBudgetMicros;
}

void PlaybackController::resetDeadlineAfterPauseOrSeek() {
    credit_ = creditThreshold() - 1u;
    deadlineReset_ = true;
}

void PlaybackController::beginBootstrap() {
    hostPhase_ = PlaybackPhase::Bootstrapping;
}

void PlaybackController::beginPlayback() {
    hostPhase_ = PlaybackPhase::Live;
    resetDeadlineAfterPauseOrSeek();
}

void PlaybackController::noteReconnecting() {
    hostPhase_ = PlaybackPhase::Reconnecting;
    resetDeadlineAfterPauseOrSeek();
}

void PlaybackController::noteRejected(ParseStatus why) {
    hostPhase_ = PlaybackPhase::Rejected;
    failure_ = why;
}

void PlaybackController::noteFailure(ParseStatus why) {
    noteFailure(why, TimelineFault::None);
}

void PlaybackController::noteFailure(ParseStatus why, TimelineFault fault) {
    hostPhase_ = PlaybackPhase::Failed;
    failure_ = why;
    lastFault_ = fault;
}

void PlaybackController::clearFailure() {
    if (hostPhase_ == PlaybackPhase::Failed
        || hostPhase_ == PlaybackPhase::Rejected) {
        hostPhase_ = PlaybackPhase::Live;
    }
    failure_ = ParseStatus::Ok;
    lastFault_ = TimelineFault::None;
}

OuterSlotPlan PlaybackController::planOuterSlot(
    uint64_t elapsedMicrosSinceLastSlot) {
    slotMicrosUsed_ = 0u;
    slotTicksPlanned_ = 0u;
    slotTicksAllowed_ = budgetTickCap();

    applyPendingCommands();

    if (hostPhase_ == PlaybackPhase::Failed
        || hostPhase_ == PlaybackPhase::Rejected) {
        return holdPlan();
    }
    if (hostPhase_ == PlaybackPhase::Preflight
        || hostPhase_ == PlaybackPhase::Bootstrapping) {
        return idlePlan();
    }
    if (hostPhase_ == PlaybackPhase::Reconnecting) return holdPlan();

    if (seekActive_ || seekTaken_) return holdPlan();

    const size_t buffered = timeline_.bufferedFrames();

    if (fastForward_) {
        const uint32_t cursor = timeline_.consumeCursor();
        if (cursor >= seekTargetFrame_) {
            fastForward_ = false;
            resetDeadlineAfterPauseOrSeek();
            return holdPlan();
        }
        uint64_t remaining = static_cast<uint64_t>(seekTargetFrame_) - cursor;
        if (remaining > slotTicksAllowed_) remaining = slotTicksAllowed_;
        if (remaining > buffered) remaining = buffered;
        if (remaining == 0u) return holdPlan();
        OuterSlotPlan plan{};
        plan.hiddenTicks = static_cast<uint32_t>(remaining);
        plan.present = false;
        plan.hold = false;
        slotTicksPlanned_ = plan.totalTicks();
        return plan;
    }

    if (paused_) {
        if (pendingSteps_ > 0u && buffered > 0u) {
            --pendingSteps_;
            OuterSlotPlan plan{};
            plan.hiddenTicks = 0u;
            plan.present = true;
            plan.hold = false;
            slotTicksPlanned_ = 1u;
            return plan;
        }
        return holdPlan();
    }

    uint64_t elapsed = elapsedMicrosSinceLastSlot;
    if (deadlineReset_) {
        elapsed = tuning_.nominalTickMicros;
        deadlineReset_ = false;
    } else if (elapsed > tuning_.maxSlotElapsedMicros) {
        elapsed = tuning_.maxSlotElapsedMicros;
    }
    credit_ += elapsed * static_cast<uint64_t>(speedPermille_);

    const uint64_t threshold = creditThreshold();
    uint64_t owed = credit_ / threshold;
    credit_ %= threshold;

    if (owed > slotTicksAllowed_) owed = slotTicksAllowed_;
    if (owed > buffered) owed = buffered;

    if (owed == 0u) {
        return holdPlan();
    }

    OuterSlotPlan plan{};
    plan.hiddenTicks = static_cast<uint32_t>(owed - 1u);
    plan.present = true;
    plan.hold = false;
    slotTicksPlanned_ = plan.totalTicks();
    return plan;
}

PlaybackPhase PlaybackController::phase() const {
    switch (hostPhase_) {
        case PlaybackPhase::Preflight:
        case PlaybackPhase::Bootstrapping:
        case PlaybackPhase::Reconnecting:
        case PlaybackPhase::Rejected:
        case PlaybackPhase::Failed:
            return hostPhase_;
        default:
            break;
    }
    const TimelineTerminal& terminal = timeline_.terminal();
    const size_t buffered = timeline_.bufferedFrames();
    if (terminal.present && buffered == 0u
        && timeline_.consumeCursor() >= terminal.nextRecord) {
        return PlaybackPhase::Complete;
    }
    if (buffered == 0u) return PlaybackPhase::Buffering;
    if (seeking() || speedPermille_ > kSpeedNormal) {
        return terminal.present ? PlaybackPhase::SealedCatchUp
                                : PlaybackPhase::CatchingUp;
    }
    return PlaybackPhase::Live;
}

PlaybackStatus PlaybackController::status() const {
    PlaybackStatus out{};
    out.phase = phase();
    out.currentFrame = timeline_.consumeCursor();
    uint32_t verified = 0u;
    out.lastVerifiedFrame = timeline_.lastConsumedFrame(verified)
                                ? verified
                                : timeline_.firstFrame();
    out.matchOrdinal = currentMatchOrdinal();
    out.matchCount = matchCount();
    out.speedPermille = speedPermille_;
    out.paused = paused_;
    out.seeking = seeking();
    out.buffering = out.phase == PlaybackPhase::Buffering;
    out.bufferedFrames = timeline_.bufferedFrames();
    out.rollingDigest = timeline_.rollingDigest();
    out.failure = failure_;
    return out;
}

}
