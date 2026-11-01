#include "ReplayRecovery.h"

#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"

#include <algorithm>
#include <cstring>

namespace qclient::replay {

using namespace qreplay;

const char* recoveryClassText(RecoveryClass value) {
    switch (value) {
        case RecoveryClass::Nothing: return "nothing recoverable";
        case RecoveryClass::MetadataOnly: return "metadata only";
        case RecoveryClass::NotBootable: return "valid but not bootable";
        case RecoveryClass::Playable: return "valid and playable";
    }
    return "unknown";
}

namespace {

class PlanVisitor final : public ReplayChunkVisitor {
public:
    explicit PlanVisitor(RecoveryPlan& plan) : plan_(plan) {}

    bool onChunk(const ChunkLocation& location, const uint8_t*, size_t,
                 const DecodedChunk& decoded) override {
        plan_.validBytes = location.endOffset();
        plan_.validChunks = location.header.sequence + 1u;
        plan_.lastValidSequence = location.header.sequence;

        if (decoded.manifest != nullptr) {
            plan_.manifest = *decoded.manifest;
            plan_.hasManifest = true;
        } else if (decoded.sessionStart != nullptr) {
            plan_.hasSessionStart = true;
            plan_.firstTimelineFrame = decoded.sessionStart->firstTimelineFrame;
            plan_.lastValidTimelineFrame = plan_.firstTimelineFrame;
            plan_.lastRollingDigest = decoded.sessionStart->initialDigest;
            plan_.lastPostStateHash = decoded.sessionStart->initialStateHash;
        } else if (decoded.matchStart != nullptr) {
            ++plan_.startedMatches;
            plan_.lastMatchOpen = true;
            plan_.openMatchId = decoded.matchStart->matchId;
            plan_.lastValidMatchId = decoded.matchStart->matchId;
            if (decoded.matchStart->independentBootstrap)
                plan_.anyIndependentBootstrap = true;
        } else if (decoded.matchEnd != nullptr) {
            ++plan_.completedMatches;
            plan_.lastMatchOpen = false;
            plan_.openMatchId = 0u;
            plan_.lastRollingDigest = decoded.matchEnd->rollingDigest;
            plan_.lastPostStateHash = decoded.matchEnd->postTailStateHash;
        } else if (decoded.sessionEnd != nullptr) {
            plan_.hasSessionEnd = true;
            plan_.lastRollingDigest = decoded.sessionEnd->rollingDigest;
            plan_.lastPostStateHash = decoded.sessionEnd->postTailStateHash;
        } else if (decoded.footer != nullptr) {
            plan_.hasFooter = true;
        } else if (decoded.frames != nullptr && !decoded.frames->empty()) {
            plan_.retainedFrames += location.header.recordCount;
            plan_.lastValidTimelineFrame = location.header.lastTimelineFrame;
            const ReplayFrameV1& last = decoded.frames->back();
            plan_.lastRollingDigest = last.rollingDigest;
            plan_.lastPostStateHash = last.postStateHash;
        } else if (decoded.events != nullptr && !decoded.events->empty()) {
            plan_.lastRollingDigest = decoded.events->back().rollingDigest;
        }
        return true;
    }

private:
    RecoveryPlan& plan_;
};

class EmitVisitor final : public ReplayChunkVisitor {
public:
    EmitVisitor(ReplayWriter& writer, uint64_t limitBytes)
        : writer_(writer), limitBytes_(limitBytes) {}

    bool failed() const { return failed_; }
    const char* detail() const { return detail_; }
    uint32_t openMatchId() const { return openMatchId_; }
    uint32_t lastFrame() const { return lastFrame_; }
    uint8_t openRoundsSideZero() const { return roundsSideZero_; }
    uint8_t openRoundsSideOne() const { return roundsSideOne_; }

