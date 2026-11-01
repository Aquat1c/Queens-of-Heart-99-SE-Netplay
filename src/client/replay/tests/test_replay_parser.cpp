#include "TestHarness.h"

#include "ReplayTestFixtures.h"

#include "client/replay/ReplayParser.h"

#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"

#include <string>
#include <vector>

using namespace qreplay;
using namespace qclient::replay;
using namespace qclient::replay::test;

namespace {

ParseResult parseBytes(const std::vector<uint8_t>& bytes, ParseMode mode,
                       ParsedReplay& out) {
    MemoryByteSource source(bytes.data(), bytes.size());
    ParseOptions options{};
    options.mode = mode;
    return ReplayParser::parse(source, options, out);
}

ParseStatus statusOf(const std::vector<uint8_t>& bytes,
                     ParseMode mode = ParseMode::Verify) {
    ParsedReplay parsed;
    return parseBytes(bytes, mode, parsed).status;
}

bool findChunk(const std::vector<uint8_t>& bytes, ChunkKind kind,
               size_t& headerOffset, ChunkHeader& header) {
    size_t offset = kPrefixBytes;
    while (offset + kChunkHeaderBytes <= bytes.size()) {
        ChunkHeader candidate{};
        if (decodeChunkHeader(bytes.data() + offset, kChunkHeaderBytes,
                              candidate) != ParseStatus::Ok) {
            return false;
        }
        if (candidate.kind == kind) {
            headerOffset = offset;
            header = candidate;
            return true;
        }
        offset += kChunkHeaderBytes + candidate.payloadBytes;
    }
    return false;
}

void rewriteHeader(std::vector<uint8_t>& bytes, size_t headerOffset,
                   const ChunkHeader& header) {
    std::vector<uint8_t> encoded;
    encodeChunkHeader(header, encoded);
    for (size_t i = 0; i < kChunkHeaderBytes; ++i)
        bytes[headerOffset + i] = encoded[i];
}

struct OptionalCounter final : ReplayChunkVisitor {
    uint32_t skipped = 0u;
    uint32_t total = 0u;
    bool onChunk(const ChunkLocation&, const uint8_t*, size_t,
                 const DecodedChunk& decoded) override {
        ++total;
        if (decoded.skippedOptional) ++skipped;
        return true;
    }
};

bool spliceOptionalChunk(const std::vector<uint8_t>& source,
                         bool corruptPayloadCrc,
                         std::vector<uint8_t>& out) {
    ChunkHeader startHeader{};
    size_t startOffset = 0u;
    if (!findChunk(source, ChunkKind::SessionStart, startOffset, startHeader))
        return false;
    const size_t insertAt =
        startOffset + kChunkHeaderBytes + startHeader.payloadBytes;

    const std::vector<uint8_t> payload = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    ChunkHeader proof{};
    proof.kind = ChunkKind::ProofBlock;
    proof.schemaVersion = 1u;
    proof.critical = 0u;
    proof.codec = ChunkCodec::Raw;
    proof.sequence = startHeader.sequence + 1u;
    proof.recordCount = 1u;
    proof.payloadBytes = static_cast<uint32_t>(payload.size());
    proof.decodedBytes = proof.payloadBytes;
    proof.payloadCrc32c = crc32c(payload.data(), payload.size());
    if (corruptPayloadCrc) proof.payloadCrc32c ^= 0x01u;

    out.assign(source.begin(), source.begin() + static_cast<ptrdiff_t>(insertAt));
    std::vector<uint8_t> chunk;
    encodeChunkHeader(proof, chunk);
    out.insert(out.end(), chunk.begin(), chunk.end());
    out.insert(out.end(), payload.begin(), payload.end());

    size_t cursor = insertAt;
    while (cursor + kChunkHeaderBytes <= source.size()) {
        ChunkHeader following{};
        if (decodeChunkHeader(source.data() + cursor, kChunkHeaderBytes,
                              following) != ParseStatus::Ok) {
            return false;
        }
        ChunkHeader shifted = following;
        shifted.sequence = following.sequence + 1u;
        std::vector<uint8_t> encoded;
        encodeChunkHeader(shifted, encoded);
        out.insert(out.end(), encoded.begin(), encoded.end());
        out.insert(out.end(),
                   source.begin()
                       + static_cast<ptrdiff_t>(cursor + kChunkHeaderBytes),
                   source.begin()
                       + static_cast<ptrdiff_t>(cursor + kChunkHeaderBytes
                                                + following.payloadBytes));
        cursor += kChunkHeaderBytes + following.payloadBytes;
    }
    return true;
}

}

