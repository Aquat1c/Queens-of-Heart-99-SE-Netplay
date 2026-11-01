#include "TestHarness.h"

#include "ReplayTestFixtures.h"

#include "client/replay/ReplayAnnotations.h"
#include "client/replay/ReplayBroker.h"
#include "client/replay/ReplayCatalog.h"
#include "client/replay/ReplayParser.h"
#include "client/replay/ReplayRecorderHost.h"
#include "client/replay/ReplayRecovery.h"
#include "client/replay/ReplayWriter.h"

#include "game/replay/ReplayIpcWorker.h"

#include "platform/WidePath.h"
#include "replay/ReplayCodec.h"
#include "replay/ReplayRecorderQueue.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace qreplay;
using namespace qclient::replay;
using namespace qclient::replay::test;

namespace {

enum class Mutation : uint8_t {
    FlipBit = 0,
    SetByte,
    Truncate,
    Extend,
    ZeroRun,
    MaxRun,
    SwapRegions,
    InsertSyncWord,
    Count,
};

std::vector<uint8_t> mutate(const std::vector<uint8_t>& input,
                            Xorshift64& random) {
    std::vector<uint8_t> out = input;
    if (out.empty()) return out;
    const Mutation mutation = static_cast<Mutation>(
        random.below(static_cast<uint32_t>(Mutation::Count)));
    const size_t at = random.below(static_cast<uint32_t>(out.size()));
    switch (mutation) {
        case Mutation::FlipBit:
            out[at] ^= static_cast<uint8_t>(1u << random.below(8u));
            break;
        case Mutation::SetByte:
            out[at] = static_cast<uint8_t>(random.below(256u));
            break;
        case Mutation::Truncate:
            out.resize(at);
            break;
        case Mutation::Extend: {
            const size_t extra = random.below(64u) + 1u;
            for (size_t i = 0; i < extra; ++i)
                out.push_back(static_cast<uint8_t>(random.below(256u)));
            break;
        }
        case Mutation::ZeroRun: {
            const size_t run = random.below(32u) + 1u;
            for (size_t i = at; i < out.size() && i < at + run; ++i) out[i] = 0u;
            break;
        }
        case Mutation::MaxRun: {
            const size_t run = random.below(32u) + 1u;
            for (size_t i = at; i < out.size() && i < at + run; ++i)
                out[i] = 0xFFu;
            break;
        }
        case Mutation::SwapRegions: {
            const size_t other = random.below(static_cast<uint32_t>(out.size()));
            const size_t run = random.below(24u) + 1u;
            for (size_t i = 0; i < run; ++i) {
                if (at + i >= out.size() || other + i >= out.size()) break;
                const uint8_t tmp = out[at + i];
                out[at + i] = out[other + i];
                out[other + i] = tmp;
            }
            break;
        }
        case Mutation::InsertSyncWord: {
            for (unsigned i = 0; i < 4u; ++i) {
                if (at + i >= out.size()) break;
                out[at + i] = static_cast<uint8_t>(kChunkSyncWord >> (i * 8u));
            }
            break;
        }
        default:
            break;
    }
    return out;
}

bool looksUntouched(const ParsedReplay& parsed) {
    return parsed.file.frames.empty() && parsed.file.events.empty()
        && parsed.file.matchStarts.empty() && parsed.file.matchEnds.empty()
        && parsed.chunkIndex.empty();
}

}

QNP_TEST(replay_fuzz_container_parser_rejects_cleanly) {
    FixtureOptions options{};
    options.matchCount = 2u;
    options.framesPerMatch = 6u;
    options.roundsPerMatch = 1u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    uint32_t accepted = 0u;
    uint32_t rejected = 0u;
    for (uint64_t seed = 1u; seed <= 3000u; ++seed) {
        Xorshift64 random(seed * 0x9E3779B97F4A7C15ull);
        std::vector<uint8_t> bytes = fixture.bytes;
        const uint32_t rounds = 1u + random.below(3u);
        for (uint32_t r = 0; r < rounds; ++r) bytes = mutate(bytes, random);

        MemoryByteSource source(bytes.data(), bytes.size());
        ParseOptions parseOptions{};
        parseOptions.mode = ParseMode::Full;
        ParsedReplay parsed;
        const ParseResult result =
            ReplayParser::parse(source, parseOptions, parsed);

        if (result.ok()) {
            ++accepted;
            QNP_CHECK(parsed.hasManifest);
            continue;
        }
        ++rejected;
        if (result.offset > bytes.size()) {
            std::printf("    seed %llu offset %llu size %zu status %s\n",
                        static_cast<unsigned long long>(seed),
                        static_cast<unsigned long long>(result.offset),
                        bytes.size(), parseStatusText(result.status));
        }
        QNP_CHECK(result.offset <= bytes.size());
        QNP_CHECK(result.status != ParseStatus::Ok);
        if (!looksUntouched(parsed) && parsed.file.frames.size() > 0u) {
            QNP_CHECK(parsed.file.frames.size()
                      <= parseOptions.maxRetainedFrames);
        }
    }
    QNP_CHECK(rejected > 2500u);
    QNP_CHECK(accepted < 200u);
}

QNP_TEST(replay_fuzz_metadata_mode_is_as_strict_as_verify) {
    FixtureOptions options{};
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    for (uint64_t seed = 1u; seed <= 1500u; ++seed) {
        Xorshift64 random(seed * 0xD1B54A32D192ED03ull);
        std::vector<uint8_t> bytes = mutate(fixture.bytes, random);
        MemoryByteSource source(bytes.data(), bytes.size());
        ParseOptions parseOptions{};
        parseOptions.mode = ParseMode::Metadata;
        ParsedReplay parsed;
        const ParseResult result =
            ReplayParser::parse(source, parseOptions, parsed);
        QNP_CHECK(result.offset <= bytes.size());
        if (!result.ok()) {
            QNP_CHECK(!parsed.hasFooter || parsed.file.footer.matchCount
                                               <= kMaxMatchesPerSet);
        }
        if (parsed.hasFooter) {
            QNP_CHECK(parsed.file.footer.matchCount <= kMaxMatchesPerSet);
            QNP_CHECK(parsed.file.footer.totalFrames <= kMaxTimelineFrames);
            QNP_CHECK(parsed.file.footer.matches.size()
                      == parsed.file.footer.matchCount);
        }
    }
}

QNP_TEST(replay_fuzz_recovery_never_exceeds_the_input) {
    FixtureOptions options{};
    options.matchCount = 2u;
    options.finalize = false;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    for (uint64_t seed = 1u; seed <= 2000u; ++seed) {
        Xorshift64 random(seed * 0xA24BAED4963EE407ull);
        std::vector<uint8_t> bytes = mutate(fixture.bytes, random);
        MemoryByteSource source(bytes.data(), bytes.size());
        const RecoveryPlan plan = ReplayRecovery::plan(source);

        QNP_CHECK(plan.validBytes <= bytes.size());
        if (!plan.prefixValid) {
            QNP_CHECK(plan.classification == RecoveryClass::Nothing);
            QNP_CHECK(plan.validBytes == 0u);
        }
        if (plan.classification == RecoveryClass::Playable) {
            QNP_CHECK(plan.hasManifest);
            QNP_CHECK(plan.hasSessionStart);
            QNP_CHECK(plan.anyIndependentBootstrap);
            QNP_CHECK(plan.retainedFrames > 0u);
        }
        QNP_CHECK(plan.retainedFrames <= kMaxTimelineFrames);
        QNP_CHECK(plan.startedMatches <= kMaxMatchesPerSet);
    }
}

