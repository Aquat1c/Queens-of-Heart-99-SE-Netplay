#pragma once

#include "ReplayWriter.h"

#include "replay/ConfirmedTimeline.h"
#include "replay/ReplayCompatibilityIdentityV3.h"
#include "replay/ReplayIntegrity.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qclient::replay {

enum class WriterStageV2 : uint16_t {
    None = 0,
    CreatePart,
    Prefix,
    Manifest,
    Start,
    InitialEvents,
    InitialFrame,
    TimelineEvents,
    TimelineFrames,
    GameEnd,
    Footer,
    Flush,
    Close,
    Rename,
    CommitAck,
    CompatibilityIdentity,
};

struct WriterSummaryV2 {
    qreplay::GameIdentityV2 identity{};
    qreplay::TerminalReason terminalReason =
        qreplay::TerminalReason::RecordingFailure;
    std::wstring partPath;
    std::wstring finalPath;
    std::array<uint8_t, 32> canonicalGameDigest{};
    std::array<uint8_t, 32> fileDigest{};
    uint32_t chunkCount = 0u;
    uint32_t frameCount = 0u;
    uint32_t eventCount = 0u;
    uint64_t bytes = 0u;
    bool footerWritten = false;
    bool finalized = false;
    bool compatibilityIdentityAttested = false;
};

using WriterFaultInjectorV2 = std::function<bool(WriterStageV2)>;

class ReplayWriterV2 {
public:
    ReplayWriterV2();
    ~ReplayWriterV2();
    ReplayWriterV2(const ReplayWriterV2&) = delete;
    ReplayWriterV2& operator=(const ReplayWriterV2&) = delete;

    void setSink(std::unique_ptr<ReplayFileSink> sink);
    void setFaultInjector(WriterFaultInjectorV2 injector);

    bool begin(const WriterConfig& config,
               const qreplay::BeginGameBundleV2& begin);
    bool begin(
        const WriterConfig& config,
        const qreplay::BeginGameBundleV2& begin,
        const qreplay::ReplayCompatibilityIdentityEnvelopeV3&
            compatibilityIdentity);
    bool append(const qreplay::CanonicalTickBundleV2& tick);

    bool writePalette(const qreplay::ReplayPaletteV2& palette);

    bool finalize(const qreplay::EndGameBundleV2& end);

    bool makeAbnormalEnd(qreplay::TerminalReason reason,
                         qreplay::ConfirmedGameEndV2& out) const;

    void abandon();

    bool isOpen() const { return open_; }
    bool isFinalized() const { return finalized_; }
    WriterError error() const { return error_; }
    WriterStageV2 errorStage() const { return errorStage_; }
    const char* errorText() const { return writerErrorText(error_); }
    const WriterSummaryV2& summary() const { return summary_; }
    const std::wstring& partPath() const { return partPath_; }
    const std::wstring& finalPath() const { return finalPath_; }
    uint32_t nextRecord() const { return validator_.nextRecord(); }
    uint32_t nextEventSequence() const {
        return validator_.nextEventSequence();
    }
    uint64_t rollingDigest() const { return validator_.rollingDigest(); }
    uint64_t lastPostStateHash() const { return lastPostStateHash_; }
    uint32_t chunkCount() const { return sequence_; }
    uint64_t bytesWritten() const { return bytesWritten_; }
    const std::vector<uint8_t>& stagedBytesForTesting() const {
        return stagedBytes_;
    }
    uint32_t flushCount() const { return flushCount_; }

private:
    bool beginImpl(
        const WriterConfig& config,
        const qreplay::BeginGameBundleV2& begin,
        const qreplay::ReplayCompatibilityIdentityEnvelopeV3*
            compatibilityIdentity);
    bool fault(WriterStageV2 stage);
    bool fail(WriterError error, WriterStageV2 stage);
    bool rawWrite(const uint8_t* data, size_t bytes, bool hash);
    bool appendChunk(qreplay::ChunkKindV2 kind, uint32_t firstRecord,
                     uint32_t nextRecord, uint32_t recordCount,
                     const std::vector<uint8_t>& payload,
                     WriterStageV2 stage, uint64_t* offsetOut = nullptr);
    bool writeEvents(const qreplay::FixedEventListV2& events,
                     WriterStageV2 stage);
    bool paletteWritten_[4] = { false, false, false, false };
    bool queueFrame(const qreplay::ConfirmedGameFrameV2& frame,
                    WriterStageV2 stage);
    bool flushFrames(WriterStageV2 stage);
    bool writeTick(const qreplay::CanonicalTickBundleV2& tick,
                   WriterStageV2 eventStage, WriterStageV2 frameStage);
    bool appendCanonicalTick(
        const qreplay::CanonicalTickBundleV2& tick);
    bool maybeFlush(bool force);
    bool writeFooter(const qreplay::ConfirmedGameEndV2& end);
    void noteRoundEvents(const qreplay::FixedEventListV2& events,
                         uint64_t chunkOffset);
    void closeBestEffort();

    std::unique_ptr<ReplayFileSink> sink_;
    WriterFaultInjectorV2 faultInjector_;
    WriterConfig config_{};
    WriterSummaryV2 summary_{};
    qreplay::ReplayPrefixV2 prefix_{};
    qreplay::ConfirmedTimelineValidatorV2 validator_{};
    qreplay::CanonicalGameDigestV2 canonical_{};
    qreplay::Sha256 fileSha_{};

    std::vector<qreplay::ConfirmedGameFrameV2> pendingFrames_;
    std::vector<qreplay::RoundIndexEntryV2> completedRounds_;
    qreplay::RoundIndexEntryV2 openRound_{};
    bool hasOpenRound_ = false;

    std::wstring partPath_;
    std::wstring finalPath_;
    std::vector<uint8_t> stagedBytes_;
    uint64_t manifestOffset_ = 0u;
    uint64_t startOffset_ = 0u;
    uint64_t firstInputOffset_ = 0u;
    uint64_t gameEndOffset_ = 0u;
    uint64_t bytesWritten_ = 0u;
    uint64_t lastFlushTick_ = 0u;
    uint64_t lastPostStateHash_ = 0u;
    uint32_t sequence_ = 0u;
    uint32_t frameCount_ = 0u;
    uint32_t eventCount_ = 0u;
    uint32_t chunksSinceFlush_ = 0u;
    uint32_t flushCount_ = 0u;
    bool open_ = false;
    bool finalized_ = false;
    WriterError error_ = WriterError::None;
    WriterStageV2 errorStage_ = WriterStageV2::None;
};

}
