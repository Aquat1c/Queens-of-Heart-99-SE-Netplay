#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace qnetplay {

struct RollbackConfig {
    uint8_t localPlayer = 0;
    uint8_t inputDelayFrames = 1;
    uint8_t maxRollbackFrames = 8;
    uint16_t neutralInput = 0;
    uint32_t firstFrame = 0;
    uint32_t maxRemoteFutureFrames = 120;
    size_t historyCapacity = 4096;
};

struct TaggedInput {
    uint32_t frame = 0;
    uint16_t value = 0;
};

struct CanonicalInputs {
    uint32_t frame = 0;
    uint16_t p1 = 0;
    uint16_t p2 = 0;
    bool remotePredicted = false;
};

enum class RemoteInputResult {
    Inserted,
    Duplicate,
    Conflict,
    TooOld,
    TooFuture,
    InvalidSession,
    InvalidValue,
    Count,
};

const char* remoteInputResultName(RemoteInputResult result);

enum class InputDelayChangeResult {
    Applied,
    Deferred,
    Invalid,
};
const char* inputDelayChangeResultName(InputDelayChangeResult result);

enum class InputDelayChangeBlocker {
    None,
    InvalidConfiguration,
    ReplayInProgress,
    PendingMismatch,
    CaptureTransaction,
    UnconfirmedBoundary,
};
const char* inputDelayChangeBlockerName(InputDelayChangeBlocker blocker);

enum class ActionKind {
    Stall,
    Advance,
    Rollback,
};
const char* actionKindName(ActionKind kind);

enum class StallReason {
    None,
    LocalInputMissing,
    PredictionLimit,
    ReplayInProgress,
    InvalidSession,
    Count,
};

const char* stallReasonName(StallReason reason);

struct FrameAction {
    ActionKind kind = ActionKind::Stall;
    StallReason stallReason = StallReason::None;
    uint32_t frame = 0;
    uint32_t rollbackFrom = 0;
    uint32_t replayUntilExclusive = 0;
    CanonicalInputs inputs{};
};

struct ConfirmedFrame {
    uint32_t frame = 0;
    uint16_t p1 = 0;
    uint16_t p2 = 0;
    uint64_t preStateHash = 0;
};

struct RollbackStats {
    uint64_t uniqueCaptures = 0;
    uint64_t repeatedCaptureCalls = 0;
    uint64_t localSubmissions = 0;
    uint64_t visibleAdvances = 0;
    uint64_t predictedAdvances = 0;
    uint64_t replayedFrames = 0;
    uint64_t rollbackCount = 0;
    uint64_t rollbackFrames = 0;
    uint64_t mismatchingRemoteInputs = 0;
    uint64_t confirmedFrames = 0;
    uint64_t stallPolls = 0;
    uint64_t stallEpisodes = 0;
    std::array<uint64_t, static_cast<size_t>(StallReason::Count)>
        stallPollsByReason{};
    std::array<uint64_t, static_cast<size_t>(StallReason::Count)>
        stallEpisodesByReason{};
    std::array<uint64_t, static_cast<size_t>(RemoteInputResult::Count)>
        remoteInputResults{};
    uint64_t remoteOutOfOrderInputs = 0;
    uint64_t remoteHolePrefixAdvances = 0;
    uint32_t maxRollbackDepth = 0;
    uint32_t maxSpeculativeFrames = 0;
    uint32_t maxConsecutiveStallPolls = 0;
    uint32_t maxRemoteHoleSpan = 0;
};

struct RollbackDiagnostics {
    bool valid = false;
    uint32_t frontier = 0;
    uint8_t inputDelayFrames = 0;
    uint8_t maxRollbackFrames = 0;

    bool localInputAtFrontier = false;
    bool remoteActualAtFrontier = false;
    uint32_t speculativeFrames = 0;

    uint32_t nextRemoteExpected = 0;
    bool hasRemoteContiguousThrough = false;
    uint32_t remoteContiguousThrough = 0;
    bool hasRemoteNewestActual = false;
    uint32_t remoteNewestActual = 0;
    bool hasRemoteHole = false;
    uint32_t remoteHoleSpan = 0;

    uint32_t nextConfirmedFrame = 0;
    bool hasStableConfirmedThrough = false;
    uint32_t stableConfirmedThrough = 0;
    size_t pendingLocalSubmissions = 0;
    size_t pendingConfirmedFrames = 0;

    bool pendingRollback = false;
    uint32_t pendingRollbackFrom = 0;
    bool replayInProgress = false;
    uint32_t replayFrom = 0;
    uint32_t replayUntilExclusive = 0;
    uint32_t nextReplayFrame = 0;

    StallReason activeStallReason = StallReason::None;
    uint32_t consecutiveStallPolls = 0;
};

class RollbackSession {
public:
    explicit RollbackSession(const RollbackConfig& config);

    bool reset(const RollbackConfig& config);
    bool valid() const { return _valid; }
    const RollbackConfig& config() const { return _config; }

    uint32_t currentFrame() const { return _frontier; }
    bool needsLocalCapture() const;

    InputDelayChangeResult tryChangeInputDelayAtConfirmedBoundary(
        uint8_t inputDelayFrames);
    InputDelayChangeBlocker inputDelayChangeBlocker(
        uint8_t inputDelayFrames) const;

