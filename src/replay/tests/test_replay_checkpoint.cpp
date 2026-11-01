#include "TestHarness.h"

#include "replay/ReplayCheckpoint.h"
#include "replay/ReplayIntegrity.h"

#include <cstring>
#include <vector>

using namespace qreplay;

namespace {

constexpr size_t kSampleBytes = 620u;

constexpr size_t kOffMagic = 0u;
constexpr size_t kOffSchema = 4u;
constexpr size_t kOffReserved = 6u;
constexpr size_t kOffDecodedTotal = 180u;
constexpr size_t kOffRegionCount = 188u;
constexpr size_t kOffRelocationCount = 192u;
constexpr size_t kOffFirstRegion = 200u;

void writeU32At(std::vector<uint8_t>& blob, size_t off, uint32_t value) {
    blob[off + 0u] = static_cast<uint8_t>(value & 0xFFu);
    blob[off + 1u] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    blob[off + 2u] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    blob[off + 3u] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void writeU64At(std::vector<uint8_t>& blob, size_t off, uint64_t value) {
    writeU32At(blob, off, static_cast<uint32_t>(value & 0xFFFFFFFFull));
    writeU32At(blob, off + 4u, static_cast<uint32_t>(value >> 32));
}

void resealBlob(std::vector<uint8_t>& blob) {
    const uint32_t crc = crc32c(blob.data(), kCheckpointHeaderBytes - 4u);
    writeU32At(blob, kCheckpointHeaderBytes - 4u, crc);
    const std::array<uint8_t, 32> digest =
        sha256(blob.data(), blob.size() - kCheckpointDigestBytes);
    std::memcpy(blob.data() + blob.size() - kCheckpointDigestBytes,
                digest.data(), digest.size());
}

SimulationContract sampleContract() {
    SimulationContract contract{};
    for (size_t i = 0; i < contract.executableSha256.size(); ++i) {
        contract.executableSha256[i] = static_cast<uint8_t>(0x10u + i);
    }
    for (size_t i = 0; i < contract.contentManifestRoot.size(); ++i) {
        contract.contentManifestRoot[i] = static_cast<uint8_t>(0xA0u + i);
    }
    contract.runtimeSimulationId = 0x1122334455667788ull;
    contract.stateRegionFingerprint = 0x99AABBCCDDEEFF00ull;
    contract.stateHashProjectionId = 0x0102030405060708ull;
    contract.stateHashAlgorithmId = 0x1010101010101010ull;
    contract.bootstrapCodecVersion = 3u;
    contract.eventSchemaVersion = 5u;
    contract.characterCatalogId = 0xC0FFEEull;
    contract.fpuControlWord = 0x027Fu;
    contract.mxcsr = 0x1F80u;
    return contract;
}

ReplayCheckpoint sampleCheckpoint() {
    ReplayCheckpoint checkpoint{};
    checkpoint.schemaVersion = kCheckpointSchemaVersion;
    checkpoint.regionCatalogDigest = 0xFEEDFACECAFEBEEFull;
    checkpoint.timelineFrame = 4321u;
    checkpoint.recordCursor = 4321u;
    checkpoint.eventCursor = 9u;
    checkpoint.preStateHash = 0xDEADBEEF12345678ull;
    checkpoint.rollingDigest = 0x0F0F0F0F0F0F0F0Full;
    checkpoint.contract = sampleContract();
    checkpoint.bootstrapFingerprint = 0x5555AAAA5555AAAAull;

    CheckpointRegion extension{};
    extension.regionId = kRegionIdExtensionState;
    extension.hash = 0x1111ull;
    extension.bytes = {0xDEu, 0xADu, 0xBEu, 0xEFu};
    checkpoint.regions.push_back(extension);

    CheckpointRegion gameplay{};
    gameplay.regionId = 7u;
    gameplay.hash = 0x2222ull;
    gameplay.bytes.resize(300u);
    for (size_t i = 0; i < gameplay.bytes.size(); ++i) {
        gameplay.bytes[i] = static_cast<uint8_t>((i * 7u) & 0xFFu);
    }
    checkpoint.regions.push_back(gameplay);

    CheckpointRelocation relocation{};
    relocation.regionId = 7u;
    relocation.byteOffset = 16u;
    relocation.ownerKind = 3u;
    relocation.ownerIndex = 2u;
    relocation.targetRegionId = kRegionIdExtensionState;
    relocation.targetIndex = 1u;
    checkpoint.relocations.push_back(relocation);
    return checkpoint;
}

std::vector<uint8_t> encodeSample() {
    std::vector<uint8_t> blob;
    if (!encodeCheckpoint(sampleCheckpoint(), blob)) blob.clear();
    return blob;
}

ReplayCheckpoint twoRegionCheckpoint() {
    ReplayCheckpoint checkpoint{};
    checkpoint.regionCatalogDigest = 7u;
    CheckpointRegion a{};
    a.regionId = 5u;
    a.bytes = {1u, 2u, 3u, 4u};
    CheckpointRegion b{};
    b.regionId = 9u;
    b.bytes = {5u, 6u, 7u, 8u};
    checkpoint.regions.push_back(a);
    checkpoint.regions.push_back(b);
    return checkpoint;
}

ReplayCheckpoint sentinel() {
    ReplayCheckpoint out{};
    out.timelineFrame = 0xDEADBEEFu;
    return out;
}

bool untouched(const ReplayCheckpoint& out) {
    return out.timelineFrame == 0xDEADBEEFu && out.regions.empty()
        && out.relocations.empty();
}

}

QNP_TEST(CheckpointGoldenRoundTrip) {
    const ReplayCheckpoint original = sampleCheckpoint();
    std::vector<uint8_t> blob;
    std::array<uint8_t, 32> digest{};
    QNP_REQUIRE(encodeCheckpoint(original, blob, &digest));
    QNP_CHECK(blob.size() == kSampleBytes);

    ReplayCheckpoint decoded = sentinel();
    QNP_REQUIRE(decodeCheckpoint(blob.data(), blob.size(), decoded)
                == ParseStatus::Ok);
    QNP_CHECK(decoded.schemaVersion == kCheckpointSchemaVersion);
    QNP_CHECK(decoded.regionCatalogDigest == original.regionCatalogDigest);
    QNP_CHECK(decoded.timelineFrame == original.timelineFrame);
    QNP_CHECK(decoded.recordCursor == original.recordCursor);
    QNP_CHECK(decoded.eventCursor == original.eventCursor);
    QNP_CHECK(decoded.preStateHash == original.preStateHash);
    QNP_CHECK(decoded.rollingDigest == original.rollingDigest);
    QNP_CHECK(decoded.bootstrapFingerprint == original.bootstrapFingerprint);
    QNP_CHECK(decoded.contract.runtimeSimulationId
              == original.contract.runtimeSimulationId);
    QNP_CHECK(decoded.contract.stateRegionFingerprint
              == original.contract.stateRegionFingerprint);
    QNP_CHECK(decoded.contract.executableSha256
              == original.contract.executableSha256);
    QNP_CHECK(decoded.contract.contentManifestRoot
              == original.contract.contentManifestRoot);
    QNP_CHECK(decoded.contract.fpuControlWord == 0x027Fu);
    QNP_CHECK(decoded.contract.mxcsr == 0x1F80u);
    QNP_CHECK(decoded.wholeDigest == digest);

    QNP_REQUIRE(decoded.regions.size() == 2u);
    QNP_CHECK(decoded.regions[0].regionId == kRegionIdExtensionState);
    QNP_CHECK(decoded.regions[0].hash == 0x1111ull);
    QNP_CHECK(decoded.regions[0].bytes == original.regions[0].bytes);
    QNP_CHECK(decoded.regions[1].regionId == 7u);
    QNP_CHECK(decoded.regions[1].bytes == original.regions[1].bytes);

    QNP_REQUIRE(decoded.relocations.size() == 1u);
    QNP_CHECK(decoded.relocations[0].regionId == 7u);
    QNP_CHECK(decoded.relocations[0].byteOffset == 16u);
    QNP_CHECK(decoded.relocations[0].ownerKind == 3u);
    QNP_CHECK(decoded.relocations[0].ownerIndex == 2u);
    QNP_CHECK(decoded.relocations[0].targetRegionId == kRegionIdExtensionState);
    QNP_CHECK(decoded.relocations[0].targetIndex == 1u);

    std::vector<uint8_t> again;
    QNP_REQUIRE(encodeCheckpoint(decoded, again));
    QNP_CHECK(again == blob);
}

QNP_TEST(CheckpointEncodeAppendsAndRestoresOnFailure) {
    std::vector<uint8_t> blob = {0xAAu, 0xBBu, 0xCCu};
    QNP_REQUIRE(encodeCheckpoint(sampleCheckpoint(), blob));
    QNP_CHECK(blob.size() == 3u + kSampleBytes);
    QNP_CHECK(blob[0] == 0xAAu && blob[1] == 0xBBu && blob[2] == 0xCCu);

    ReplayCheckpoint decoded = sentinel();
    QNP_CHECK(decodeCheckpoint(blob.data() + 3, blob.size() - 3u, decoded)
              == ParseStatus::Ok);

    const size_t before = blob.size();
    ReplayCheckpoint bad = sampleCheckpoint();
    bad.schemaVersion = 2u;
    QNP_CHECK(!encodeCheckpoint(bad, blob));
    QNP_CHECK(blob.size() == before);
}

QNP_TEST(CheckpointTruncationAtEveryByteFails) {
    const std::vector<uint8_t> blob = encodeSample();
    QNP_REQUIRE(blob.size() == kSampleBytes);

    uint32_t accepted = 0u;
    for (size_t n = 0; n < blob.size(); ++n) {
        ReplayCheckpoint out = sentinel();
        const ParseStatus status = decodeCheckpoint(blob.data(), n, out);
        if (status == ParseStatus::Ok) ++accepted;
        if (!untouched(out)) ++accepted;
    }
    QNP_CHECK(accepted == 0u);
    ReplayCheckpoint out = sentinel();
    QNP_CHECK(decodeCheckpoint(nullptr, 0u, out) == ParseStatus::Truncated);
}

QNP_TEST(CheckpointDetectsCorruptionByLayer) {
    const std::vector<uint8_t> good = encodeSample();
    QNP_REQUIRE(good.size() == kSampleBytes);

    {
        std::vector<uint8_t> blob = good;
        blob[300] ^= 0x01u;
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::BadChunkPayloadCrc);
        QNP_CHECK(untouched(out));
    }
    {
        std::vector<uint8_t> blob = good;
        blob[16] ^= 0x40u;
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::BadChunkHeaderCrc);
        QNP_CHECK(untouched(out));
    }
    {
        std::vector<uint8_t> blob = good;
        blob[blob.size() - kCheckpointDigestBytes - 4u - 24u + 8u] ^= 0x10u;
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::BadChunkPayloadCrc);
    }
    {
        std::vector<uint8_t> blob = good;
        blob[blob.size() - 1u] ^= 0x80u;
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::FileDigestMismatch);
        QNP_CHECK(untouched(out));
    }
    {
        std::vector<uint8_t> blob = good;
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::Ok);
    }
}