QNP_TEST(replay_parser_accepts_a_complete_fixture) {
    FixtureOptions options{};
    options.matchCount = 2u;
    options.framesPerMatch = 12u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);
    QNP_REQUIRE(!fixture.bytes.empty());

    ParsedReplay parsed;
    const ParseResult result = parseBytes(fixture.bytes, ParseMode::Full, parsed);
    QNP_CHECK(result.ok());
    QNP_CHECK(parsed.hasManifest);
    QNP_CHECK(parsed.hasSessionStart);
    QNP_CHECK(parsed.hasFooter);
    QNP_CHECK(parsed.matches.size() == 2u);
    QNP_CHECK(parsed.totalFrames == 24u);
    QNP_CHECK(parsed.file.frames.size() == 24u);
    QNP_CHECK(parsed.file.events.size() == 4u);
    QNP_CHECK(parsed.file.prefix.sessionUuidLow == options.uuidLow);
    QNP_CHECK(parsed.file.manifest.display.title == options.title);
    QNP_CHECK(parsed.file.footer.finalRollingDigest == fixture.finalDigest);
    QNP_CHECK(parsed.compatibilityIdentity.legacyUnattested());
    for (size_t i = 0; i < parsed.file.frames.size(); ++i)
        QNP_CHECK(parsed.file.frames[i] == fixture.frames[i]);
}

QNP_TEST(replay_parser_metadata_mode_does_not_stream_the_middle) {
    FixtureOptions options{};
    options.matchCount = 3u;
    options.framesPerMatch = 400u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    MemoryByteSource source(fixture.bytes.data(), fixture.bytes.size());
    ParseOptions parseOptions{};
    parseOptions.mode = ParseMode::Metadata;
    ParsedReplay parsed;
    const ParseResult result = ReplayParser::parse(source, parseOptions, parsed);
    QNP_CHECK(result.ok());
    QNP_CHECK(parsed.hasManifest);
    QNP_CHECK(parsed.hasFooter);
    QNP_CHECK(parsed.matches.size() == 3u);
    QNP_CHECK(parsed.totalFrames == 1200u);
    QNP_CHECK(source.bytesRead() < fixture.bytes.size());
}

QNP_TEST(replay_parser_full_mode_can_narrow_to_one_match) {
    FixtureOptions options{};
    options.matchCount = 3u;
    options.framesPerMatch = 10u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    MemoryByteSource source(fixture.bytes.data(), fixture.bytes.size());
    ParseOptions parseOptions{};
    parseOptions.mode = ParseMode::Full;
    parseOptions.matchIdFilter = 2u;
    ParsedReplay parsed;
    QNP_CHECK(ReplayParser::parse(source, parseOptions, parsed).ok());
    QNP_CHECK(parsed.file.frames.size() == 10u);
    QNP_CHECK(parsed.totalFrames == 30u);
}

QNP_TEST(replay_parser_incomplete_file_has_no_footer) {
    FixtureOptions options{};
    options.finalize = false;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    ParsedReplay parsed;
    const ParseResult result = parseBytes(fixture.bytes, ParseMode::Verify, parsed);
    QNP_CHECK(result.ok());
    QNP_CHECK(!parsed.hasFooter);

    MemoryByteSource source(fixture.bytes.data(), fixture.bytes.size());
    ParseOptions strict{};
    strict.mode = ParseMode::Verify;
    strict.requireFooter = true;
    ParsedReplay other;
    QNP_CHECK(ReplayParser::parse(source, strict, other).status
              == ParseStatus::MissingFooter);
}

