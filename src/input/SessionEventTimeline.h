#pragma once

#include <cstdint>

namespace qinput {

enum class SessionEventKind : uint8_t {
    PauseSet = 0,          // value = desired paused state (not a toggle)
    TerminateSession = 1,  // value = bounded terminal reason
    RematchCommit = 2,     // value = canonical prompt frame
    RematchDecline = 3,    // value = canonical prompt frame
    ReturnToCharacterSelect = 4,
};

enum class SessionEventState : uint8_t {
    Idle = 0,
    Requested = 1,
    Quiescing = 2,
    Prepared = 3,
    AtBoundary = 4,
    Committed = 5,
    Applied = 6,
    Failed = 7,
};

struct QuiesceReport {
    bool valid = false;
    uint32_t nextFrame = 0;
    uint32_t assignedTailExclusive = 0;
    uint32_t oldestRestorable = 0;
    uint32_t spectatorFrontier = 0;
    uint32_t lifecyclePhaseKey = 0;
    bool delayTransitionPending = false;
    bool correctionPending = false;
};

struct SessionEvent {
    uint32_t eventId = 0;
    SessionEventKind kind = SessionEventKind::PauseSet;
    uint32_t value = 0;
    uint32_t boundary = 0;
    SessionEventState state = SessionEventState::Idle;
};

constexpr uint32_t kSessionEventMaxDrainFrames = 120;

class SessionEventTimeline {
public:
    void reset(bool isHost) {
        isHost_ = isHost;
        current_ = SessionEvent{};
        nextEventId_ = 1;
        lastAppliedId_ = 0;
        paused_ = false;
        terminated_ = false;
    }

    bool isHost() const { return isHost_; }
    bool paused() const { return paused_; }
    bool terminated() const { return terminated_; }
    const SessionEvent& current() const { return current_; }
    uint32_t lastAppliedId() const { return lastAppliedId_; }

    bool hostAcceptRequest(SessionEventKind kind, uint32_t value) {
        if (!isHost_ || terminated_) return false;
        if (kind == SessionEventKind::PauseSet
            && (value != 0) == paused_
            && current_.state == SessionEventState::Idle) {
            return false;
        }
        if (current_.state != SessionEventState::Idle) {
            const bool terminatePreempts =
                kind == SessionEventKind::TerminateSession
                && current_.kind != SessionEventKind::TerminateSession;
            const bool returnPreempts =
                kind == SessionEventKind::ReturnToCharacterSelect
                && current_.kind != SessionEventKind::TerminateSession
                && current_.kind
                       != SessionEventKind::ReturnToCharacterSelect;
            const bool declinePreemptsCommit =
                kind == SessionEventKind::RematchDecline
                && current_.kind == SessionEventKind::RematchCommit;
            if ((terminatePreempts || returnPreempts
                 || declinePreemptsCommit)
                && current_.state < SessionEventState::Committed) {
                current_ = SessionEvent{};
            } else {
                return false;
            }
        }
        current_.eventId = nextEventId_++;
        current_.kind = kind;
        current_.value = value;
        current_.boundary = 0;
        current_.state = SessionEventState::Quiescing;
        localReport_ = QuiesceReport{};
        remoteReport_ = QuiesceReport{};
        return true;
    }

    bool hostChooseBoundary(uint32_t& boundaryOut) {
        if (!isHost_ || current_.state != SessionEventState::Quiescing
            || !localReport_.valid || !remoteReport_.valid) {
            return false;
        }
        if (localReport_.lifecyclePhaseKey
            != remoteReport_.lifecyclePhaseKey) {
            fail();
            return false;
        }
        if (localReport_.delayTransitionPending
            || remoteReport_.delayTransitionPending) {
            fail();
            return false;
        }
        uint32_t boundary = localReport_.nextFrame;
        const uint32_t floors[5] = {
            remoteReport_.nextFrame,
            localReport_.assignedTailExclusive,
            remoteReport_.assignedTailExclusive,
            localReport_.spectatorFrontier,
            remoteReport_.spectatorFrontier,
        };
        for (uint32_t floor : floors)
            if (static_cast<int32_t>(floor - boundary) > 0)
                boundary = floor;
        const uint32_t reference =
            localReport_.nextFrame < remoteReport_.nextFrame
                ? localReport_.nextFrame : remoteReport_.nextFrame;
        if (boundary - reference > kSessionEventMaxDrainFrames) {
            fail();
            return false;
        }
        if (static_cast<int32_t>(boundary - localReport_.oldestRestorable)
                < 0
            || static_cast<int32_t>(boundary
                                    - remoteReport_.oldestRestorable)
                < 0) {
            fail();
            return false;
        }
        current_.boundary = boundary;
        current_.state = SessionEventState::Prepared;
        boundaryOut = boundary;
        return true;
    }