    InputDelayChangeResult tryChangeMaxRollback(uint8_t maxRollbackFrames);
    InputDelayChangeBlocker maxRollbackChangeBlocker(
        uint8_t maxRollbackFrames) const;
    bool changeInputDelayAtConfirmedBoundary(
        const uint8_t inputDelayFrames) {
        return tryChangeInputDelayAtConfirmedBoundary(inputDelayFrames)
            == InputDelayChangeResult::Applied;
    }

    bool captureLocalInput(uint32_t sourceFrame, uint16_t value);

    bool produceLocalInputAhead(uint16_t value, uint32_t maxLeadFrames);

    uint32_t localInputProducedThrough() const;

    bool popLocalSubmission(TaggedInput& out);
    size_t pendingLocalSubmissions() const {
        return _pendingLocalSubmissions.size();
    }

    RemoteInputResult receiveRemoteInput(uint32_t frame, uint16_t value);

    FrameAction nextAction();
    FrameAction nextQuiesceDrainAction();
    bool inputsForFrame(uint32_t frame, CanonicalInputs& out) const;

    bool commitAdvance(const CanonicalInputs& inputs, uint64_t preStateHash);

    // No local capture is legal during the rollback transaction.
    bool beginRollback(uint32_t rollbackFrom, uint32_t replayUntilExclusive);
    bool commitReplayFrame(const CanonicalInputs& inputs,
                           uint64_t preStateHash);
    bool finishRollback();
    bool finishRollbackAtBoundary();
    bool finishRollbackBeforeBoundary();
    bool replayInProgress() const { return _replayActive; }

    bool hasPendingRollback() const { return _hasPendingMismatch; }
    uint32_t pendingRollbackFrom() const { return _pendingMismatch; }

    bool hasRemoteContiguousThrough() const {
        return _nextRemoteContiguous != _config.firstFrame;
    }
    uint32_t remoteContiguousThrough() const {
        return _nextRemoteContiguous - 1u;
    }
    bool hasStableConfirmedThrough() const {
        return _nextConfirmedFrame != _config.firstFrame;
    }
    uint32_t stableConfirmedThrough() const {
        return _nextConfirmedFrame - 1u;
    }
    bool hasRemoteNewestActual() const { return _hasRemoteNewest; }
    uint32_t remoteNewestActual() const { return _remoteNewestActual; }
    bool popConfirmedFrame(ConfirmedFrame& out);

    uint16_t lastCapturedWord() const { return _capturedValue; }
    bool hasCapturedWord() const { return _hasCapture; }

    uint32_t assignedLocalTailExclusive() const;
    uint32_t oldestRestorableFrame() const { return _nextConfirmedFrame; }
    bool fillLocalNeutralThrough(uint32_t boundaryExclusive);

    bool getLocalInput(uint32_t frame, uint16_t& out) const;
    bool getRemoteActual(uint32_t frame, uint16_t& out) const;
    bool getConsumedFrame(uint32_t frame,
                          CanonicalInputs& inputs,
                          uint64_t& preStateHash) const;

    const RollbackStats& stats() const { return _stats; }
    RollbackDiagnostics diagnostics() const;

private:
    struct InputSlot {
        uint32_t frame = 0;
        uint16_t value = 0;
        bool valid = false;
    };

    struct ConsumedSlot {
        uint32_t frame = 0;
        CanonicalInputs inputs{};
        uint64_t preStateHash = 0;
        bool valid = false;
    };

    RollbackConfig _config{};
    bool _valid = false;
    uint32_t _frontier = 0;
    uint32_t _nextRemoteContiguous = 0;
    uint32_t _nextConfirmedFrame = 0;
    bool _hasRemoteNewest = false;
    uint32_t _remoteNewestActual = 0;

    std::vector<InputSlot> _localInputs;
    std::vector<InputSlot> _remoteInputs;
    std::vector<ConsumedSlot> _consumed;
    std::deque<TaggedInput> _pendingLocalSubmissions;
    std::deque<ConfirmedFrame> _confirmedFrames;

    uint32_t _producedThrough = 0;
    bool _hasProduced = false;
    bool _hasCapture = false;
    uint32_t _capturedSourceFrame = 0;
    uint16_t _capturedValue = 0;

    bool _hasPendingMismatch = false;
    uint32_t _pendingMismatch = 0;

    bool _replayActive = false;
    uint32_t _replayFrom = 0;
    uint32_t _replayUntil = 0;
    uint32_t _nextReplayFrame = 0;

    StallReason _activeStallReason = StallReason::None;
    uint32_t _consecutiveStallPolls = 0;
    RollbackStats _stats{};

    static bool frameAfter(uint32_t left, uint32_t right);
    static bool frameBefore(uint32_t left, uint32_t right);
    static uint32_t forwardDistance(uint32_t from, uint32_t to) {
        return to - from;
    }

    size_t indexFor(uint32_t frame) const {
        return static_cast<size_t>(frame % _config.historyCapacity);
    }
    bool storeLocal(uint32_t frame, uint16_t value, bool queueSubmission);
    bool predictRemote(uint32_t frame, uint16_t& value, bool& predicted) const;
    bool adoptExistingCapture();
    FrameAction nextActionInternal(bool requireSourceCapture);
    uint32_t speculativeFrames() const;
    void markMismatch(uint32_t frame);
    void advanceRemoteContiguous();
    void updateStableConfirmed();
    bool recordConsumed(const CanonicalInputs& inputs, uint64_t preStateHash);
    bool completeRollback(uint32_t replayEndExclusive,
                          bool discardSpeculativeSuffix);
    RemoteInputResult recordRemoteInputResult(RemoteInputResult result);
    void recordStall(StallReason reason);
    void clearStall();
};

}