QNP_TEST(replay_parser_rejects_truncation_at_every_byte) {
    FixtureOptions options{};
    options.matchCount = 1u;
    options.framesPerMatch = 3u;
    options.roundsPerMatch = 1u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);
    QNP_REQUIRE(fixture.bytes.size() > kPrefixBytes);

    size_t accepted = 0u;
    for (size_t cut = 0; cut < fixture.bytes.size(); ++cut) {
        std::vector<uint8_t> truncated(fixture.bytes.begin(),
                                       fixture.bytes.begin()
                                           + static_cast<ptrdiff_t>(cut));
        ParsedReplay parsed;
        const ParseResult result =
            parseBytes(truncated, ParseMode::Verify, parsed);
        if (result.ok()) {
            QNP_CHECK(!parsed.hasFooter);
            ++accepted;
        }
    }
    QNP_CHECK(accepted <= fixture.chunkOffsets.size());
    QNP_CHECK(accepted >= 1u);

    ParsedReplay whole;
    QNP_CHECK(parseBytes(fixture.bytes, ParseMode::Verify, whole).ok());
    QNP_CHECK(whole.hasFooter);
}

QNP_TEST(replay_parser_rejects_a_file_shorter_than_the_prefix) {
    std::vector<uint8_t> bytes(kPrefixBytes - 1u, 0u);
    QNP_CHECK(statusOf(bytes) == ParseStatus::Truncated);
    QNP_CHECK(statusOf(std::vector<uint8_t>()) == ParseStatus::Truncated);
}

QNP_TEST(replay_parser_rejects_prefix_tampering) {
    const Fixture fixture = makeFixture(FixtureOptions{});
    QNP_REQUIRE(fixture.ok);

    {
        std::vector<uint8_t> bytes = fixture.bytes;
        bytes[0] ^= 0xFFu;
        QNP_CHECK(statusOf(bytes) == ParseStatus::BadMagic);
    }
    {
        for (size_t i = 4; i < kPrefixBytes; ++i) {
            std::vector<uint8_t> bytes = fixture.bytes;
            bytes[i] ^= 0x01u;
            const ParseStatus status = statusOf(bytes);
            QNP_CHECK(status == ParseStatus::BadPrefixCrc);
            if (status != ParseStatus::BadPrefixCrc) break;
        }
    }
    {
        ReplayPrefix prefix = fixture.prefix;
        prefix.containerMajor = 9u;
        prefix.minimumReaderMajor = 9u;
        std::vector<uint8_t> header;
        encodePrefix(prefix, header);
        std::vector<uint8_t> bytes = fixture.bytes;
        for (size_t i = 0; i < kPrefixBytes; ++i) bytes[i] = header[i];
        QNP_CHECK(statusOf(bytes) == ParseStatus::UnsupportedContainerVersion);
    }
    {
        ReplayPrefix prefix = fixture.prefix;
        prefix.featureFlags = 0x80000000u;
        std::vector<uint8_t> header;
        encodePrefix(prefix, header);
        std::vector<uint8_t> bytes = fixture.bytes;
        for (size_t i = 0; i < kPrefixBytes; ++i) bytes[i] = header[i];
        QNP_CHECK(statusOf(bytes) == ParseStatus::UnsupportedFeature);
    }
}

QNP_TEST(replay_parser_rejects_every_single_bit_flip) {
    FixtureOptions options{};
    options.framesPerMatch = 2u;
    options.roundsPerMatch = 1u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    size_t survivors = 0u;
    for (size_t i = kPrefixBytes; i < fixture.bytes.size(); ++i) {
        std::vector<uint8_t> bytes = fixture.bytes;
        bytes[i] ^= 0x01u;
        ParsedReplay parsed;
        if (parseBytes(bytes, ParseMode::Verify, parsed).ok()) ++survivors;
    }
    QNP_CHECK(survivors == 0u);
}

QNP_TEST(replay_parser_rejects_a_bad_sync_word) {
    const Fixture fixture = makeFixture(FixtureOptions{});
    QNP_REQUIRE(fixture.ok);
    std::vector<uint8_t> bytes = fixture.bytes;
    bytes[kPrefixBytes] = 0x00u;
    QNP_CHECK(statusOf(bytes) == ParseStatus::BadSyncWord);
}