QNP_TEST(replay_fuzz_raw_payload_decoders_reject_cleanly) {
    for (uint64_t seed = 1u; seed <= 4000u; ++seed) {
        Xorshift64 random(seed * 0x2545F4914F6CDD1Dull);
        const size_t length = random.below(600u);
        std::vector<uint8_t> bytes(length);
        for (size_t i = 0; i < length; ++i)
            bytes[i] = static_cast<uint8_t>(random.below(256u));

        SessionManifest manifest{};
        DeterministicStart start{};
        MatchStartRecord matchStart{};
        MatchEndRecord matchEnd{};
        SessionEndRecord sessionEnd{};
        FooterIndex footer{};
        std::vector<ReplayFrameV1> frames;
        std::vector<ReplayEvent> events;

        decodeSessionManifest(bytes.data(), bytes.size(), manifest);
        decodeDeterministicStart(bytes.data(), bytes.size(), start);
        decodeMatchStart(bytes.data(), bytes.size(), matchStart);
        decodeMatchEnd(bytes.data(), bytes.size(), matchEnd);
        decodeSessionEnd(bytes.data(), bytes.size(), sessionEnd);
        const ParseStatus footerStatus =
            decodeFooterIndex(bytes.data(), bytes.size(), footer);
        const uint32_t claimedFrames = random.below(kMaxRecordsPerInputBlock + 8u);
        decodeInputBlock(bytes.data(), bytes.size(), 0u, claimedFrames, frames);
        const uint32_t claimedEvents = random.below(kMaxEventsPerBlock + 8u);
        decodeEventBlock(bytes.data(), bytes.size(), claimedEvents, events);

        QNP_CHECK(frames.size() <= kMaxRecordsPerInputBlock);
        QNP_CHECK(events.size() <= kMaxEventsPerBlock);
        QNP_CHECK(footer.matches.size() <= kMaxMatchesPerSet);
        if (footerStatus != ParseStatus::Ok) QNP_CHECK(footer.matches.empty());
        QNP_CHECK(manifest.display.title.size() <= kMaxLabelBytes);
        QNP_CHECK(manifest.display.routeLabel.size() <= kMaxLabelBytes);
        for (const SourceLaneDescriptor& lane : manifest.lanes)
            QNP_CHECK(lane.name.size() <= kMaxLabelBytes);
    }
}

QNP_TEST(replay_fuzz_chunk_header_decoder_is_total) {
    for (uint64_t seed = 1u; seed <= 20000u; ++seed) {
        Xorshift64 random(seed * 0x9E3779B97F4A7C15ull);
        uint8_t bytes[kChunkHeaderBytes];
        for (size_t i = 0; i < kChunkHeaderBytes; ++i)
            bytes[i] = static_cast<uint8_t>(random.below(256u));
        if ((seed & 1u) != 0u) {
            for (unsigned i = 0; i < 4u; ++i)
                bytes[i] = static_cast<uint8_t>(kChunkSyncWord >> (i * 8u));
        }
        ChunkHeader header{};
        const ParseStatus status =
            decodeChunkHeader(bytes, kChunkHeaderBytes, header);
        if (status != ParseStatus::Ok) continue;
        QNP_CHECK(header.payloadBytes <= kMaxChunkPayloadBytes);
        QNP_CHECK(header.decodedBytes <= kMaxCheckpointDecodedBytes);
        QNP_CHECK(header.sequence < kMaxChunksPerFile);
        QNP_CHECK(header.firstTimelineFrame <= kMaxTimelineFrames);
        QNP_CHECK(header.lastTimelineFrame <= kMaxTimelineFrames);
        QNP_CHECK(header.firstTimelineFrame <= header.lastTimelineFrame);
        QNP_CHECK(isKnownChunkKind(static_cast<uint16_t>(header.kind)));
    }
}

QNP_TEST(replay_fuzz_annotation_store_rejects_cleanly) {
    ReplayAnnotationStore source;
    QNP_REQUIRE(source.setTitle(1u, 2u, "Alpha"));
    QNP_REQUIRE(source.setNote(1u, 2u, "a note"));
    QNP_REQUIRE(source.setTags(1u, 2u, { "one", "two" }));
    QNP_REQUIRE(source.setFavorite(3u, 4u, true));
    const std::vector<uint8_t> valid = source.encode();

    for (uint64_t seed = 1u; seed <= 4000u; ++seed) {
        Xorshift64 random(seed * 0xBF58476D1CE4E5B9ull);
        std::vector<uint8_t> bytes = mutate(valid, random);
        ReplayAnnotationStore store;
        const bool ok = store.decode(bytes.data(), bytes.size());
        if (!ok) {
            QNP_CHECK(store.size() == 0u);
            continue;
        }
        QNP_CHECK(store.size() <= ReplayAnnotationStore::kMaxEntries);
        for (const ReplayAnnotation& entry : store.entries()) {
            QNP_CHECK(entry.title.size() <= kMaxLabelBytes);
            QNP_CHECK(entry.note.size() <= kMaxNoteBytes);
            QNP_CHECK(entry.tags.size() <= kMaxTags);
            QNP_CHECK(ReplayAnnotationStore::validTitle(entry.title));
            QNP_CHECK(ReplayAnnotationStore::validNote(entry.note));
            QNP_CHECK(ReplayAnnotationStore::validTagSet(entry.tags));
        }
    }
}

QNP_TEST(replay_fuzz_catalog_cache_rejects_cleanly) {
    ReplayCatalogCache source;
    for (uint32_t i = 0; i < 4u; ++i) {
        ReplayEntry entry{};
        entry.identity.known = true;
        entry.identity.volumeId = 0x1000u + i;
        entry.identity.fileId = 0x2000u + i;
        entry.identity.sizeBytes = 1024u + i;
        entry.identity.modifiedUnixSeconds = 1767225600 + i;
        entry.path = L"C:\\replays\\file" + std::to_wstring(i) + L".q99r";
        entry.hasManifest = true;
        entry.manifest = makeManifest(FixtureOptions{});
        source.store(entry);
    }
    const std::vector<uint8_t> valid = source.encode();
    QNP_REQUIRE(!valid.empty());

    for (uint64_t seed = 1u; seed <= 4000u; ++seed) {
        Xorshift64 random(seed * 0x94D049BB133111EBull);
        std::vector<uint8_t> bytes = mutate(valid, random);
        ReplayCatalogCache cache;
        const bool ok = cache.decode(bytes.data(), bytes.size());
        if (!ok) {
            QNP_CHECK(cache.size() == 0u);
            continue;
        }
        QNP_CHECK(cache.size() <= ReplayCatalogCache::kMaxRecords);
        ReplayEntry entry{};
        FileIdentity identity{};
        identity.known = true;
        identity.volumeId = 0x1000u;
        identity.fileId = 0x2000u;
        identity.sizeBytes = 1024u;
        identity.modifiedUnixSeconds = 1767225600;
        if (cache.lookup(identity, entry)) {
            QNP_CHECK(entry.matches.size()
                      <= ReplayEntry::kMaxRetainedMatches);
        }
    }
}

