#pragma once

#include <cstdint>

#include "audio/AudioReconciler.h"
#include "audio/AudioSink.h"
#include "audio/AudioTimeline.h"
#include "audio/AudioTypes.h"
#include "audio/QohBgmBoundaryGuard.h"
#include "audio/WaveMetadata.h"

namespace qaudio {

struct NativeAudioOps {
    int (*load)(int sample, const char* path) = nullptr;
    int (*unload)(int sample) = nullptr;
    int (*playDuplicate)(int sample, unsigned int slot, int voices,
                         int flags) = nullptr;
    int (*playBase)(int sample, int flags) = nullptr;
    int (*stop)(int index, int domain) = nullptr;
    int (*bgm)(int trackId, int flags) = nullptr;
    int (*setCursor)(const AudioEndpoint& endpoint,
                     uint32_t cursorBytes) = nullptr;
};

class AssetCatalog : public IAssetCatalog {
public:
    void reset();
    void seal(bool sealed) { sealed_ = sealed; }
    bool sealed() const { return sealed_; }

    void observeLoad(int16_t sample, const char* path, bool succeeded,
                     const WaveFacts& facts);
    void observeUnload(int16_t sample);

    bool present(int16_t sample) const;
    AssetKey assetOf(int16_t sample) const;
    const char* pathOf(int16_t sample) const;
    uint64_t missCount() const { return misses_; }

    WaveFacts factsFor(int16_t sample,
                       const AssetKey& asset) const override;

    void snapshot(ResourceMap& out) const;

private:
    struct Entry {
        bool present = false;
        AssetKey asset{};
        WaveFacts facts{};
        char path[kAudioPathCapacity] = {};
    };
    Entry entries_[kMaxSampleId + 1];
    uint32_t nextGeneration_ = 1;
    bool sealed_ = false;
    mutable uint64_t misses_ = 0;
};

class QohDirectSoundSink : public IAudioSink {
public:
    void configure(const NativeAudioOps& ops) { ops_ = ops; }
    void reset();
    void setMuted(bool muted) { muted_ = muted; }
    bool muted() const { return muted_; }
    void bumpDeviceGeneration() { ++deviceGeneration_; }
    void discardReplayTakeoverTransientOwners();

    bool load(int16_t sample, const char* ownedPath) override;
    bool unload(int16_t sample) override;
    SinkResult playDuplicate(int16_t sample, uint16_t slot,
                             uint32_t flags) override;
    SinkResult playBase(int16_t sample, uint32_t flags) override;
    SinkResult playAtCursor(const AudioEndpoint& endpoint, int16_t sample,
                            uint32_t flags, uint32_t cursorBytes) override;
    bool stopIfOwner(const AudioEndpoint& endpoint,
                     const PhysicalToken& owner) override;
    PhysicalToken currentOwner(const AudioEndpoint& endpoint) override;
    uint32_t deviceGeneration() override { return deviceGeneration_; }

    uint64_t failures() const { return failures_; }
    uint64_t staleStopsRejected() const { return staleStops_; }

private:
    PhysicalToken issue(const AudioEndpoint& endpoint);

    NativeAudioOps ops_{};
    OwnerSnapshot owners_{};
    uint32_t deviceGeneration_ = 1;
    uint32_t nextEndpointGeneration_ = 1;
    bool muted_ = false;
    uint64_t failures_ = 0;
    uint64_t staleStops_ = 0;
};

class QohAudioRuntime {
public:
    struct ReplayCheckpoint;
    void configure(const NativeAudioOps& ops);
    void resetSession();
    void setMuted(bool muted) { sink_.setMuted(muted); }
    void setHorizons(const AudioHorizonConfig& horizons);

    AssetCatalog& catalog() { return catalog_; }
    QohBgmBoundaryGuard& bgm() { return bgm_; }
    QohDirectSoundSink& sink() { return sink_; }
    void onDeviceTeardown();
    const AudioTelemetry& telemetry() const { return telemetry_; }
    bool failedClosed() const;

