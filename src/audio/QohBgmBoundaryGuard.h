#pragma once

#include <cstdint>

#include "audio/AudioTypes.h"

namespace qaudio {

constexpr uint32_t kBgmOwnedCallsiteA = 0x42786Au;
constexpr uint32_t kBgmOwnedCallsiteB = 0x4278A8u;

constexpr bool bgmOwnedCallsite(uint32_t callsiteRva) {
    return callsiteRva == kBgmOwnedCallsiteA
        || callsiteRva == kBgmOwnedCallsiteB;
}

// Track 255 is the verified "stop BGM" request.
constexpr uint8_t kBgmStopTrack = 255;

struct PendingBgmCommand {
    uint32_t epoch = 0;
    uint32_t frame = 0;
    uint32_t callsiteRva = 0;
    uint16_t ordinal = 0;
    uint8_t trackId = 0;
    uint32_t flags = 0;
    bool committed = false;
    bool valid = false;
};

constexpr size_t kBgmQueueCapacity = 16;

class QohBgmBoundaryGuard {
public:
    void reset() {
        for (size_t i = 0; i < kBgmQueueCapacity; ++i)
            queue_[i] = PendingBgmCommand{};
        count_ = 0;
        failedClosed_ = false;
        hasDrained_ = false;
        lastDrainedTrack_ = 0;
        lastDrainedFlags_ = 0;
    }

    bool failedClosed() const { return failedClosed_; }
    size_t pendingCount() const { return count_; }
    const PendingBgmCommand* pending(size_t index) const {
        return index < count_ ? &queue_[index] : nullptr;
    }

    bool capture(uint32_t epoch, uint32_t frame, uint32_t callsiteRva,
                 uint16_t ordinal, uint8_t trackId, uint32_t flags,
                 AudioTelemetry& telemetry) {
        if (!bgmOwnedCallsite(callsiteRva)) {
            ++telemetry.bgmUnexpectedOwnedCallsite;
            failedClosed_ = true;
            return false;
        }
        const PendingBgmCommand* newest =
            count_ > 0 ? &queue_[count_ - 1] : nullptr;
        const bool sameAsLatest = newest != nullptr
            ? (newest->trackId == trackId && newest->flags == flags)
            : (hasDrained_ && lastDrainedTrack_ == trackId
               && lastDrainedFlags_ == flags);
        if (sameAsLatest) {
            ++telemetry.bgmSteadyStateSuppressed;
            return true;
        }
        if (count_ >= kBgmQueueCapacity) {
            ++telemetry.bgmQueueOverflow;
            failedClosed_ = true;
            return false;
        }
        PendingBgmCommand& command = queue_[count_++];
        command = PendingBgmCommand{};
        command.epoch = epoch;
        command.frame = frame;
        command.callsiteRva = callsiteRva;
        command.ordinal = ordinal;
        command.trackId = trackId;
        command.flags = flags;
        command.valid = true;
        ++telemetry.bgmCommandQueued;
        return true;
    }

    void commitFrame(uint32_t frame, AudioTelemetry& telemetry) {
        for (size_t i = 0; i < count_; ++i) {
            if (queue_[i].frame == frame && !queue_[i].committed) {
                queue_[i].committed = true;
                ++telemetry.bgmCommandCommitted;
            }
        }
    }
    void abortFrame(uint32_t frame, AudioTelemetry& telemetry) {
        removeIf(telemetry, [frame](const PendingBgmCommand& command) {
            return command.frame == frame && !command.committed;
        });
    }

    void removeRange(uint32_t restoreFrame, uint32_t frontierFrame,
                     AudioTelemetry& telemetry) {
        removeIf(telemetry,
                 [restoreFrame, frontierFrame](
                     const PendingBgmCommand& command) {
                     return command.frame >= restoreFrame
                         && command.frame < frontierFrame;
                 });
    }

    template <typename Executor>
    size_t drainConfirmed(uint32_t confirmedThrough, Executor&& execute,
                          AudioTelemetry& telemetry) {
        size_t drained = 0;
        size_t write = 0;
        uint8_t lastTrack = 0xFE;
        for (size_t read = 0; read < count_; ++read) {
            PendingBgmCommand& command = queue_[read];
            const bool ready = command.committed
                && command.frame <= confirmedThrough;
            if (!ready) {
                queue_[write++] = command;
                continue;
            }
            if (command.trackId == lastTrack)
                ++telemetry.bgmSameTrackCommandPreserved;
            lastTrack = command.trackId;
            hasDrained_ = true;
            lastDrainedTrack_ = command.trackId;
            lastDrainedFlags_ = command.flags;
            execute(command);
            ++drained;
        }
        count_ = write;
        return drained;
    }

    bool foldForSpectator(uint32_t confirmedThrough,
                          PendingBgmCommand& latestOut) {
        bool found = false;
        for (size_t i = 0; i < count_; ++i) {
            if (queue_[i].committed && queue_[i].frame <= confirmedThrough) {
                latestOut = queue_[i];
                found = true;
            }
        }
        if (found) {
            hasDrained_ = true;
            lastDrainedTrack_ = latestOut.trackId;
            lastDrainedFlags_ = latestOut.flags;
            size_t write = 0;
            for (size_t read = 0; read < count_; ++read) {
                if (!(queue_[read].committed
                      && queue_[read].frame <= confirmedThrough)) {
                    queue_[write++] = queue_[read];
                }
            }
            count_ = write;
        }
        return found;
    }

private:
    template <typename Predicate>
    void removeIf(AudioTelemetry& telemetry, Predicate&& predicate) {
        size_t write = 0;
        for (size_t read = 0; read < count_; ++read) {
            if (predicate(queue_[read])) {
                ++telemetry.bgmCommandDiscarded;
                continue;
            }
            queue_[write++] = queue_[read];
        }
        count_ = write;
    }

    PendingBgmCommand queue_[kBgmQueueCapacity];
    size_t count_ = 0;
    bool failedClosed_ = false;
    bool hasDrained_ = false;
    uint8_t lastDrainedTrack_ = 0;
    uint32_t lastDrainedFlags_ = 0;
};

}