QNP_TEST(CheckpointRejectsFramingLies) {
    const std::vector<uint8_t> good = encodeSample();
    QNP_REQUIRE(good.size() == kSampleBytes);

    {
        std::vector<uint8_t> blob = good;
        writeU32At(blob, kOffMagic, 0x12345678u);
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::BadMagic);
    }
    {
        std::vector<uint8_t> blob = good;
        blob[kOffSchema] = 2u;
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::UnsupportedContainerVersion);
        QNP_CHECK(untouched(out));
    }
    {
        std::vector<uint8_t> blob = good;
        blob[kOffSchema] = 0u;
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::UnsupportedContainerVersion);
    }
    {
        std::vector<uint8_t> blob = good;
        blob[kOffReserved] = 1u;
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::UnsupportedContainerVersion);
    }
    {
        std::vector<uint8_t> blob = good;
        blob.insert(blob.end() - kCheckpointDigestBytes, 0x55u);
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::LimitExceeded);
    }
    {
        ReplayCheckpoint bad = sampleCheckpoint();
        bad.schemaVersion = 99u;
        std::vector<uint8_t> blob;
        QNP_CHECK(!encodeCheckpoint(bad, blob));
        QNP_CHECK(blob.empty());
    }
}

QNP_TEST(CheckpointRejectsOverLimitCountsAndSizes) {
    const std::vector<uint8_t> good = encodeSample();
    QNP_REQUIRE(good.size() == kSampleBytes);

    {
        std::vector<uint8_t> blob = good;
        writeU32At(blob, kOffRegionCount, kMaxCheckpointRegions + 1u);
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::LimitExceeded);
        QNP_CHECK(untouched(out));
    }
    {
        std::vector<uint8_t> blob = good;
        writeU32At(blob, kOffRelocationCount, kMaxCheckpointRelocations + 1u);
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::LimitExceeded);
    }
    {
        std::vector<uint8_t> blob = good;
        writeU64At(blob, kOffDecodedTotal,
                   static_cast<uint64_t>(kMaxCheckpointTotalBytes) + 1ull);
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::LimitExceeded);
    }
    {
        std::vector<uint8_t> blob = good;
        writeU32At(blob, kOffFirstRegion + 4u, kMaxCheckpointRegionBytes + 1u);
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::LimitExceeded);
        QNP_CHECK(untouched(out));
    }
    {
        std::vector<uint8_t> blob = good;
        writeU64At(blob, kOffDecodedTotal, 999u);
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::LimitExceeded);
    }
    {
        std::vector<uint8_t> blob = good;
        writeU32At(blob, kOffFirstRegion + 16u, 5u);
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::LimitExceeded);
    }
}

