#include "RollbackSession.h"

#include "input/InputActions.h"

#include <algorithm>
#include <limits>

namespace qnetplay {

namespace {

constexpr uint32_t kHalfSerialSpace = 0x80000000u;
constexpr size_t kMinimumHistoryCapacity = 64u;

}

const char* remoteInputResultName(const RemoteInputResult result) {
    switch (result) {
        case RemoteInputResult::Inserted: return "inserted";
        case RemoteInputResult::Duplicate: return "duplicate";
        case RemoteInputResult::Conflict: return "conflict";
        case RemoteInputResult::TooOld: return "too-old";
        case RemoteInputResult::TooFuture: return "too-future";
        case RemoteInputResult::InvalidSession: return "invalid-session";
        case RemoteInputResult::InvalidValue: return "invalid-value";
        case RemoteInputResult::Count: break;
    }
    return "unknown";
}

const char* inputDelayChangeResultName(
    const InputDelayChangeResult result) {
    switch (result) {
        case InputDelayChangeResult::Applied: return "applied";
        case InputDelayChangeResult::Deferred: return "deferred";
        case InputDelayChangeResult::Invalid: return "invalid";
    }
    return "unknown";
}

const char* inputDelayChangeBlockerName(
    const InputDelayChangeBlocker blocker) {
    switch (blocker) {
        case InputDelayChangeBlocker::None: return "none";
        case InputDelayChangeBlocker::InvalidConfiguration:
            return "invalid-configuration";
        case InputDelayChangeBlocker::ReplayInProgress:
            return "replay-in-progress";
        case InputDelayChangeBlocker::PendingMismatch:
            return "pending-mismatch";
        case InputDelayChangeBlocker::CaptureTransaction:
            return "capture-transaction";
        case InputDelayChangeBlocker::UnconfirmedBoundary:
            return "unconfirmed-boundary";
    }
    return "unknown";
}

const char* actionKindName(const ActionKind kind) {
    switch (kind) {
        case ActionKind::Stall: return "stall";
        case ActionKind::Advance: return "advance";
        case ActionKind::Rollback: return "rollback";
    }
    return "unknown";
}

const char* stallReasonName(const StallReason reason) {
    switch (reason) {
        case StallReason::None: return "none";
        case StallReason::LocalInputMissing: return "local-input-missing";
        case StallReason::PredictionLimit: return "prediction-limit";
        case StallReason::ReplayInProgress: return "replay-in-progress";
        case StallReason::InvalidSession: return "invalid-session";
        case StallReason::Count: break;
    }
    return "unknown";
}

bool RollbackSession::frameAfter(const uint32_t left,
                                 const uint32_t right) {
    const uint32_t distance = left - right;
    return distance != 0u && distance < kHalfSerialSpace;
}

bool RollbackSession::frameBefore(const uint32_t left,
                                  const uint32_t right) {
    return frameAfter(right, left);
}

RollbackSession::RollbackSession(const RollbackConfig& config) {
    (void)reset(config);
}

bool RollbackSession::reset(const RollbackConfig& config) {
    _config = config;
    _valid = config.localPlayer <= 1u
        && config.firstFrame != std::numeric_limits<uint32_t>::max()
        && config.historyCapacity >= kMinimumHistoryCapacity
        && config.historyCapacity
            > static_cast<size_t>(config.inputDelayFrames)
                + static_cast<size_t>(config.maxRollbackFrames) + 2u
        && config.historyCapacity < static_cast<size_t>(kHalfSerialSpace)
        && config.maxRemoteFutureFrames < kHalfSerialSpace
        && static_cast<size_t>(config.maxRemoteFutureFrames)
            < config.historyCapacity
        && static_cast<uint64_t>(config.firstFrame)
             + config.inputDelayFrames
            < std::numeric_limits<uint32_t>::max();

    const size_t capacity = _valid ? config.historyCapacity : 1u;
    _localInputs.assign(capacity, InputSlot{});
    _remoteInputs.assign(capacity, InputSlot{});
    _consumed.assign(capacity, ConsumedSlot{});
    _pendingLocalSubmissions.clear();
    _confirmedFrames.clear();

    _frontier = config.firstFrame;
    _nextRemoteContiguous = config.firstFrame;
    _hasRemoteNewest = false;
    _remoteNewestActual = 0;
    _nextConfirmedFrame = config.firstFrame;
    _hasCapture = false;
    _capturedSourceFrame = config.firstFrame;
    _capturedValue = config.neutralInput;
    _producedThrough = config.firstFrame;
    _hasPendingMismatch = false;
    _pendingMismatch = config.firstFrame;
    _replayActive = false;
    _replayFrom = config.firstFrame;
    _replayUntil = config.firstFrame;
    _nextReplayFrame = config.firstFrame;
    _activeStallReason = StallReason::None;
    _consecutiveStallPolls = 0;
    _stats = {};

    if (!_valid) {
        return false;
    }

    for (uint32_t offset = 0u;
         offset < static_cast<uint32_t>(config.inputDelayFrames);
         ++offset) {
        if (!storeLocal(config.firstFrame + offset,
                        config.neutralInput,
                        true)) {
            _valid = false;
            return false;
        }
    }
    return true;
}

InputDelayChangeResult
RollbackSession::tryChangeInputDelayAtConfirmedBoundary(
    const uint8_t inputDelayFrames) {
    const InputDelayChangeBlocker blocker =
        inputDelayChangeBlocker(inputDelayFrames);
    if (blocker == InputDelayChangeBlocker::InvalidConfiguration)
        return InputDelayChangeResult::Invalid;
    if (blocker != InputDelayChangeBlocker::None)
        return InputDelayChangeResult::Deferred;

    if (inputDelayFrames > _config.inputDelayFrames) {
        const uint32_t prefixFrames =
            static_cast<uint32_t>(inputDelayFrames)
            + (_hasCapture ? 1u : 0u);
        for (uint32_t offset = 0u;
             offset < prefixFrames;
             ++offset) {
            const uint32_t frame = _frontier + offset;
            uint16_t existing = 0u;
            if (!getLocalInput(frame, existing)
                && !storeLocal(frame, _config.neutralInput, true)) {
                return InputDelayChangeResult::Invalid;
            }
        }
    }

    _config.inputDelayFrames = inputDelayFrames;
    return InputDelayChangeResult::Applied;
}

uint32_t RollbackSession::localInputProducedThrough() const {
    const uint32_t delayedTarget =
        _frontier + static_cast<uint32_t>(_config.inputDelayFrames);
    if (!_hasProduced) return delayedTarget;
    return frameAfter(_producedThrough, delayedTarget)
        ? _producedThrough : delayedTarget;
}

bool RollbackSession::produceLocalInputAhead(const uint16_t value,
                                             const uint32_t maxLeadFrames) {
    if (!_valid || _replayActive) return false;
    if (_hasPendingMismatch) return false;
    if (!qinput::isCanonicalFightInput(value)) return false;

    // The frontier's delayed slot belongs to captureLocalInput; producing it here could submit a different word for the same frame.
    const uint32_t delayedTarget =
        _frontier + static_cast<uint32_t>(_config.inputDelayFrames);
    const uint32_t next = localInputProducedThrough() + 1u;
    if (!frameAfter(next, delayedTarget)) return false;

    const uint32_t lead = forwardDistance(delayedTarget, next);
    if (lead > maxLeadFrames) return false;
    if (static_cast<size_t>(lead)
            + static_cast<size_t>(_config.inputDelayFrames)
            + static_cast<size_t>(_config.maxRollbackFrames) + 2u
        >= _config.historyCapacity) {
        return false;
    }
    if (next == std::numeric_limits<uint32_t>::max()) return false;

    uint16_t existing = 0;
    if (getLocalInput(next, existing)) {
        _producedThrough = next;
        _hasProduced = true;
        return false;
    }
    if (!storeLocal(next, value, true)) return false;
    _producedThrough = next;
    _hasProduced = true;
    ++_stats.uniqueCaptures;
    return true;
}

InputDelayChangeBlocker RollbackSession::maxRollbackChangeBlocker(
    const uint8_t maxRollbackFrames) const {
    const size_t newMax = static_cast<size_t>(maxRollbackFrames);
    if (!_valid
        || _config.historyCapacity
            <= static_cast<size_t>(_config.inputDelayFrames) + newMax + 2u) {
        return InputDelayChangeBlocker::InvalidConfiguration;
    }
    if (maxRollbackFrames >= _config.maxRollbackFrames)
        return InputDelayChangeBlocker::None;

    if (_replayActive)
        return InputDelayChangeBlocker::ReplayInProgress;
    if (_hasPendingMismatch)
        return InputDelayChangeBlocker::PendingMismatch;
    if (speculativeFrames() > static_cast<uint32_t>(maxRollbackFrames))
        return InputDelayChangeBlocker::UnconfirmedBoundary;
    return InputDelayChangeBlocker::None;
}

InputDelayChangeResult RollbackSession::tryChangeMaxRollback(
    const uint8_t maxRollbackFrames) {
    const InputDelayChangeBlocker blocker =
        maxRollbackChangeBlocker(maxRollbackFrames);
    if (blocker == InputDelayChangeBlocker::InvalidConfiguration)
        return InputDelayChangeResult::Invalid;
    if (blocker != InputDelayChangeBlocker::None)
        return InputDelayChangeResult::Deferred;
    _config.maxRollbackFrames = maxRollbackFrames;
    return InputDelayChangeResult::Applied;
}

InputDelayChangeBlocker RollbackSession::inputDelayChangeBlocker(
    const uint8_t inputDelayFrames) const {
    const size_t newDelay = static_cast<size_t>(inputDelayFrames);
    if (!_valid
        || _config.historyCapacity
            <= newDelay
                + static_cast<size_t>(_config.maxRollbackFrames) + 2u
        || static_cast<uint64_t>(_frontier) + newDelay
            >= std::numeric_limits<uint32_t>::max()) {
        return InputDelayChangeBlocker::InvalidConfiguration;
    }
    if (_replayActive)
        return InputDelayChangeBlocker::ReplayInProgress;
    if (_hasPendingMismatch)
        return InputDelayChangeBlocker::PendingMismatch;
    if (_hasCapture && _capturedSourceFrame != _frontier)
        return InputDelayChangeBlocker::CaptureTransaction;
    if (_nextConfirmedFrame != _frontier)
        return InputDelayChangeBlocker::UnconfirmedBoundary;
    return InputDelayChangeBlocker::None;
}

bool RollbackSession::storeLocal(const uint32_t frame,
                                 const uint16_t value,
                                 const bool queueSubmission) {
    if (!_valid) {
        return false;
    }
    InputSlot& slot = _localInputs[indexFor(frame)];
    if (slot.valid && slot.frame == frame) {
        return slot.value == value;
    }
    slot = InputSlot{frame, value, true};
    if (queueSubmission) {
        _pendingLocalSubmissions.push_back(TaggedInput{frame, value});
        ++_stats.localSubmissions;
    }
    return true;
}

uint32_t RollbackSession::assignedLocalTailExclusive() const {
    if (!_valid) return _frontier;
    uint32_t frame = _frontier;
    const uint32_t limit =
        _frontier + static_cast<uint32_t>(_config.historyCapacity);
    uint16_t existing = 0;
    while (frame != limit && getLocalInput(frame, existing)) ++frame;
    return frame;
}

bool RollbackSession::fillLocalNeutralThrough(
    const uint32_t boundaryExclusive) {
    if (!_valid || _replayActive) return false;
    if (boundaryExclusive - _frontier
        > static_cast<uint32_t>(_config.historyCapacity) / 2u) {
        return false;
    }
    for (uint32_t frame = assignedLocalTailExclusive();
         frameBefore(frame, boundaryExclusive); ++frame) {
        if (!storeLocal(frame, _config.neutralInput, true)) return false;
    }
    return true;
}

bool RollbackSession::needsLocalCapture() const {
    if (!_valid || _replayActive)
        return false;
    if (_hasCapture && _capturedSourceFrame == _frontier)
        return false;

    uint16_t existing = 0;
    const uint32_t targetFrame =
        _frontier + static_cast<uint32_t>(_config.inputDelayFrames);
    return !getLocalInput(targetFrame, existing);
}

bool RollbackSession::captureLocalInput(const uint32_t sourceFrame,
                                        const uint16_t value) {
    if (!_valid || _replayActive || sourceFrame != _frontier) {
        return false;
    }
    if (!qinput::isCanonicalFightInput(value)) {
        return false;
    }
    if (_hasCapture && _capturedSourceFrame == sourceFrame) {
        ++_stats.repeatedCaptureCalls;
        return _capturedValue == value;
    }

    const uint32_t targetFrame =
        sourceFrame + static_cast<uint32_t>(_config.inputDelayFrames);
    uint16_t existing = 0;
    if (getLocalInput(targetFrame, existing)) {
        _hasCapture = true;
        _capturedSourceFrame = sourceFrame;
        _capturedValue = existing;
        ++_stats.repeatedCaptureCalls;
        return existing == value;
    }
    if (targetFrame == std::numeric_limits<uint32_t>::max()
        || !storeLocal(targetFrame, value, true)) {
        return false;
    }
    _hasCapture = true;
    _capturedSourceFrame = sourceFrame;
    _capturedValue = value;
    ++_stats.uniqueCaptures;
    return true;
}

bool RollbackSession::adoptExistingCapture() {
    if (_hasCapture && _capturedSourceFrame == _frontier)
        return true;
    uint16_t existing = 0;
    const uint32_t targetFrame =
        _frontier + static_cast<uint32_t>(_config.inputDelayFrames);
    if (!getLocalInput(targetFrame, existing))
        return false;
    _hasCapture = true;
    _capturedSourceFrame = _frontier;
    _capturedValue = existing;
    return true;
}

bool RollbackSession::popLocalSubmission(TaggedInput& out) {
    if (_pendingLocalSubmissions.empty()) {
        return false;
    }
    out = _pendingLocalSubmissions.front();
    _pendingLocalSubmissions.pop_front();
    return true;
}

bool RollbackSession::getLocalInput(const uint32_t frame,
                                    uint16_t& out) const {
    if (!_valid) {
        return false;
    }
    const InputSlot& slot = _localInputs[indexFor(frame)];
    if (!slot.valid || slot.frame != frame) {
        return false;
    }
    out = slot.value;
    return true;
}

bool RollbackSession::getRemoteActual(const uint32_t frame,
                                      uint16_t& out) const {
    if (!_valid) {
        return false;
    }
    const InputSlot& slot = _remoteInputs[indexFor(frame)];
    if (!slot.valid || slot.frame != frame) {
        return false;
    }
    out = slot.value;
    return true;
}

void RollbackSession::advanceRemoteContiguous() {
    for (;;) {
        const InputSlot& slot =
            _remoteInputs[indexFor(_nextRemoteContiguous)];
        if (!slot.valid || slot.frame != _nextRemoteContiguous) {
            break;
        }
        ++_nextRemoteContiguous;
    }
}

void RollbackSession::markMismatch(const uint32_t frame) {
    if (!_hasPendingMismatch || frameBefore(frame, _pendingMismatch)) {
        _pendingMismatch = frame;
        _hasPendingMismatch = true;
    }
}

RemoteInputResult RollbackSession::receiveRemoteInput(
    const uint32_t frame,
    const uint16_t value) {
    if (!_valid) {
        return recordRemoteInputResult(RemoteInputResult::InvalidSession);
    }
    if (!qinput::isCanonicalFightInput(value)) {
        return recordRemoteInputResult(RemoteInputResult::InvalidValue);
    }

    if (frameBefore(frame, _frontier)) {
        const uint32_t age = forwardDistance(frame, _frontier);
        if (age >= _config.historyCapacity) {
            return recordRemoteInputResult(RemoteInputResult::TooOld);
        }
    } else if (frameAfter(frame, _frontier)
               && forwardDistance(_frontier, frame)
                    > _config.maxRemoteFutureFrames) {
        return recordRemoteInputResult(RemoteInputResult::TooFuture);
    } else if (frame != _frontier
               && !frameBefore(frame, _frontier)
               && !frameAfter(frame, _frontier)) {
        return recordRemoteInputResult(RemoteInputResult::TooFuture);
    }

    const InputSlot& existing = _remoteInputs[indexFor(frame)];
    if (existing.valid && existing.frame == frame) {
        return recordRemoteInputResult(
            existing.value == value
                ? RemoteInputResult::Duplicate
                : RemoteInputResult::Conflict);
    }

    const ConsumedSlot* consumed = nullptr;
    if (frameBefore(frame, _frontier)) {
        consumed = &_consumed[indexFor(frame)];
        if (!consumed->valid || consumed->frame != frame) {
            return recordRemoteInputResult(RemoteInputResult::TooOld);
        }
    }

    const uint32_t contiguousBefore = _nextRemoteContiguous;
    const bool arrivedBeyondHole =
        frameAfter(frame, _nextRemoteContiguous);
    if (arrivedBeyondHole) {
        ++_stats.remoteOutOfOrderInputs;
        const uint32_t span =
            forwardDistance(_nextRemoteContiguous, frame) + 1u;
        _stats.maxRemoteHoleSpan =
            std::max(_stats.maxRemoteHoleSpan, span);
    }

    InputSlot& slot = _remoteInputs[indexFor(frame)];
    slot = InputSlot{frame, value, true};

    if (!_hasRemoteNewest || frameAfter(frame, _remoteNewestActual)) {
        _remoteNewestActual = frame;
        _hasRemoteNewest = true;
    }

    if (consumed != nullptr) {
        const uint16_t usedRemote = _config.localPlayer == 0u
            ? consumed->inputs.p2
            : consumed->inputs.p1;
        if (usedRemote != value) {
            ++_stats.mismatchingRemoteInputs;
            markMismatch(frame);
        }
    }

    advanceRemoteContiguous();
    if (_nextRemoteContiguous != contiguousBefore
        && _hasRemoteNewest
        && (frameAfter(_remoteNewestActual, contiguousBefore)
            || _remoteNewestActual == contiguousBefore)) {
        ++_stats.remoteHolePrefixAdvances;
    }
    updateStableConfirmed();
    return recordRemoteInputResult(RemoteInputResult::Inserted);
}

bool RollbackSession::predictRemote(const uint32_t frame,
                                    uint16_t& value,
                                    bool& predicted) const {
    if (getRemoteActual(frame, value)) {
        predicted = false;
        return true;
    }

    predicted = true;
    value = _config.neutralInput;
    uint32_t cursor = frame;
    for (size_t searched = 0u;
         searched < _config.historyCapacity;
         ++searched) {
        if (cursor == _config.firstFrame) {
            break;
        }
        --cursor;
        if (getRemoteActual(cursor, value)) {
            return true;
        }
    }
    return true;
}

bool RollbackSession::inputsForFrame(const uint32_t frame,
                                     CanonicalInputs& out) const {
    uint16_t local = 0u;
    uint16_t remote = 0u;
    bool predicted = false;
    if (!getLocalInput(frame, local)
        || !predictRemote(frame, remote, predicted)) {
        return false;
    }

    out = {};
    out.frame = frame;
    out.remotePredicted = predicted;
    if (_config.localPlayer == 0u) {
        out.p1 = local;
        out.p2 = remote;
    } else {
        out.p1 = remote;
        out.p2 = local;
    }
    return true;
}

uint32_t RollbackSession::speculativeFrames() const {
    if (_frontier == _nextRemoteContiguous
        || frameAfter(_nextRemoteContiguous, _frontier)) {
        return 0u;
    }
    if (frameAfter(_frontier, _nextRemoteContiguous)) {
        return forwardDistance(_nextRemoteContiguous, _frontier);
    }
    return std::numeric_limits<uint32_t>::max();
}

RemoteInputResult RollbackSession::recordRemoteInputResult(
    const RemoteInputResult result) {
    const size_t index = static_cast<size_t>(result);
    if (index < _stats.remoteInputResults.size())
        ++_stats.remoteInputResults[index];
    return result;
}

void RollbackSession::recordStall(const StallReason reason) {
    ++_stats.stallPolls;
    const size_t index = static_cast<size_t>(reason);
    if (index < _stats.stallPollsByReason.size())
        ++_stats.stallPollsByReason[index];
    if (_activeStallReason == StallReason::None) {
        ++_stats.stallEpisodes;
        _consecutiveStallPolls = 0;
    }
    if (_activeStallReason != reason) {
        if (index < _stats.stallEpisodesByReason.size())
            ++_stats.stallEpisodesByReason[index];
        _activeStallReason = reason;
    }
    ++_consecutiveStallPolls;
    _stats.maxConsecutiveStallPolls =
        std::max(_stats.maxConsecutiveStallPolls,
                 _consecutiveStallPolls);
}

void RollbackSession::clearStall() {
    _activeStallReason = StallReason::None;
    _consecutiveStallPolls = 0;
}

FrameAction RollbackSession::nextAction() {
    return nextActionInternal(true);
}

FrameAction RollbackSession::nextQuiesceDrainAction() {
    return nextActionInternal(false);
}

FrameAction RollbackSession::nextActionInternal(
    const bool requireSourceCapture) {
    FrameAction action{};
    action.frame = _frontier;

    if (!_valid) {
        action.stallReason = StallReason::InvalidSession;
        recordStall(action.stallReason);
        return action;
    }
    if (_replayActive) {
        action.stallReason = StallReason::ReplayInProgress;
        recordStall(action.stallReason);
        return action;
    }
    if (_hasPendingMismatch) {
        action.kind = ActionKind::Rollback;
        action.rollbackFrom = _pendingMismatch;
        action.replayUntilExclusive = _frontier;
        clearStall();
        return action;
    }
    if (requireSourceCapture && !adoptExistingCapture()) {
        action.stallReason = StallReason::LocalInputMissing;
        recordStall(action.stallReason);
        return action;
    }

    uint16_t currentActual = 0u;
    const bool hasCurrentActual = getRemoteActual(_frontier, currentActual);
    const uint32_t speculative = speculativeFrames();
    if (!hasCurrentActual
        && speculative >= static_cast<uint32_t>(_config.maxRollbackFrames)) {
        action.stallReason = StallReason::PredictionLimit;
        recordStall(action.stallReason);
        return action;
    }
    if (hasCurrentActual && speculative != 0u
        && speculative >= static_cast<uint32_t>(_config.maxRollbackFrames)) {
        action.stallReason = StallReason::PredictionLimit;
        recordStall(action.stallReason);
        return action;
    }
    if (!inputsForFrame(_frontier, action.inputs)) {
        action.stallReason = StallReason::LocalInputMissing;
        recordStall(action.stallReason);
        return action;
    }

    action.kind = ActionKind::Advance;
    action.stallReason = StallReason::None;
    clearStall();
    return action;
}

bool RollbackSession::recordConsumed(const CanonicalInputs& inputs,
                                     const uint64_t preStateHash) {
    ConsumedSlot& slot = _consumed[indexFor(inputs.frame)];
    slot.frame = inputs.frame;
    slot.inputs = inputs;
    slot.preStateHash = preStateHash;
    slot.valid = true;
    return true;
}

bool RollbackSession::commitAdvance(const CanonicalInputs& inputs,
                                    const uint64_t preStateHash) {
    if (!_valid || _replayActive || _hasPendingMismatch
        || inputs.frame != _frontier) {
        return false;
    }
    CanonicalInputs expected{};
    if (!inputsForFrame(_frontier, expected)
        || expected.p1 != inputs.p1
        || expected.p2 != inputs.p2
        || expected.remotePredicted != inputs.remotePredicted) {
        return false;
    }
    if (!recordConsumed(inputs, preStateHash)) {
        return false;
    }

    ++_frontier;
    ++_stats.visibleAdvances;
    if (inputs.remotePredicted)
        ++_stats.predictedAdvances;
    _hasCapture = false;

    const uint32_t speculative = speculativeFrames();
    _stats.maxSpeculativeFrames =
        std::max(_stats.maxSpeculativeFrames, speculative);
    updateStableConfirmed();
    return true;
}

bool RollbackSession::beginRollback(const uint32_t rollbackFrom,
                                    const uint32_t replayUntilExclusive) {
    if (!_valid || _replayActive || !_hasPendingMismatch
        || rollbackFrom != _pendingMismatch
        || replayUntilExclusive != _frontier
        || !frameBefore(rollbackFrom, replayUntilExclusive)) {
        return false;
    }
    const uint32_t depth =
        forwardDistance(rollbackFrom, replayUntilExclusive);
    if (depth == 0u
        || depth > static_cast<uint32_t>(_config.maxRollbackFrames)
        || depth >= _config.historyCapacity) {
        return false;
    }

    _replayActive = true;
    _replayFrom = rollbackFrom;
    _replayUntil = replayUntilExclusive;
    _nextReplayFrame = rollbackFrom;
    return true;
}

bool RollbackSession::commitReplayFrame(const CanonicalInputs& inputs,
                                        const uint64_t preStateHash) {
    if (!_valid || !_replayActive
        || _nextReplayFrame == _replayUntil
        || inputs.frame != _nextReplayFrame) {
        return false;
    }
    CanonicalInputs expected{};
    if (!inputsForFrame(_nextReplayFrame, expected)
        || expected.p1 != inputs.p1
        || expected.p2 != inputs.p2
        || expected.remotePredicted != inputs.remotePredicted) {
        return false;
    }
    if (!recordConsumed(inputs, preStateHash)) {
        return false;
    }
    ++_nextReplayFrame;
    ++_stats.replayedFrames;
    return true;
}

bool RollbackSession::finishRollback() {
    if (!_valid || !_replayActive || _nextReplayFrame != _replayUntil) {
        return false;
    }
    return completeRollback(_replayUntil, false);
}

bool RollbackSession::finishRollbackAtBoundary() {
    if (!_valid || !_replayActive
        || _nextReplayFrame == _replayFrom
        || (_nextReplayFrame != _replayUntil
            && !frameBefore(_nextReplayFrame, _replayUntil))) {
        return false;
    }
    return completeRollback(
        _nextReplayFrame, _nextReplayFrame != _replayUntil);
}

bool RollbackSession::finishRollbackBeforeBoundary() {
    if (!_valid || !_replayActive
        || _nextReplayFrame == _replayUntil
        || !frameBefore(_nextReplayFrame, _replayUntil)) {
        return false;
    }
    return completeRollback(
        _nextReplayFrame, _nextReplayFrame != _replayUntil);
}

bool RollbackSession::completeRollback(
    const uint32_t replayEndExclusive,
    const bool discardSpeculativeSuffix) {
    if (!_valid || !_replayActive
        || replayEndExclusive != _nextReplayFrame
        || (replayEndExclusive != _replayUntil
            && !frameBefore(replayEndExclusive, _replayUntil))) {
        return false;
    }

    if (discardSpeculativeSuffix
        && frameAfter(_nextConfirmedFrame, replayEndExclusive)) {
        return false;
    }

    bool rewoundCapture = false;
    uint16_t rewoundCaptureValue = 0;
    if (discardSpeculativeSuffix) {
        const uint32_t captureTarget =
            replayEndExclusive
            + static_cast<uint32_t>(_config.inputDelayFrames);
        if (!getLocalInput(captureTarget, rewoundCaptureValue)) {
            return false;
        }
        rewoundCapture = true;
    }

    const uint32_t depth = forwardDistance(_replayFrom, _replayUntil);
    ++_stats.rollbackCount;
    _stats.rollbackFrames += depth;
    _stats.maxRollbackDepth = std::max(_stats.maxRollbackDepth, depth);

    if (discardSpeculativeSuffix) {
        uint32_t frame = replayEndExclusive;
        while (frame != _replayUntil) {
            ConsumedSlot& consumed = _consumed[indexFor(frame)];
            if (consumed.valid && consumed.frame == frame)
                consumed = ConsumedSlot{};
            ++frame;
        }
        _frontier = replayEndExclusive;
        _hasCapture = rewoundCapture;
        _capturedSourceFrame = replayEndExclusive;
        _capturedValue = rewoundCaptureValue;
    }

    _replayActive = false;
    _hasPendingMismatch = false;

    uint32_t frame = _replayFrom;
    while (frame != replayEndExclusive) {
        uint16_t actual = 0u;
        const ConsumedSlot& consumed = _consumed[indexFor(frame)];
        if (getRemoteActual(frame, actual)
            && consumed.valid && consumed.frame == frame) {
            const uint16_t usedRemote = _config.localPlayer == 0u
                ? consumed.inputs.p2
                : consumed.inputs.p1;
            if (usedRemote != actual) {
                markMismatch(frame);
                break;
            }
        }
        ++frame;
    }

    updateStableConfirmed();
    return true;
}

void RollbackSession::updateStableConfirmed() {
    if (!_valid || _replayActive || _hasPendingMismatch
        || _nextConfirmedFrame == _frontier) {
        return;
    }

    uint32_t confirmedEnd = _nextRemoteContiguous;
    if (frameAfter(confirmedEnd, _frontier)) {
        confirmedEnd = _frontier;
    }
    while (_nextConfirmedFrame != confirmedEnd) {
        const ConsumedSlot& slot = _consumed[indexFor(_nextConfirmedFrame)];
        if (!slot.valid || slot.frame != _nextConfirmedFrame) {
            break;
        }
        _confirmedFrames.push_back(ConfirmedFrame{
            slot.frame,
            slot.inputs.p1,
            slot.inputs.p2,
            slot.preStateHash,
        });
        ++_stats.confirmedFrames;
        ++_nextConfirmedFrame;
    }
}

RollbackDiagnostics RollbackSession::diagnostics() const {
    RollbackDiagnostics out{};
    out.valid = _valid;
    out.frontier = _frontier;
    out.inputDelayFrames = _config.inputDelayFrames;
    out.maxRollbackFrames = _config.maxRollbackFrames;
    out.speculativeFrames = _valid ? speculativeFrames() : 0u;

    uint16_t ignored = 0u;
    out.localInputAtFrontier =
        _valid && getLocalInput(_frontier, ignored);
    out.remoteActualAtFrontier =
        _valid && getRemoteActual(_frontier, ignored);

    out.nextRemoteExpected = _nextRemoteContiguous;
    out.hasRemoteContiguousThrough =
        _nextRemoteContiguous != _config.firstFrame;
    out.remoteContiguousThrough =
        out.hasRemoteContiguousThrough
            ? _nextRemoteContiguous - 1u : _config.firstFrame;
    out.hasRemoteNewestActual = _hasRemoteNewest;
    out.remoteNewestActual = _remoteNewestActual;
    out.hasRemoteHole =
        _hasRemoteNewest
        && frameAfter(_remoteNewestActual, _nextRemoteContiguous);
    if (out.hasRemoteHole) {
        out.remoteHoleSpan =
            forwardDistance(_nextRemoteContiguous,
                            _remoteNewestActual) + 1u;
    }

    out.nextConfirmedFrame = _nextConfirmedFrame;
    out.hasStableConfirmedThrough =
        _nextConfirmedFrame != _config.firstFrame;
    out.stableConfirmedThrough =
        out.hasStableConfirmedThrough
            ? _nextConfirmedFrame - 1u : _config.firstFrame;
    out.pendingLocalSubmissions = _pendingLocalSubmissions.size();
    out.pendingConfirmedFrames = _confirmedFrames.size();

    out.pendingRollback = _hasPendingMismatch;
    out.pendingRollbackFrom = _pendingMismatch;
    out.replayInProgress = _replayActive;
    out.replayFrom = _replayFrom;
    out.replayUntilExclusive = _replayUntil;
    out.nextReplayFrame = _nextReplayFrame;
    out.activeStallReason = _activeStallReason;
    out.consecutiveStallPolls = _consecutiveStallPolls;
    return out;
}

bool RollbackSession::popConfirmedFrame(ConfirmedFrame& out) {
    if (_confirmedFrames.empty()) {
        return false;
    }
    out = _confirmedFrames.front();
    _confirmedFrames.pop_front();
    return true;
}

bool RollbackSession::getConsumedFrame(const uint32_t frame,
                                       CanonicalInputs& inputs,
                                       uint64_t& preStateHash) const {
    if (!_valid) {
        return false;
    }
    const ConsumedSlot& slot = _consumed[indexFor(frame)];
    if (!slot.valid || slot.frame != frame) {
        return false;
    }
    inputs = slot.inputs;
    preStateHash = slot.preStateHash;
    return true;
}

}
