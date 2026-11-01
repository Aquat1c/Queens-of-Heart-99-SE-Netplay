#pragma once

#include "ReplayParserV2.h"
#include "ReplayWriterV2.h"

#include <array>
#include <string>

namespace qclient::replay {

enum class RecoveryClassV2 : uint8_t {
    Nothing = 0,
    MetadataOnly,
    PlayableUnsealedPrefix,
    FooterCompleteRenameFailure,
};

struct RecoveryPlanV2 {
    RecoveryClassV2 classification = RecoveryClassV2::Nothing;
    qreplay::ParseResult parseResult{};
    ParsedReplayV2 parsed{};
    std::array<uint8_t, 32> sourceRangeDigest{};
    uint64_t sourceRangeBytes = 0u;
};

struct RecoveryOutcomeV2 {
    bool ok = false;
    RecoveryPlanV2 plan{};
    WriterSummaryV2 summary{};
    WriterError writerError = WriterError::None;
    WriterStageV2 writerStage = WriterStageV2::None;
    std::wstring completedRenamePath;
    std::string detail;
};

class ReplayRecoveryV2 {
public:
    static RecoveryPlanV2 plan(
        ReplayByteSource& source, bool sourceNameEndsInPart = true);
    static RecoveryPlanV2 planFile(const std::wstring& path);

    static bool execute(
        const RecoveryPlanV2& plan, const std::wstring& outputRoot,
        const std::wstring& sourceFileName, RecoveryOutcomeV2& out,
        ReplayWriterV2* writer = nullptr);

    static bool finishFooterCompleteRename(
        const std::wstring& partPath, RecoveryOutcomeV2& out);

    static void deriveRecoveredUuid(
        const std::array<uint8_t, 32>& sourceRangeDigest,
        uint64_t& low, uint64_t& high);
};

}