QNP_TEST(CheckpointEncoderEnforcesLimits) {
    {
        ReplayCheckpoint many{};
        for (uint32_t i = 0; i <= kMaxCheckpointRegions; ++i) {
            CheckpointRegion region{};
            region.regionId = i + 1u;
            region.bytes.push_back(static_cast<uint8_t>(i));
            many.regions.push_back(region);
        }
        QNP_CHECK(many.regions.size() == kMaxCheckpointRegions + 1u);
        std::vector<uint8_t> blob;
        QNP_CHECK(!encodeCheckpoint(many, blob));
        QNP_CHECK(blob.empty());

        many.regions.pop_back();
        QNP_CHECK(encodeCheckpoint(many, blob));
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::Ok);
        QNP_CHECK(out.regions.size() == kMaxCheckpointRegions);
    }
    {
        ReplayCheckpoint big{};
        CheckpointRegion region{};
        region.regionId = 4u;
        region.bytes.resize(
            static_cast<size_t>(kMaxCheckpointRegionBytes) + 1u, 0u);
        big.regions.push_back(region);
        std::vector<uint8_t> blob;
        QNP_CHECK(!encodeCheckpoint(big, blob));
        QNP_CHECK(blob.empty());
    }
    {
        ReplayCheckpoint relocs{};
        CheckpointRegion region{};
        region.regionId = 4u;
        region.bytes.resize(8u, 0u);
        relocs.regions.push_back(region);
        for (uint32_t i = 0; i <= kMaxCheckpointRelocations; ++i) {
            CheckpointRelocation reloc{};
            reloc.regionId = 4u;
            reloc.byteOffset = i & 7u;
            reloc.targetRegionId = 4u;
            relocs.relocations.push_back(reloc);
        }
        std::vector<uint8_t> blob;
        QNP_CHECK(!encodeCheckpoint(relocs, blob));
        QNP_CHECK(blob.empty());
    }
}

