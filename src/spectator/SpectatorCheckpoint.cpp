#include "SpectatorCheckpoint.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace qnetplay::spectator {
namespace {

constexpr size_t kMagicOffset = 0;
constexpr size_t kVersionOffset = 4;
constexpr size_t kHeaderBytesOffset = 8;
constexpr size_t kFlagsOffset = 12;
constexpr size_t kSessionOffset = 16;
constexpr size_t kGameOffset = 24;
constexpr size_t kCheckpointOffset = 32;
constexpr size_t kContractOffset = 40;
constexpr size_t kBootstrapOffset = 48;
constexpr size_t kChainPrefixOffset = 56;
constexpr size_t kStateHashOffset = 64;
constexpr size_t kTimelineOffset = 72;
constexpr size_t kRecordCursorOffset = 76;
constexpr size_t kSchemaOffset = 80;
constexpr size_t kSchemaVersionOffset = 84;
constexpr size_t kRegionCountOffset = 88;
constexpr size_t kChunkBytesOffset = 92;
constexpr size_t kCompressionOffset = 96;
constexpr size_t kPayloadBytesOffset = 100;
constexpr size_t kCompatibilityIdentityOffset =
    kLegacyCheckpointHeaderBytes;

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readLe64(const uint8_t* p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(p[i]) << (i * 8);
    return value;
}

void writeLe32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = static_cast<uint8_t>(value >> (i * 8));
}

void writeLe64(uint8_t* p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>(value >> (i * 8));
}