QNP_TEST(replay_fuzz_sanitizer_is_total_and_bounded) {
    for (uint64_t seed = 1u; seed <= 6000u; ++seed) {
        Xorshift64 random(seed * 0x2545F4914F6CDD1Dull);
        const size_t length = random.below(256u);
        std::string text;
        text.reserve(length);
        for (size_t i = 0; i < length; ++i)
            text.push_back(static_cast<char>(random.below(256u)));

        const size_t limit = 1u + random.below(128u);
        const std::string safe = sanitizeForDisplay(text, limit);
        QNP_CHECK(safe.size() <= limit + 3u);
        for (char ch : safe) {
            const unsigned char byte = static_cast<unsigned char>(ch);
            QNP_CHECK(byte != 0x1Bu);
            QNP_CHECK(byte >= 0x20u || byte >= 0x80u);
            QNP_CHECK(byte != 0x7Fu);
        }
        QNP_CHECK(sanitizeForDisplay(safe, limit + 8u) == safe);
    }
}

QNP_TEST(replay_fuzz_filename_policy_is_total) {
    for (uint64_t seed = 1u; seed <= 4000u; ++seed) {
        Xorshift64 random(seed * 0xBF58476D1CE4E5B9ull);
        const size_t length = random.below(160u);
        std::string text;
        for (size_t i = 0; i < length; ++i)
            text.push_back(static_cast<char>(random.below(256u)));
        const std::string token = ReplayWriter::sanitizeNameToken(text);
        QNP_CHECK(!token.empty());
        QNP_CHECK(token.size() <= 48u);
        for (char ch : token) {
            const bool safe = (ch >= 'A' && ch <= 'Z')
                || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
                || ch == '-';
            QNP_CHECK(safe);
        }
        QNP_CHECK(isSafeReplayPath(qplat::widen("C:\\replays\\" + token
                                                + ".q99r")));
    }
}

namespace {

namespace qgr = qgame::replay;
namespace qwire = qreplay::ipc;

constexpr uint32_t kWireGeneration = 0x51A2C3D4u;
constexpr uint64_t kWireNonce = 0x0F1E2D3C4B5A6978ull;
constexpr uint32_t kWireGamePid = 0x00001234u;

constexpr size_t kOffMagic = 0u;
constexpr size_t kOffWireVersion = 4u;
constexpr size_t kOffChannel = 6u;
constexpr size_t kOffType = 7u;
constexpr size_t kOffPayloadBytes = 32u;
constexpr size_t kOffHeaderCrc = 40u;

std::vector<uint8_t> wirePayload(size_t bytes, uint8_t seed) {
    std::vector<uint8_t> out;
    out.reserve(bytes);
    for (size_t i = 0; i < bytes; ++i)
        out.push_back(static_cast<uint8_t>(seed + i * 31u + (i >> 3)));
    return out;
}

void poke16(std::vector<uint8_t>& bytes, size_t off, uint16_t value) {
    for (unsigned i = 0; i < 2u; ++i)
        bytes[off + i] = static_cast<uint8_t>(value >> (i * 8u));
}

void poke32(std::vector<uint8_t>& bytes, size_t off, uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i)
        bytes[off + i] = static_cast<uint8_t>(value >> (i * 8u));
}

void restampHeaderCrc(std::vector<uint8_t>& bytes) {
    poke32(bytes, kOffHeaderCrc,
           crc32c(bytes.data(), qwire::kReplayIpcHeaderCrcBytes));
}

std::vector<uint8_t> runtimeEncode(qgr::IpcMessageKind kind,
                                   uint32_t generation, uint64_t nonce,
                                   uint32_t sequence, uint32_t param0,
                                   uint32_t param1,
                                   const std::vector<uint8_t>& payload) {
    qgr::IpcFrameHeader header{};
    header.magic = qgr::kReplayIpcMagic;
    header.wireVersion = qgr::kReplayIpcSchemaVersion;
    header.channel = static_cast<uint8_t>(
        qwire::channelForMessageType(static_cast<uint8_t>(kind)));
    header.type = static_cast<uint8_t>(kind);
    header.generation = generation;
    header.nonce = nonce;
    header.sequence = sequence;
    header.param0 = param0;
    header.param1 = param1;
    header.payloadBytes = static_cast<uint32_t>(payload.size());
    header.payloadCrc32c =
        payload.empty() ? 0u : crc32c(payload.data(), payload.size());
    std::vector<uint8_t> bytes;
    qgr::encodeIpcHeader(header, bytes);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<uint8_t> casterEncode(ReplayIpcType type, uint32_t generation,
                                  uint64_t nonce, uint32_t sequence,
                                  uint32_t param0, uint32_t param1,
                                  const std::vector<uint8_t>& payload) {
    ReplayIpcFrame frame{};
    frame.channel = qwire::channelForMessageType(static_cast<uint8_t>(type));
    frame.type = type;
    frame.generation = generation;
    frame.nonce = nonce;
    frame.sequence = sequence;
    frame.param0 = param0;
    frame.param1 = param1;
    frame.payload = payload;
    std::vector<uint8_t> bytes;
    QNP_CHECK(encodeIpcFrame(frame, bytes));
    return bytes;
}

IpcFrameStatus runtimeDecode(const std::vector<uint8_t>& bytes,
                             qgr::IpcFrameHeader& header,
                             std::vector<uint8_t>& payload) {
    qgr::IpcFrameHeader decoded{};
    const IpcFrameStatus status =
        qgr::decodeIpcHeaderStatus(bytes.data(), bytes.size(), decoded);
    if (status != IpcFrameStatus::Ok) return status;
    size_t total = 0u;
    if (!qwire::frameBytes(decoded, total)) return IpcFrameStatus::TooLarge;
    if (bytes.size() < total) return IpcFrameStatus::NeedMore;
    const uint8_t* body = bytes.data() + qgr::kIpcFrameHeaderBytes;
    const uint32_t crc =
        decoded.payloadBytes == 0u ? 0u : crc32c(body, decoded.payloadBytes);
    if (crc != decoded.payloadCrc32c) return IpcFrameStatus::BadPayloadCrc;
    header = decoded;
    payload.assign(body, body + decoded.payloadBytes);
    return IpcFrameStatus::Ok;
}

struct WireCase {
    const char* name;
    qgr::IpcMessageKind kind;
    size_t payloadBytes;
    uint32_t param0;
    uint32_t param1;
};

const WireCase kWireCases[] = {
    {"Hello", qgr::IpcMessageKind::Hello, qwire::kHelloPayloadBytes, 1u, 0u},
    {"SeekRequest", qgr::IpcMessageKind::SeekRequest, 0u, 120u, 480u},
    {"Bye", qgr::IpcMessageKind::Bye, 0u, 0u, 0u},
    {"SessionInfo", qgr::IpcMessageKind::SessionInfo, qwire::kSessionInfoBytes,
     0u, 0u},
    {"SessionStart", qgr::IpcMessageKind::SessionStart,
     kDeterministicStartBytes, 0u, 0u},
    {"InputBlock", qgr::IpcMessageKind::InputBlock, 512u, 90u, 4u},
    {"EventBlock", qgr::IpcMessageKind::EventBlock, kEventRecordBytes, 0u, 1u},
    {"Terminal", qgr::IpcMessageKind::Terminal, qwire::kTerminalPayloadBytes,
     900u, 0u},
    {"RecorderHello", qgr::IpcMessageKind::RecorderHello,
     qwire::kHelloPayloadBytes, 1u, kWireGeneration},
    {"RecorderFrames", qgr::IpcMessageKind::RecorderFrames, 1024u, 0u, 11u},
    {"RecorderEvents", qgr::IpcMessageKind::RecorderEvents, 152u, 0u, 2u},
    {"RecorderTerminal", qgr::IpcMessageKind::RecorderTerminal,
     qwire::kTerminalPayloadBytes, 900u, 0u},
    {"RecorderManifest", qgr::IpcMessageKind::RecorderManifest, 300u, 0u, 0u},
    {"RecorderSessionStart", qgr::IpcMessageKind::RecorderSessionStart,
     kDeterministicStartBytes, 0u, 0u},
    {"RecorderMatchStart", qgr::IpcMessageKind::RecorderMatchStart,
     kMatchStartBytes, 3u, 0u},
    {"RecorderMatchEnd", qgr::IpcMessageKind::RecorderMatchEnd, kMatchEndBytes,
     3u, 0u},
};

}

