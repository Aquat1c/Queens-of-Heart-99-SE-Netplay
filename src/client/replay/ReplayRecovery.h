#pragma once

#include "ReplayParser.h"
#include "ReplayWriter.h"

#include "replay/ReplayTypes.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace qclient::replay {

enum class RecoveryClass : uint8_t {
    Nothing = 0,
    MetadataOnly,
    NotBootable,
    Playable,
};

const char* recoveryClassText(RecoveryClass value);

struct RecoveryPlan {
    bool prefixValid = false;
    qreplay::ParseResult stop{};

    uint64_t validBytes = 0u;
    uint32_t validChunks = 0u;
    uint32_t lastValidSequence = 0u;
    uint32_t lastValidMatchId = 0u;
    uint32_t lastValidTimelineFrame = 0u;
    uint32_t firstTimelineFrame = 0u;

    bool hasManifest = false;
    bool hasSessionStart = false;
    bool hasFooter = false;
    bool hasSessionEnd = false;

    uint32_t startedMatches = 0u;
    uint32_t completedMatches = 0u;
    bool lastMatchOpen = false;
    uint32_t openMatchId = 0u;
    bool anyIndependentBootstrap = false;
    uint32_t retainedFrames = 0u;

    RecoveryClass classification = RecoveryClass::Nothing;

    qreplay::ReplayPrefix sourcePrefix{};
    qreplay::SessionManifest manifest{};

    std::array<uint8_t, 32> sourceDigest{};

    uint64_t lastRollingDigest = 0u;
    uint64_t lastPostStateHash = 0u;
};

struct RecoveryProvenance {
    uint64_t sourceSessionUuidLow = 0u;
    uint64_t sourceSessionUuidHigh = 0u;
    std::array<uint8_t, 32> sourceDigest{};
    std::string sourceFileName;
    uint64_t salvagedBytes = 0u;
    uint32_t lastValidTimelineFrame = 0u;
    RecoveryClass classification = RecoveryClass::Nothing;
};

struct RecoveryOutcome {
    bool ok = false;
    RecoveryPlan plan{};
    RecoveryProvenance provenance{};
    WriterSummary summary{};
    WriterError writerError = WriterError::None;
    std::string detail;
};

class ReplayRecovery {
public:
    static RecoveryPlan plan(ReplayByteSource& source);
    static RecoveryPlan planFile(const std::wstring& path);

    static bool execute(ReplayByteSource& source,
                        const RecoveryPlan& plan,
                        const std::wstring& outputRoot,
                        const std::wstring& sourceFileName,
                        RecoveryOutcome& out,
                        ReplayWriter* writer = nullptr);

    static void deriveRecoveredUuid(const std::array<uint8_t, 32>& sourceDigest,
                                    uint64_t& low,
                                    uint64_t& high);
};

}
