#include "game/replay/ReplayTimelineSource.h"

#include "Log.h"

#include <algorithm>
#include <cstring>

namespace qgame::replay {

const char* sourceStateText(SourceState state) {
    switch (state) {
        case SourceState::Idle: return "idle";
        case SourceState::WaitingForSession: return "waiting-session";
        case SourceState::WaitingForStart: return "waiting-start";
        case SourceState::Streaming: return "streaming";
        case SourceState::Sealed: return "sealed";
        case SourceState::Faulted: return "faulted";
    }
    return "unknown";
}

ReplayTimelineSource::ReplayTimelineSource()
    : timeline_(new (std::nothrow) qreplay::ConfirmedTimelineBuffer(1u)) {}

ReplayTimelineSource::~ReplayTimelineSource() { stop(); }

bool ReplayTimelineSource::start(ReplayIpcWorker& worker,
                                 uint64_t expectedManifestDigest,
                                 const TimelineSourceOptions& options) {
    stop();

    options_ = options;
    if (options_.timelineCapacityFrames == 0u)
        options_.timelineCapacityFrames = 1u;
    if (options_.pumpFrameBudget == 0u) options_.pumpFrameBudget = 1u;
    if (options_.snapshotIntervalFrames == 0u)
        options_.snapshotIntervalFrames = 1u;

    worker_ = &worker;
    expectedManifestDigest_ = expectedManifestDigest;

    timeline_.reset(new (std::nothrow)
                        qreplay::ConfirmedTimelineBuffer(
                            options_.timelineCapacityFrames));
    if (!timeline_) return false;

    snapshots_.clear();
    if (options_.snapshotSlots > 0u && options_.snapshotSlotBytes > 0u) {
        snapshots_.resize(options_.snapshotSlots);
        for (SnapshotSlot& slot : snapshots_) {
            slot.buffer.assign(options_.snapshotSlotBytes, 0u);
            slot.used = false;
            slot.frame = 0u;
            slot.bytes = 0u;
        }
    }
    snapshotRoundRobin_ = 0u;
    lastSnapshotFrame_ = 0u;
    haveSnapshotFrame_ = false;
    snapshotCaptures_ = 0u;
    snapshotRestores_ = 0u;
    matchStarts_.clear();

    session_ = BrokerSessionInfo{};
    descriptor_ = qreplay::DeterministicStartDescriptor{};
    sessionKnown_ = false;
    startInstalled_ = false;
    pendingFrameValid_ = false;
    pendingEventValid_ = false;
    pendingTerminalValid_ = false;
    fault_ = SourceFault::None;
    faultDetail_ = 0u;
    timelineStatus_ = qreplay::ParseStatus::Ok;
    state_ = SourceState::WaitingForSession;
    started_ = true;
    return true;
}

void ReplayTimelineSource::stop() {
    for (SnapshotSlot& slot : snapshots_) {
        if (!slot.buffer.empty())
            std::memset(slot.buffer.data(), 0, slot.buffer.size());
        slot.used = false;
        slot.frame = 0u;
        slot.bytes = 0u;
    }
    snapshots_.clear();
    snapshots_.shrink_to_fit();

    worker_ = nullptr;
    started_ = false;
    sessionKnown_ = false;
    startInstalled_ = false;
    state_ = SourceState::Idle;
    pendingFrameValid_ = false;
    pendingEventValid_ = false;
    pendingTerminalValid_ = false;
    matchStarts_.clear();
}

bool ReplayTimelineSource::addMatchStart(uint32_t ordinal,
                                         uint32_t firstTimelineFrame) {
    if (matchStarts_.size() >= qreplay::kMaxMatchesPerSet) return false;
    for (const MatchStartPoint& point : matchStarts_)
        if (point.ordinal == ordinal) return false;
    MatchStartPoint point{};
    point.ordinal = ordinal;
    point.firstFrame = firstTimelineFrame;
    matchStarts_.push_back(point);
    std::sort(matchStarts_.begin(), matchStarts_.end(),
              [](const MatchStartPoint& a, const MatchStartPoint& b) {
                  return a.firstFrame < b.firstFrame;
              });
    return true;
}

void ReplayTimelineSource::setFault(SourceFault fault, uint32_t detail) {
    if (fault_ != SourceFault::None) return;
    fault_ = fault;
    faultDetail_ = detail;
    state_ = SourceState::Faulted;
    QLOG("[replay.verify] mismatch frame=%u expected=- actual=- "
         "region=source(%s)",
         detail, sourceFaultText(fault));
}

qreplay::TimelineFault ReplayTimelineSource::timelineFault() const {
    return timeline_ ? timeline_->lastFault() : qreplay::TimelineFault::None;
}

void ReplayTimelineSource::installStart(
    const qreplay::DeterministicStart& start) {
    qreplay::DeterministicStartDescriptor desc{};
    desc.start = start;
    desc.contract.runtimeSimulationId = session_.simulationContractId;
    desc.bootstrapKind =
        static_cast<qreplay::BootstrapKind>(session_.bootstrapKind);
    desc.vsRow = session_.vsRow;
    desc.cpuMask = session_.cpuMask;
    desc.slotToSourceRoute = session_.slotToSourceRoute;
    desc.activeLaneMask = session_.activeLaneMask;
    desc.activeSlotMask = session_.activeSlotMask;
    desc.bootstrapFingerprint = session_.bootstrapFingerprint;

    descriptor_ = desc;
    timeline_->reset(desc);
    startInstalled_ = true;
    state_ = SourceState::Streaming;

    QLOG("[replay.play] session=%016llX match=%u contract=%016llX "
         "content=%016llX startFrame=%u",
         static_cast<unsigned long long>(session_.sessionUuidLow),
         session_.matchOrdinal,
         static_cast<unsigned long long>(session_.simulationContractId),
         static_cast<unsigned long long>(session_.contentDigest),
         start.firstTimelineFrame);
}

void ReplayTimelineSource::applyControl(const BrokerControl& control) {
    switch (control.kind) {
        case BrokerControl::Kind::SessionInfo: {
            if (expectedManifestDigest_ != 0u
                && control.info.manifestDigest != expectedManifestDigest_) {
                setFault(SourceFault::ManifestMismatch, 0u);
                return;
            }
            session_ = control.info;
            sessionKnown_ = true;
            if (state_ == SourceState::WaitingForSession)
                state_ = SourceState::WaitingForStart;
            return;
        }
        case BrokerControl::Kind::Start: {
            if (!sessionKnown_) {
                setFault(SourceFault::UnexpectedMessage, 0u);
                return;
            }
            installStart(control.start);
            return;
        }
        case BrokerControl::Kind::Terminal: {
            qreplay::TimelineTerminal terminal{};
            terminal.present = true;
            terminal.nextRecord = control.lastTimelineFrame;
            terminal.finalDigest = control.rollingDigest;
            terminal.postTailStateHash = control.postTailStateHash;
            terminal.reason =
                static_cast<qreplay::TerminalReason>(control.terminalReason);
            pendingTerminal_ = terminal;
            pendingTerminalValid_ = true;
            return;
        }
        case BrokerControl::Kind::Fault:
            setFault(control.fault, control.faultDetail);
            return;
    }
}

size_t ReplayTimelineSource::pump() {
    if (!started_ || !worker_ || !timeline_) return 0u;

    BrokerControl control{};
    while (worker_->popControl(control)) {
        applyControl(control);
        if (state_ == SourceState::Faulted) return 0u;
    }

    if (!startInstalled_) return 0u;

    size_t appended = 0u;
    qreplay::ParseStatus why = qreplay::ParseStatus::Ok;

    while (appended < options_.pumpFrameBudget) {
        if (!pendingFrameValid_) {
            if (!worker_->popFrame(pendingFrame_)) break;
            pendingFrameValid_ = true;
        }
        if (!timeline_->append(pendingFrame_, why)) {
            const qreplay::TimelineFault tf = timeline_->lastFault();
            if (tf == qreplay::TimelineFault::Overflow) {
                break;
            }
            timelineStatus_ = why;
            switch (tf) {
                case qreplay::TimelineFault::FrameGap:
                    setFault(SourceFault::FrameGap,
                             pendingFrame_.timelineFrame);
                    break;
                case qreplay::TimelineFault::BadActionMask:
                    setFault(SourceFault::InvalidActionMask,
                             pendingFrame_.timelineFrame);
                    break;
                case qreplay::TimelineFault::BadLaneShape:
                    setFault(SourceFault::InvalidLaneShape,
                             pendingFrame_.timelineFrame);
                    break;
                case qreplay::TimelineFault::DigestChainBreak:
                    setFault(SourceFault::DigestBreak,
                             pendingFrame_.timelineFrame);
                    break;
                default:
                    setFault(SourceFault::DecodeFailed,
                             pendingFrame_.timelineFrame);
                    break;
            }
            pendingFrameValid_ = false;
            return appended;
        }
        pendingFrameValid_ = false;
        ++appended;
    }

    size_t events = 0u;
    while (events < options_.pumpEventBudget) {
        if (!pendingEventValid_) {
            if (!worker_->popEvent(pendingEvent_)) break;
            pendingEventValid_ = true;
        }
        if (!timeline_->appendEvent(pendingEvent_, why)) {
            const qreplay::TimelineFault tf = timeline_->lastFault();
            if (tf == qreplay::TimelineFault::EventOverflow
                || tf == qreplay::TimelineFault::EventOutOfOrder)
                break;
            timelineStatus_ = why;
            setFault(SourceFault::DecodeFailed, pendingEvent_.timelineFrame);
            pendingEventValid_ = false;
            return appended;
        }
        pendingEventValid_ = false;
        ++events;
    }

    applyPendingTerminal();
    return appended;
}

void ReplayTimelineSource::applyPendingTerminal() {
    if (!pendingTerminalValid_ || !timeline_) return;
    if (timeline_->nextFrame() < pendingTerminal_.nextRecord) return;
    timeline_->setTerminal(pendingTerminal_);
    pendingTerminalValid_ = false;
    if (state_ == SourceState::Streaming) state_ = SourceState::Sealed;
}

bool ReplayTimelineSource::timelineStarted() const {
    return timeline_ && timeline_->started();
}

uint32_t ReplayTimelineSource::consumeCursor() const {
    return timeline_ ? timeline_->consumeCursor() : 0u;
}

uint32_t ReplayTimelineSource::retentionFloor() const {
    return timeline_ ? timeline_->retentionFloor() : 0u;
}

void ReplayTimelineSource::trimConsumedBelow(uint32_t frame) {
    if (timeline_) timeline_->trimConsumedBelow(frame);
}

size_t ReplayTimelineSource::bufferedFrames() const {
    return timeline_ ? timeline_->bufferedFrames() : 0u;
}

uint32_t ReplayTimelineSource::nextFrame() const {
    return timeline_ ? timeline_->nextFrame() : 0u;
}

uint64_t ReplayTimelineSource::rollingDigest() const {
    return timeline_ ? timeline_->rollingDigest() : 0u;
}

bool ReplayTimelineSource::peek(qreplay::ReplayFrameV1& out) const {
    return timeline_ && timeline_->peek(out);
}

bool ReplayTimelineSource::consume(uint64_t observedPreStateHash,
                                   qreplay::ParseStatus& why) {
    if (!timeline_) {
        why = qreplay::ParseStatus::Truncated;
        return false;
    }
    if (!timeline_->consume(observedPreStateHash, why)) {
        timelineStatus_ = why;
        return false;
    }
    return true;
}

void ReplayTimelineSource::recordPostState(uint32_t afterFrame,
                                           uint64_t postHash) {
    if (timeline_) timeline_->recordPostState(afterFrame, postHash);
}

bool ReplayTimelineSource::verifyTerminal(qreplay::ParseStatus& why) const {
    if (!timeline_) {
        why = qreplay::ParseStatus::Truncated;
        return false;
    }
    return timeline_->verifyTerminal(why);
}

bool ReplayTimelineSource::terminalKnown() const {
    return timeline_ && timeline_->terminal().present;
}

const qreplay::TimelineTerminal& ReplayTimelineSource::terminal() const {
    static const qreplay::TimelineTerminal kNone{};
    return timeline_ ? timeline_->terminal() : kNone;
}

bool ReplayTimelineSource::peekEvent(qreplay::ReplayEvent& out) const {
    return timeline_ && timeline_->peekEvent(out);
}

bool ReplayTimelineSource::consumeEvent(qreplay::ReplayEvent& out) {
    return timeline_ && timeline_->consumeEvent(out);
}

size_t ReplayTimelineSource::snapshotCount() const {
    size_t used = 0u;
    for (const SnapshotSlot& slot : snapshots_)
        if (slot.used) ++used;
    return used;
}

uint64_t ReplayTimelineSource::snapshotBytesResident() const {
    uint64_t total = 0u;
    for (const SnapshotSlot& slot : snapshots_)
        if (slot.used) total += slot.bytes;
    return total;
}

void ReplayTimelineSource::captureSnapshotAt(uint32_t frame) {
    if (snapshots_.empty() || !hooks_.captureSnapshot) return;

    for (const SnapshotSlot& slot : snapshots_)
        if (slot.used && slot.frame == frame) return;

    size_t target = snapshots_.size();
    for (size_t i = 0; i < snapshots_.size(); ++i) {
        if (!snapshots_[i].used) { target = i; break; }
    }
    if (target == snapshots_.size()) {
        uint32_t oldest = 0xFFFFFFFFu;
        for (size_t i = 0; i < snapshots_.size(); ++i) {
            if (snapshots_[i].frame <= oldest) {
                oldest = snapshots_[i].frame;
                target = i;
            }
        }
    }
    if (target >= snapshots_.size()) return;

    SnapshotSlot& slot = snapshots_[target];
    size_t written = 0u;
    if (!hooks_.captureSnapshot(hooks_.user, slot.buffer.data(),
                                slot.buffer.size(), written)
        || written == 0u || written > slot.buffer.size()) {
        slot.used = false;
        slot.bytes = 0u;
        return;
    }
    slot.used = true;
    slot.frame = frame;
    slot.bytes = written;
    ++snapshotCaptures_;
    lastSnapshotFrame_ = frame;
    haveSnapshotFrame_ = true;
}

void ReplayTimelineSource::maybeCaptureSnapshot(uint32_t frameJustCompleted,
                                                bool markerBoundary) {
    if (!started_ || snapshots_.empty()) return;
    const bool cadence =
        !haveSnapshotFrame_
        || (frameJustCompleted >= lastSnapshotFrame_
            && (frameJustCompleted - lastSnapshotFrame_)
                   >= options_.snapshotIntervalFrames);
    if (!markerBoundary && !cadence) return;
    captureSnapshotAt(frameJustCompleted);
}

const ReplayTimelineSource::SnapshotSlot*
ReplayTimelineSource::bestSnapshotFor(uint32_t targetFrame) const {
    const SnapshotSlot* best = nullptr;
    for (const SnapshotSlot& slot : snapshots_) {
        if (!slot.used) continue;
        if (slot.frame > targetFrame) continue;
        if (!best || slot.frame > best->frame) best = &slot;
    }
    return best;
}

bool ReplayTimelineSource::nearestMatchStart(uint32_t targetFrame,
                                             uint32_t& frameOut,
                                             uint32_t& ordinalOut) const {
    bool found = false;
    for (const MatchStartPoint& point : matchStarts_) {
        if (point.firstFrame > targetFrame) break;
        frameOut = point.firstFrame;
        ordinalOut = point.ordinal;
        found = true;
    }
    return found;
}

SeekOutcome ReplayTimelineSource::positionFor(uint32_t targetFrame) {
    SeekOutcome out{};
    out.targetFrame = targetFrame;
    if (!started_ || !timeline_ || !startInstalled_) return out;

    out.fromFrame = timeline_->consumeCursor();

    uint32_t positioned = descriptor_.start.firstTimelineFrame;
    const SnapshotSlot* snapshot = bestSnapshotFor(targetFrame);

    if (snapshot && hooks_.restoreSnapshot) {
        if (hooks_.restoreSnapshot(hooks_.user, snapshot->buffer.data(),
                                   snapshot->bytes)) {
            positioned = snapshot->frame;
            out.usedSnapshot = true;
            ++snapshotRestores_;
        } else {
            snapshot = nullptr;
        }
    } else {
        snapshot = nullptr;
    }

    if (!out.usedSnapshot) {
        uint32_t matchFrame = 0u;
        uint32_t ordinal = 0u;
        if (nearestMatchStart(targetFrame, matchFrame, ordinal)) {
            positioned = matchFrame;
            out.usedMatchStart = true;
        }
        out.requiresBootstrap = true;
    }

    if (positioned > targetFrame) positioned = targetFrame;
    out.checkpointFrame = positioned;

    qreplay::ParseStatus why = qreplay::ParseStatus::Ok;
    if (positioned >= timeline_->retentionFloor()
        && positioned <= timeline_->nextFrame()
        && timeline_->rewindConsumeTo(positioned, why)) {
        out.requestedRestream = false;
    } else {
        qreplay::DeterministicStartDescriptor desc = descriptor_;
        desc.start.firstTimelineFrame = positioned;
        descriptor_ = desc;
        timeline_->reset(desc);
        pendingFrameValid_ = false;
        pendingEventValid_ = false;
        pendingTerminalValid_ = false;
        if (worker_) worker_->requestSeek(positioned, targetFrame);
        out.requestedRestream = true;
        state_ = SourceState::Streaming;
    }

    out.fastForwardFrames =
        (targetFrame > positioned) ? (targetFrame - positioned) : 0u;
    out.ok = true;
    return out;
}

}
