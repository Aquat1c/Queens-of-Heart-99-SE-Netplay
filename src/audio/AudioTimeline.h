#pragma once

#include <cstddef>
#include <cstdint>

#include "audio/AudioTypes.h"

namespace qaudio {

constexpr size_t kTimelineFrames = 64;
constexpr size_t kMaxCommandsPerFrame = 48;
constexpr size_t kMaxEmissionsPerFrame = 48;

struct FrameJournal {
    uint32_t epoch = 0;
    uint32_t layoutGeneration = 0;
    uint32_t frame = 0;
    uint16_t commandCount = 0;
    bool valid = false;
    bool overflowed = false;
    uint64_t intentDigest = 0;
    AudioCommand commands[kMaxCommandsPerFrame];
};

struct EmittedOccurrence {
    AudioSemanticKey key{};
    uint32_t frame = 0;
    uint16_t ordinal = 0;
    AudioEndpoint endpoint{};
    int16_t sampleId = -1;
    PhysicalToken token{};
    uint32_t startFrame = 0;
    uint32_t durationFrames = 0;
    bool looping = false;
    bool valid = false;
};

struct FrameEmissions {
    uint32_t frame = 0;
    uint16_t count = 0;
    bool valid = false;
    EmittedOccurrence entries[kMaxEmissionsPerFrame];
};

class AudioTimeline {
public:
    void reset() {
        for (size_t i = 0; i < kTimelineFrames; ++i) {
            journals_[i] = FrameJournal{};
            emissions_[i] = FrameEmissions{};
        }
        capture_ = FrameJournal{};
        capturing_ = false;
        correcting_ = false;
        stagingCount_ = 0;
        overflowFailClosed_ = false;
    }

    bool beginCapture(uint32_t epoch, uint32_t layoutGeneration, uint32_t frame) {
        if (capturing_) return false;
        capture_ = FrameJournal{};
        capture_.epoch = epoch;
        capture_.layoutGeneration = layoutGeneration;
        capture_.frame = frame;
        capture_.valid = true;
        capturing_ = true;
        return true;
    }

    bool capturing() const { return capturing_; }

    bool append(const AudioCommand& command, AudioTelemetry& telemetry) {
        if (!capturing_) return false;
        if (capture_.commandCount >= kMaxCommandsPerFrame) {
            capture_.overflowed = true;
            overflowFailClosed_ = true;
            ++telemetry.journalOverflow;
            return false;
        }
        AudioCommand stored = command;
        stored.epoch = capture_.epoch;
        stored.layoutGeneration = capture_.layoutGeneration;
        stored.frame = capture_.frame;
        stored.ordinal = capture_.commandCount;
        capture_.commands[capture_.commandCount++] = stored;
        ++telemetry.intentGenerated;
        return true;
    }

    uint16_t captureCount() const { return capture_.commandCount; }

    bool commitCapture() {
        if (!capturing_) return false;
        normalize(capture_);
        capture_.intentDigest = digestJournal(capture_);
        if (correcting_) {
            if (stagingCount_ >= kTimelineFrames) {
                capturing_ = false;
                overflowFailClosed_ = true;
                return false;
            }
            staging_[stagingCount_++] = capture_;
        } else {
            journals_[capture_.frame % kTimelineFrames] = capture_;
        }
        capturing_ = false;
        return true;
    }

    void abortCapture() {
        capture_ = FrameJournal{};
        capturing_ = false;
    }

    bool beginCorrection() {
        if (correcting_ || capturing_) return false;
        correcting_ = true;
        stagingCount_ = 0;
        return true;
    }

    bool correcting() const { return correcting_; }
    size_t stagedCount() const { return stagingCount_; }
    const FrameJournal* staged(size_t index) const {
        return index < stagingCount_ ? &staging_[index] : nullptr;
    }

    bool commitCorrection() {
        if (!correcting_) return false;
        for (size_t i = 0; i < stagingCount_; ++i)
            journals_[staging_[i].frame % kTimelineFrames] = staging_[i];
        stagingCount_ = 0;
        correcting_ = false;
        return true;
    }

    void abortCorrection() {
        stagingCount_ = 0;
        correcting_ = false;
    }

    bool recordEmission(const EmittedOccurrence& occurrence,
                        AudioTelemetry& telemetry) {
        FrameEmissions& slot = emissions_[occurrence.frame % kTimelineFrames];
        if (!slot.valid || slot.frame != occurrence.frame) {
            slot = FrameEmissions{};
            slot.frame = occurrence.frame;
            slot.valid = true;
        }
        if (slot.count >= kMaxEmissionsPerFrame) {
            overflowFailClosed_ = true;
            ++telemetry.journalOverflow;
            return false;
        }
        EmittedOccurrence stored = occurrence;
        stored.valid = true;
        slot.entries[slot.count++] = stored;
        return true;
    }

    void replaceEmissions(uint32_t frame, const EmittedOccurrence* entries,
                          size_t count) {
        FrameEmissions& slot = emissions_[frame % kTimelineFrames];
        slot = FrameEmissions{};
        slot.frame = frame;
        slot.valid = true;
        for (size_t i = 0; i < count && i < kMaxEmissionsPerFrame; ++i) {
            slot.entries[slot.count] = entries[i];
            slot.entries[slot.count].valid = true;
            ++slot.count;
        }
    }

    const FrameJournal* journalFor(uint32_t frame) const {
        const FrameJournal& slot = journals_[frame % kTimelineFrames];
        return slot.valid && slot.frame == frame ? &slot : nullptr;
    }

    const FrameEmissions* emissionsFor(uint32_t frame) const {
        const FrameEmissions& slot = emissions_[frame % kTimelineFrames];
        return slot.valid && slot.frame == frame ? &slot : nullptr;
    }

    bool overflowFailClosed() const { return overflowFailClosed_; }

    uint64_t intentDigestFor(uint32_t frame) const {
        const FrameJournal* journal = journalFor(frame);
        return journal ? journal->intentDigest : 0;
    }

private:
    static void normalize(FrameJournal& journal);

    static uint64_t digestJournal(const FrameJournal& journal) {
        uint64_t digest = kFnvOffsetBasis;
        for (uint16_t i = 0; i < journal.commandCount; ++i)
            digest = digestCommand(journal.commands[i], digest);
        return digest;
    }

    FrameJournal journals_[kTimelineFrames];
    FrameEmissions emissions_[kTimelineFrames];
    FrameJournal staging_[kTimelineFrames];
    size_t stagingCount_ = 0;
    FrameJournal capture_{};
    bool capturing_ = false;
    bool correcting_ = false;
    bool overflowFailClosed_ = false;
};

}
