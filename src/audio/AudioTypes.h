#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace qaudio {

enum class AudioTickDisposition : uint8_t {
    ImmediateOffline,
    ForwardVisible,
    ForwardHidden,
    HistoricalCorrection,
    SpectatorConfirmedVisible,
    SpectatorConfirmedCatchup,
    BgmPhysicalService,
    PhysicalFlush,
};

constexpr bool dispositionProducesIntent(AudioTickDisposition d) {
    return d == AudioTickDisposition::ForwardVisible
        || d == AudioTickDisposition::ForwardHidden
        || d == AudioTickDisposition::HistoricalCorrection
        || d == AudioTickDisposition::SpectatorConfirmedVisible
        || d == AudioTickDisposition::SpectatorConfirmedCatchup;
}

constexpr bool dispositionForbidsNativeAudio(AudioTickDisposition d) {
    return d == AudioTickDisposition::HistoricalCorrection
        || d == AudioTickDisposition::SpectatorConfirmedCatchup;
}

enum class AudioKind : uint8_t {
    LoadAttempt,
    Unload,
    Stop,
    PlayDuplicate,
    PlayBase,
    Replace,
};

enum class LoadOutcome : uint8_t {
    NotApplicable,
    Present,
    Absent,
};

struct AudioEndpoint {
    enum class Type : uint8_t { Duplicate, Base };
    Type type = Type::Duplicate;
    uint16_t index = 0;

    bool operator==(const AudioEndpoint& other) const {
        return type == other.type && index == other.index;
    }
    bool operator!=(const AudioEndpoint& other) const {
        return !(*this == other);
    }
};

constexpr uint16_t kMaxSampleId = 255;
constexpr uint16_t kMaxDuplicateSlot = 63;

constexpr bool validSampleId(int32_t sample) {
    return sample >= 0 && sample <= kMaxSampleId;
}
constexpr bool validDuplicateSlot(int32_t slot) {
    return slot >= 0 && slot <= kMaxDuplicateSlot;
}
constexpr bool validEndpoint(const AudioEndpoint& endpoint) {
    return endpoint.type == AudioEndpoint::Type::Duplicate
        ? endpoint.index <= kMaxDuplicateSlot
        : endpoint.index <= kMaxSampleId;
}

struct AssetKey {
    uint64_t pathHash = 0;
    uint32_t generation = 0;

    bool operator==(const AssetKey& other) const {
        return pathHash == other.pathHash && generation == other.generation;
    }
};

struct WaveFacts {
    uint32_t dataBytes = 0;
    uint32_t byteRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bitsPerSample = 0;
    bool looping = false;
    uint32_t durationFrames = 0;
    bool valid = false;
};

constexpr size_t kAudioPathCapacity = 260;

struct AudioCommand {
    uint32_t epoch = 0;
    uint32_t layoutGeneration = 0;
    uint32_t frame = 0;
    uint16_t ordinal = 0;
    AudioKind kind = AudioKind::Stop;
    AudioEndpoint endpoint{};
    int16_t sampleId = -1;
    uint16_t voices = 0;
    uint32_t flags = 0;
    uint32_t callsiteRva = 0;
    AssetKey asset{};
    LoadOutcome loadOutcome = LoadOutcome::NotApplicable;
    // Load only. QOH builds paths in stack buffers, so keep an owned copy: retaining the caller's pointer past the hook is a use-after-scope bug.
    char ownedPath[kAudioPathCapacity] = {};

    void setPath(const char* path) {
        ownedPath[0] = '\0';
        if (path == nullptr) return;
        std::strncpy(ownedPath, path, kAudioPathCapacity - 1);
        ownedPath[kAudioPathCapacity - 1] = '\0';
    }
};

struct AudioSemanticKey {
    AudioKind kind = AudioKind::Stop;
    AudioEndpoint endpoint{};
    int16_t sampleId = -1;
    uint32_t assetGeneration = 0;
    uint32_t flags = 0;
    uint32_t callsiteRva = 0;

    bool operator==(const AudioSemanticKey& other) const {
        return kind == other.kind && endpoint == other.endpoint
            && sampleId == other.sampleId
            && assetGeneration == other.assetGeneration
            && flags == other.flags && callsiteRva == other.callsiteRva;
    }
};