    AudioTickDisposition disposition() const { return disposition_; }
    void setDisposition(AudioTickDisposition disposition) {
        disposition_ = disposition;
    }
    bool beginTick(uint32_t epoch, uint32_t layoutGeneration, uint32_t frame);
    bool commitTick();
    void abortTick();
    bool beginReplayTakeover();
    bool restoreAfterReplayTakeover();
    void endReplayTakeover();
    bool captureReplayStart();
    bool restoreReplayStart();
    bool captureReplayCheckpoint(ReplayCheckpoint& checkpoint);
    bool restoreReplayCheckpoint(const ReplayCheckpoint& checkpoint);
    void flushPending(uint32_t confirmedThrough);
    void flushPendingMuted(uint32_t confirmedThrough);

    bool capturesIntent() const;
    bool forbidsNativeAudio() const;

    void captureLoad(int16_t sample, const char* path, bool cachedSuccess,
                     const AssetKey& asset, uint32_t callsiteRva);
    void captureUnload(int16_t sample, uint32_t callsiteRva);
    void captureStop(const AudioEndpoint& endpoint, uint32_t callsiteRva);
    void capturePlayDuplicate(int16_t sample, uint16_t slot, uint16_t voices,
                              uint32_t flags, uint32_t callsiteRva);
    void capturePlayBase(int16_t sample, uint32_t flags,
                         uint32_t callsiteRva);
    bool captureBgm(uint8_t trackId, uint32_t flags, uint32_t callsiteRva);

    bool beginCorrection(uint32_t restoreFrame, uint32_t frontierFrame);
    bool commitCorrection();
    void abortCorrection();
    bool correcting() const { return correcting_; }

    void rotateEpoch(uint32_t newEpoch);

    void formatLedger(char* out, size_t capacity) const;

private:
    void executePlan(const ReconcileResult& plan);
    void emitJournal(const FrameJournal& journal);

    AudioTimeline timeline_;
    AssetCatalog catalog_;
    QohDirectSoundSink sink_;
    NativeAudioOps ops_{};
    QohBgmBoundaryGuard bgm_;
    AudioTelemetry telemetry_{};
    AudioHorizonConfig horizons_{};
    ReconcileResult plan_{};
    ResourceMap resources_{};

    AudioTickDisposition disposition_ = AudioTickDisposition::ImmediateOffline;
    uint32_t epoch_ = 0;
    uint32_t layoutGeneration_ = 0;
    uint32_t frame_ = 0;
    uint32_t correctionRestore_ = 0;
    uint32_t correctionFrontier_ = 0;
    uint16_t ordinal_ = 0;
    bool correcting_ = false;
    bool ticking_ = false;
    static constexpr size_t kPendingCapacity = 64;
    struct PendingFrame {
        uint32_t frame = 0;
        AudioTickDisposition disposition =
            AudioTickDisposition::ImmediateOffline;
    };
    PendingFrame pendingFrames_[kPendingCapacity] = {};
    size_t pendingCount_ = 0;
    uint64_t correctedNewExpired_ = 0;
    bool failClosed_ = false;

    struct ReplayTakeoverBaseline {
        AssetCatalog catalog{};
        QohDirectSoundSink sink{};
        QohBgmBoundaryGuard bgm{};
        AudioTelemetry telemetry{};
        ResourceMap resources{};
        AudioTickDisposition disposition =
            AudioTickDisposition::ImmediateOffline;
        uint32_t epoch = 0u;
        uint32_t layoutGeneration = 0u;
        uint32_t frame = 0u;
        uint16_t ordinal = 0u;
        PendingFrame pendingFrames[kPendingCapacity]{};
        size_t pendingCount = 0u;
        uint64_t correctedNewExpired = 0u;
        bool failClosed = false;
        bool valid = false;
    };
    ReplayTakeoverBaseline replayTakeoverBaseline_{};
    ReplayTakeoverBaseline replayStartBaseline_{};
    bool captureReplayBaseline(ReplayTakeoverBaseline& baseline);
    bool restoreReplayBaseline(const ReplayTakeoverBaseline& baseline);
    uint64_t replayCheckpointSession_ = 1u;

public:
    struct ReplayCheckpoint {
    private:
        friend class QohAudioRuntime;
        ReplayTakeoverBaseline baseline{};
        const QohAudioRuntime* owner = nullptr;
        uint64_t session = 0u;
        uint32_t deviceGeneration = 0u;
    };
};

QohAudioRuntime& runtime();

}
