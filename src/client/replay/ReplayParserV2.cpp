#include "ReplayParserV2.h"

#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <limits>

namespace qclient::replay {

using namespace qreplay;

namespace {

ParseResult failure(
    const ParseStatus status, const uint64_t offset,
    const uint32_t sequence, const uint32_t frame,
    const char* detail) {
    ParseResult result{};
    result.status = status;
    result.offset = offset;
    result.chunkSequence = sequence;
    result.lastValidTimelineFrame = frame;
    result.detail = detail != nullptr ? detail : "";
    return result;
}

bool add64(const uint64_t a, const uint64_t b, uint64_t& out) {
    if (a > std::numeric_limits<uint64_t>::max() - b) return false;
    out = a + b;
    return true;
}

bool hashRange(
    ReplayByteSource& source, const uint64_t bytes,
    std::array<uint8_t, 32>& digest) {
    Sha256 sha;
    std::vector<uint8_t> buffer(64u * 1024u);
    uint64_t offset = 0u;
    while (offset < bytes) {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(
            buffer.size(), bytes - offset));
        if (!source.read(offset, buffer.data(), take)) return false;
        sha.update(buffer.data(), take);
        offset += take;
    }
    sha.finish(digest);
    return true;
}

struct TickAssembler {
    ConfirmedTimelineValidatorV2 validator;
    FixedEventListV2 pre{};
    FixedEventListV2 post{};
    ConfirmedGameFrameV2 frame{};
    bool hasFrame = false;
    uint32_t stagedFrame = 0u;
    uint64_t boundaryEnd = 0u;
    uint64_t lastCommittedBoundary = 0u;

    bool addEvent(
        const ConfirmedGameEventV2& event, const uint64_t chunkEnd,
        std::vector<CanonicalTickBundleV2>& retained,
        const bool retain, const size_t frameCap, const size_t eventCap,
        uint32_t& retainedEvents, ParseStatus& why) {
        if (event.phase == GameEventPhaseV2::PreFrame) {
            if (hasFrame) {
                if (event.replayFrame == frame.replayFrame)
                    return false;
                if (!commit(
                        retained, retain, frameCap, eventCap,
                        retainedEvents, why)) {
                    return false;
                }
            }
            if (event.replayFrame != validator.nextRecord()
                || pre.count >= kMaxEventsPerTickV2) {
                return false;
            }
            pre.events[pre.count++] = event;
            stagedFrame = event.replayFrame;
        } else {
            if (!hasFrame || event.replayFrame != frame.replayFrame
                || post.count >= kMaxEventsPerTickV2) {
                return false;
            }
            post.events[post.count++] = event;
        }
        boundaryEnd = std::max(boundaryEnd, chunkEnd);
        return true;
    }

    bool addFrame(
        const ConfirmedGameFrameV2& value, const uint64_t chunkEnd,
        std::vector<CanonicalTickBundleV2>& retained,
        const bool retain, const size_t frameCap, const size_t eventCap,
        uint32_t& retainedEvents, ParseStatus& why) {
        if (hasFrame
            && !commit(
                retained, retain, frameCap, eventCap, retainedEvents,
                why)) {
            return false;
        }
        if (value.replayFrame != validator.nextRecord()
            || (pre.count != 0u && stagedFrame != value.replayFrame)) {
            return false;
        }
        frame = value;
        hasFrame = true;
        stagedFrame = value.replayFrame;
        boundaryEnd = std::max(boundaryEnd, chunkEnd);
        return true;
    }

    bool commit(
        std::vector<CanonicalTickBundleV2>& retained,
        const bool retain, const size_t frameCap, const size_t eventCap,
        uint32_t& retainedEvents, ParseStatus& why) {
        if (!hasFrame) return false;
        CanonicalTickBundleV2 tick{};
        tick.preEvents = pre;
        tick.frame = frame;
        tick.postEvents = post;
        ConfirmedTimelineValidatorV2 candidate = validator;
        if (!candidate.append(tick, why)) return false;
        const size_t events = static_cast<size_t>(pre.count)
            + static_cast<size_t>(post.count);
        if (retain) {
            if (retained.size() >= frameCap
                || retainedEvents > eventCap
                || events > eventCap - retainedEvents) {
                why = ParseStatus::LimitExceeded;
                return false;
            }
            retained.push_back(tick);
            retainedEvents += static_cast<uint32_t>(events);
        }
        validator = candidate;
        lastCommittedBoundary = boundaryEnd;
        pre = FixedEventListV2{};
        post = FixedEventListV2{};
        frame = ConfirmedGameFrameV2{};
        hasFrame = false;
        stagedFrame = validator.nextRecord();
        boundaryEnd = 0u;
        return true;
    }
};

}

