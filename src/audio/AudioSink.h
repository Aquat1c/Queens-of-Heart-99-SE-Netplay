#pragma once

#include <cstdint>

#include "audio/AudioTypes.h"

namespace qaudio {

struct SinkResult {
    bool ok = false;
    PhysicalToken token{};
};

class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    virtual bool load(int16_t sample, const char* ownedPath) = 0;
    virtual bool unload(int16_t sample) = 0;

    virtual SinkResult playDuplicate(int16_t sample, uint16_t slot,
                                     uint32_t flags) = 0;
    virtual SinkResult playBase(int16_t sample, uint32_t flags) = 0;
    virtual SinkResult playAtCursor(const AudioEndpoint& endpoint,
                                    int16_t sample, uint32_t flags,
                                    uint32_t cursorBytes) = 0;
    virtual bool stopIfOwner(const AudioEndpoint& endpoint,
                             const PhysicalToken& owner) = 0;

    virtual PhysicalToken currentOwner(const AudioEndpoint& endpoint) = 0;
    virtual uint32_t deviceGeneration() = 0;
};

struct SinkAction {
    enum class Type : uint8_t {
        StopIfOwner,
        PlayDuplicate,
        PlayBase,
        PlayAtCursor,
        Load,
        Unload,
    };
    Type type = Type::StopIfOwner;
    AudioEndpoint endpoint{};
    int16_t sampleId = -1;
    uint32_t flags = 0;
    PhysicalToken owner{};
    uint32_t cursorBytes = 0;
    uint32_t sourceFrame = 0;
    uint16_t sourceOrdinal = 0;
    AudioSemanticKey key{};
    uint32_t startFrame = 0;
    uint32_t durationFrames = 0;
    bool looping = false;
    char ownedPath[kAudioPathCapacity] = {};
};

}
