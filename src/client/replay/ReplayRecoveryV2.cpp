#include "ReplayRecoveryV2.h"

#include "replay/ReplayIntegrity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace qclient::replay {

using namespace qreplay;

namespace {

std::string baseNameToken(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    const std::wstring base =
        slash == std::wstring::npos ? path : path.substr(slash + 1u);
    return ReplayWriter::sanitizeNameToken(
        sanitizeForDisplay(base, 96u), 48u);
}

bool rebuildCanonicalEnd(
    const ConfirmedGameStartV2& start,
    const std::vector<CanonicalTickBundleV2>& ticks,
    ConfirmedGameEndV2& end) {
    CanonicalGameDigestV2 digest;
    if (!digest.begin(start)) return false;
    for (const CanonicalTickBundleV2& tick : ticks) {
        for (size_t i = 0u; i < tick.preEvents.count; ++i)
            if (!digest.append(tick.preEvents.events[i])) return false;
        if (!digest.append(tick.frame)) return false;
        for (size_t i = 0u; i < tick.postEvents.count; ++i)
            if (!digest.append(tick.postEvents.events[i])) return false;
    }
    end.canonicalGameDigest.fill(0u);
    std::array<uint8_t, 32> canonical{};
    if (!digest.finish(end, canonical)) return false;
    end.canonicalGameDigest = canonical;
    return true;
}

}

RecoveryPlanV2 ReplayRecoveryV2::plan(
    ReplayByteSource& source, const bool sourceNameEndsInPart) {
    RecoveryPlanV2 plan{};
    ParseOptionsV2 options{};
    options.retainTimeline = true;
    options.allowUnsealedPrefix = true;
    options.sourceNameEndsInPart = sourceNameEndsInPart;
    options.verifyFileDigest = true;
    plan.parseResult = ReplayParserV2::parse(source, options, plan.parsed);
    plan.sourceRangeBytes = plan.parsed.validatedSourceBytes;
    plan.sourceRangeDigest = plan.parsed.validatedSourceRangeDigest;
    switch (plan.parsed.classification) {
        case ReplayClassificationV2::FooterCompleteRenameFailure:
            plan.classification =
                RecoveryClassV2::FooterCompleteRenameFailure;
            break;
        case ReplayClassificationV2::UnsealedValidatedPrefix:
            plan.classification =
                RecoveryClassV2::PlayableUnsealedPrefix;
            break;
        case ReplayClassificationV2::MetadataOnly:
            plan.classification = RecoveryClassV2::MetadataOnly;
            break;
        default:
            plan.classification = RecoveryClassV2::Nothing;
            break;
    }
    return plan;
}

RecoveryPlanV2 ReplayRecoveryV2::planFile(const std::wstring& path) {
    FileByteSource source;
    if (!source.open(path)) {
        RecoveryPlanV2 plan{};
        plan.parseResult.status = ParseStatus::IoError;
        plan.parseResult.detail = openErrorText(source.error());
        return plan;
    }
    const bool part = path.size() >= 10u
        && path.substr(path.size() - 10u) == L".q99r.part";
    return ReplayRecoveryV2::plan(source, part);
}

void ReplayRecoveryV2::deriveRecoveredUuid(
    const std::array<uint8_t, 32>& sourceRangeDigest,
    uint64_t& low, uint64_t& high) {
    static const char kDomain[] = "Q99R-RECOVERED-PREFIX-V2";
    Sha256 sha;
    sha.update(
        reinterpret_cast<const uint8_t*>(kDomain), sizeof(kDomain) - 1u);
    sha.update(sourceRangeDigest.data(), sourceRangeDigest.size());
    std::array<uint8_t, 32> digest{};
    sha.finish(digest);
    low = 0u;
    high = 0u;
    for (unsigned i = 0u; i < 8u; ++i) {
        low |= static_cast<uint64_t>(digest[i]) << (i * 8u);
        high |= static_cast<uint64_t>(digest[8u + i]) << (i * 8u);
    }
    if (low == 0u && high == 0u) high = 1u;
}