QNP_TEST(replay_parser_rejects_unknown_and_reserved_header_fields) {
    const Fixture fixture = makeFixture(FixtureOptions{});
    QNP_REQUIRE(fixture.ok);
    size_t offset = 0u;
    ChunkHeader header{};
    QNP_REQUIRE(findChunk(fixture.bytes, ChunkKind::InputBlock, offset, header));

    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.kind = static_cast<ChunkKind>(4000u);
        rewriteHeader(bytes, offset, broken);
        QNP_CHECK(statusOf(bytes) == ParseStatus::UnknownCriticalChunk);
    }
    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.critical = 0u;
        rewriteHeader(bytes, offset, broken);
        QNP_CHECK(statusOf(bytes) == ParseStatus::UnknownCriticalChunk);
    }
    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.codec = static_cast<ChunkCodec>(7u);
        rewriteHeader(bytes, offset, broken);
        QNP_CHECK(statusOf(bytes) == ParseStatus::UnknownChunkCodec);
    }
    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.schemaVersion = 2u;
        rewriteHeader(bytes, offset, broken);
        QNP_CHECK(statusOf(bytes) == ParseStatus::UnknownCriticalChunk);
    }
    {
        std::vector<uint8_t> bytes = fixture.bytes;
        bytes[offset + 10u] = 0x01u;
        uint8_t scratch[kChunkHeaderBytes];
        for (size_t i = 0; i < kChunkHeaderBytes; ++i)
            scratch[i] = bytes[offset + i];
        for (unsigned i = 0; i < 4u; ++i)
            scratch[kChunkHeaderBytes - 4u + i] = 0u;
        const uint32_t crc = crc32c(scratch, kChunkHeaderBytes);
        for (unsigned i = 0; i < 4u; ++i) {
            bytes[offset + kChunkHeaderBytes - 4u + i] =
                static_cast<uint8_t>(crc >> (i * 8u));
        }
        QNP_CHECK(statusOf(bytes) == ParseStatus::UnsupportedContainerVersion);
    }
    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.decodedBytes = header.payloadBytes + 1u;
        rewriteHeader(bytes, offset, broken);
        QNP_CHECK(statusOf(bytes) == ParseStatus::LimitExceeded);
    }
    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.payloadBytes = kMaxChunkPayloadBytes + 1u;
        broken.decodedBytes = broken.payloadBytes;
        rewriteHeader(bytes, offset, broken);
        QNP_CHECK(statusOf(bytes) == ParseStatus::LimitExceeded);
    }
    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.payloadBytes = 0xFFFFFF00u;
        broken.decodedBytes = broken.payloadBytes;
        rewriteHeader(bytes, offset, broken);
        QNP_CHECK(statusOf(bytes) == ParseStatus::LimitExceeded);
    }
    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.recordCount = header.recordCount + 1u;
        rewriteHeader(bytes, offset, broken);
        QNP_CHECK(statusOf(bytes) == ParseStatus::FrameGap);
    }
    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.firstTimelineFrame = header.lastTimelineFrame;
        broken.lastTimelineFrame = header.firstTimelineFrame;
        rewriteHeader(bytes, offset, broken);
        const ParseStatus status = statusOf(bytes);
        QNP_CHECK(status == ParseStatus::FrameConflict
                  || status == ParseStatus::FrameGap);
    }
}

QNP_TEST(replay_parser_rejects_sequence_regression_and_gaps) {
    const Fixture fixture = makeFixture(FixtureOptions{});
    QNP_REQUIRE(fixture.ok);
    size_t offset = 0u;
    ChunkHeader header{};
    QNP_REQUIRE(findChunk(fixture.bytes, ChunkKind::MatchStart, offset, header));

    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.sequence = 0u;
        rewriteHeader(bytes, offset, broken);
        QNP_CHECK(statusOf(bytes) == ParseStatus::SequenceRegression);
    }
    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.sequence = header.sequence + 5u;
        rewriteHeader(bytes, offset, broken);
        QNP_CHECK(statusOf(bytes) == ParseStatus::SequenceRegression);
    }
}

QNP_TEST(replay_parser_rejects_a_missing_manifest_or_session_start) {
    const Fixture fixture = makeFixture(FixtureOptions{});
    QNP_REQUIRE(fixture.ok);

    ChunkHeader manifestHeader{};
    size_t manifestOffset = 0u;
    QNP_REQUIRE(findChunk(fixture.bytes, ChunkKind::SessionManifest,
                          manifestOffset, manifestHeader));
    const size_t manifestBytes = kChunkHeaderBytes + manifestHeader.payloadBytes;
    std::vector<uint8_t> bytes = fixture.bytes;
    bytes.erase(bytes.begin() + static_cast<ptrdiff_t>(manifestOffset),
                bytes.begin()
                    + static_cast<ptrdiff_t>(manifestOffset + manifestBytes));
    QNP_CHECK(statusOf(bytes) == ParseStatus::MissingManifest);
}