inline AudioSemanticKey semanticKeyOf(const AudioCommand& command) {
    AudioSemanticKey key;
    key.kind = command.kind;
    key.endpoint = command.endpoint;
    key.sampleId = command.sampleId;
    key.assetGeneration = command.asset.generation;
    key.flags = command.flags;
    key.callsiteRva = command.callsiteRva;
    return key;
}

struct PhysicalToken {
    uint32_t deviceGeneration = 0;
    uint32_t endpointGeneration = 0;

    bool operator==(const PhysicalToken& other) const {
        return deviceGeneration == other.deviceGeneration
            && endpointGeneration == other.endpointGeneration;
    }
    constexpr bool valid() const { return endpointGeneration != 0; }
};

struct AudioHorizonConfig {
    uint32_t hiddenForwardAudioHorizonFrames = 2;
    uint32_t spectatorAudioHorizonFrames = 6;
};

constexpr uint32_t clampHiddenForwardHorizon(uint32_t frames) {
    return frames > 12u ? 12u : frames;
}
constexpr uint32_t clampSpectatorHorizon(uint32_t frames) {
    return frames > 30u ? 30u : frames;
}

struct AudioTelemetry {
    uint64_t intentGenerated = 0;
    uint64_t physicalPlayed = 0;
    uint64_t physicalRestartAvoided = 0;
    uint64_t replayCreditConsumed = 0;
    uint64_t correctedNewPlayed = 0;
    uint64_t oldOnlyCanceled = 0;
    uint64_t staleStopRejected = 0;
    uint64_t endpointReplaced = 0;
    uint64_t cursorResumed = 0;
    uint64_t loadDeferred = 0;
    uint64_t assetReconciled = 0;
    uint64_t deviceGenerationChanged = 0;
    uint64_t deviceAssetRehydrated = 0;
    uint64_t bufferLostRecovered = 0;
    uint64_t bgmCommandQueued = 0;
    uint64_t bgmCommandCommitted = 0;
    uint64_t bgmCommandDiscarded = 0;
    uint64_t bgmSameTrackCommandPreserved = 0;
    uint64_t bgmSteadyStateSuppressed = 0;
    uint64_t bgmInternalRecursion = 0;
    uint64_t bgmUnexpectedOwnedCallsite = 0;
    uint64_t bgmQueueOverflow = 0;
    uint64_t hiddenForwardQueued = 0;
    uint64_t hiddenForwardCoalesced = 0;
    uint64_t spectatorStaleSkipped = 0;
    uint64_t journalOverflow = 0;
    uint64_t sinkFailure = 0;
};

constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

inline uint64_t fnv1aBytes(const void* data, size_t length, uint64_t seed) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

inline uint64_t normalizedPathHash(const char* path) {
    if (path == nullptr) return 0;
    uint64_t hash = kFnvOffsetBasis;
    for (const char* p = path; *p != '\0'; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if (c == '/') c = '\\';
        hash ^= c;
        hash *= kFnvPrime;
    }
    return hash;
}

inline uint64_t digestCommand(const AudioCommand& command, uint64_t seed) {
    struct DigestView {
        uint32_t frame;
        uint16_t ordinal;
        uint8_t kind;
        uint8_t endpointType;
        uint16_t endpointIndex;
        int16_t sampleId;
        uint16_t voices;
        uint16_t pad;
        uint32_t flags;
        uint32_t callsiteRva;
        uint64_t assetPathHash;
        uint32_t assetGeneration;
        uint8_t loadOutcome;
        uint8_t pad2[3];
    } view = {};
    view.frame = command.frame;
    view.ordinal = command.ordinal;
    view.kind = static_cast<uint8_t>(command.kind);
    view.endpointType = static_cast<uint8_t>(command.endpoint.type);
    view.endpointIndex = command.endpoint.index;
    view.sampleId = command.sampleId;
    view.voices = command.voices;
    view.flags = command.flags;
    view.callsiteRva = command.callsiteRva;
    view.assetPathHash = command.asset.pathHash;
    view.assetGeneration = command.asset.generation;
    view.loadOutcome = static_cast<uint8_t>(command.loadOutcome);
    return fnv1aBytes(&view, sizeof(view), seed);
}

}
