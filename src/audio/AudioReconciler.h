#pragma once

#include <cstddef>
#include <cstdint>

#include "audio/AudioSink.h"
#include "audio/AudioTimeline.h"
#include "audio/AudioTypes.h"
#include "audio/WaveMetadata.h"

namespace qaudio {

constexpr size_t kMaxTrackedEndpoints = 96;
constexpr size_t kMaxGenerationsPerEndpoint = 24;
constexpr size_t kMaxReconcileActions = 192;
constexpr size_t kMaxCorrectedEmissions = 192;
constexpr uint32_t kMatchMaxFrameDistance = 30;

class IAssetCatalog {
public:
    virtual ~IAssetCatalog() = default;
    virtual WaveFacts factsFor(int16_t sample,
                               const AssetKey& asset) const = 0;
};

struct OwnerSnapshotEntry {
    AudioEndpoint endpoint{};
    PhysicalToken token{};
    bool valid = false;
};

struct OwnerSnapshot {
    OwnerSnapshotEntry entries[kMaxTrackedEndpoints];
    size_t count = 0;

    PhysicalToken ownerOf(const AudioEndpoint& endpoint) const {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].valid && entries[i].endpoint == endpoint)
                return entries[i].token;
        }
        return PhysicalToken{};
    }
    bool set(const AudioEndpoint& endpoint, const PhysicalToken& token) {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].valid && entries[i].endpoint == endpoint) {
                entries[i].token = token;
                return true;
            }
        }
        if (count >= kMaxTrackedEndpoints) return false;
        entries[count].endpoint = endpoint;
        entries[count].token = token;
        entries[count].valid = true;
        ++count;
        return true;
    }
};

struct ResourceEntry {
    bool present = false;
    AssetKey asset{};
    char ownedPath[kAudioPathCapacity] = {};
};

struct ResourceMap {
    ResourceEntry samples[kMaxSampleId + 1];
};

struct PlannedEmission {
    EmittedOccurrence occurrence{};
    int tokenFromAction = -1;
};

struct ReconcileCounters {
    uint64_t replayCreditConsumed = 0;
    uint64_t correctedNewPlayed = 0;
    uint64_t correctedNewExpiredSkipped = 0;
    uint64_t oldOnlyCanceled = 0;
    uint64_t endpointReplaced = 0;
    uint64_t cursorResumed = 0;
    uint64_t assetReconciled = 0;
    uint64_t matchedTotal = 0;
    uint64_t correctedTotal = 0;
    uint64_t oldTotal = 0;
};

struct ReconcileInput {
    const AudioTimeline* timeline = nullptr;
    uint32_t restoreFrame = 0;
    uint32_t frontierFrame = 0;
    const FrameJournal* const* corrected = nullptr;
    size_t correctedCount = 0;
    const IAssetCatalog* catalog = nullptr;
    const OwnerSnapshot* owners = nullptr;
    const ResourceMap* baselineResources = nullptr;
};

struct ReconcileResult {
    bool valid = false;
    SinkAction actions[kMaxReconcileActions];
    size_t actionCount = 0;
    PlannedEmission emissions[kMaxCorrectedEmissions];
    size_t emissionCount = 0;
    ResourceMap correctedResources{};
    ReconcileCounters counters{};
};

void reconcileCorrection(const ReconcileInput& input,
                         ReconcileResult& result);

void foldResources(ResourceMap& map, const FrameJournal& journal);

}