QNP_TEST(CheckpointEnforcesRegionIdentityAndOrder) {
    {
        ReplayCheckpoint anonymous = twoRegionCheckpoint();
        anonymous.regions[0].regionId = kRegionIdNone;
        std::vector<uint8_t> blob;
        QNP_CHECK(!encodeCheckpoint(anonymous, blob));
    }
    {
        ReplayCheckpoint descending = twoRegionCheckpoint();
        descending.regions[0].regionId = 9u;
        descending.regions[1].regionId = 5u;
        std::vector<uint8_t> blob;
        QNP_CHECK(!encodeCheckpoint(descending, blob));
    }
    {
        ReplayCheckpoint duplicate = twoRegionCheckpoint();
        duplicate.regions[1].regionId = duplicate.regions[0].regionId;
        std::vector<uint8_t> blob;
        QNP_CHECK(!encodeCheckpoint(duplicate, blob));
    }

    std::vector<uint8_t> good;
    QNP_REQUIRE(encodeCheckpoint(twoRegionCheckpoint(), good));
    QNP_CHECK(good.size() == 300u);
    const size_t secondRegion = kOffFirstRegion + 20u + 8u + 4u;

    {
        std::vector<uint8_t> blob = good;
        writeU32At(blob, secondRegion, 3u);
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::SequenceRegression);
        QNP_CHECK(untouched(out));
    }
    {
        std::vector<uint8_t> blob = good;
        writeU32At(blob, secondRegion, 5u);
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::SequenceRegression);
    }
    {
        std::vector<uint8_t> blob = good;
        writeU32At(blob, kOffFirstRegion, kRegionIdNone);
        resealBlob(blob);
        ReplayCheckpoint out = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
                  == ParseStatus::BadMatchIndex);
    }
}