QNP_TEST(replay_ipc_wire_round_trips_every_message_type) {
    uint32_t sequence = 0u;
    for (const WireCase& testCase : kWireCases) {
        ++sequence;
        const std::vector<uint8_t> payload = wirePayload(
            testCase.payloadBytes, static_cast<uint8_t>(testCase.kind));
        const ReplayIpcType casterType =
            static_cast<ReplayIpcType>(testCase.kind);

        const std::vector<uint8_t> fromRuntime =
            runtimeEncode(testCase.kind, kWireGeneration, kWireNonce, sequence,
                          testCase.param0, testCase.param1, payload);
        const std::vector<uint8_t> fromCaster =
            casterEncode(casterType, kWireGeneration, kWireNonce, sequence,
                         testCase.param0, testCase.param1, payload);

        QNP_CHECK(fromRuntime == fromCaster);
        QNP_CHECK(fromRuntime.size()
                  == qwire::kReplayIpcHeaderBytes + testCase.payloadBytes);

        ReplayIpcFrame decodedByCaster{};
        size_t consumed = 0u;
        QNP_REQUIRE(decodeIpcFrame(fromRuntime.data(), fromRuntime.size(),
                                   decodedByCaster, consumed)
                    == IpcFrameStatus::Ok);
        QNP_CHECK(consumed == fromRuntime.size());
        QNP_CHECK(decodedByCaster.type == casterType);
        QNP_CHECK(decodedByCaster.channel
                  == qwire::channelForMessageType(
                         static_cast<uint8_t>(testCase.kind)));
        QNP_CHECK(decodedByCaster.wireVersion == qwire::kReplayIpcWireVersion);
        QNP_CHECK(decodedByCaster.generation == kWireGeneration);
        QNP_CHECK(decodedByCaster.nonce == kWireNonce);
        QNP_CHECK(decodedByCaster.sequence == sequence);
        QNP_CHECK(decodedByCaster.param0 == testCase.param0);
        QNP_CHECK(decodedByCaster.param1 == testCase.param1);
        QNP_CHECK(decodedByCaster.payload == payload);

        qgr::IpcFrameHeader decodedByRuntime{};
        std::vector<uint8_t> runtimePayload;
        QNP_REQUIRE(runtimeDecode(fromCaster, decodedByRuntime, runtimePayload)
                    == IpcFrameStatus::Ok);
        QNP_CHECK(decodedByRuntime.type == static_cast<uint8_t>(testCase.kind));
        QNP_CHECK(decodedByRuntime.channel
                  == static_cast<uint8_t>(qwire::channelForMessageType(
                         static_cast<uint8_t>(testCase.kind))));
        QNP_CHECK(decodedByRuntime.wireVersion == qgr::kReplayIpcSchemaVersion);
        QNP_CHECK(decodedByRuntime.generation == kWireGeneration);
        QNP_CHECK(decodedByRuntime.nonce == kWireNonce);
        QNP_CHECK(decodedByRuntime.sequence == sequence);
        QNP_CHECK(decodedByRuntime.param0 == testCase.param0);
        QNP_CHECK(decodedByRuntime.param1 == testCase.param1);
        QNP_CHECK(decodedByRuntime.payloadBytes == testCase.payloadBytes);
        QNP_CHECK(runtimePayload == payload);

        QNP_CHECK(
            qgr::isRecorderOutboundKind(static_cast<uint8_t>(testCase.kind))
            == (decodedByCaster.channel == ReplayIpcChannel::Recorder));
    }
}

