#include "ReplayCheckpoint.h"

#include "ReplayCodec.h"
#include "ReplayIntegrity.h"

namespace qreplay {

namespace {

// Fixed contract encoding (128 bytes), written field by field: padding and ABI must never change a checkpoint's bytes.
void putContract(std::vector<uint8_t>& out, const SimulationContract& c) {
    putBytes(out, c.executableSha256.data(), c.executableSha256.size());
    putU64(out, c.runtimeSimulationId);
    putU64(out, c.stateRegionFingerprint);
    putU64(out, c.stateHashProjectionId);
    putU64(out, c.stateHashAlgorithmId);
    putU64(out, c.bootstrapCodecVersion);
    putU64(out, c.eventSchemaVersion);
    putBytes(out, c.contentManifestRoot.data(), c.contentManifestRoot.size());
    putU64(out, c.characterCatalogId);
    putU32(out, c.fpuControlWord);
    putU32(out, c.mxcsr);
}

bool getContract(const uint8_t* data, size_t len, size_t& off,
                 SimulationContract& out) {
    SimulationContract c{};
    if (!getBytes(data, len, off, c.executableSha256.data(),
                  c.executableSha256.size())) {
        return false;
    }
    if (!getU64(data, len, off, c.runtimeSimulationId)) return false;
    if (!getU64(data, len, off, c.stateRegionFingerprint)) return false;
    if (!getU64(data, len, off, c.stateHashProjectionId)) return false;
    if (!getU64(data, len, off, c.stateHashAlgorithmId)) return false;
    if (!getU64(data, len, off, c.bootstrapCodecVersion)) return false;
    if (!getU64(data, len, off, c.eventSchemaVersion)) return false;
    if (!getBytes(data, len, off, c.contentManifestRoot.data(),
                  c.contentManifestRoot.size())) {
        return false;
    }
    if (!getU64(data, len, off, c.characterCatalogId)) return false;
    if (!getU32(data, len, off, c.fpuControlWord)) return false;
    if (!getU32(data, len, off, c.mxcsr)) return false;
    out = c;
    return true;
}

constexpr size_t kHeaderCrcOffset = kCheckpointHeaderBytes - 4u;

}

uint32_t checkpointChunkCount(uint32_t byteLength) {
    if (byteLength == 0u) return 0u;
    return (byteLength + kCheckpointChunkBytes - 1u) / kCheckpointChunkBytes;
}

bool encodeCheckpoint(const ReplayCheckpoint& checkpoint,
                      std::vector<uint8_t>& out,
                      std::array<uint8_t, 32>* digestOut) {
    if (checkpoint.schemaVersion != kCheckpointSchemaVersion) return false;
    if (checkpoint.regions.size() > kMaxCheckpointRegions) return false;
    if (checkpoint.relocations.size() > kMaxCheckpointRelocations) return false;

    uint64_t decodedTotal = 0u;
    uint32_t previousId = 0u;
    for (size_t i = 0; i < checkpoint.regions.size(); ++i) {
        const CheckpointRegion& region = checkpoint.regions[i];
        if (region.regionId == kRegionIdNone) return false;
        if (i != 0u && region.regionId <= previousId) return false;
        previousId = region.regionId;
        if (region.bytes.size() > kMaxCheckpointRegionBytes) return false;
        decodedTotal += static_cast<uint64_t>(region.bytes.size());
        if (decodedTotal > kMaxCheckpointTotalBytes) return false;
    }
    for (size_t i = 0; i < checkpoint.relocations.size(); ++i) {
        const CheckpointRelocation& reloc = checkpoint.relocations[i];
        if (reloc.regionId == kRegionIdNone) return false;
        if (reloc.targetRegionId == kRegionIdNone) return false;
        bool found = false;
        for (size_t r = 0; r < checkpoint.regions.size(); ++r) {
            if (checkpoint.regions[r].regionId != reloc.regionId) continue;
            found = reloc.byteOffset < checkpoint.regions[r].bytes.size();
            break;
        }
        if (!found) return false;
    }

    const size_t base = out.size();

    std::vector<uint8_t> header;
    header.reserve(kCheckpointHeaderBytes);
    putU32(header, kCheckpointMagic);
    putU16(header, checkpoint.schemaVersion);
    putU16(header, 0u);
    putU64(header, checkpoint.regionCatalogDigest);
    putU32(header, checkpoint.timelineFrame);
    putU32(header, checkpoint.recordCursor);
    putU32(header, checkpoint.eventCursor);
    putU64(header, checkpoint.preStateHash);
    putU64(header, checkpoint.rollingDigest);
    putU64(header, checkpoint.bootstrapFingerprint);
    putContract(header, checkpoint.contract);
    putU64(header, decodedTotal);
    putU32(header, static_cast<uint32_t>(checkpoint.regions.size()));
    putU32(header, static_cast<uint32_t>(checkpoint.relocations.size()));
    if (header.size() != kHeaderCrcOffset) {
        out.resize(base);
        return false;
    }
    const uint32_t headerCrc = crc32c(header.data(), header.size());
    putU32(header, headerCrc);
    out.insert(out.end(), header.begin(), header.end());

    for (size_t i = 0; i < checkpoint.regions.size(); ++i) {
        const CheckpointRegion& region = checkpoint.regions[i];
        const uint32_t byteLength = static_cast<uint32_t>(region.bytes.size());
        const uint32_t chunks = checkpointChunkCount(byteLength);
        putU32(out, region.regionId);
        putU32(out, byteLength);
        putU64(out, region.hash);
        putU32(out, chunks);
        uint32_t written = 0u;
        for (uint32_t c = 0; c < chunks; ++c) {
            uint32_t take = byteLength - written;
            if (take > kCheckpointChunkBytes) take = kCheckpointChunkBytes;
            putU32(out, take);
            putU32(out, crc32c(region.bytes.data() + written, take));
            putBytes(out, region.bytes.data() + written, take);
            written += take;
        }
    }

    std::vector<uint8_t> relocs;
    relocs.reserve(checkpoint.relocations.size() * kCheckpointRelocationBytes);
    for (size_t i = 0; i < checkpoint.relocations.size(); ++i) {
        const CheckpointRelocation& reloc = checkpoint.relocations[i];
        putU32(relocs, reloc.regionId);
        putU32(relocs, reloc.byteOffset);
        putU32(relocs, reloc.ownerKind);
        putU32(relocs, reloc.ownerIndex);
        putU32(relocs, reloc.targetRegionId);
        putU32(relocs, reloc.targetIndex);
    }
    out.insert(out.end(), relocs.begin(), relocs.end());
    putU32(out, crc32c(relocs.data(), relocs.size()));

    const std::array<uint8_t, 32> digest =
        sha256(out.data() + base, out.size() - base);
    putBytes(out, digest.data(), digest.size());
    if (digestOut != nullptr) *digestOut = digest;
    return true;
}

ParseStatus decodeCheckpoint(const uint8_t* data, size_t len,
                             ReplayCheckpoint& out) {
    if (data == nullptr) return ParseStatus::Truncated;
    size_t minimum = 0u;
    if (!addChecked(kCheckpointHeaderBytes, kCheckpointDigestBytes, minimum)) {
        return ParseStatus::IntegerOverflow;
    }
    if (len < minimum) return ParseStatus::Truncated;
    const size_t bodyLen = len - kCheckpointDigestBytes;

    size_t off = 0u;
    uint32_t magic = 0u;
    if (!getU32(data, bodyLen, off, magic)) return ParseStatus::Truncated;
    if (magic != kCheckpointMagic) return ParseStatus::BadMagic;

    uint16_t schemaVersion = 0u;
    uint16_t reserved = 0u;
    if (!getU16(data, bodyLen, off, schemaVersion)) return ParseStatus::Truncated;
    if (!getU16(data, bodyLen, off, reserved)) return ParseStatus::Truncated;
    if (schemaVersion != kCheckpointSchemaVersion) {
        return ParseStatus::UnsupportedContainerVersion;
    }
    if (reserved != 0u) return ParseStatus::UnsupportedContainerVersion;

    ReplayCheckpoint decoded{};
    decoded.schemaVersion = schemaVersion;
    if (!getU64(data, bodyLen, off, decoded.regionCatalogDigest)) {
        return ParseStatus::Truncated;
    }
    if (!getU32(data, bodyLen, off, decoded.timelineFrame)) {
        return ParseStatus::Truncated;
    }
    if (!getU32(data, bodyLen, off, decoded.recordCursor)) {
        return ParseStatus::Truncated;
    }
    if (!getU32(data, bodyLen, off, decoded.eventCursor)) {
        return ParseStatus::Truncated;
    }
    if (!getU64(data, bodyLen, off, decoded.preStateHash)) {
        return ParseStatus::Truncated;
    }
    if (!getU64(data, bodyLen, off, decoded.rollingDigest)) {
        return ParseStatus::Truncated;
    }
    if (!getU64(data, bodyLen, off, decoded.bootstrapFingerprint)) {
        return ParseStatus::Truncated;
    }
    if (!getContract(data, bodyLen, off, decoded.contract)) {
        return ParseStatus::Truncated;
    }

    uint64_t declaredTotal = 0u;
    uint32_t regionCount = 0u;
    uint32_t relocationCount = 0u;
    uint32_t headerCrc = 0u;
    if (!getU64(data, bodyLen, off, declaredTotal)) return ParseStatus::Truncated;
    if (!getU32(data, bodyLen, off, regionCount)) return ParseStatus::Truncated;
    if (!getU32(data, bodyLen, off, relocationCount)) {
        return ParseStatus::Truncated;
    }
    if (off != kHeaderCrcOffset) return ParseStatus::Truncated;
    if (!getU32(data, bodyLen, off, headerCrc)) return ParseStatus::Truncated;
    if (crc32c(data, kHeaderCrcOffset) != headerCrc) {
        return ParseStatus::BadChunkHeaderCrc;
    }

    if (regionCount > kMaxCheckpointRegions) return ParseStatus::LimitExceeded;
    if (relocationCount > kMaxCheckpointRelocations) {
        return ParseStatus::LimitExceeded;
    }
    if (declaredTotal > kMaxCheckpointTotalBytes) {
        return ParseStatus::LimitExceeded;
    }

    decoded.regions.reserve(regionCount);
    uint64_t runningTotal = 0u;
    uint32_t previousId = 0u;
    for (uint32_t i = 0; i < regionCount; ++i) {
        uint32_t regionId = 0u;
        uint32_t byteLength = 0u;
        uint64_t hash = 0u;
        uint32_t chunkCount = 0u;
        if (!getU32(data, bodyLen, off, regionId)) return ParseStatus::Truncated;
        if (!getU32(data, bodyLen, off, byteLength)) return ParseStatus::Truncated;
        if (!getU64(data, bodyLen, off, hash)) return ParseStatus::Truncated;
        if (!getU32(data, bodyLen, off, chunkCount)) return ParseStatus::Truncated;

        if (regionId == kRegionIdNone) return ParseStatus::BadMatchIndex;
        if (i != 0u && regionId <= previousId) {
            return ParseStatus::SequenceRegression;
        }
        previousId = regionId;
        if (byteLength > kMaxCheckpointRegionBytes) {
            return ParseStatus::LimitExceeded;
        }
        runningTotal += static_cast<uint64_t>(byteLength);
        if (runningTotal > kMaxCheckpointTotalBytes) {
            return ParseStatus::LimitExceeded;
        }
        if (chunkCount != checkpointChunkCount(byteLength)) {
            return ParseStatus::LimitExceeded;
        }
        size_t framing = 0u;
        size_t required = 0u;
        if (!mulChecked(static_cast<size_t>(chunkCount), 8u, framing)) {
            return ParseStatus::IntegerOverflow;
        }
        if (!addChecked(framing, static_cast<size_t>(byteLength), required)) {
            return ParseStatus::IntegerOverflow;
        }
        size_t end = 0u;
        if (!addChecked(off, required, end)) return ParseStatus::IntegerOverflow;
        if (end > bodyLen) return ParseStatus::Truncated;

        CheckpointRegion region{};
        region.regionId = regionId;
        region.hash = hash;
        region.bytes.resize(byteLength);
        uint32_t read = 0u;
        for (uint32_t c = 0; c < chunkCount; ++c) {
            uint32_t chunkBytes = 0u;
            uint32_t chunkCrc = 0u;
            if (!getU32(data, bodyLen, off, chunkBytes)) {
                return ParseStatus::Truncated;
            }
            if (!getU32(data, bodyLen, off, chunkCrc)) {
                return ParseStatus::Truncated;
            }
            uint32_t expect = byteLength - read;
            if (expect > kCheckpointChunkBytes) expect = kCheckpointChunkBytes;
            if (chunkBytes != expect) return ParseStatus::LimitExceeded;
            if (!getBytes(data, bodyLen, off, region.bytes.data() + read,
                          chunkBytes)) {
                return ParseStatus::Truncated;
            }
            if (crc32c(region.bytes.data() + read, chunkBytes) != chunkCrc) {
                return ParseStatus::BadChunkPayloadCrc;
            }
            read += chunkBytes;
        }
        decoded.regions.push_back(std::move(region));
    }
    if (runningTotal != declaredTotal) return ParseStatus::LimitExceeded;

    size_t relocBytes = 0u;
    if (!mulChecked(static_cast<size_t>(relocationCount),
                    kCheckpointRelocationBytes, relocBytes)) {
        return ParseStatus::IntegerOverflow;
    }
    size_t relocEnd = 0u;
    if (!addChecked(off, relocBytes, relocEnd)) {
        return ParseStatus::IntegerOverflow;
    }
    if (relocEnd > bodyLen) return ParseStatus::Truncated;
    const size_t relocStart = off;
    decoded.relocations.reserve(relocationCount);
    for (uint32_t i = 0; i < relocationCount; ++i) {
        CheckpointRelocation reloc{};
        if (!getU32(data, bodyLen, off, reloc.regionId)) {
            return ParseStatus::Truncated;
        }
        if (!getU32(data, bodyLen, off, reloc.byteOffset)) {
            return ParseStatus::Truncated;
        }
        if (!getU32(data, bodyLen, off, reloc.ownerKind)) {
            return ParseStatus::Truncated;
        }
        if (!getU32(data, bodyLen, off, reloc.ownerIndex)) {
            return ParseStatus::Truncated;
        }
        if (!getU32(data, bodyLen, off, reloc.targetRegionId)) {
            return ParseStatus::Truncated;
        }
        if (!getU32(data, bodyLen, off, reloc.targetIndex)) {
            return ParseStatus::Truncated;
        }
        if (reloc.regionId == kRegionIdNone
            || reloc.targetRegionId == kRegionIdNone) {
            return ParseStatus::BadMatchIndex;
        }
        bool inside = false;
        for (size_t r = 0; r < decoded.regions.size(); ++r) {
            if (decoded.regions[r].regionId != reloc.regionId) continue;
            inside = reloc.byteOffset < decoded.regions[r].bytes.size();
            break;
        }
        if (!inside) return ParseStatus::BadMatchIndex;
        decoded.relocations.push_back(reloc);
    }
    uint32_t relocCrc = 0u;
    if (!getU32(data, bodyLen, off, relocCrc)) return ParseStatus::Truncated;
    if (crc32c(data + relocStart, relocBytes) != relocCrc) {
        return ParseStatus::BadChunkPayloadCrc;
    }

    if (off != bodyLen) return ParseStatus::LimitExceeded;

    const std::array<uint8_t, 32> computed = sha256(data, bodyLen);
    for (size_t i = 0; i < computed.size(); ++i) {
        if (computed[i] != data[bodyLen + i]) {
            return ParseStatus::FileDigestMismatch;
        }
    }
    decoded.wholeDigest = computed;

    out = std::move(decoded);
    return ParseStatus::Ok;
}

}