QNP_TEST(CheckpointRejectsDanglingRelocations) {
    {
        ReplayCheckpoint dangling = sampleCheckpoint();
        dangling.relocations[0].regionId = 12345u;
        std::vector<uint8_t> blob;
        QNP_CHECK(!encodeCheckpoint(dangling, blob));
    }
    {
        ReplayCheckpoint outside = sampleCheckpoint();
        outside.relocations[0].byteOffset = 100000u;
        std::vector<uint8_t> blob;
        QNP_CHECK(!encodeCheckpoint(outside, blob));
    }
    {
        ReplayCheckpoint anonymousTarget = sampleCheckpoint();
        anonymousTarget.relocations[0].targetRegionId = kRegionIdNone;
        std::vector<uint8_t> blob;
        QNP_CHECK(!encodeCheckpoint(anonymousTarget, blob));
    }

    std::vector<uint8_t> blob = encodeSample();
    QNP_REQUIRE(blob.size() == kSampleBytes);
    const size_t relocStart = blob.size() - kCheckpointDigestBytes - 4u - 24u;
    writeU32At(blob, relocStart, 4444u);
    resealBlob(blob);
    const uint32_t relocCrc = crc32c(blob.data() + relocStart, 24u);
    writeU32At(blob, relocStart + 24u, relocCrc);
    resealBlob(blob);
    ReplayCheckpoint out = sentinel();
    QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), out)
              == ParseStatus::BadMatchIndex);
    QNP_CHECK(untouched(out));
}

QNP_TEST(CheckpointHandlesDegenerateShapes) {
    {
        ReplayCheckpoint empty{};
        empty.timelineFrame = 7u;
        std::vector<uint8_t> blob;
        QNP_REQUIRE(encodeCheckpoint(empty, blob));
        QNP_CHECK(blob.size()
                  == kCheckpointHeaderBytes + 4u + kCheckpointDigestBytes);
        ReplayCheckpoint out = sentinel();
        QNP_REQUIRE(decodeCheckpoint(blob.data(), blob.size(), out)
                    == ParseStatus::Ok);
        QNP_CHECK(out.timelineFrame == 7u);
        QNP_CHECK(out.regions.empty());
        QNP_CHECK(out.relocations.empty());
    }
    {
        ReplayCheckpoint sparse{};
        CheckpointRegion region{};
        region.regionId = 2u;
        region.hash = 0x77ull;
        sparse.regions.push_back(region);
        std::vector<uint8_t> blob;
        QNP_REQUIRE(encodeCheckpoint(sparse, blob));
        QNP_CHECK(blob.size() == kCheckpointHeaderBytes
                                     + kCheckpointRegionHeaderBytes + 4u
                                     + kCheckpointDigestBytes);
        ReplayCheckpoint out = sentinel();
        QNP_REQUIRE(decodeCheckpoint(blob.data(), blob.size(), out)
                    == ParseStatus::Ok);
        QNP_REQUIRE(out.regions.size() == 1u);
        QNP_CHECK(out.regions[0].regionId == 2u);
        QNP_CHECK(out.regions[0].hash == 0x77ull);
        QNP_CHECK(out.regions[0].bytes.empty());
    }
    {
        ReplayCheckpoint chunked{};
        CheckpointRegion region{};
        region.regionId = 3u;
        region.bytes.resize(kCheckpointChunkBytes + 1u);
        for (size_t i = 0; i < region.bytes.size(); ++i) {
            region.bytes[i] = static_cast<uint8_t>(i & 0xFFu);
        }
        chunked.regions.push_back(region);
        QNP_CHECK(checkpointChunkCount(kCheckpointChunkBytes + 1u) == 2u);
        QNP_CHECK(checkpointChunkCount(kCheckpointChunkBytes) == 1u);
        QNP_CHECK(checkpointChunkCount(0u) == 0u);

        std::vector<uint8_t> blob;
        QNP_REQUIRE(encodeCheckpoint(chunked, blob));
        ReplayCheckpoint out = sentinel();
        QNP_REQUIRE(decodeCheckpoint(blob.data(), blob.size(), out)
                    == ParseStatus::Ok);
        QNP_REQUIRE(out.regions.size() == 1u);
        QNP_CHECK(out.regions[0].bytes == chunked.regions[0].bytes);

        blob[blob.size() - kCheckpointDigestBytes - 4u - 1u] ^= 0x01u;
        ReplayCheckpoint bad = sentinel();
        QNP_CHECK(decodeCheckpoint(blob.data(), blob.size(), bad)
                  == ParseStatus::BadChunkPayloadCrc);
    }
}