QNP_TEST(replay_ipc_wire_payloads_round_trip_between_both_sides) {
    qwire::HelloPayload hello{};
    hello.nonce = kWireNonce;
    hello.gamePid = kWireGamePid;
    hello.wireVersion = qgr::kReplayIpcSchemaVersion;
    hello.featureBits = 0x00A5u;
    hello.manifestDigest = 0xABCDEF0123456789ull;
    std::vector<uint8_t> helloBytes;
    QNP_REQUIRE(qwire::encodeHello(hello, helloBytes));
    QNP_CHECK(helloBytes.size() == qwire::kHelloPayloadBytes);
    ReplayIpcHello decodedHello{};
    QNP_REQUIRE(
        qwire::decodeHello(helloBytes.data(), helloBytes.size(), decodedHello));
    QNP_CHECK(decodedHello.nonce == hello.nonce);
    QNP_CHECK(decodedHello.gamePid == hello.gamePid);
    QNP_CHECK(decodedHello.featureBits == hello.featureBits);
    QNP_CHECK(decodedHello.manifestDigest == hello.manifestDigest);
    poke16(helloBytes, 12u,
           static_cast<uint16_t>(qwire::kReplayIpcWireVersion + 1u));
    ReplayIpcHello untouched{};
    untouched.gamePid = 0xFEEDFACEu;
    QNP_CHECK(
        !qwire::decodeHello(helloBytes.data(), helloBytes.size(), untouched));
    QNP_CHECK(untouched.gamePid == 0xFEEDFACEu);

    qgr::BrokerSessionInfo info{};
    info.manifestDigest = 0x1122334455667788ull;
    info.sessionUuidLow = 0x0101010102020202ull;
    info.sessionUuidHigh = 0x0303030304040404ull;
    info.simulationContractId = 0x00C0FFEE00C0FFEEull;
    info.contentDigest = 0x00DEC0DE00DEC0DEull;
    info.matchId = 7u;
    info.matchOrdinal = 2u;
    info.firstTimelineFrame = 120u;
    info.lastTimelineFrame = 4200u;
    info.totalFrames = 4081u;
    info.matchCount = 3u;
    info.bootstrapKind = 2u;
    info.vsRow = 4u;
    info.cpuMask = 0x02u;
    info.activeLaneMask = 0x0Fu;
    info.activeSlotMask = 0x07u;
    info.slotToSourceRoute = {{3u, 2u, 1u, 0u}};
    info.bootstrapFingerprint = 0x0BADF00D0BADF00Dull;

    std::vector<uint8_t> infoBytes;
    QNP_REQUIRE(qgr::encodeBrokerSessionInfo(info, infoBytes));
    QNP_CHECK(infoBytes.size() == qgr::kBrokerSessionInfoBytes);
    QNP_CHECK(infoBytes.size() == 88u);
    ReplayIpcSessionInfo decodedInfo{};
    QNP_REQUIRE(qwire::decodeSessionInfo(infoBytes.data(), infoBytes.size(),
                                         decodedInfo));
    QNP_CHECK(decodedInfo.manifestDigest == info.manifestDigest);
    QNP_CHECK(decodedInfo.sessionUuidLow == info.sessionUuidLow);
    QNP_CHECK(decodedInfo.sessionUuidHigh == info.sessionUuidHigh);
    QNP_CHECK(decodedInfo.simulationContractId == info.simulationContractId);
    QNP_CHECK(decodedInfo.contentDigest == info.contentDigest);
    QNP_CHECK(decodedInfo.matchId == info.matchId);
    QNP_CHECK(decodedInfo.matchOrdinal == info.matchOrdinal);
    QNP_CHECK(decodedInfo.firstTimelineFrame == info.firstTimelineFrame);
    QNP_CHECK(decodedInfo.lastTimelineFrame == info.lastTimelineFrame);
    QNP_CHECK(decodedInfo.totalFrames == info.totalFrames);
    QNP_CHECK(decodedInfo.matchCount == info.matchCount);
    QNP_CHECK(decodedInfo.bootstrapKind == info.bootstrapKind);
    QNP_CHECK(decodedInfo.vsRow == info.vsRow);
    QNP_CHECK(decodedInfo.cpuMask == info.cpuMask);
    QNP_CHECK(decodedInfo.activeLaneMask == info.activeLaneMask);
    QNP_CHECK(decodedInfo.activeSlotMask == info.activeSlotMask);
    QNP_CHECK(decodedInfo.slotToSourceRoute == info.slotToSourceRoute);
    QNP_CHECK(decodedInfo.bootstrapFingerprint == info.bootstrapFingerprint);

    std::vector<uint8_t> hostile = infoBytes;
    hostile[64] = 9u;
    ReplayIpcSessionInfo keep{};
    keep.matchId = 0x5A5A5A5Au;
    QNP_CHECK(!qwire::decodeSessionInfo(hostile.data(), hostile.size(), keep));
    QNP_CHECK(keep.matchId == 0x5A5A5A5Au);
    hostile = infoBytes;
    hostile[67] = 0u;
    QNP_CHECK(!qwire::decodeSessionInfo(hostile.data(), hostile.size(), keep));
    hostile = infoBytes;
    hostile[69] = 1u;
    QNP_CHECK(!qwire::decodeSessionInfo(hostile.data(), hostile.size(), keep));

    qwire::TerminalPayload terminal{};
    terminal.reason =
        static_cast<uint16_t>(qreplay::TerminalReason::RecorderOverflow);
    terminal.lastTimelineFrame = 4200u;
    terminal.postTailStateHash = 0xDEADBEEFCAFEBABEull;
    terminal.rollingDigest = 0x0123456789ABCDEFull;
    std::vector<uint8_t> terminalBytes;
    QNP_REQUIRE(qwire::encodeTerminal(terminal, terminalBytes));
    QNP_CHECK(terminalBytes.size() == 24u);
    ReplayIpcTerminal decodedTerminal{};
    QNP_REQUIRE(qwire::decodeTerminal(terminalBytes.data(),
                                      terminalBytes.size(), decodedTerminal));
    QNP_CHECK(decodedTerminal.reason == terminal.reason);
    QNP_CHECK(decodedTerminal.lastTimelineFrame == terminal.lastTimelineFrame);
    QNP_CHECK(decodedTerminal.postTailStateHash == terminal.postTailStateHash);
    QNP_CHECK(decodedTerminal.rollingDigest == terminal.rollingDigest);
    terminalBytes[0] = 0xFFu;
    QNP_CHECK(!qwire::decodeTerminal(terminalBytes.data(), terminalBytes.size(),
                                     decodedTerminal));
}