ParseResult ReplayParserV2::parse(
    ReplayByteSource& source, const ParseOptionsV2& options,
    ParsedReplayV2& out) {
    out = ParsedReplayV2{};
    out.compatibilityIdentity =
        makeLegacyUnattestedReplayCompatibilityIdentityV3();
    out.sourceBytes = source.size();
    if (out.sourceBytes > kMaxFileBytes)
        return failure(
            ParseStatus::LimitExceeded, 0u, 0u, 0u,
            "file exceeds v2 byte limit");
    if (out.sourceBytes < kPrefixBytes)
        return failure(
            ParseStatus::Truncated, 0u, 0u, 0u, "truncated v2 prefix");

    std::array<uint8_t, kPrefixBytes> prefixBytes{};
    if (!source.read(0u, prefixBytes.data(), prefixBytes.size()))
        return failure(ParseStatus::IoError, 0u, 0u, 0u, "prefix read");
    ParseStatus status =
        decodePrefixV2(prefixBytes.data(), prefixBytes.size(), out.prefix);
    if (status != ParseStatus::Ok)
        return failure(status, 0u, 0u, 0u, "v2 prefix rejected");

    const bool requiresCompatibilityIdentity =
        (out.prefix.featureFlags & kFeatureCompatibilityIdentityV3) != 0u;

    Sha256 fileSha;
    fileSha.update(prefixBytes.data(), prefixBytes.size());

    uint64_t offset = kPrefixBytes;
    uint32_t expectedSequence = 0u;
    uint32_t gameGeneration = 0u;
    uint32_t retainedEvents = 0u;
    uint32_t seenEvents = 0u;
    uint32_t seenFrames = 0u;
    TickAssembler assembler{};
    bool validatorBegun = false;
    bool sawInput = false;
    bool sawEvent = false;
    bool terminalFinalized = false;
    uint64_t footerOffset = 0u;
    uint64_t manifestOffset = 0u;
    uint64_t startOffset = 0u;
    uint64_t firstInputOffset = 0u;
    uint64_t gameEndOffset = 0u;

    while (offset < out.sourceBytes) {
        const uint64_t headerOffset = offset;
        if (out.sourceBytes - offset < kChunkHeaderBytes) {
            status = ParseStatus::Truncated;
            out.lastValidChunkEnd = offset;
            break;
        }
        std::array<uint8_t, kChunkHeaderBytes> headerBytes{};
        if (!source.read(offset, headerBytes.data(), headerBytes.size()))
            return failure(
                ParseStatus::IoError, offset, expectedSequence,
                assembler.validator.nextRecord(), "chunk header read");
        ChunkHeaderV2 header{};
        status = decodeChunkHeaderV2(
            headerBytes.data(), headerBytes.size(), header);
        if (status != ParseStatus::Ok) {
            out.lastValidChunkEnd = offset;
            break;
        }
        if (header.sequence != expectedSequence) {
            status = ParseStatus::SequenceRegression;
            out.lastValidChunkEnd = offset;
            break;
        }
        if (gameGeneration == 0u)
            gameGeneration = header.gameGeneration;
        if (header.gameGeneration != gameGeneration) {
            status = ParseStatus::BadMatchIndex;
            out.lastValidChunkEnd = offset;
            break;
        }
        uint64_t payloadOffset = 0u;
        uint64_t chunkEnd = 0u;
        if (!add64(offset, kChunkHeaderBytes, payloadOffset)
            || !add64(payloadOffset, header.payloadBytes, chunkEnd)
            || chunkEnd > out.sourceBytes) {
            status = ParseStatus::Truncated;
            out.lastValidChunkEnd = offset;
            break;
        }
        std::vector<uint8_t> payload(header.payloadBytes);
        if (!payload.empty()
            && !source.read(
                payloadOffset, payload.data(), payload.size())) {
            return failure(
                ParseStatus::IoError, payloadOffset, expectedSequence,
                assembler.validator.nextRecord(), "chunk payload read");
        }
        if (crc32c(payload.data(), payload.size())
            != header.payloadCrc32c) {
            status = ParseStatus::BadChunkPayloadCrc;
            out.lastValidChunkEnd = offset;
            break;
        }

        ChunkLocationV2 location{};
        location.headerOffset = headerOffset;
        location.payloadOffset = payloadOffset;
        location.header = header;
        if (options.collectChunkIndex) out.chunks.push_back(location);

        if (out.hasFooter) {
            status = ParseStatus::DuplicateSingletonChunk;
            out.lastValidChunkEnd = offset;
            break;
        }

        bool meaningful = true;
        switch (header.kind) {
            case ChunkKindV2::GameManifest: {
                if (expectedSequence != 0u || out.hasManifest
                    || header.recordCount != 1u
                    || header.firstRecord != 0u
                    || header.nextRecord != 0u) {
                    status = ParseStatus::DuplicateSingletonChunk;
                    meaningful = false;
                    break;
                }
                GameManifestV2 manifest{};
                status = decodeGameManifestV2(
                    payload.data(), payload.size(), manifest);
                if (status != ParseStatus::Ok
                    || manifest.identity.gameUuidLow
                           != out.prefix.gameUuidLow
                    || manifest.identity.gameUuidHigh
                           != out.prefix.gameUuidHigh
                    || manifest.identity.gameGeneration
                           != gameGeneration) {
                    if (status == ParseStatus::Ok)
                        status = ParseStatus::BadMatchIndex;
                    meaningful = false;
                    break;
                }
                out.start.manifest = manifest;
                out.hasManifest = true;
                manifestOffset = headerOffset;
                break;
            }
            case ChunkKindV2::CompatibilityIdentity: {
                if (!requiresCompatibilityIdentity || !out.hasManifest
                    || out.hasCompatibilityIdentity || out.hasStart
                    || expectedSequence != 1u || header.recordCount != 1u
                    || header.firstRecord != 0u || header.nextRecord != 0u) {
                    status = ParseStatus::BadMatchIndex;
                    meaningful = false;
                    break;
                }
                std::string identityError;
                const ReplayCompatibilityIdentityDecodeStatusV3 decoded =
                    decodeReplayCompatibilityIdentityEnvelopeV3(
                        payload.data(), payload.size(),
                        out.compatibilityIdentity, identityError);
                if (decoded != ReplayCompatibilityIdentityDecodeStatusV3::Ok
                    || !out.compatibilityIdentity.attested()) {
                    status = ParseStatus::BadMatchIndex;
                    meaningful = false;
                    break;
                }
                out.hasCompatibilityIdentity = true;
                break;
            }
            case ChunkKindV2::GameStart: {
                if (!out.hasManifest || out.hasStart || sawInput
                    || expectedSequence
                        != (requiresCompatibilityIdentity ? 2u : 1u)
                    || (requiresCompatibilityIdentity
                        && !out.hasCompatibilityIdentity)
                    || header.recordCount != 1u) {
                    status = ParseStatus::DuplicateSingletonChunk;
                    meaningful = false;
                    break;
                }
                status = decodeDeterministicGameStartV2(
                    payload.data(), payload.size(), out.start.start);
                if (status != ParseStatus::Ok) {
                    meaningful = false;
                    break;
                }
                ParseStatus why = ParseStatus::Ok;
                if (!assembler.validator.begin(out.start, why)) {
                    status = why;
                    meaningful = false;
                    break;
                }
                out.hasStart = true;
                validatorBegun = true;
                startOffset = headerOffset;
                break;
            }
            case ChunkKindV2::InputBlock: {
                if (!validatorBegun || out.hasEnd
                    || header.firstRecord != seenFrames
                    || header.nextRecord
                        != header.firstRecord + header.recordCount) {
                    status = ParseStatus::FrameGap;
                    meaningful = false;
                    break;
                }
                std::vector<ConfirmedGameFrameV2> frames;
                status = decodeInputBlockV2(
                    payload.data(), payload.size(), header.firstRecord,
                    header.recordCount,
                    out.start.manifest.activeLaneMask, frames);
                if (status != ParseStatus::Ok) {
                    meaningful = false;
                    break;
                }
                ParseStatus why = ParseStatus::Ok;
                for (const ConfirmedGameFrameV2& frame : frames) {
                    if (!assembler.addFrame(
                            frame, chunkEnd, out.ticks,
                            options.retainTimeline,
                            options.maxRetainedFrames,
                            options.maxRetainedEvents, retainedEvents,
                            why)) {
                        status = why == ParseStatus::Ok
                            ? ParseStatus::FrameGap : why;
                        meaningful = false;
                        break;
                    }
                    ++seenFrames;
                }
                sawInput = meaningful;
                if (meaningful && firstInputOffset == 0u)
                    firstInputOffset = headerOffset;
                break;
            }
            case ChunkKindV2::Palette: {
                qreplay::ReplayPaletteV2 palette{};
                const ParseStatus why = decodePaletteBlockV2(
                    payload.data(), payload.size(), palette);
                if (why != ParseStatus::Ok) {
                    meaningful = false;
                    break;
                }
                bool replaced = false;
                for (qreplay::ReplayPaletteV2& seen : out.palettes) {
                    if (seen.side == palette.side) {
                        seen = palette;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) out.palettes.push_back(palette);
                break;
            }
            case ChunkKindV2::EventBlock: {
                if (!validatorBegun || out.hasEnd
                    || header.recordCount == 0u) {
                    status = ParseStatus::BadEventPayload;
                    meaningful = false;
                    break;
                }
                std::vector<ConfirmedGameEventV2> events;
                status = decodeEventBlockV2(
                    payload.data(), payload.size(), seenEvents,
                    header.recordCount, events);
                if (status != ParseStatus::Ok) {
                    meaningful = false;
                    break;
                }
                ParseStatus why = ParseStatus::Ok;
                for (const ConfirmedGameEventV2& event : events) {
                    if (event.replayFrame < header.firstRecord
                        || event.replayFrame >= header.nextRecord
                        || !assembler.addEvent(
                            event, chunkEnd, out.ticks,
                            options.retainTimeline,
                            options.maxRetainedFrames,
                            options.maxRetainedEvents, retainedEvents,
                            why)) {
                        status = why == ParseStatus::Ok
                            ? ParseStatus::BadEventPayload : why;
                        meaningful = false;
                        break;
                    }
                    ++seenEvents;
                }
                sawEvent = meaningful;
                break;
            }
            case ChunkKindV2::GameEnd: {
                if (!validatorBegun || out.hasEnd || !sawInput
                    || header.recordCount != 1u) {
                    status = ParseStatus::DuplicateSingletonChunk;
                    meaningful = false;
                    break;
                }
                ParseStatus why = ParseStatus::Ok;
                if (assembler.hasFrame
                    && !assembler.commit(
                        out.ticks, options.retainTimeline,
                        options.maxRetainedFrames,
                        options.maxRetainedEvents, retainedEvents, why)) {
                    status = why;
                    meaningful = false;
                    break;
                }
                status = decodeConfirmedGameEndV2(
                    payload.data(), payload.size(), out.end);
                if (status != ParseStatus::Ok
                    || header.nextRecord != out.end.nextRecord
                    || header.firstRecord + 1u
                        != header.nextRecord
                    || !assembler.validator.seal(out.end, why)) {
                    if (status == ParseStatus::Ok) status = why;
                    meaningful = false;
                    break;
                }
                out.hasEnd = true;
                terminalFinalized = true;
                gameEndOffset = headerOffset;
                break;
            }
            case ChunkKindV2::FooterIndex: {
                if (!out.hasEnd || out.hasFooter
                    || chunkEnd != out.sourceBytes
                    || header.recordCount != 1u) {
                    status = ParseStatus::BadMatchIndex;
                    meaningful = false;
                    break;
                }
                status = decodeFooterIndexV2(
                    payload.data(), payload.size(), out.footer);
                if (status != ParseStatus::Ok) {
                    meaningful = false;
                    break;
                }
                footerOffset = headerOffset;
                out.hasFooter = true;
                break;
            }
            case ChunkKindV2::ProofBlock:
            case ChunkKindV2::Checkpoint:
                break;
        }
        if (!meaningful) {
            out.lastValidChunkEnd = offset;
            break;
        }

        if (header.kind != ChunkKindV2::FooterIndex) {
            fileSha.update(headerBytes.data(), headerBytes.size());
            if (!payload.empty())
                fileSha.update(payload.data(), payload.size());
        }

        ++expectedSequence;
        out.chunkCount = expectedSequence;
        out.lastValidChunkEnd = chunkEnd;
        offset = chunkEnd;
    }

    const bool stoppedOnInvalid =
        offset < out.sourceBytes && status != ParseStatus::Ok;

    out.frameCount = assembler.validator.nextRecord();
    out.eventCount = seenEvents;
    out.nextEventSequence = assembler.validator.nextEventSequence();
    out.finalRollingDigest = assembler.validator.rollingDigest();
    if (out.frameCount != 0u && options.retainTimeline
        && !out.ticks.empty()) {
        out.lastPostStateHash = out.ticks.back().frame.core.postStateHash;
    }

    if (out.hasFooter) {
        if (!terminalFinalized || assembler.hasFrame
            || out.footer.chunkCount != expectedSequence
            || out.footer.frameCount != out.end.nextRecord
            || out.footer.frameCount != seenFrames
            || out.footer.eventCount != seenEvents
            || out.footer.manifestOffset != manifestOffset
            || out.footer.startOffset != startOffset
            || out.footer.firstInputOffset != firstInputOffset
            || out.footer.gameEndOffset != gameEndOffset
            || gameEndOffset >= footerOffset
            || out.footer.finalRollingDigest
                != out.end.finalRollingDigest
            || out.footer.canonicalGameDigest
                != out.end.canonicalGameDigest) {
            return failure(
                ParseStatus::BadMatchIndex, footerOffset,
                expectedSequence, out.frameCount,
                "v2 footer does not describe the walked game");
        }

        if (options.verifyFileDigest) {
            FooterIndexV2 projection = out.footer;
            std::vector<uint8_t> projectionBytes;
            if (!encodeFooterIndexFileDigestProjectionV2(
                    projection, projectionBytes)) {
                return failure(
                    ParseStatus::BadMatchIndex, footerOffset,
                    expectedSequence, out.frameCount,
                    "v2 footer projection invalid");
            }
            ChunkHeaderV2 projectedHeader{};
            projectedHeader.kind = ChunkKindV2::FooterIndex;
            projectedHeader.schemaVersion = 2u;
            projectedHeader.critical = 1u;
            projectedHeader.codec = ChunkCodec::Raw;
            projectedHeader.sequence = expectedSequence - 1u;
            projectedHeader.gameGeneration = gameGeneration;
            projectedHeader.firstRecord = 0u;
            projectedHeader.nextRecord = out.end.nextRecord;
            projectedHeader.recordCount = 1u;
            projectedHeader.payloadBytes =
                static_cast<uint32_t>(projectionBytes.size());
            projectedHeader.decodedBytes =
                projectedHeader.payloadBytes;
            std::vector<uint8_t> projectedHeaderBytes;
            encodeChunkHeaderV2(projectedHeader, projectedHeaderBytes);
            std::fill(
                projectedHeaderBytes.end() - 8,
                projectedHeaderBytes.end(), 0u);
            Sha256 projected = fileSha;
            projected.update(
                projectedHeaderBytes.data(),
                projectedHeaderBytes.size());
            projected.update(
                projectionBytes.data(), projectionBytes.size());
            projected.finish(out.computedFileDigest);
            if (out.computedFileDigest != out.footer.fileDigest) {
                return failure(
                    ParseStatus::FileDigestMismatch, footerOffset,
                    expectedSequence, out.frameCount,
                    "v2 file digest mismatch");
            }
            out.fileDigestVerified = true;
        }
        out.validatedSourceBytes = out.sourceBytes;
        if (!hashRange(
                source, out.validatedSourceBytes,
                out.validatedSourceRangeDigest)) {
            return failure(
                ParseStatus::IoError, 0u, expectedSequence,
                out.frameCount, "source-range digest read");
        }
        out.structurallyVerified = true;
        out.hasFrameZero = out.frameCount != 0u;
        out.classification = options.sourceNameEndsInPart
            ? ReplayClassificationV2::FooterCompleteRenameFailure
            : ReplayClassificationV2::Sealed;
        ParseResult ok{};
        ok.offset = out.sourceBytes;
        ok.chunkSequence = expectedSequence;
        ok.lastValidTimelineFrame =
            out.frameCount == 0u ? 0u : out.frameCount - 1u;
        return ok;
    }

    if (out.hasEnd) {
        return failure(
            ParseStatus::MissingFooter, out.lastValidChunkEnd,
            expectedSequence, out.frameCount,
            "GameEnd exists without FooterIndex");
    }

    if (assembler.hasFrame) {
        const bool frameZeroBegin =
            assembler.frame.replayFrame == 0u
            && assembler.pre.count >= 2u
            && assembler.pre.events[0].kind
                == GameEventKindV2::GameStarted
            && assembler.pre.events[1].kind
                == GameEventKindV2::RoundStarted;
        if (assembler.post.count != 0u || frameZeroBegin) {
            ParseStatus why = ParseStatus::Ok;
            if (!assembler.commit(
                    out.ticks, options.retainTimeline,
                    options.maxRetainedFrames,
                    options.maxRetainedEvents, retainedEvents, why)) {
                return failure(
                    why, out.lastValidChunkEnd, expectedSequence,
                    assembler.validator.nextRecord(),
                    "unsealed tail composite rejected");
            }
        }
    }
    out.frameCount = assembler.validator.nextRecord();
    out.eventCount = assembler.validator.nextEventSequence();
    out.nextEventSequence = assembler.validator.nextEventSequence();
    out.finalRollingDigest = assembler.validator.rollingDigest();
    if (options.retainTimeline && !out.ticks.empty())
        out.lastPostStateHash = out.ticks.back().frame.core.postStateHash;
    out.hasFrameZero = out.frameCount != 0u;
    out.prebufferTooLarge =
        out.frameCount > options.maxRetainedFrames;

    if (options.allowUnsealedPrefix && out.hasManifest && out.hasStart
        && (!requiresCompatibilityIdentity || out.hasCompatibilityIdentity)
        && sawEvent && out.hasFrameZero
        && assembler.lastCommittedBoundary != 0u) {
        out.validatedSourceBytes = assembler.lastCommittedBoundary;
        if (!hashRange(
                source, out.validatedSourceBytes,
                out.validatedSourceRangeDigest)) {
            return failure(
                ParseStatus::IoError, 0u, expectedSequence,
                out.frameCount, "unsealed source-range digest read");
        }
        out.classification =
            ReplayClassificationV2::UnsealedValidatedPrefix;
        out.structurallyVerified = !stoppedOnInvalid;
        ParseResult ok{};
        ok.offset = stoppedOnInvalid
            ? out.lastValidChunkEnd : out.validatedSourceBytes;
        ok.chunkSequence = expectedSequence;
        ok.lastValidTimelineFrame = out.frameCount - 1u;
        ok.detail = stoppedOnInvalid
            ? "validated prefix stops before corrupt/torn tail"
            : "validated unsealed prefix";
        return ok;
    }

    out.classification =
        out.hasManifest ? ReplayClassificationV2::MetadataOnly
                        : ReplayClassificationV2::Invalid;
    return failure(
        stoppedOnInvalid ? status : ParseStatus::MissingFooter,
        out.lastValidChunkEnd, expectedSequence, out.frameCount,
        "no independently playable v2 endpoint");
}

ParseResult ReplayParserV2::parseFile(
    const std::wstring& path, const ParseOptionsV2& options,
    ParsedReplayV2& out) {
    FileByteSource source;
    if (!source.open(path)) {
        return failure(
            ParseStatus::IoError, 0u, 0u, 0u,
            openErrorText(source.error()));
    }
    ParseOptionsV2 effective = options;
    if (path.size() >= 5u) {
        std::wstring lower = path;
        std::transform(
            lower.begin(), lower.end(), lower.begin(),
            [](const wchar_t c) {
                return static_cast<wchar_t>(::towlower(c));
            });
        effective.sourceNameEndsInPart =
            lower.size() >= 10u
            && lower.compare(
                lower.size() - 10u, 10u, L".q99r.part") == 0;
    }
    ParseResult result = parse(source, effective, out);
    out.fileIdentity = source.identity();
    return result;
}

}
