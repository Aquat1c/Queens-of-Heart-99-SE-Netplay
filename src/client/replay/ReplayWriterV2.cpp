#include "ReplayWriterV2.h"

#include "platform/WidePath.h"
#include "replay/ReplayCodec.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <ctime>

namespace qclient::replay {

using namespace qreplay;

ReplayWriterV2::ReplayWriterV2() {
    pendingFrames_.reserve(kMaxRecordsPerInputBlock);
}

ReplayWriterV2::~ReplayWriterV2() { abandon(); }

void ReplayWriterV2::setSink(std::unique_ptr<ReplayFileSink> sink) {
    if (!open_) sink_ = std::move(sink);
}

void ReplayWriterV2::setFaultInjector(WriterFaultInjectorV2 injector) {
    if (!open_) faultInjector_ = std::move(injector);
}

bool ReplayWriterV2::fault(const WriterStageV2 stage) {
    return faultInjector_ && faultInjector_(stage);
}

bool ReplayWriterV2::fail(
    const WriterError error, const WriterStageV2 stage) {
    if (error_ == WriterError::None) {
        error_ = error;
        errorStage_ = stage;
    }
    if (sink_) {
        (void)sink_->close();
        (void)sink_->discard();
    }
    open_ = false;
    stagedBytes_.clear();
    return false;
}

bool ReplayWriterV2::rawWrite(
    const uint8_t* data, const size_t bytes, const bool hash) {
    if (data == nullptr || bytes == 0u)
        return fail(WriterError::InvalidRecord, errorStage_);
    if (bytesWritten_ > kMaxFileBytes
        || bytes > kMaxFileBytes - bytesWritten_) {
        return fail(WriterError::LimitExceeded, errorStage_);
    }
    stagedBytes_.insert(stagedBytes_.end(), data, data + bytes);
    if (hash) fileSha_.update(data, bytes);
    bytesWritten_ += static_cast<uint64_t>(bytes);
    return true;
}

bool ReplayWriterV2::appendChunk(
    const ChunkKindV2 kind, const uint32_t firstRecord,
    const uint32_t nextRecord, const uint32_t recordCount,
    const std::vector<uint8_t>& payload, const WriterStageV2 stage,
    uint64_t* offsetOut) {
    if (!open_ || payload.empty() || payload.size() > kMaxChunkPayloadBytes
        || sequence_ >= kMaxChunksPerFile || firstRecord > nextRecord) {
        return fail(WriterError::InvalidRecord, stage);
    }
    errorStage_ = stage;
    if (fault(stage)) return fail(WriterError::WriteFailed, stage);

    ChunkHeaderV2 header{};
    header.kind = kind;
    header.schemaVersion = 2u;
    header.critical = isCriticalChunkKindV2(kind) ? 1u : 0u;
    header.codec = ChunkCodec::Raw;
    header.sequence = sequence_;
    header.gameGeneration = prefix_.gameUuidLow == 0u
            && prefix_.gameUuidHigh == 0u
        ? 0u : summary_.identity.gameGeneration;
    header.firstRecord = firstRecord;
    header.nextRecord = nextRecord;
    header.recordCount = recordCount;
    header.payloadBytes = static_cast<uint32_t>(payload.size());
    header.decodedBytes = header.payloadBytes;
    header.payloadCrc32c = crc32c(payload.data(), payload.size());

    std::vector<uint8_t> encodedHeader;
    encodeChunkHeaderV2(header, encodedHeader);
    ChunkHeaderV2 verified{};
    if (encodedHeader.size() != kChunkHeaderBytes
        || decodeChunkHeaderV2(
               encodedHeader.data(), encodedHeader.size(), verified)
               != ParseStatus::Ok) {
        return fail(WriterError::InvalidRecord, stage);
    }
    const uint64_t offset = bytesWritten_;
    if (!rawWrite(encodedHeader.data(), encodedHeader.size(), true)
        || !rawWrite(payload.data(), payload.size(), true)) {
        return false;
    }
    if (offsetOut) *offsetOut = offset;
    ++sequence_;
    ++chunksSinceFlush_;
    return maybeFlush(false);
}

bool ReplayWriterV2::maybeFlush(const bool force) {
    if (!open_) return false;
    const uint64_t now = ::GetTickCount64();
    const bool chunksDue = config_.flushEveryChunks != 0u
        && chunksSinceFlush_ >= config_.flushEveryChunks;
    const bool timeDue = config_.flushEveryMs != 0u
        && now - lastFlushTick_ >= config_.flushEveryMs;
    if (!force && !chunksDue && !timeDue) return true;
    errorStage_ = WriterStageV2::Flush;
    if (fault(WriterStageV2::Flush))
        return fail(WriterError::FlushFailed, WriterStageV2::Flush);
    ++flushCount_;
    chunksSinceFlush_ = 0u;
    lastFlushTick_ = now;
    return true;
}

bool ReplayWriterV2::writePalette(const ReplayPaletteV2& palette) {
    if (!open_ || finalized_) return false;
    if (palette.side > 3u) return false;
    if (paletteWritten_[palette.side]) return true;
    std::vector<uint8_t> payload;
    if (!encodePaletteBlockV2(palette, payload))
        return fail(WriterError::InvalidRecord, WriterStageV2::Start);
    const uint32_t frontier = validator_.nextRecord();
    if (!appendChunk(ChunkKindV2::Palette, frontier, frontier, 0u, payload,
                     WriterStageV2::Start)) {
        return false;
    }
    paletteWritten_[palette.side] = true;
    return true;
}

bool ReplayWriterV2::writeEvents(
    const FixedEventListV2& events, const WriterStageV2 stage) {
    if (events.count == 0u) return true;
    if (events.count > kMaxEventsPerTickV2) {
        return fail(WriterError::InvalidRecord, stage);
    }
    std::vector<uint8_t> payload;
    if (!encodeEventBlockV2(events.events.data(), events.count, payload))
        return fail(WriterError::InvalidRecord, stage);
    uint32_t first = events.events[0].replayFrame;
    uint32_t next = first + 1u;
    for (size_t i = 1u; i < events.count; ++i) {
        first = std::min(first, events.events[i].replayFrame);
        next = std::max(next, events.events[i].replayFrame + 1u);
    }
    uint64_t chunkOffset = 0u;
    if (!appendChunk(
            ChunkKindV2::EventBlock, first, next, events.count, payload,
            stage, &chunkOffset)) {
        return false;
    }
    eventCount_ += events.count;
    noteRoundEvents(events, chunkOffset);
    return true;
}

bool ReplayWriterV2::queueFrame(
    const ConfirmedGameFrameV2& frame, const WriterStageV2 stage) {
    if (!pendingFrames_.empty()
        && frame.replayFrame
            != pendingFrames_.back().replayFrame + 1u) {
        if (!flushFrames(stage)) return false;
    }
    pendingFrames_.push_back(frame);
    if (pendingFrames_.size() >= kMaxRecordsPerInputBlock)
        return flushFrames(stage);
    return true;
}

bool ReplayWriterV2::flushFrames(const WriterStageV2 stage) {
    if (pendingFrames_.empty()) return true;
    std::vector<uint8_t> payload;
    if (!encodeInputBlockV2(
            pendingFrames_.data(), pendingFrames_.size(), payload)) {
        return fail(WriterError::InvalidRecord, stage);
    }
    const uint32_t first = pendingFrames_.front().replayFrame;
    const uint32_t next =
        pendingFrames_.back().replayFrame + 1u;
    uint64_t offset = 0u;
    if (!appendChunk(
            ChunkKindV2::InputBlock, first, next,
            static_cast<uint32_t>(pendingFrames_.size()), payload, stage,
            &offset)) {
        return false;
    }
    if (firstInputOffset_ == 0u) firstInputOffset_ = offset;
    frameCount_ += static_cast<uint32_t>(pendingFrames_.size());
    lastPostStateHash_ = pendingFrames_.back().core.postStateHash;
    pendingFrames_.clear();
    return true;
}

bool ReplayWriterV2::writeTick(
    const CanonicalTickBundleV2& tick, const WriterStageV2 eventStage,
    const WriterStageV2 frameStage) {
    if (tick.preEvents.count != 0u) {
        if (!flushFrames(frameStage)
            || !writeEvents(tick.preEvents, eventStage)) {
            return false;
        }
    }
    if (!queueFrame(tick.frame, frameStage)) return false;
    if (tick.postEvents.count != 0u) {
        if (!flushFrames(frameStage)
            || !writeEvents(tick.postEvents, eventStage)) {
            return false;
        }
    }
    return true;
}

bool ReplayWriterV2::appendCanonicalTick(
    const CanonicalTickBundleV2& tick) {
    for (size_t i = 0u; i < tick.preEvents.count; ++i)
        if (!canonical_.append(tick.preEvents.events[i])) return false;
    if (!canonical_.append(tick.frame)) return false;
    for (size_t i = 0u; i < tick.postEvents.count; ++i)
        if (!canonical_.append(tick.postEvents.events[i])) return false;
    return true;
}

void ReplayWriterV2::noteRoundEvents(
    const FixedEventListV2& events, const uint64_t chunkOffset) {
    for (size_t i = 0u; i < events.count; ++i) {
        const ConfirmedGameEventV2& event = events.events[i];
        if (event.kind == GameEventKindV2::RoundStarted) {
            openRound_ = RoundIndexEntryV2{};
            openRound_.roundOrdinal = event.roundOrdinal;
            openRound_.firstRecord = event.replayFrame;
            openRound_.firstEventSequence = event.eventSequence;
            openRound_.firstChunkOffset = chunkOffset;
            hasOpenRound_ = true;
        } else if (event.kind == GameEventKindV2::RoundEnded
                   && hasOpenRound_
                   && event.roundOrdinal == openRound_.roundOrdinal
                   && chunkOffset > openRound_.firstChunkOffset) {
            openRound_.winningSideMask = event.winningSideMask;
            openRound_.roundTallies = event.roundTallies;
            openRound_.nextRecord = event.replayFrame + 1u;
            openRound_.nextEventSequence = event.eventSequence + 1u;
            openRound_.lastChunkOffset = chunkOffset;
            completedRounds_.push_back(openRound_);
            hasOpenRound_ = false;
        }
    }
}

bool ReplayWriterV2::begin(
    const WriterConfig& config, const BeginGameBundleV2& beginBundle) {
    return beginImpl(config, beginBundle, nullptr);
}

bool ReplayWriterV2::begin(
    const WriterConfig& config, const BeginGameBundleV2& beginBundle,
    const ReplayCompatibilityIdentityEnvelopeV3& compatibilityIdentity) {
    return beginImpl(config, beginBundle, &compatibilityIdentity);
}

bool ReplayWriterV2::beginImpl(
    const WriterConfig& config, const BeginGameBundleV2& beginBundle,
    const ReplayCompatibilityIdentityEnvelopeV3* const compatibilityIdentity) {
    if (open_ || finalized_) {
        return fail(
            open_ ? WriterError::AlreadyOpen : WriterError::Finalized,
            WriterStageV2::CreatePart);
    }

    ParseStatus why = ParseStatus::Ok;
    ConfirmedTimelineValidatorV2 candidate;
    if (!candidate.begin(beginBundle.start, why)
        || !candidate.append(beginBundle.frameZero, why)
        || beginBundle.frameZero.frame.replayFrame != 0u) {
        return fail(WriterError::InvalidRecord, WriterStageV2::Manifest);
    }

    std::vector<uint8_t> compatibilityPayload;
    if (compatibilityIdentity != nullptr) {
        std::string identityError;
        if (!encodeReplayCompatibilityIdentityEnvelopeV3(
                *compatibilityIdentity, compatibilityPayload,
                identityError)) {
            return fail(
                WriterError::InvalidRecord,
                WriterStageV2::CompatibilityIdentity);
        }
    }

    config_ = config;
    summary_ = WriterSummaryV2{};
    stagedBytes_.clear();
    partPath_.clear();
    finalPath_.clear();
    summary_.identity = beginBundle.start.manifest.identity;
    prefix_ = ReplayPrefixV2{};
    prefix_.gameUuidLow = summary_.identity.gameUuidLow;
    prefix_.gameUuidHigh = summary_.identity.gameUuidHigh;
    if (beginBundle.start.manifest.activeFighterMask
        & static_cast<uint8_t>(~0x03u)) {
        prefix_.featureFlags |= kFeatureFourPlayer;
    }
    if (beginBundle.start.manifest.cpuMask != 0u)
        prefix_.featureFlags |= kFeatureCpuFighters;
    if (compatibilityIdentity != nullptr)
        prefix_.featureFlags |= kFeatureCompatibilityIdentityV3;
    summary_.compatibilityIdentityAttested = compatibilityIdentity != nullptr;

    if (config_.rootDirectory.empty())
        return fail(WriterError::RootUnusable, WriterStageV2::CreatePart);
    std::wstring root = qplat::absolutePathW(config_.rootDirectory);
    if (root.empty()) root = config_.rootDirectory;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();
    if (root.empty())
        return fail(WriterError::RootUnusable, WriterStageV2::CreatePart);

    if (!sink_) sink_.reset(new Win32FileSink());
    const int64_t stamp = config_.recordedUnixSeconds != 0
        ? config_.recordedUnixSeconds
        : static_cast<int64_t>(::_time64(nullptr));
    const std::wstring base = ReplayWriter::buildBaseName(
        stamp, config_.nameHint, summary_.identity.gameUuidHigh);
    const uint32_t attempts =
        config_.maxNameAttempts == 0u ? 1u : config_.maxNameAttempts;
    bool created = false;
    for (uint32_t attempt = 0u; attempt < attempts; ++attempt) {
        std::wstring candidate = root + L"\\" + base;
        if (attempt != 0u) {
            wchar_t suffix[20];
            _snwprintf_s(
                suffix, _countof(suffix), _TRUNCATE, L"_%u", attempt + 1u);
            candidate += suffix;
        }
        errorStage_ = WriterStageV2::CreatePart;
        if (fault(WriterStageV2::CreatePart))
            return fail(WriterError::CreateFailed, WriterStageV2::CreatePart);
        finalPath_ = candidate + L".q99r";
        created = true;
        break;
    }
    if (!created)
        return fail(WriterError::NameExhausted, WriterStageV2::CreatePart);

    open_ = true;
    fileSha_.reset();
    lastFlushTick_ = ::GetTickCount64();
    validator_ = ConfirmedTimelineValidatorV2{};
    if (!validator_.begin(beginBundle.start, why))
        return fail(WriterError::InvalidRecord, WriterStageV2::Manifest);
    canonical_ = CanonicalGameDigestV2{};
    if (!canonical_.begin(beginBundle.start))
        return fail(WriterError::InvalidRecord, WriterStageV2::Manifest);

    std::vector<uint8_t> payload;
    std::vector<uint8_t> prefixBytes;
    encodePrefixV2(prefix_, prefixBytes);
    ReplayPrefixV2 verifiedPrefix{};
    errorStage_ = WriterStageV2::Prefix;
    if (fault(WriterStageV2::Prefix)
        || prefixBytes.size() != kPrefixBytes
        || decodePrefixV2(
               prefixBytes.data(), prefixBytes.size(), verifiedPrefix)
               != ParseStatus::Ok
        || !rawWrite(prefixBytes.data(), prefixBytes.size(), true)) {
        if (error_ == WriterError::None)
            fail(WriterError::WriteFailed, WriterStageV2::Prefix);
        return false;
    }

    if (!encodeGameManifestV2(beginBundle.start.manifest, payload)
        || !appendChunk(
            ChunkKindV2::GameManifest, 0u, 0u, 1u, payload,
            WriterStageV2::Manifest, &manifestOffset_)) {
        return false;
    }
    payload.clear();
    if (compatibilityIdentity != nullptr
        && !appendChunk(
            ChunkKindV2::CompatibilityIdentity, 0u, 0u, 1u,
            compatibilityPayload, WriterStageV2::CompatibilityIdentity)) {
        return false;
    }
    if (!encodeDeterministicGameStartV2(beginBundle.start.start, payload)
        || !appendChunk(
            ChunkKindV2::GameStart, 0u, 0u, 1u, payload,
            WriterStageV2::Start, &startOffset_)) {
        return false;
    }

    if (!validator_.append(beginBundle.frameZero, why)
        || !appendCanonicalTick(beginBundle.frameZero)
        || !writeTick(
            beginBundle.frameZero, WriterStageV2::InitialEvents,
            WriterStageV2::InitialFrame)
        || !flushFrames(WriterStageV2::InitialFrame)
        || !maybeFlush(true)) {
        if (error_ == WriterError::None)
            fail(WriterError::InvalidRecord, WriterStageV2::InitialFrame);
        return false;
    }
    lastPostStateHash_ =
        beginBundle.frameZero.frame.core.postStateHash;
    summary_.partPath.clear();
    return true;
}

bool ReplayWriterV2::append(const CanonicalTickBundleV2& tick) {
    if (!open_) return fail(WriterError::NotOpen, WriterStageV2::TimelineFrames);
    ParseStatus why = ParseStatus::Ok;
    ConfirmedTimelineValidatorV2 candidate = validator_;
    if (!candidate.append(tick, why))
        return fail(WriterError::InvalidRecord, WriterStageV2::TimelineFrames);
    if (!writeTick(
            tick, WriterStageV2::TimelineEvents,
            WriterStageV2::TimelineFrames)) {
        return false;
    }
    if (!appendCanonicalTick(tick))
        return fail(
            WriterError::InvalidRecord, WriterStageV2::TimelineFrames);
    validator_ = candidate;
    lastPostStateHash_ = tick.frame.core.postStateHash;
    return true;
}

bool ReplayWriterV2::makeAbnormalEnd(
    const TerminalReason reason, ConfirmedGameEndV2& out) const {
    if (!open_ || reason == TerminalReason::Completed
        || validator_.nextRecord() == 0u) {
        return false;
    }
    ConfirmedGameEndV2 end{};
    end.reason = reason;
    end.nextRecord = validator_.nextRecord();
    end.winningSideMask = 0u;
    end.winnerSlot = -1;
    end.postTailStateHash = lastPostStateHash_;
    end.finalRollingDigest = validator_.rollingDigest();
    CanonicalGameDigestV2 canonical = canonical_;
    std::array<uint8_t, 32> digest{};
    if (!canonical.finish(end, digest)) return false;
    end.canonicalGameDigest = digest;
    out = end;
    return true;
}

bool ReplayWriterV2::writeFooter(const ConfirmedGameEndV2& end) {
    FooterIndexV2 footer{};
    footer.chunkCount = sequence_ + 1u;
    footer.frameCount = frameCount_;
    footer.eventCount = eventCount_;
    footer.manifestOffset = manifestOffset_;
    footer.startOffset = startOffset_;
    footer.firstInputOffset = firstInputOffset_;
    footer.gameEndOffset = gameEndOffset_;
    footer.finalRollingDigest = end.finalRollingDigest;
    footer.canonicalGameDigest = end.canonicalGameDigest;
    footer.rounds = completedRounds_;

    std::vector<uint8_t> projection;
    if (!encodeFooterIndexFileDigestProjectionV2(footer, projection))
        return fail(WriterError::InvalidRecord, WriterStageV2::Footer);

    ChunkHeaderV2 header{};
    header.kind = ChunkKindV2::FooterIndex;
    header.schemaVersion = 2u;
    header.critical = 1u;
    header.codec = ChunkCodec::Raw;
    header.sequence = sequence_;
    header.gameGeneration = summary_.identity.gameGeneration;
    header.firstRecord = 0u;
    header.nextRecord = end.nextRecord;
    header.recordCount = 1u;
    header.payloadBytes = static_cast<uint32_t>(projection.size());
    header.decodedBytes = header.payloadBytes;
    header.payloadCrc32c = 0u;
    std::vector<uint8_t> domainHeader;
    encodeChunkHeaderV2(header, domainHeader);
    if (domainHeader.size() != kChunkHeaderBytes)
        return fail(WriterError::InvalidRecord, WriterStageV2::Footer);
    std::fill(domainHeader.end() - 8, domainHeader.end(), 0u);

    Sha256 projected = fileSha_;
    projected.update(domainHeader.data(), domainHeader.size());
    projected.update(projection.data(), projection.size());
    projected.finish(footer.fileDigest);

    std::vector<uint8_t> payload;
    if (!encodeFooterIndexV2(footer, payload))
        return fail(WriterError::InvalidRecord, WriterStageV2::Footer);
    header.payloadCrc32c = crc32c(payload.data(), payload.size());
    std::vector<uint8_t> encodedHeader;
    encodeChunkHeaderV2(header, encodedHeader);
    errorStage_ = WriterStageV2::Footer;
    if (fault(WriterStageV2::Footer)
        || !rawWrite(encodedHeader.data(), encodedHeader.size(), false)
        || !rawWrite(payload.data(), payload.size(), false)) {
        if (error_ == WriterError::None)
            fail(WriterError::WriteFailed, WriterStageV2::Footer);
        return false;
    }
    ++sequence_;
    summary_.fileDigest = footer.fileDigest;
    summary_.footerWritten = true;
    return true;
}

bool ReplayWriterV2::finalize(const EndGameBundleV2& endBundle) {
    if (!open_) return fail(WriterError::NotOpen, WriterStageV2::GameEnd);

    ParseStatus why = ParseStatus::Ok;
    ConfirmedTimelineValidatorV2 candidate = validator_;
    if (endBundle.containsFinalFrame
        && !candidate.append(endBundle.finalFrame, why)) {
        return fail(WriterError::InvalidRecord, WriterStageV2::GameEnd);
    }
    if (!candidate.seal(endBundle.end, why))
        return fail(WriterError::InvalidRecord, WriterStageV2::GameEnd);
    if (endBundle.end.canonicalGameDigest == std::array<uint8_t, 32>{})
        return fail(WriterError::InvalidRecord, WriterStageV2::GameEnd);

    if (endBundle.containsFinalFrame
        && !writeTick(
            endBundle.finalFrame, WriterStageV2::TimelineEvents,
            WriterStageV2::TimelineFrames)) {
        return false;
    }
    if (endBundle.containsFinalFrame
        && !appendCanonicalTick(endBundle.finalFrame)) {
        return fail(WriterError::InvalidRecord, WriterStageV2::GameEnd);
    }
    CanonicalGameDigestV2 canonicalCandidate = canonical_;
    std::array<uint8_t, 32> computedCanonical{};
    if (!canonicalCandidate.finish(endBundle.end, computedCanonical)
        || computedCanonical != endBundle.end.canonicalGameDigest) {
        return fail(WriterError::InvalidRecord, WriterStageV2::GameEnd);
    }
    canonical_ = canonicalCandidate;
    if (!flushFrames(WriterStageV2::TimelineFrames)) return false;

    std::vector<uint8_t> payload;
    if (!encodeConfirmedGameEndV2(endBundle.end, payload)
        || !appendChunk(
            ChunkKindV2::GameEnd,
            endBundle.end.nextRecord - 1u, endBundle.end.nextRecord, 1u,
            payload, WriterStageV2::GameEnd, &gameEndOffset_)) {
        return false;
    }
    validator_ = candidate;
    summary_.terminalReason = endBundle.end.reason;
    summary_.canonicalGameDigest = endBundle.end.canonicalGameDigest;

    if (!writeFooter(endBundle.end) || !maybeFlush(true)) return false;

    errorStage_ = WriterStageV2::Close;
    if (fault(WriterStageV2::Close))
        return fail(WriterError::FlushFailed, WriterStageV2::Close);
    open_ = false;

    errorStage_ = WriterStageV2::Rename;
    if (fault(WriterStageV2::Rename))
        return fail(WriterError::RenameFailed, WriterStageV2::Rename);
    const std::wstring stem =
        finalPath_.substr(0u, finalPath_.size() - 5u);
    const uint32_t attempts =
        config_.maxNameAttempts == 0u ? 1u : config_.maxNameAttempts;
    bool published = false;
    std::wstring target;
    for (uint32_t attempt = 1u; attempt <= attempts; ++attempt) {
        target = stem;
        if (attempt != 1u) {
            wchar_t suffix[20];
            _snwprintf_s(
                suffix, _countof(suffix), _TRUNCATE, L"_%u", attempt);
            target += suffix;
        }
        target += L".q99r";
        if (!sink_->createNew(target)) {
            const WriterError e = sink_->lastError();
            if (e == WriterError::DiskFull || e == WriterError::AccessDenied
                || e == WriterError::RenameFailed) {
                return fail(e, WriterStageV2::Rename);
            }
            continue;
        }
        if ((!stagedBytes_.empty()
             && !sink_->append(stagedBytes_.data(), stagedBytes_.size()))
            || !sink_->flush() || !sink_->close()) {
            WriterError e = sink_->lastError();
            if (e == WriterError::None) e = WriterError::WriteFailed;
            return fail(e, WriterStageV2::Rename);
        }
        published = true;
        break;
    }
    if (!published)
        return fail(WriterError::NameExhausted, WriterStageV2::Rename);

    finalPath_ = target;
    finalized_ = true;
    summary_.partPath.clear();
    summary_.finalPath = finalPath_;
    summary_.chunkCount = sequence_;
    summary_.frameCount = frameCount_;
    summary_.eventCount = eventCount_;
    summary_.bytes = bytesWritten_;
    summary_.finalized = true;
    stagedBytes_.clear();
    return true;
}

void ReplayWriterV2::closeBestEffort() {
    if (sink_ && open_) (void)sink_->close();
    open_ = false;
}

void ReplayWriterV2::abandon() {
    if (!open_ && stagedBytes_.empty()) return;
    closeBestEffort();
    if (sink_) (void)sink_->discard();
    stagedBytes_.clear();
    summary_.partPath.clear();
    summary_.chunkCount = sequence_;
    summary_.frameCount = frameCount_;
    summary_.eventCount = eventCount_;
    summary_.bytes = bytesWritten_;
}

}