QNP_TEST(replay_ipc_wire_rejects_every_way_a_peer_can_lie) {
    const std::vector<uint8_t> payload = wirePayload(96u, 0x40u);
    const std::vector<uint8_t> good =
        runtimeEncode(qgr::IpcMessageKind::RecorderFrames, kWireGeneration,
                      kWireNonce, 1u, 0u, 1u, payload);

    ReplayIpcFrame casterOut{};
    casterOut.sequence = 0xA5A5A5A5u;
    casterOut.payload.assign(3u, 0x7Eu);
    qgr::IpcFrameHeader runtimeOut{};
    runtimeOut.sequence = 0xA5A5A5A5u;

    auto expectBoth = [&](const std::vector<uint8_t>& bytes,
                          IpcFrameStatus expected) {
        ReplayIpcFrame frame = casterOut;
        size_t consumed = 0xDEADu;
        QNP_CHECK(decodeIpcFrame(bytes.data(), bytes.size(), frame, consumed)
                  == expected);
        QNP_CHECK(frame.sequence == 0xA5A5A5A5u);
        QNP_CHECK(frame.payload.size() == 3u);
        QNP_CHECK(consumed == 0xDEADu);

        qgr::IpcFrameHeader header = runtimeOut;
        std::vector<uint8_t> body;
        QNP_CHECK(runtimeDecode(bytes, header, body) == expected);
        QNP_CHECK(header.sequence == 0xA5A5A5A5u);
        QNP_CHECK(body.empty());
    };

    {
        ReplayIpcFrame frame{};
        size_t consumed = 0u;
        QNP_REQUIRE(decodeIpcFrame(good.data(), good.size(), frame, consumed)
                    == IpcFrameStatus::Ok);
        qgr::IpcFrameHeader header{};
        std::vector<uint8_t> body;
        QNP_REQUIRE(runtimeDecode(good, header, body) == IpcFrameStatus::Ok);
    }

    expectBoth(std::vector<uint8_t>(good.begin(), good.begin() + 43),
               IpcFrameStatus::NeedMore);
    expectBoth(std::vector<uint8_t>(), IpcFrameStatus::NeedMore);

    expectBoth(std::vector<uint8_t>(good.begin(), good.end() - 1),
               IpcFrameStatus::NeedMore);

    {
        std::vector<uint8_t> bytes = good;
        bytes[20] ^= 0x01u;
        expectBoth(bytes, IpcFrameStatus::BadHeaderCrc);
        bytes = good;
        bytes[kOffHeaderCrc] ^= 0x80u;
        expectBoth(bytes, IpcFrameStatus::BadHeaderCrc);
    }

    {
        std::vector<uint8_t> bytes = good;
        bytes[qwire::kReplayIpcHeaderBytes + 5u] ^= 0x10u;
        expectBoth(bytes, IpcFrameStatus::BadPayloadCrc);
    }

    {
        std::vector<uint8_t> bytes = good;
        poke32(bytes, kOffPayloadBytes, qwire::kReplayIpcMaxPayloadBytes + 1u);
        restampHeaderCrc(bytes);
        expectBoth(bytes, IpcFrameStatus::TooLarge);
        poke32(bytes, kOffPayloadBytes, 0xFFFFFFFFu);
        restampHeaderCrc(bytes);
        expectBoth(bytes, IpcFrameStatus::TooLarge);
    }

    {
        std::vector<uint8_t> bytes = good;
        poke32(bytes, kOffMagic, 0x52504251u);
        restampHeaderCrc(bytes);
        expectBoth(bytes, IpcFrameStatus::BadMagic);
    }

    {
        std::vector<uint8_t> bytes = good;
        poke16(bytes, kOffWireVersion,
               static_cast<uint16_t>(qwire::kReplayIpcWireVersion + 1u));
        restampHeaderCrc(bytes);
        expectBoth(bytes, IpcFrameStatus::BadWireVersion);
    }

    {
        std::vector<uint8_t> bytes = good;
        bytes[kOffChannel] = 7u;
        restampHeaderCrc(bytes);
        expectBoth(bytes, IpcFrameStatus::UnknownChannel);

        bytes = good;
        bytes[kOffChannel] = static_cast<uint8_t>(ReplayIpcChannel::Broker);
        restampHeaderCrc(bytes);
        expectBoth(bytes, IpcFrameStatus::UnknownChannel);
    }

    {
        std::vector<uint8_t> bytes = good;
        bytes[kOffType] = 200u;
        restampHeaderCrc(bytes);
        expectBoth(bytes, IpcFrameStatus::UnknownType);
        bytes[kOffType] = 0u;
        restampHeaderCrc(bytes);
        expectBoth(bytes, IpcFrameStatus::UnknownType);
    }

    {
        std::vector<uint8_t> bytes = good;
        bytes.insert(bytes.end(), good.begin(), good.end());
        ReplayIpcFrame frame{};
        size_t consumed = 0u;
        QNP_REQUIRE(decodeIpcFrame(bytes.data(), bytes.size(), frame, consumed)
                    == IpcFrameStatus::Ok);
        QNP_CHECK(consumed == good.size());
    }
}

QNP_TEST(replay_ipc_recorder_host_refuses_a_stale_or_forged_worker) {
    RecorderHostConfig config{};
    config.gamePid = kWireGamePid;
    config.generation = kWireGeneration;
    config.nonce = kWireNonce;

    qwire::HelloPayload hello{};
    hello.nonce = kWireNonce;
    hello.gamePid = kWireGamePid;
    hello.wireVersion = qgr::kReplayIpcSchemaVersion;
    std::vector<uint8_t> helloBytes;
    QNP_REQUIRE(qwire::encodeHello(hello, helloBytes));

    auto runtimeHello = [&](uint32_t generation, uint64_t nonce,
                            uint32_t sequence,
                            const std::vector<uint8_t>& body) {
        const std::vector<uint8_t> bytes =
            runtimeEncode(qgr::IpcMessageKind::RecorderHello, generation, nonce,
                          sequence, 1u, generation, body);
        ReplayIpcFrame frame{};
        size_t consumed = 0u;
        QNP_CHECK(decodeIpcFrame(bytes.data(), bytes.size(), frame, consumed)
                  == IpcFrameStatus::Ok);
        return frame;
    };

    {
        ReplayRecorderHost host;
        host.setConfigForTesting(config);
        QNP_CHECK(!host.feedFrameForTesting(
            runtimeHello(kWireGeneration + 1u, kWireNonce, 1u, helloBytes)));
        QNP_CHECK(host.status().reason == RecorderDisableReason::ProtocolError);
    }
    {
        ReplayRecorderHost host;
        host.setConfigForTesting(config);
        QNP_CHECK(!host.feedFrameForTesting(
            runtimeHello(kWireGeneration, kWireNonce ^ 1ull, 1u, helloBytes)));
        QNP_CHECK(host.status().reason == RecorderDisableReason::ProtocolError);
    }
    {
        ReplayRecorderHost host;
        host.setConfigForTesting(config);
        QNP_CHECK(!host.feedFrameForTesting(
            runtimeHello(kWireGeneration, kWireNonce, 2u, helloBytes)));
        QNP_CHECK(host.status().reason == RecorderDisableReason::ProtocolError);
    }
    {
        qwire::HelloPayload wrongPid = hello;
        wrongPid.gamePid = kWireGamePid + 1u;
        std::vector<uint8_t> wrongPidBytes;
        QNP_REQUIRE(qwire::encodeHello(wrongPid, wrongPidBytes));
        ReplayRecorderHost host;
        host.setConfigForTesting(config);
        QNP_CHECK(!host.feedFrameForTesting(
            runtimeHello(kWireGeneration, kWireNonce, 1u, wrongPidBytes)));
        QNP_CHECK(host.status().reason
                  == RecorderDisableReason::HandshakeRejected);
    }
    {
        ReplayRecorderHost host;
        host.setConfigForTesting(config);
        QNP_CHECK(host.feedFrameForTesting(
            runtimeHello(kWireGeneration, kWireNonce, 1u, helloBytes)));
        QNP_CHECK(host.status().reason == RecorderDisableReason::None);
    }
}