    void submitLocalReport(const QuiesceReport& report) {
        if (current_.state == SessionEventState::Quiescing)
            localReport_ = report;
    }
    void submitRemoteReport(const QuiesceReport& report) {
        if (current_.state == SessionEventState::Quiescing)
            remoteReport_ = report;
    }

    bool peerBeginQuiesce(uint32_t eventId, SessionEventKind kind,
                          uint32_t value) {
        if (isHost_ || terminated_) return false;
        if (eventId <= lastAppliedId_) return false;
        if (current_.state != SessionEventState::Idle
            && current_.eventId != eventId) {
            const bool terminateReplacement =
                kind == SessionEventKind::TerminateSession;
            const bool returnReplacement =
                kind == SessionEventKind::ReturnToCharacterSelect
                && current_.kind != SessionEventKind::TerminateSession;
            if (!terminateReplacement && !returnReplacement) return false;
        }
        current_.eventId = eventId;
        current_.kind = kind;
        current_.value = value;
        current_.boundary = 0;
        current_.state = SessionEventState::Quiescing;
        return true;
    }

    bool acceptPrepare(uint32_t eventId, uint32_t boundary,
                       uint32_t localFrontier) {
        if (current_.eventId != eventId
            || current_.state != SessionEventState::Quiescing) {
            return current_.eventId == eventId
                && current_.state >= SessionEventState::Prepared;
        }
        if (static_cast<int32_t>(boundary - localFrontier) < 0) {
            fail();
            return false;
        }
        current_.boundary = boundary;
        current_.state = SessionEventState::Prepared;
        return true;
    }

    bool markAtBoundary(uint32_t eventId) {
        if (current_.eventId != eventId
            || current_.state != SessionEventState::Prepared) {
            return false;
        }
        current_.state = SessionEventState::AtBoundary;
        return true;
    }

    bool commit(uint32_t eventId) {
        if (current_.eventId != eventId
            || current_.state != SessionEventState::AtBoundary) {
            return current_.eventId == eventId
                && current_.state >= SessionEventState::Committed;
        }
        current_.state = SessionEventState::Committed;
        return true;
    }

    bool applyOnce(uint32_t eventId) {
        if (current_.eventId != eventId
            || current_.state != SessionEventState::Committed
            || eventId <= lastAppliedId_) {
            return false;
        }
        current_.state = SessionEventState::Applied;
        lastAppliedId_ = eventId;
        if (current_.kind == SessionEventKind::PauseSet) {
            paused_ = current_.value != 0;
        } else if (current_.kind == SessionEventKind::TerminateSession) {
            terminated_ = true;
        } else if (current_.kind
                   == SessionEventKind::ReturnToCharacterSelect) {
            paused_ = false;
        }
        current_ = SessionEvent{};
        return true;
    }

    bool abortBeforePrepare(uint32_t eventId) {
        if (current_.eventId != eventId
            || current_.state != SessionEventState::Quiescing) {
            return false;
        }
        current_ = SessionEvent{};
        return true;
    }

    void fail() {
        current_.state = SessionEventState::Failed;
        terminated_ = true;
    }

private:
    bool isHost_ = false;
    SessionEvent current_{};
    QuiesceReport localReport_{};
    QuiesceReport remoteReport_{};
    uint32_t nextEventId_ = 1;
    uint32_t lastAppliedId_ = 0;
    bool paused_ = false;
    bool terminated_ = false;
};

}