uint32_t crcUpdate(uint32_t crc, const uint8_t* bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

uint32_t crc32(const uint8_t* bytes, size_t length) {
    return ~crcUpdate(0xFFFFFFFFu, bytes, length);
}

uint64_t avalanche(uint64_t value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

void mixByte(uint64_t& hash, uint8_t value) {
    hash ^= value;
    hash *= 0x100000001B3ull;
}

void mix32(uint64_t& hash, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        mixByte(hash, static_cast<uint8_t>(value >> (i * 8)));
}

void mix64(uint64_t& hash, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        mixByte(hash, static_cast<uint8_t>(value >> (i * 8)));
}

void writeCompatibilityIdentity(
    uint8_t* const output,
    const compat::CompatibilityIdentityWire& identity) {
    writeLe32(output, identity.schemaVersion);
    writeLe32(output + 4, identity.protocolVersion);
    std::copy_n(reinterpret_cast<const uint8_t*>(
                    identity.runtimeBuild.profileId),
                compat::kCompatibilityProfileIdBytes, output + 8);
    std::copy_n(identity.runtimeBuild.executableSha256,
                compat::kCompatibilitySha256Bytes, output + 40);
    writeLe64(output + 72, identity.runtimeBuild.executableSize);
    writeLe32(output + 80, identity.runtimeBuild.peTimestamp);
    writeLe32(output + 84,
              identity.simulationContent.manifestSchemaVersion);
    writeLe32(output + 88, identity.simulationContent.manifestFlags);
    std::copy_n(identity.simulationContent.simulationRootSha256,
                compat::kCompatibilitySha256Bytes, output + 92);
    std::copy_n(identity.simulationContent.validationRootSha256,
                compat::kCompatibilitySha256Bytes, output + 124);
    writeLe32(output + 156, identity.simulationContent.entryCount);
    writeLe32(output + 160,
              identity.simulationContent.requiredEntryCount);
    writeLe64(output + 164, identity.stateSchemaId);
    writeLe64(output + 172, identity.modSimulationId);
    writeLe64(output + 180, identity.timingProfileId);
    writeLe32(output + 188, identity.reserved);
}

bool readCompatibilityIdentity(
    const uint8_t* const input,
    compat::CompatibilityIdentityWire& output) {
    compat::CompatibilityIdentityWire decoded{};
    decoded.schemaVersion = readLe32(input);
    decoded.protocolVersion = readLe32(input + 4);
    std::copy_n(input + 8, compat::kCompatibilityProfileIdBytes,
                reinterpret_cast<uint8_t*>(decoded.runtimeBuild.profileId));
    std::copy_n(input + 40, compat::kCompatibilitySha256Bytes,
                decoded.runtimeBuild.executableSha256);
    decoded.runtimeBuild.executableSize = readLe64(input + 72);
    decoded.runtimeBuild.peTimestamp = readLe32(input + 80);
    decoded.simulationContent.manifestSchemaVersion = readLe32(input + 84);
    decoded.simulationContent.manifestFlags = readLe32(input + 88);
    std::copy_n(input + 92, compat::kCompatibilitySha256Bytes,
                decoded.simulationContent.simulationRootSha256);
    std::copy_n(input + 124, compat::kCompatibilitySha256Bytes,
                decoded.simulationContent.validationRootSha256);
    decoded.simulationContent.entryCount = readLe32(input + 156);
    decoded.simulationContent.requiredEntryCount = readLe32(input + 160);
    decoded.stateSchemaId = readLe64(input + 164);
    decoded.modSimulationId = readLe64(input + 172);
    decoded.timingProfileId = readLe64(input + 180);
    decoded.reserved = readLe32(input + 188);
    if (!compat::isValidCompatibilityIdentity(decoded)) return false;
    output = decoded;
    return true;
}

void mixCompatibilityIdentity(
    uint64_t& hash,
    const compat::CompatibilityIdentityWire& identity) {
    mix32(hash, identity.schemaVersion);
    mix32(hash, identity.protocolVersion);
    for (size_t i = 0; i < compat::kCompatibilityProfileIdBytes; ++i)
        mixByte(hash, static_cast<uint8_t>(identity.runtimeBuild.profileId[i]));
    for (uint8_t byte : identity.runtimeBuild.executableSha256)
        mixByte(hash, byte);
    mix64(hash, identity.runtimeBuild.executableSize);
    mix32(hash, identity.simulationContent.manifestSchemaVersion);
    mix32(hash, identity.simulationContent.manifestFlags);
    for (uint8_t byte : identity.simulationContent.simulationRootSha256)
        mixByte(hash, byte);
    for (uint8_t byte : identity.simulationContent.validationRootSha256)
        mixByte(hash, byte);
    mix32(hash, identity.simulationContent.entryCount);
    mix32(hash, identity.simulationContent.requiredEntryCount);
    mix64(hash, identity.stateSchemaId);
    mix64(hash, identity.modSimulationId);
    mix64(hash, identity.timingProfileId);
    mix32(hash, identity.reserved);
}

bool validCompression(uint8_t value) {
    return value == static_cast<uint8_t>(CheckpointCompression::None);
}

bool validScalars(const Checkpoint& checkpoint,
                  const bool requireCompatibilityIdentity) {
    return checkpoint.identity.sessionId != 0
        && checkpoint.identity.gameId != 0
        && checkpoint.identity.checkpointId != 0
        && checkpoint.simulationContract != 0
        && checkpoint.regionSchema != 0
        && checkpoint.chunkBytes != 0
        && checkpoint.regions.size() <= kMaxCheckpointRegions
        && (!requireCompatibilityIdentity
            || compat::isValidCompatibilityIdentity(
                checkpoint.compatibilityIdentity))
        && validCompression(
               static_cast<uint8_t>(checkpoint.compression));
}

uint64_t checkpointDigestV1(const Checkpoint& checkpoint) {
    uint64_t hash = 0xCBF29CE484222325ull;
    mix64(hash, 0x5153502D43484B31ull); // "QSP-CHK1"
    mix64(hash, checkpoint.identity.sessionId);
    mix64(hash, checkpoint.identity.gameId);
    mix64(hash, checkpoint.identity.checkpointId);
    mix32(hash, checkpoint.identity.timelineFrame);
    mix32(hash, checkpoint.identity.recordCursor);
    mix64(hash, checkpoint.simulationContract);
    mix64(hash, checkpoint.bootstrapFingerprint);
    mix32(hash, checkpoint.regionSchema);
    mix32(hash, checkpoint.regionSchemaVersion);
    mix64(hash, checkpoint.digestChainPrefix);
    mix64(hash, checkpoint.stateHash);
    mixByte(hash, static_cast<uint8_t>(checkpoint.compression));
    mix32(hash, static_cast<uint32_t>(checkpoint.regions.size()));
    for (const CheckpointRegion& region : checkpoint.regions) {
        mix32(hash, region.id);
        mix32(hash, region.byteLength);
        mix64(hash, region.hash);
    }
    return avalanche(hash);
}

}

uint64_t checkpointRegionHash(uint32_t id,
                              const uint8_t* bytes,
                              size_t length) {
    uint64_t hash = 0xCBF29CE484222325ull;
    mix64(hash, 0x5153502D52474E31ull); // "QSP-RGN1"
    mix32(hash, id);
    mix64(hash, static_cast<uint64_t>(length));
    for (size_t i = 0; i < length; ++i) mixByte(hash, bytes[i]);
    return avalanche(hash);
}

uint64_t checkpointDigest(const Checkpoint& checkpoint) {
    uint64_t hash = 0xCBF29CE484222325ull;
    mix64(hash, 0x5153502D43484B32ull); // "QSP-CHK2"
    mix64(hash, checkpoint.identity.sessionId);
    mix64(hash, checkpoint.identity.gameId);
    mix64(hash, checkpoint.identity.checkpointId);
    mix32(hash, checkpoint.identity.timelineFrame);
    mix32(hash, checkpoint.identity.recordCursor);
    mix64(hash, checkpoint.simulationContract);
    mixCompatibilityIdentity(hash, checkpoint.compatibilityIdentity);
    mix64(hash, checkpoint.bootstrapFingerprint);
    mix32(hash, checkpoint.regionSchema);
    mix32(hash, checkpoint.regionSchemaVersion);
    mix64(hash, checkpoint.digestChainPrefix);
    mix64(hash, checkpoint.stateHash);
    mixByte(hash, static_cast<uint8_t>(checkpoint.compression));
    mix32(hash, static_cast<uint32_t>(checkpoint.regions.size()));
    for (const CheckpointRegion& region : checkpoint.regions) {
        mix32(hash, region.id);
        mix32(hash, region.byteLength);
        mix64(hash, region.hash);
    }
    return avalanche(hash);
}

uint64_t checkpointDigestV3(const Checkpoint& checkpoint) {
    uint64_t hash = 0xCBF29CE484222325ull;
    mix64(hash, 0x5153502D43484B33ull); // "QSP-CHK3"
    mix64(hash, checkpoint.identity.sessionId);
    mix64(hash, checkpoint.identity.gameId);
    mix64(hash, checkpoint.identity.checkpointId);
    mix32(hash, checkpoint.identity.timelineFrame);
    mix32(hash, checkpoint.identity.recordCursor);
    mix32(hash, checkpoint.eventCursor);
    mix64(hash, checkpoint.simulationContract);
    mixCompatibilityIdentity(hash, checkpoint.compatibilityIdentity);
    mix64(hash, checkpoint.bootstrapFingerprint);
    mix32(hash, checkpoint.regionSchema);
    mix32(hash, checkpoint.regionSchemaVersion);
    mix64(hash, checkpoint.digestChainPrefix);
    mix64(hash, checkpoint.stateHash);
    mixByte(hash, static_cast<uint8_t>(checkpoint.compression));
    mix32(hash, static_cast<uint32_t>(checkpoint.regions.size()));
    for (const CheckpointRegion& region : checkpoint.regions) {
        mix32(hash, region.id);
        mix32(hash, region.byteLength);
        mix64(hash, region.hash);
    }
    return avalanche(hash);
}

bool checkpointMatchesCompatibilityIdentity(
    const Checkpoint& checkpoint,
    const compat::CompatibilityIdentityWire& expectedIdentity) {
    return checkpoint.contentIdentityAttested
        && compat::isValidCompatibilityIdentity(expectedIdentity)
        && compat::semanticallyEqual(
            checkpoint.compatibilityIdentity, expectedIdentity);
}

size_t checkpointEncodedSize(const Checkpoint& checkpoint) {
    if (!validScalars(checkpoint, true)) return 0;
    size_t payload = 0;
    for (const CheckpointRegion& region : checkpoint.regions) {
        if (region.bytes.size() > kMaxCheckpointRegionBytes) return 0;
        if (region.byteLength != region.bytes.size()) return 0;
        if (payload > kMaxCheckpointTotalBytes - region.bytes.size())
            return 0;
        payload += region.bytes.size();
    }
    if (payload > kMaxCheckpointTotalBytes) return 0;
    return kCheckpointHeaderBytesV3
        + checkpoint.regions.size() * kCheckpointRegionEntryBytes
        + payload
        + kCheckpointTrailerBytes;
}

bool encodeCheckpoint(const Checkpoint& requested,
                      std::vector<uint8_t>& out) {
    if (!validScalars(requested, true)) return false;
    const size_t total = checkpointEncodedSize(requested);
    if (total == 0) return false;

    Checkpoint canonical = requested;
    size_t payloadBytes = 0;
    for (CheckpointRegion& region : canonical.regions) {
        const uint64_t derived = checkpointRegionHash(
            region.id, region.bytes.data(), region.bytes.size());
        if (region.hash != 0 && region.hash != derived) return false;
        region.hash = derived;
        payloadBytes += region.bytes.size();
    }
    canonical.contentIdentityAttested = true;
    canonical.eventCursorPresent = true;
    const uint64_t derivedDigest = checkpointDigestV3(canonical);
    if (requested.digest != 0 && requested.digest != derivedDigest)
        return false;
    canonical.digest = derivedDigest;

    std::vector<uint8_t> encoded(total, 0);
    uint8_t* base = encoded.data();
    writeLe32(base + kMagicOffset, kCheckpointMagic);
    writeLe32(base + kVersionOffset, kCheckpointVersion3);
    writeLe32(base + kHeaderBytesOffset,
              static_cast<uint32_t>(kCheckpointHeaderBytesV3));
    writeLe32(base + kFlagsOffset, 0);
    writeLe64(base + kSessionOffset, canonical.identity.sessionId);
    writeLe64(base + kGameOffset, canonical.identity.gameId);
    writeLe64(base + kCheckpointOffset, canonical.identity.checkpointId);
    writeLe64(base + kContractOffset, canonical.simulationContract);
    writeLe64(base + kBootstrapOffset, canonical.bootstrapFingerprint);
    writeLe64(base + kChainPrefixOffset, canonical.digestChainPrefix);
    writeLe64(base + kStateHashOffset, canonical.stateHash);
    writeLe32(base + kTimelineOffset, canonical.identity.timelineFrame);
    writeLe32(base + kRecordCursorOffset, canonical.identity.recordCursor);
    writeLe32(base + kSchemaOffset, canonical.regionSchema);
    writeLe32(base + kSchemaVersionOffset, canonical.regionSchemaVersion);
    writeLe32(base + kRegionCountOffset,
              static_cast<uint32_t>(canonical.regions.size()));
    writeLe32(base + kChunkBytesOffset, canonical.chunkBytes);
    base[kCompressionOffset] =
        static_cast<uint8_t>(canonical.compression);
    writeLe32(base + kPayloadBytesOffset,
              static_cast<uint32_t>(payloadBytes));
    writeCompatibilityIdentity(base + kCompatibilityIdentityOffset,
                               canonical.compatibilityIdentity);
    writeLe32(base + kCheckpointHeaderBytes, canonical.eventCursor);

    size_t tableOffset = kCheckpointHeaderBytesV3;
    size_t bodyOffset = kCheckpointHeaderBytesV3
        + canonical.regions.size() * kCheckpointRegionEntryBytes;
    for (const CheckpointRegion& region : canonical.regions) {
        writeLe32(base + tableOffset, region.id);
        writeLe32(base + tableOffset + 4, region.byteLength);
        writeLe64(base + tableOffset + 8, region.hash);
        tableOffset += kCheckpointRegionEntryBytes;
        std::copy(region.bytes.begin(), region.bytes.end(),
                  encoded.begin()
                      + static_cast<std::ptrdiff_t>(bodyOffset));
        bodyOffset += region.bytes.size();
    }

    const size_t trailerOffset = total - kCheckpointTrailerBytes;
    writeLe64(base + trailerOffset, canonical.digest);
    writeLe64(base + trailerOffset + 8, canonical.hostMacLow);
    writeLe64(base + trailerOffset + 16, canonical.hostMacHigh);
    writeLe32(base + trailerOffset + 24,
              crc32(base, trailerOffset + 24));
    out.swap(encoded);
    return true;
}

CheckpointError decodeCheckpoint(const uint8_t* bytes,
                                 size_t length,
                                 Checkpoint& out) {
    if (!bytes) return CheckpointError::InvalidArgument;
    if (length < kLegacyCheckpointHeaderBytes + kCheckpointTrailerBytes)
        return CheckpointError::TooShort;
    if (length > kMaxCheckpointTotalBytes
            + kCheckpointHeaderBytes
            + kMaxCheckpointRegions * kCheckpointRegionEntryBytes
            + kCheckpointTrailerBytes) {
        return CheckpointError::TooLarge;
    }
    if (readLe32(bytes + kMagicOffset) != kCheckpointMagic)
        return CheckpointError::BadMagic;
    const uint32_t version = readLe32(bytes + kVersionOffset);
    size_t headerBytes = 0;
    bool contentIdentityAttested = false;
    bool eventCursorPresent = false;
    if (version == kLegacyCheckpointVersion) {
        headerBytes = kLegacyCheckpointHeaderBytes;
    } else if (version == kCheckpointVersion) {
        headerBytes = kCheckpointHeaderBytes;
        contentIdentityAttested = true;
    } else if (version == kCheckpointVersion3) {
        headerBytes = kCheckpointHeaderBytesV3;
        contentIdentityAttested = true;
        eventCursorPresent = true;
    } else {
        return CheckpointError::BadVersion;
    }
    if (length < headerBytes + kCheckpointTrailerBytes)
        return CheckpointError::TooShort;
    if (readLe32(bytes + kHeaderBytesOffset)
            != static_cast<uint32_t>(headerBytes)
        || readLe32(bytes + kFlagsOffset) != 0
        || bytes[kCompressionOffset + 1] != 0
        || bytes[kCompressionOffset + 2] != 0
        || bytes[kCompressionOffset + 3] != 0) {
        return CheckpointError::BadHeader;
    }
    if (!validCompression(bytes[kCompressionOffset]))
        return CheckpointError::BadCompression;

    const uint32_t regionCount = readLe32(bytes + kRegionCountOffset);
    if (regionCount == 0 || regionCount > kMaxCheckpointRegions)
        return CheckpointError::RegionLimit;
    const uint32_t chunkBytes = readLe32(bytes + kChunkBytesOffset);
    if (chunkBytes == 0) return CheckpointError::ChunkLimit;
    const uint32_t payloadBytes = readLe32(bytes + kPayloadBytesOffset);
    if (payloadBytes > kMaxCheckpointTotalBytes)
        return CheckpointError::TotalTooLarge;

    const size_t tableBytes =
        static_cast<size_t>(regionCount) * kCheckpointRegionEntryBytes;
    if (tableBytes > length) return CheckpointError::Truncated;
    size_t expected = headerBytes;
    if (expected > (std::numeric_limits<size_t>::max)() - tableBytes)
        return CheckpointError::TotalTooLarge;
    expected += tableBytes;
    if (expected > (std::numeric_limits<size_t>::max)() - payloadBytes)
        return CheckpointError::TotalTooLarge;
    expected += payloadBytes;
    if (expected
        > (std::numeric_limits<size_t>::max)() - kCheckpointTrailerBytes) {
        return CheckpointError::TotalTooLarge;
    }
    expected += kCheckpointTrailerBytes;
    if (length != expected) return CheckpointError::Truncated;

    size_t sum = 0;
    for (uint32_t i = 0; i < regionCount; ++i) {
        const uint8_t* entry = bytes + headerBytes
            + static_cast<size_t>(i) * kCheckpointRegionEntryBytes;
        const uint32_t byteLength = readLe32(entry + 4);
        if (byteLength > kMaxCheckpointRegionBytes)
            return CheckpointError::RegionTooLarge;
        if (sum > kMaxCheckpointTotalBytes - byteLength)
            return CheckpointError::TotalTooLarge;
        sum += byteLength;
    }
    if (sum != payloadBytes) return CheckpointError::Truncated;

    const size_t trailerOffset = length - kCheckpointTrailerBytes;
    if (readLe32(bytes + trailerOffset + 24)
        != crc32(bytes, trailerOffset + 24)) {
        return CheckpointError::BadCrc;
    }

    Checkpoint decoded{};
    decoded.identity.sessionId = readLe64(bytes + kSessionOffset);
    decoded.identity.gameId = readLe64(bytes + kGameOffset);
    decoded.identity.checkpointId = readLe64(bytes + kCheckpointOffset);
    decoded.identity.timelineFrame = readLe32(bytes + kTimelineOffset);
    decoded.identity.recordCursor = readLe32(bytes + kRecordCursorOffset);
    decoded.simulationContract = readLe64(bytes + kContractOffset);
    decoded.contentIdentityAttested = contentIdentityAttested;
    if (contentIdentityAttested
        && !readCompatibilityIdentity(
            bytes + kCompatibilityIdentityOffset,
            decoded.compatibilityIdentity)) {
        return CheckpointError::BadHeader;
    }
    decoded.bootstrapFingerprint = readLe64(bytes + kBootstrapOffset);
    decoded.regionSchema = readLe32(bytes + kSchemaOffset);
    decoded.regionSchemaVersion = readLe32(bytes + kSchemaVersionOffset);
    decoded.digestChainPrefix = readLe64(bytes + kChainPrefixOffset);
    decoded.stateHash = readLe64(bytes + kStateHashOffset);
    decoded.compression =
        static_cast<CheckpointCompression>(bytes[kCompressionOffset]);
    decoded.chunkBytes = chunkBytes;
    decoded.eventCursorPresent = eventCursorPresent;
    if (eventCursorPresent)
        decoded.eventCursor = readLe32(bytes + kCheckpointHeaderBytes);
    decoded.digest = readLe64(bytes + trailerOffset);
    decoded.hostMacLow = readLe64(bytes + trailerOffset + 8);
    decoded.hostMacHigh = readLe64(bytes + trailerOffset + 16);

    decoded.regions.reserve(regionCount);
    size_t bodyOffset = headerBytes + tableBytes;
    for (uint32_t i = 0; i < regionCount; ++i) {
        const uint8_t* entry = bytes + headerBytes
            + static_cast<size_t>(i) * kCheckpointRegionEntryBytes;
        CheckpointRegion region{};
        region.id = readLe32(entry);
        region.byteLength = readLe32(entry + 4);
        region.hash = readLe64(entry + 8);
        region.bytes.assign(bytes + bodyOffset,
                            bytes + bodyOffset + region.byteLength);
        if (checkpointRegionHash(region.id, region.bytes.data(),
                                 region.bytes.size())
            != region.hash) {
            return CheckpointError::BadRegionHash;
        }
        bodyOffset += region.byteLength;
        decoded.regions.push_back(std::move(region));
    }

    if (!validScalars(decoded, contentIdentityAttested))
        return CheckpointError::BadHeader;
    const uint64_t expectedDigest = eventCursorPresent
        ? checkpointDigestV3(decoded)
        : (contentIdentityAttested ? checkpointDigest(decoded)
                                   : checkpointDigestV1(decoded));
    if (expectedDigest != decoded.digest)
        return CheckpointError::BadDigest;

    out = std::move(decoded);
    return CheckpointError::None;
}

bool planCheckpointChunks(size_t totalBytes,
                          uint32_t chunkBytes,
                          CheckpointChunkPlan& out) {
    if (totalBytes == 0 || chunkBytes == 0) return false;
    if (totalBytes > kMaxCheckpointTotalBytes) return false;
    const size_t count = (totalBytes + chunkBytes - 1u) / chunkBytes;
    if (count == 0 || count > kMaxCheckpointChunkCount) return false;
    CheckpointChunkPlan plan{};
    plan.totalBytes = static_cast<uint32_t>(totalBytes);
    plan.chunkBytes = chunkBytes;
    plan.chunkCount = static_cast<uint32_t>(count);
    out = plan;
    return true;
}

bool CheckpointReassembler::begin(uint64_t checkpointId,
                                  const CheckpointChunkPlan& plan) {
    reset();
    if (checkpointId == 0
        || plan.totalBytes == 0
        || plan.chunkBytes == 0
        || plan.chunkCount == 0
        || plan.chunkCount > kMaxCheckpointChunkCount
        || plan.totalBytes > kMaxCheckpointTotalBytes) {
        return false;
    }
    const uint64_t covered = static_cast<uint64_t>(plan.chunkCount)
        * static_cast<uint64_t>(plan.chunkBytes);
    if (covered < plan.totalBytes
        || covered - plan.totalBytes >= plan.chunkBytes) {
        return false;
    }
    _bytes.assign(plan.totalBytes, 0);
    _have.assign(plan.chunkCount, false);
    _plan = plan;
    _checkpointId = checkpointId;
    _received = 0;
    _active = true;
    return true;
}

void CheckpointReassembler::reset() {
    _active = false;
    _checkpointId = 0;
    _plan = CheckpointChunkPlan{};
    _received = 0;
    _bytes.clear();
    _have.clear();
}

bool CheckpointReassembler::complete() const {
    return _active && _received == _plan.chunkCount;
}

CheckpointError CheckpointReassembler::acceptChunk(
    uint64_t checkpointId,
    uint32_t chunkIndex,
    uint32_t byteOffset,
    const uint8_t* bytes,
    size_t length) {
    if (!_active || checkpointId != _checkpointId || !bytes)
        return CheckpointError::InvalidArgument;
    if (chunkIndex >= _plan.chunkCount) return CheckpointError::ChunkLimit;
    if (byteOffset
        != static_cast<uint32_t>(chunkIndex) * _plan.chunkBytes) {
        return CheckpointError::InvalidArgument;
    }
    const uint32_t remaining = _plan.totalBytes - byteOffset;
    const uint32_t expected = std::min(remaining, _plan.chunkBytes);
    if (length != expected) return CheckpointError::Truncated;
    if (_have[chunkIndex]) {
        if (!std::equal(bytes, bytes + length,
                        _bytes.begin()
                            + static_cast<std::ptrdiff_t>(byteOffset))) {
            return CheckpointError::BadRegionHash;
        }
        return CheckpointError::None;
    }
    std::copy(bytes, bytes + length,
              _bytes.begin() + static_cast<std::ptrdiff_t>(byteOffset));
    _have[chunkIndex] = true;
    ++_received;
    return CheckpointError::None;
}

bool CheckpointReassembler::finish(std::vector<uint8_t>& out) {
    if (!complete()) return false;
    out = _bytes;
    reset();
    return true;
}

}