namespace {

struct RecordedSession {
    ReplayPrefix prefix{};
    SessionManifest manifest{};
    DeterministicStart start{};
    MatchStartRecord match{};
    std::vector<ReplayFrameV1> firstBlock;
    std::vector<ReplayFrameV1> secondBlock;
    std::vector<ReplayEvent> events;
    MatchEndRecord matchEnd{};
    uint64_t finalDigest = 0u;
    uint32_t lastFrame = 0u;
    uint32_t frameCount = 0u;
};

RecordedSession makeRecordedSession(const FixtureOptions& options) {
    RecordedSession session{};
    session.prefix.containerMajor = kContainerVersionMajor;
    session.prefix.containerMinor = kContainerVersionMinor;
    session.prefix.minimumReaderMajor = kContainerReaderMajor;
    session.prefix.prefixBytes = static_cast<uint16_t>(kPrefixBytes);
    session.prefix.sessionUuidLow = options.uuidLow;
    session.prefix.sessionUuidHigh = options.uuidHigh;
    session.manifest = makeManifest(options);
    session.start = makeStart(options, options.startFrame);

    session.match.matchId = 1u;
    session.match.ordinal = 1u;
    session.match.firstTimelineFrame = options.startFrame;
    session.match.independentBootstrap = true;
    session.match.start = session.start;
    session.match.slots = session.manifest.slots;
    session.match.slotToSourceRoute = session.manifest.slotToSourceRoute;
    session.match.vsRow = session.manifest.vsRow;
    session.match.cpuMask = session.manifest.cpuMask;
    session.match.rosterEpoch = session.manifest.rosterEpoch;

    uint64_t digest = options.initialDigest;
    uint32_t frame = options.startFrame;
    for (uint32_t i = 0; i < 8u; ++i)
        session.firstBlock.push_back(makeFrame(frame++, 0x03u, digest));
    for (uint32_t i = 0; i < 4u; ++i)
        session.secondBlock.push_back(makeFrame(frame++, 0x03u, digest));
    session.lastFrame = frame - 1u;
    session.frameCount = static_cast<uint32_t>(session.firstBlock.size()
                                               + session.secondBlock.size());

    for (uint32_t round = 0; round < 2u; ++round) {
        ReplayEvent ended{};
        ended.timelineFrame = session.lastFrame;
        ended.kind = EventKind::RoundEnded;
        ended.matchId = session.match.matchId;
        for (size_t slot = 0; slot < kFighterSlots; ++slot)
            ended.roster[slot].nativeCharacterId = kInactiveCharacterId;
        ended.valueA = (round % 2u) == 0u ? 0x05u : 0x0Au;
        ended.valueC = round + 1u;
        ended.stateProof = 0xB000000000000000ull + session.lastFrame;
        digest = advanceReplayDigest(digest, ended);
        ended.rollingDigest = digest;
        session.events.push_back(ended);
    }

    session.matchEnd.matchId = session.match.matchId;
    session.matchEnd.lastTimelineFrame = session.lastFrame;
    session.matchEnd.winningSideMask = 0x05u;
    session.matchEnd.roundsSideZero = 1u;
    session.matchEnd.roundsSideOne = 1u;
    session.matchEnd.reason = TerminalReason::Completed;
    session.matchEnd.postTailStateHash =
        0xA000000000000000ull + session.lastFrame + 1u;
    session.matchEnd.rollingDigest = digest;
    session.finalDigest = digest;
    return session;
}

bool feedRecorder(ReplayRecorderHost& host, uint32_t& sequence,
                  qgr::IpcMessageKind kind, uint32_t param0, uint32_t param1,
                  const std::vector<uint8_t>& payload) {
    const std::vector<uint8_t> bytes =
        runtimeEncode(kind, kWireGeneration, kWireNonce, ++sequence, param0,
                      param1, payload);
    ReplayIpcFrame frame{};
    size_t consumed = 0u;
    if (decodeIpcFrame(bytes.data(), bytes.size(), frame, consumed)
        != IpcFrameStatus::Ok) {
        return false;
    }
    return host.feedFrameForTesting(frame);
}

std::vector<uint8_t> recorderHelloBytes() {
    qwire::HelloPayload hello{};
    hello.nonce = kWireNonce;
    hello.gamePid = kWireGamePid;
    hello.wireVersion = qgr::kReplayIpcSchemaVersion;
    std::vector<uint8_t> out;
    qwire::encodeHello(hello, out);
    return out;
}

RecorderHostConfig completeRunConfig() {
    RecorderHostConfig config{};
    config.gamePid = kWireGamePid;
    config.generation = kWireGeneration;
    config.nonce = kWireNonce;
    config.replayRoot = L"R:\\replays";
    config.nameHint = "complete run";
    config.flushEveryChunks = 4u;
    config.flushEveryMs = 0u;
    return config;
}

}

