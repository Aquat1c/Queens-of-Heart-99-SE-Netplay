/* Snapshots must never be serialized: they may hold absolute pointers, heap-layout assumptions and process-owned resource identities. */
#pragma once

#include "game/replay/ReplayIpcWorker.h"
#include "replay/ReplayTimeline.h"
#include "replay/ReplayTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace qgame::replay {

struct SimulationHooks {
    void* user = nullptr;

    bool (*captureSnapshot)(void* user, uint8_t* dst, size_t cap,
                            size_t& written) = nullptr;

    bool (*restoreSnapshot)(void* user, const uint8_t* src, size_t len) = nullptr;

    uint64_t (*currentStateHash)(void* user) = nullptr;
};

struct TimelineSourceOptions {
    size_t timelineCapacityFrames = 8192u;
    size_t snapshotSlots = 4u;
    size_t snapshotSlotBytes = 1024u * 1024u;
    uint32_t snapshotIntervalFrames = 600u;
    size_t pumpFrameBudget = 256u;
    size_t pumpEventBudget = 64u;
};

enum class SourceState : uint8_t {
    Idle = 0,
    WaitingForSession,
    WaitingForStart,
    Streaming,
    Sealed,
    Faulted,
};

const char* sourceStateText(SourceState state);

struct SeekOutcome {
    bool ok = false;
    uint32_t fromFrame = 0u;
    uint32_t targetFrame = 0u;
    uint32_t checkpointFrame = 0u;
    uint32_t fastForwardFrames = 0u;
    bool usedSnapshot = false;
    bool usedMatchStart = false;
    bool requestedRestream = false;
    bool requiresBootstrap = false;
};

class ReplayTimelineSource {
public:
    ReplayTimelineSource();
    ~ReplayTimelineSource();

    ReplayTimelineSource(const ReplayTimelineSource&) = delete;
    ReplayTimelineSource& operator=(const ReplayTimelineSource&) = delete;

    bool start(ReplayIpcWorker& worker, uint64_t expectedManifestDigest,
               const TimelineSourceOptions& options = TimelineSourceOptions{});

    void stop();

    void setSimulationHooks(const SimulationHooks& hooks) { hooks_ = hooks; }

    bool addMatchStart(uint32_t ordinal, uint32_t firstTimelineFrame);

    size_t pump();

    SourceState state() const { return state_; }
    bool ready() const { return started_; }
    bool sessionKnown() const { return sessionKnown_; }

    const BrokerSessionInfo& sessionInfo() const { return session_; }
    const qreplay::DeterministicStartDescriptor& startDescriptor() const {
        return descriptor_;
    }

    qreplay::ConfirmedTimelineBuffer& timeline() { return *timeline_; }
    const qreplay::ConfirmedTimelineBuffer& timeline() const {
        return *timeline_;
    }

    bool timelineStarted() const;
    uint32_t consumeCursor() const;
    uint32_t retentionFloor() const;
    void trimConsumedBelow(uint32_t frame);

    size_t bufferedFrames() const;
    uint32_t nextFrame() const;
    uint64_t rollingDigest() const;

    bool peek(qreplay::ReplayFrameV1& out) const;
    bool consume(uint64_t observedPreStateHash, qreplay::ParseStatus& why);
    void recordPostState(uint32_t afterFrame, uint64_t postHash);
    bool verifyTerminal(qreplay::ParseStatus& why) const;

    bool terminalKnown() const;
    const qreplay::TimelineTerminal& terminal() const;

    bool peekEvent(qreplay::ReplayEvent& out) const;
    bool consumeEvent(qreplay::ReplayEvent& out);

    SourceFault fault() const { return fault_; }
    uint32_t faultDetail() const { return faultDetail_; }
    qreplay::TimelineFault timelineFault() const;
    qreplay::ParseStatus timelineStatus() const { return timelineStatus_; }

    void maybeCaptureSnapshot(uint32_t frameJustCompleted, bool markerBoundary);

    SeekOutcome positionFor(uint32_t targetFrame);

    size_t snapshotCount() const;
    uint64_t snapshotCaptures() const { return snapshotCaptures_; }
    uint64_t snapshotRestores() const { return snapshotRestores_; }
    uint64_t snapshotBytesResident() const;

private:
    struct SnapshotSlot {
        bool used = false;
        uint32_t frame = 0u;
        size_t bytes = 0u;
        std::vector<uint8_t> buffer;
    };

    void setFault(SourceFault fault, uint32_t detail);
    void applyControl(const BrokerControl& control);
    void applyPendingTerminal();
    void installStart(const qreplay::DeterministicStart& start);
    void captureSnapshotAt(uint32_t frame);
    const SnapshotSlot* bestSnapshotFor(uint32_t targetFrame) const;
    bool nearestMatchStart(uint32_t targetFrame, uint32_t& frameOut,
                           uint32_t& ordinalOut) const;

    ReplayIpcWorker* worker_ = nullptr;
    std::unique_ptr<qreplay::ConfirmedTimelineBuffer> timeline_;
    TimelineSourceOptions options_{};
    SimulationHooks hooks_{};

    uint64_t expectedManifestDigest_ = 0u;
    BrokerSessionInfo session_{};
    qreplay::DeterministicStartDescriptor descriptor_{};

    bool started_ = false;
    bool sessionKnown_ = false;
    bool startInstalled_ = false;
    SourceState state_ = SourceState::Idle;

    SourceFault fault_ = SourceFault::None;
    uint32_t faultDetail_ = 0u;
    qreplay::ParseStatus timelineStatus_ = qreplay::ParseStatus::Ok;

    qreplay::ReplayFrameV1 pendingFrame_{};
    bool pendingFrameValid_ = false;
    qreplay::ReplayEvent pendingEvent_{};
    bool pendingEventValid_ = false;

    qreplay::TimelineTerminal pendingTerminal_{};
    bool pendingTerminalValid_ = false;

    std::vector<SnapshotSlot> snapshots_;
    size_t snapshotRoundRobin_ = 0u;
    uint32_t lastSnapshotFrame_ = 0u;
    bool haveSnapshotFrame_ = false;
    uint64_t snapshotCaptures_ = 0u;
    uint64_t snapshotRestores_ = 0u;

    struct MatchStartPoint {
        uint32_t ordinal = 0u;
        uint32_t firstFrame = 0u;
    };
    std::vector<MatchStartPoint> matchStarts_;
};

}