    bool onChunk(const ChunkLocation& location, const uint8_t*, size_t,
                 const DecodedChunk& decoded) override {
        if (location.endOffset() > limitBytes_) return false;

        if (decoded.manifest != nullptr) return true;
        if (decoded.footer != nullptr) return false;
        if (decoded.sessionEnd != nullptr) return false;

        if (decoded.sessionStart != nullptr) {
            if (!writer_.writeSessionStart(*decoded.sessionStart))
                return stop("session start rejected");
            lastFrame_ = decoded.sessionStart->firstTimelineFrame;
            return true;
        }
        if (decoded.matchStart != nullptr) {
            if (!writer_.writeMatchStart(*decoded.matchStart))
                return stop("match start rejected");
            openMatchId_ = decoded.matchStart->matchId;
            roundsSideZero_ = 0u;
            roundsSideOne_ = 0u;
            return true;
        }
        if (decoded.matchEnd != nullptr) {
            if (!writer_.writeMatchEnd(*decoded.matchEnd))
                return stop("match end rejected");
            openMatchId_ = 0u;
            return true;
        }
        if (decoded.frames != nullptr) {
            if (!writer_.writeInputBlock(decoded.frames->data(),
                                         decoded.frames->size())) {
                return stop("input block rejected");
            }
            lastFrame_ = location.header.lastTimelineFrame;
            return true;
        }
        if (decoded.events != nullptr) {
            for (const ReplayEvent& event : *decoded.events) {
                if (event.kind != EventKind::RoundEnded) continue;
                if ((event.valueA & 0x05u) != 0u) ++roundsSideZero_;
                if ((event.valueA & 0x0Au) != 0u) ++roundsSideOne_;
            }
            if (!writer_.writeEventBlock(decoded.events->data(),
                                         decoded.events->size())) {
                return stop("event block rejected");
            }
            return true;
        }
        return true;
    }

private:
    bool stop(const char* detail) {
        failed_ = true;
        detail_ = detail;
        return false;
    }

    ReplayWriter& writer_;
    uint64_t limitBytes_;
    uint32_t openMatchId_ = 0u;
    uint32_t lastFrame_ = 0u;
    uint8_t roundsSideZero_ = 0u;
    uint8_t roundsSideOne_ = 0u;
    bool failed_ = false;
    const char* detail_ = "";
};

std::array<uint8_t, 32> hashRange(ReplayByteSource& source, uint64_t bytes) {
    Sha256 sha;
    std::vector<uint8_t> buffer;
    buffer.resize(64u * 1024u);
    uint64_t done = 0u;
    while (done < bytes) {
        const size_t take = static_cast<size_t>(
            std::min<uint64_t>(buffer.size(), bytes - done));
        if (!source.read(done, buffer.data(), take)) break;
        sha.update(buffer.data(), take);
        done += static_cast<uint64_t>(take);
    }
    std::array<uint8_t, 32> digest{};
    sha.finish(digest);
    return digest;
}

std::wstring baseNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1u);
}

}

void ReplayRecovery::deriveRecoveredUuid(
    const std::array<uint8_t, 32>& sourceDigest, uint64_t& low,
    uint64_t& high) {
    static const char kDomain[] = "q99r-recovered-session-v1";
    Sha256 sha;
    sha.update(reinterpret_cast<const uint8_t*>(kDomain), sizeof(kDomain) - 1u);
    sha.update(sourceDigest.data(), sourceDigest.size());
    std::array<uint8_t, 32> out{};
    sha.finish(out);
    low = 0u;
    high = 0u;
    for (unsigned i = 0; i < 8u; ++i) {
        low |= static_cast<uint64_t>(out[i]) << (i * 8u);
        high |= static_cast<uint64_t>(out[8u + i]) << (i * 8u);
    }
}

RecoveryPlan ReplayRecovery::plan(ReplayByteSource& source) {
    RecoveryPlan result{};

    uint8_t prefixBytes[kPrefixBytes];
    if (source.size() >= kPrefixBytes
        && source.read(0u, prefixBytes, kPrefixBytes)
        && decodePrefix(prefixBytes, kPrefixBytes, result.sourcePrefix)
            == ParseStatus::Ok) {
        result.prefixValid = true;
        result.validBytes = kPrefixBytes;
    }

    WalkOptions options{};
    options.decodeRecords = true;
    options.verifyFileDigest = false;
    options.stopAfterSessionStart = false;

    PlanVisitor visitor(result);
    result.stop = ReplayParser::walk(source, options, visitor);

    if (!result.prefixValid) {
        result.classification = RecoveryClass::Nothing;
        return result;
    }
    result.sourceDigest = hashRange(source, result.validBytes);

    if (!result.hasManifest) {
        result.classification = RecoveryClass::Nothing;
    } else if (!result.hasSessionStart || result.retainedFrames == 0u) {
        result.classification = RecoveryClass::MetadataOnly;
    } else if (!result.anyIndependentBootstrap) {
        result.classification = RecoveryClass::NotBootable;
    } else {
        result.classification = RecoveryClass::Playable;
    }
    return result;
}