QNP_TEST(replay_recorder_host_finalizes_a_complete_file_from_the_runtime) {
    FixtureOptions options{};
    options.startFrame = 100u;
    const RecordedSession session = makeRecordedSession(options);

    std::vector<uint8_t> manifestBody;
    QNP_REQUIRE(encodeSessionManifest(session.manifest, manifestBody));
    QNP_CHECK(manifestBody.size() <= kMaxRecorderManifestBytes);
    std::vector<uint8_t> manifestPayload;
    QNP_REQUIRE(qwire::encodeRecorderManifest(session.prefix,
                                              manifestBody.data(),
                                              manifestBody.size(),
                                              manifestPayload));
    QNP_CHECK(manifestPayload.size()
              == qwire::kRecorderManifestHeaderBytes + manifestBody.size());

    MemoryFileSink* sink = new MemoryFileSink();
    std::unique_ptr<ReplayWriter> writer(new ReplayWriter());
    writer->setSink(std::unique_ptr<ReplayFileSink>(sink));

    ReplayRecorderHost host;
    host.setWriterForTesting(std::move(writer));
    host.setConfigForTesting(completeRunConfig());

    uint32_t sequence = 0u;
    QNP_REQUIRE(feedRecorder(host, sequence, qgr::IpcMessageKind::RecorderHello,
                             1u, kWireGeneration, recorderHelloBytes()));
    QNP_CHECK(host.status().partPath.empty());

    QNP_REQUIRE(feedRecorder(host, sequence,
                             qgr::IpcMessageKind::RecorderManifest, 0u, 0u,
                             manifestPayload));
    QNP_CHECK(host.status().partPath.empty());

    std::vector<uint8_t> payload;
    QNP_REQUIRE(encodeDeterministicStart(session.start, payload));
    QNP_REQUIRE(feedRecorder(host, sequence,
                             qgr::IpcMessageKind::RecorderSessionStart,
                             session.start.firstTimelineFrame, 0u, payload));

    payload.clear();
    QNP_REQUIRE(encodeMatchStart(session.match, payload));
    QNP_REQUIRE(feedRecorder(host, sequence,
                             qgr::IpcMessageKind::RecorderMatchStart,
                             session.match.matchId, 0u, payload));

    payload.clear();
    QNP_REQUIRE(encodeInputBlock(session.firstBlock.data(),
                                 session.firstBlock.size(), payload));
    QNP_REQUIRE(feedRecorder(
        host, sequence, qgr::IpcMessageKind::RecorderFrames,
        session.firstBlock.front().timelineFrame,
        static_cast<uint32_t>(session.firstBlock.size()), payload));

    payload.clear();
    QNP_REQUIRE(encodeInputBlock(session.secondBlock.data(),
                                 session.secondBlock.size(), payload));
    QNP_REQUIRE(feedRecorder(
        host, sequence, qgr::IpcMessageKind::RecorderFrames,
        session.secondBlock.front().timelineFrame,
        static_cast<uint32_t>(session.secondBlock.size()), payload));

    payload.clear();
    QNP_REQUIRE(encodeEventBlock(session.events.data(), session.events.size(),
                                 payload));
    QNP_REQUIRE(feedRecorder(host, sequence,
                             qgr::IpcMessageKind::RecorderEvents, 0u,
                             static_cast<uint32_t>(session.events.size()),
                             payload));

    payload.clear();
    QNP_REQUIRE(encodeMatchEnd(session.matchEnd, payload));
    QNP_REQUIRE(feedRecorder(host, sequence,
                             qgr::IpcMessageKind::RecorderMatchEnd,
                             session.matchEnd.matchId, 0u, payload));

    qwire::TerminalPayload terminal{};
    terminal.reason = static_cast<uint16_t>(TerminalReason::Completed);
    terminal.lastTimelineFrame = session.lastFrame;
    terminal.postTailStateHash = session.matchEnd.postTailStateHash;
    terminal.rollingDigest = session.finalDigest;
    payload.clear();
    QNP_REQUIRE(qwire::encodeTerminal(terminal, payload));
    QNP_REQUIRE(feedRecorder(host, sequence,
                             qgr::IpcMessageKind::RecorderTerminal,
                             session.lastFrame, 0u, payload));

    const RecorderStatus status = host.status();
    QNP_CHECK(status.state == RecorderState::Finalized);
    QNP_CHECK(status.reason == RecorderDisableReason::None);
    QNP_CHECK(!status.finalPath.empty());
    QNP_CHECK(status.matches == 1u);
    QNP_CHECK(status.frames == session.frameCount);
    QNP_CHECK(status.events == session.events.size());
    QNP_CHECK(status.proofDiscontinuities == 0u);
    QNP_CHECK(sink->closed());
    QNP_CHECK(!sink->renamedTo().empty());

    MemoryByteSource source(sink->data().data(), sink->data().size());
    ParseOptions parseOptions{};
    parseOptions.mode = ParseMode::Full;
    parseOptions.requireFooter = true;
    ParsedReplay parsed;
    const ParseResult result = ReplayParser::parse(source, parseOptions, parsed);
    QNP_REQUIRE(result.ok());
    QNP_CHECK(parsed.hasManifest);
    QNP_CHECK(parsed.hasSessionStart);
    QNP_REQUIRE(parsed.hasFooter);
    QNP_CHECK(parsed.fileDigestChecked);
    QNP_CHECK(parsed.fileDigestMatched);
    QNP_CHECK(parsed.file.hasSessionEnd);
    QNP_CHECK(parsed.file.sessionEnd.reason == TerminalReason::Completed);
    QNP_CHECK(parsed.file.footer.matchCount == 1u);
    QNP_CHECK(parsed.file.footer.totalFrames == session.frameCount);
    QNP_CHECK(parsed.file.footer.finalRollingDigest == session.finalDigest);
    QNP_CHECK(parsed.totalFrames == session.frameCount);
    QNP_CHECK(parsed.firstTimelineFrame == options.startFrame);
    QNP_CHECK(parsed.lastTimelineFrame == session.lastFrame);
    QNP_CHECK(parsed.file.frames.size() == session.frameCount);

    QNP_CHECK(parsed.file.prefix.sessionUuidLow == options.uuidLow);
    QNP_CHECK(parsed.file.prefix.sessionUuidHigh == options.uuidHigh);
    QNP_CHECK(parsed.file.manifest.contract.runtimeSimulationId
              == session.manifest.contract.runtimeSimulationId);
    QNP_CHECK(parsed.file.manifest.contract.executableSha256
              == session.manifest.contract.executableSha256);
    QNP_CHECK(parsed.file.manifest.bootstrapKind
              == session.manifest.bootstrapKind);
    QNP_CHECK(parsed.file.manifest.vsRow == session.manifest.vsRow);
    QNP_CHECK(parsed.file.manifest.cpuMask == session.manifest.cpuMask);
    QNP_CHECK(parsed.file.manifest.slotToSourceRoute
              == session.manifest.slotToSourceRoute);
    QNP_CHECK(parsed.file.manifest.lanes[0].active);
    QNP_CHECK(parsed.file.manifest.lanes[1].active);
    QNP_CHECK(!parsed.file.manifest.lanes[2].active);
    QNP_CHECK(!parsed.file.manifest.lanes[3].active);
    QNP_CHECK(parsed.file.manifest.slots[2].ownership
              == SlotOwnership::Inactive);
    QNP_CHECK(parsed.file.manifest.slots[3].ownership
              == SlotOwnership::Inactive);
}

QNP_TEST(replay_recorder_host_without_a_manifest_finalizes_nothing) {
    FixtureOptions options{};
    options.startFrame = 100u;
    const RecordedSession session = makeRecordedSession(options);

    MemoryFileSink* sink = new MemoryFileSink();
    std::unique_ptr<ReplayWriter> writer(new ReplayWriter());
    writer->setSink(std::unique_ptr<ReplayFileSink>(sink));

    ReplayRecorderHost host;
    host.setWriterForTesting(std::move(writer));
    host.setConfigForTesting(completeRunConfig());

    uint32_t sequence = 0u;
    QNP_REQUIRE(feedRecorder(host, sequence, qgr::IpcMessageKind::RecorderHello,
                             1u, kWireGeneration, recorderHelloBytes()));

    std::vector<uint8_t> payload;
    QNP_REQUIRE(encodeDeterministicStart(session.start, payload));
    QNP_CHECK(!feedRecorder(host, sequence,
                            qgr::IpcMessageKind::RecorderSessionStart,
                            session.start.firstTimelineFrame, 0u, payload));

    const RecorderStatus status = host.status();
    QNP_CHECK(status.state == RecorderState::Disabled);
    QNP_CHECK(status.reason == RecorderDisableReason::ProtocolError);
    QNP_CHECK(status.partPath.empty());
    QNP_CHECK(status.finalPath.empty());
    QNP_CHECK(sink->data().empty());
    QNP_CHECK(sink->createdPath().empty());
    QNP_CHECK(sink->renamedTo().empty());

    payload.clear();
    QNP_REQUIRE(encodeInputBlock(session.firstBlock.data(),
                                 session.firstBlock.size(), payload));
    QNP_CHECK(!feedRecorder(
        host, sequence, qgr::IpcMessageKind::RecorderFrames,
        session.firstBlock.front().timelineFrame,
        static_cast<uint32_t>(session.firstBlock.size()), payload));
    QNP_CHECK(host.status().state == RecorderState::Disabled);
    QNP_CHECK(host.status().finalPath.empty());
    QNP_CHECK(sink->data().empty());

    MemoryByteSource source(sink->data().data(), sink->data().size());
    ParseOptions parseOptions{};
    parseOptions.mode = ParseMode::Full;
    ParsedReplay parsed;
    QNP_CHECK(!ReplayParser::parse(source, parseOptions, parsed).ok());
    QNP_CHECK(!parsed.hasFooter);
}
