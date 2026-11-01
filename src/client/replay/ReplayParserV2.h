#pragma once

#include "ReplayParser.h"

#include "replay/ConfirmedTimeline.h"
#include "replay/ReplayCompatibilityIdentityV3.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qclient::replay {

enum class ReplayClassificationV2 : uint8_t {
    Invalid = 0,
    MetadataOnly,
    UnsealedValidatedPrefix,
    Sealed,
    FooterCompleteRenameFailure,
};

struct ChunkLocationV2 {
    uint64_t headerOffset = 0u;
    uint64_t payloadOffset = 0u;
    qreplay::ChunkHeaderV2 header{};

    uint64_t endOffset() const {
        return payloadOffset + header.payloadBytes;
    }
};

struct ParseOptionsV2 {
    bool retainTimeline = true;
    bool allowUnsealedPrefix = true;
    bool sourceNameEndsInPart = false;
    bool collectChunkIndex = false;
    bool verifyFileDigest = true;
    size_t maxRetainedFrames =
        qreplay::kMaxPlaybackPrebufferFramesV2;
    size_t maxRetainedEvents =
        qreplay::kMaxTimelineEventsV2;
};

struct ParsedReplayV2 {
    ReplayClassificationV2 classification =
        ReplayClassificationV2::Invalid;
    qreplay::ReplayPrefixV2 prefix{};
    qreplay::ConfirmedGameStartV2 start{};
    qreplay::ConfirmedGameEndV2 end{};
    qreplay::FooterIndexV2 footer{};
    qreplay::ReplayCompatibilityIdentityEnvelopeV3 compatibilityIdentity{
        qreplay::ReplayCompatibilityIdentityStatusV3::LegacyUnattested,
        {}};
    std::vector<qreplay::CanonicalTickBundleV2> ticks;
    std::vector<qreplay::ReplayPaletteV2> palettes;
    std::vector<ChunkLocationV2> chunks;
    FileIdentity fileIdentity{};

    uint64_t sourceBytes = 0u;
    uint64_t lastValidChunkEnd = 0u;
    uint64_t validatedSourceBytes = 0u;
    std::array<uint8_t, 32> validatedSourceRangeDigest{};
    std::array<uint8_t, 32> computedFileDigest{};
    uint32_t chunkCount = 0u;
    uint32_t frameCount = 0u;
    uint32_t eventCount = 0u;
    uint32_t nextEventSequence = 0u;
    uint64_t lastPostStateHash = 0u;
    uint64_t finalRollingDigest = 0u;
    bool hasManifest = false;
    bool hasStart = false;
    bool hasFrameZero = false;
    bool hasEnd = false;
    bool hasFooter = false;
    bool hasCompatibilityIdentity = false;
    bool structurallyVerified = false;
    bool fileDigestVerified = false;
    bool prebufferTooLarge = false;

    bool sealed() const {
        return classification == ReplayClassificationV2::Sealed
            || classification
                == ReplayClassificationV2::FooterCompleteRenameFailure;
    }
};

class ReplayParserV2 {
public:
    static qreplay::ParseResult parse(
        ReplayByteSource& source, const ParseOptionsV2& options,
        ParsedReplayV2& out);
    static qreplay::ParseResult parseFile(
        const std::wstring& path, const ParseOptionsV2& options,
        ParsedReplayV2& out);
};

}