QNP_TEST(replay_parser_rejects_a_duplicate_singleton) {
    const Fixture fixture = makeFixture(FixtureOptions{});
    QNP_REQUIRE(fixture.ok);
    ChunkHeader manifestHeader{};
    size_t manifestOffset = 0u;
    QNP_REQUIRE(findChunk(fixture.bytes, ChunkKind::SessionManifest,
                          manifestOffset, manifestHeader));
    const size_t manifestBytes = kChunkHeaderBytes + manifestHeader.payloadBytes;

    std::vector<uint8_t> duplicate(
        fixture.bytes.begin() + static_cast<ptrdiff_t>(manifestOffset),
        fixture.bytes.begin()
            + static_cast<ptrdiff_t>(manifestOffset + manifestBytes));
    ChunkHeader second = manifestHeader;
    second.sequence = 1u;
    std::vector<uint8_t> encoded;
    encodeChunkHeader(second, encoded);
    for (size_t i = 0; i < kChunkHeaderBytes; ++i) duplicate[i] = encoded[i];

    std::vector<uint8_t> bytes = fixture.bytes;
    bytes.insert(bytes.begin() + static_cast<ptrdiff_t>(kPrefixBytes
                                                        + manifestBytes),
                 duplicate.begin(), duplicate.end());
    QNP_CHECK(statusOf(bytes) == ParseStatus::DuplicateSingletonChunk);
}

QNP_TEST(replay_parser_rejects_reordered_chunks) {
    FixtureOptions options{};
    options.framesPerMatch = 4u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    ChunkHeader manifestHeader{};
    ChunkHeader startHeader{};
    size_t manifestOffset = 0u;
    size_t startOffset = 0u;
    QNP_REQUIRE(findChunk(fixture.bytes, ChunkKind::SessionManifest,
                          manifestOffset, manifestHeader));
    QNP_REQUIRE(findChunk(fixture.bytes, ChunkKind::SessionStart, startOffset,
                          startHeader));
    const size_t manifestBytes = kChunkHeaderBytes + manifestHeader.payloadBytes;
    const size_t startBytes = kChunkHeaderBytes + startHeader.payloadBytes;

    std::vector<uint8_t> bytes(fixture.bytes.begin(),
                               fixture.bytes.begin() + kPrefixBytes);
    bytes.insert(bytes.end(),
                 fixture.bytes.begin() + static_cast<ptrdiff_t>(startOffset),
                 fixture.bytes.begin()
                     + static_cast<ptrdiff_t>(startOffset + startBytes));
    bytes.insert(bytes.end(),
                 fixture.bytes.begin() + static_cast<ptrdiff_t>(manifestOffset),
                 fixture.bytes.begin()
                     + static_cast<ptrdiff_t>(manifestOffset + manifestBytes));
    bytes.insert(bytes.end(),
                 fixture.bytes.begin()
                     + static_cast<ptrdiff_t>(startOffset + startBytes),
                 fixture.bytes.end());
    const ParseStatus status = statusOf(bytes);
    QNP_CHECK(status == ParseStatus::MissingManifest
              || status == ParseStatus::SequenceRegression);
}

QNP_TEST(replay_parser_rejects_a_timeline_gap_and_an_overlap) {
    FixtureOptions options{};
    options.framesPerMatch = 6u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);
    size_t offset = 0u;
    ChunkHeader header{};
    QNP_REQUIRE(findChunk(fixture.bytes, ChunkKind::InputBlock, offset, header));

    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.firstTimelineFrame = header.firstTimelineFrame + 4u;
        broken.lastTimelineFrame = header.lastTimelineFrame + 4u;
        rewriteHeader(bytes, offset, broken);
        QNP_CHECK(statusOf(bytes) == ParseStatus::FrameGap);
    }
    {
        std::vector<uint8_t> bytes = fixture.bytes;
        ChunkHeader broken = header;
        broken.firstTimelineFrame = header.firstTimelineFrame - 2u;
        broken.lastTimelineFrame = header.lastTimelineFrame - 2u;
        rewriteHeader(bytes, offset, broken);
        QNP_CHECK(statusOf(bytes) == ParseStatus::FrameConflict);
    }
}