bool ReplayRecoveryV2::execute(
    const RecoveryPlanV2& plan, const std::wstring& outputRoot,
    const std::wstring& sourceFileName, RecoveryOutcomeV2& out,
    ReplayWriterV2* suppliedWriter) {
    out = RecoveryOutcomeV2{};
    out.plan = plan;
    if (plan.classification
            != RecoveryClassV2::PlayableUnsealedPrefix
        || plan.parsed.ticks.empty()
        || plan.sourceRangeBytes == 0u) {
        out.detail = "source has no independently playable v2 prefix";
        return false;
    }

    ConfirmedGameStartV2 start = plan.parsed.start;
    uint64_t low = 0u;
    uint64_t high = 0u;
    deriveRecoveredUuid(plan.sourceRangeDigest, low, high);
    start.manifest.identity.gameUuidLow = low;
    start.manifest.identity.gameUuidHigh = high;
    if (start.manifest.identity.gameGeneration == 0u)
        start.manifest.identity.gameGeneration = 1u;
    start.manifest.display.sourceKind =
        ReplaySourceKindV2::RecoveredPrefix;
    start.manifest.display.flags &=
        static_cast<uint8_t>(~(
            DisplayMetadataSeriesV2
            | DisplayMetadataPreviousGameDigestV2));
    start.manifest.display.seriesUuidLow = 0u;
    start.manifest.display.seriesUuidHigh = 0u;
    start.manifest.display.gameOrdinalInSeries = 0u;
    start.manifest.display.previousGameDigest.fill(0u);

    ConfirmedGameEndV2 end{};
    end.reason = TerminalReason::RecoveredPrefix;
    end.nextRecord =
        static_cast<uint32_t>(plan.parsed.ticks.size());
    end.winningSideMask = 0u;
    end.winnerSlot = -1;
    end.roundTallies =
        plan.parsed.ticks.back().frame.core.after.roundTallies;
    end.postTailStateHash =
        plan.parsed.ticks.back().frame.core.postStateHash;
    end.finalRollingDigest =
        plan.parsed.finalRollingDigest;
    if (!rebuildCanonicalEnd(start, plan.parsed.ticks, end)) {
        out.detail = "validated prefix could not form a recovered terminal";
        return false;
    }

    std::unique_ptr<ReplayWriterV2> owned;
    ReplayWriterV2* writer = suppliedWriter;
    if (writer == nullptr) {
        owned.reset(new ReplayWriterV2());
        writer = owned.get();
    }
    WriterConfig config{};
    config.rootDirectory = outputRoot;
    config.nameHint =
        "recovered-" + baseNameToken(sourceFileName);
    config.recordedUnixSeconds =
        start.manifest.display.recordedUnixSeconds;

    BeginGameBundleV2 begin{};
    begin.start = start;
    begin.frameZero = plan.parsed.ticks.front();
    if (!writer->begin(config, begin)) {
        out.writerError = writer->error();
        out.writerStage = writer->errorStage();
        out.detail = writer->errorText();
        return false;
    }
    for (size_t i = 1u; i < plan.parsed.ticks.size(); ++i) {
        if (!writer->append(plan.parsed.ticks[i])) {
            out.writerError = writer->error();
            out.writerStage = writer->errorStage();
            out.detail = writer->errorText();
            writer->abandon();
            return false;
        }
    }
    EndGameBundleV2 terminal{};
    terminal.containsFinalFrame = false;
    terminal.end = end;
    if (!writer->finalize(terminal)) {
        out.writerError = writer->error();
        out.writerStage = writer->errorStage();
        out.summary = writer->summary();
        out.detail = writer->errorText();
        return false;
    }
    out.summary = writer->summary();
    out.ok = true;
    out.detail = "recovered prefix sealed into a new file";
    return true;
}

bool ReplayRecoveryV2::finishFooterCompleteRename(
    const std::wstring& partPath, RecoveryOutcomeV2& out) {
    out = RecoveryOutcomeV2{};
    out.plan = planFile(partPath);
    if (out.plan.classification
        != RecoveryClassV2::FooterCompleteRenameFailure) {
        out.detail = "source is not a footer-complete rename failure";
        return false;
    }
    if (partPath.size() < 5u
        || partPath.substr(partPath.size() - 5u) != L".part") {
        out.detail = "source does not have the .part suffix";
        return false;
    }
    const std::wstring target =
        partPath.substr(0u, partPath.size() - 5u);
    if (::MoveFileExW(partPath.c_str(), target.c_str(), 0u) == 0) {
        out.detail = "atomic no-replace rename failed";
        return false;
    }
    out.completedRenamePath = target;
    out.ok = true;
    out.detail = "footer-complete replay renamed byte-for-byte";
    return true;
}

}