RecoveryPlan ReplayRecovery::planFile(const std::wstring& path) {
    FileByteSource source;
    if (!source.open(path)) {
        RecoveryPlan result{};
        result.stop.status = ParseStatus::IoError;
        result.stop.detail = openErrorText(source.error());
        return result;
    }
    return ReplayRecovery::plan(source);
}

bool ReplayRecovery::execute(ReplayByteSource& source,
                             const RecoveryPlan& plan,
                             const std::wstring& outputRoot,
                             const std::wstring& sourceFileName,
                             RecoveryOutcome& out,
                             ReplayWriter* writer) {
    out = RecoveryOutcome{};
    out.plan = plan;
    out.provenance.sourceSessionUuidLow = plan.sourcePrefix.sessionUuidLow;
    out.provenance.sourceSessionUuidHigh = plan.sourcePrefix.sessionUuidHigh;
    out.provenance.sourceDigest = plan.sourceDigest;
    out.provenance.sourceFileName =
        sanitizeForDisplay(baseNameOf(sourceFileName), 128u);
    out.provenance.salvagedBytes = plan.validBytes;
    out.provenance.lastValidTimelineFrame = plan.lastValidTimelineFrame;
    out.provenance.classification = plan.classification;

    if (!plan.prefixValid || plan.classification == RecoveryClass::Nothing) {
        out.detail = "nothing to recover";
        return false;
    }

    ReplayWriter localWriter;
    ReplayWriter& target = writer != nullptr ? *writer : localWriter;

    SessionManifest manifest = plan.manifest;
    manifest.display.sourceKind = ReplaySourceKind::RecoveredPrefix;

    ReplayPrefix prefix = plan.sourcePrefix;
    deriveRecoveredUuid(plan.sourceDigest, prefix.sessionUuidLow,
                        prefix.sessionUuidHigh);

    WriterConfig config{};
    config.rootDirectory = outputRoot;
    config.nameHint = "recovered-" + out.provenance.sourceFileName;
    config.recordedUnixSeconds = manifest.display.recordedUnixSeconds;
    config.flushEveryChunks = 256u;
    config.flushEveryMs = 0u;

    if (!target.begin(config, prefix, manifest)) {
        out.writerError = target.error();
        out.detail = target.errorText();
        return false;
    }

    WalkOptions options{};
    options.decodeRecords = true;
    options.verifyFileDigest = false;

    EmitVisitor emitter(target, plan.validBytes);
    ReplayParser::walk(source, options, emitter);
    if (emitter.failed()) {
        target.abandon();
        out.writerError = target.error();
        out.detail = emitter.detail();
        return false;
    }

    const uint32_t lastFrame =
        std::max(emitter.lastFrame(), plan.firstTimelineFrame);

    if (emitter.openMatchId() != 0u) {
        MatchEndRecord end{};
        end.matchId = emitter.openMatchId();
        end.lastTimelineFrame = lastFrame;
        end.winningSideMask = 0u;
        end.roundsSideZero = emitter.openRoundsSideZero();
        end.roundsSideOne = emitter.openRoundsSideOne();
        end.reason = TerminalReason::RecoveredPrefix;
        end.postTailStateHash = plan.lastPostStateHash;
        end.rollingDigest = plan.lastRollingDigest;
        if (!target.writeMatchEnd(end)) {
            target.abandon();
            out.writerError = target.error();
            out.detail = "synthesized match terminal rejected";
            return false;
        }
    }

    ReplayEvent terminal{};
    terminal.timelineFrame = lastFrame;
    terminal.kind = EventKind::SegmentTerminal;
    terminal.matchId = 0u;
    terminal.terminalReason = TerminalReason::RecoveredPrefix;
    terminal.stateProof = plan.lastPostStateHash;
    terminal.rollingDigest = plan.lastRollingDigest;
    if (!target.writeEventBlock(&terminal, 1u)) {
        target.abandon();
        out.writerError = target.error();
        out.detail = "synthesized terminal event rejected";
        return false;
    }

    SessionEndRecord sessionEnd{};
    sessionEnd.lastTimelineFrame = lastFrame;
    sessionEnd.reason = TerminalReason::RecoveredPrefix;
    sessionEnd.postTailStateHash = plan.lastPostStateHash;
    sessionEnd.rollingDigest = plan.lastRollingDigest;
    sessionEnd.matchCount = target.matchCount();
    if (!target.finalize(sessionEnd)) {
        out.writerError = target.error();
        out.detail = target.errorText();
        out.summary = target.summary();
        return false;
    }

    out.ok = true;
    out.summary = target.summary();
    return true;
}

}