QNP_TEST(replay_parser_rejects_a_footer_that_disagrees_with_the_file) {
    const Fixture fixture = makeFixture(FixtureOptions{});
    QNP_REQUIRE(fixture.ok);
    size_t offset = 0u;
    ChunkHeader header{};
    QNP_REQUIRE(findChunk(fixture.bytes, ChunkKind::FooterIndex, offset, header));

    FooterIndex footer{};
    QNP_REQUIRE(decodeFooterIndex(fixture.bytes.data() + offset
                                      + kChunkHeaderBytes,
                                  header.payloadBytes, footer)
                == ParseStatus::Ok);
    footer.totalFrames += 1u;
    std::vector<uint8_t> payload;
    QNP_REQUIRE(encodeFooterIndex(footer, payload));

    std::vector<uint8_t> bytes(fixture.bytes.begin(),
                               fixture.bytes.begin()
                                   + static_cast<ptrdiff_t>(offset));
    ChunkHeader repaired = header;
    repaired.payloadCrc32c = crc32c(payload.data(), payload.size());
    std::vector<uint8_t> encoded;
    encodeChunkHeader(repaired, encoded);
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    QNP_CHECK(statusOf(bytes) == ParseStatus::FileDigestMismatch);
}

QNP_TEST(replay_parser_rejects_crc_good_but_sha_bad) {
    const Fixture fixture = makeFixture(FixtureOptions{});
    QNP_REQUIRE(fixture.ok);
    size_t offset = 0u;
    ChunkHeader header{};
    QNP_REQUIRE(findChunk(fixture.bytes, ChunkKind::FooterIndex, offset, header));

    std::vector<uint8_t> bytes = fixture.bytes;
    const size_t payloadOffset = offset + kChunkHeaderBytes;
    bytes[bytes.size() - 1u] ^= 0x01u;
    ChunkHeader repaired = header;
    repaired.payloadCrc32c =
        crc32c(bytes.data() + payloadOffset, header.payloadBytes);
    rewriteHeader(bytes, offset, repaired);
    QNP_CHECK(statusOf(bytes) == ParseStatus::FileDigestMismatch);
}

QNP_TEST(replay_parser_rejects_content_after_the_footer) {
    const Fixture fixture = makeFixture(FixtureOptions{});
    QNP_REQUIRE(fixture.ok);
    std::vector<uint8_t> bytes = fixture.bytes;
    bytes.push_back(0x00u);
    QNP_CHECK(statusOf(bytes) == ParseStatus::BadMatchIndex);
}

QNP_TEST(replay_parser_skips_an_unknown_optional_chunk_only_after_crc) {
    FixtureOptions options{};
    options.framesPerMatch = 4u;
    options.finalize = false;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    std::vector<uint8_t> good;
    std::vector<uint8_t> bad;
    QNP_REQUIRE(spliceOptionalChunk(fixture.bytes, false, good));
    QNP_REQUIRE(spliceOptionalChunk(fixture.bytes, true, bad));

    WalkOptions walk{};
    walk.verifyFileDigest = false;

    OptionalCounter counter;
    MemoryByteSource goodSource(good.data(), good.size());
    QNP_CHECK(ReplayParser::walk(goodSource, walk, counter).ok());
    QNP_CHECK(counter.skipped == 1u);
    QNP_CHECK(counter.total > 5u);

    OptionalCounter badCounter;
    MemoryByteSource badSource(bad.data(), bad.size());
    QNP_CHECK(ReplayParser::walk(badSource, walk, badCounter).status
              == ParseStatus::BadChunkPayloadCrc);
    QNP_CHECK(badCounter.skipped == 0u);
}

QNP_TEST(replay_parser_rejects_control_characters_in_labels) {
    FixtureOptions options{};
    options.laneOneName = std::string("\x1b[31mRED\x1b[0m");
    SessionManifest manifest = makeManifest(options);
    std::vector<uint8_t> payload;
    QNP_CHECK(!encodeSessionManifest(manifest, payload));

    options.laneOneName = std::string("bad\xC0\xAF utf8");
    manifest = makeManifest(options);
    payload.clear();
    QNP_CHECK(!encodeSessionManifest(manifest, payload));

    options.laneOneName = std::string(kMaxLabelBytes + 1u, 'x');
    manifest = makeManifest(options);
    payload.clear();
    QNP_CHECK(!encodeSessionManifest(manifest, payload));
}

QNP_TEST(replay_sanitizer_never_emits_an_escape_sequence) {
    const std::string hostile =
        std::string("\x1b]0;title\x07 \x1b[2J normal \x7f \x01 end");
    const std::string safe = sanitizeForDisplay(hostile);
    for (char ch : safe) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        QNP_CHECK(byte >= 0x20u && byte != 0x7Fu);
    }
    QNP_CHECK(safe.find("normal") != std::string::npos);

    QNP_CHECK(sanitizeForDisplay(std::string("\xC0\xAF")) == "?");
    QNP_CHECK(sanitizeForDisplay(std::string("\xED\xA0\x80")) == "?");
    QNP_CHECK(sanitizeForDisplay(std::string("\xE2\x82")) == "??");
    QNP_CHECK(sanitizeForDisplay(std::string("\xE2\x82\xAC")) == "\xE2\x82\xAC");
    const std::string clipped = sanitizeForDisplay(std::string(64, 'a'), 10u);
    QNP_CHECK(clipped.size() <= 10u);
    QNP_CHECK(clipped.substr(clipped.size() - 3u) == "...");
}

QNP_TEST(replay_path_policy_rejects_traversal_and_streams) {
    QNP_CHECK(isSafeReplayPath(L"C:\\replays\\a.q99r"));
    QNP_CHECK(isSafeReplayPath(L"replays\\a.q99r"));
    QNP_CHECK(!isSafeReplayPath(L"C:\\replays\\..\\windows\\system32\\x.q99r"));
    QNP_CHECK(!isSafeReplayPath(L"C:\\replays\\.\\a.q99r"));
    QNP_CHECK(!isSafeReplayPath(L"C:\\replays\\a.q99r:hidden"));
    QNP_CHECK(!isSafeReplayPath(L"C:\\replays\\*.q99r"));
    QNP_CHECK(!isSafeReplayPath(L"C:\\replays\\a|b.q99r"));
    QNP_CHECK(!isSafeReplayPath(L""));
}

QNP_TEST(replay_parser_footer_search_is_bounded_and_verified) {
    FixtureOptions options{};
    options.matchCount = 2u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    MemoryByteSource source(fixture.bytes.data(), fixture.bytes.size());
    ChunkLocation location{};
    FooterIndex footer{};
    QNP_CHECK(ReplayParser::findFooter(source, location, footer));
    QNP_CHECK(footer.matchCount == 2u);
    QNP_CHECK(location.endOffset() == fixture.bytes.size());

    std::vector<uint8_t> bytes = fixture.bytes;
    for (unsigned i = 0; i < 4u; ++i) {
        bytes[bytes.size() - 8u + i] =
            static_cast<uint8_t>(kChunkSyncWord >> (i * 8u));
    }
    MemoryByteSource planted(bytes.data(), bytes.size());
    ChunkLocation other{};
    FooterIndex otherFooter{};
    const bool found = ReplayParser::findFooter(planted, other, otherFooter);
    QNP_CHECK(!found || other.headerOffset == location.headerOffset);

    FixtureOptions incompleteOptions{};
    incompleteOptions.finalize = false;
    const Fixture incomplete = makeFixture(incompleteOptions);
    QNP_REQUIRE(incomplete.ok);
    MemoryByteSource incompleteSource(incomplete.bytes.data(),
                                      incomplete.bytes.size());
    QNP_CHECK(!ReplayParser::findFooter(incompleteSource, other, otherFooter));
}

QNP_TEST(replay_parser_enforces_the_retention_budget) {
    FixtureOptions options{};
    options.matchCount = 1u;
    options.framesPerMatch = 64u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    MemoryByteSource source(fixture.bytes.data(), fixture.bytes.size());
    ParseOptions parseOptions{};
    parseOptions.mode = ParseMode::Full;
    parseOptions.maxRetainedFrames = 8u;
    ParsedReplay parsed;
    const ParseResult result = ReplayParser::parse(source, parseOptions, parsed);
    QNP_CHECK(result.status == ParseStatus::LimitExceeded);
    QNP_CHECK(parsed.file.frames.size() <= 8u);
}
