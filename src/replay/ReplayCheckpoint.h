#pragma once

#include "ReplayTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qreplay {

constexpr uint32_t kCheckpointMagic = 0x50433951u;
constexpr uint16_t kCheckpointSchemaVersion = 1u;

constexpr size_t kCheckpointHeaderBytes = 200u;
constexpr size_t kCheckpointDigestBytes = 32u;
constexpr size_t kCheckpointRegionHeaderBytes = 20u;
constexpr size_t kCheckpointRelocationBytes = 24u;
constexpr uint32_t kCheckpointChunkBytes = 65536u;

constexpr uint32_t kMaxCheckpointRegions = 64u;
constexpr uint32_t kMaxCheckpointRegionBytes = 8u * 1024u * 1024u;
constexpr uint32_t kMaxCheckpointTotalBytes = kMaxCheckpointDecodedBytes;
constexpr uint32_t kMaxCheckpointRelocations = 4096u;

constexpr uint32_t kRegionIdNone = 0u;
constexpr uint32_t kRegionIdExtensionState = 1u;

struct CheckpointRegion {
    uint32_t regionId = kRegionIdNone;
    std::vector<uint8_t> bytes;
    uint64_t hash = 0u;
};

struct CheckpointRelocation {
    uint32_t regionId = kRegionIdNone;
    uint32_t byteOffset = 0u;
    uint32_t ownerKind = 0u;
    uint32_t ownerIndex = 0u;
    uint32_t targetRegionId = kRegionIdNone;
    uint32_t targetIndex = 0u;
};

struct ReplayCheckpoint {
    uint16_t schemaVersion = kCheckpointSchemaVersion;
    uint64_t regionCatalogDigest = 0u;
    uint32_t timelineFrame = 0u;
    uint32_t recordCursor = 0u;
    uint32_t eventCursor = 0u;
    uint64_t preStateHash = 0u;
    uint64_t rollingDigest = 0u;
    SimulationContract contract{};
    uint64_t bootstrapFingerprint = 0u;
    std::vector<CheckpointRegion> regions;
    std::vector<CheckpointRelocation> relocations;
    std::array<uint8_t, 32> wholeDigest{};
};

bool encodeCheckpoint(const ReplayCheckpoint& checkpoint,
                      std::vector<uint8_t>& out,
                      std::array<uint8_t, 32>* digestOut = nullptr);

ParseStatus decodeCheckpoint(const uint8_t* data, size_t len,
                             ReplayCheckpoint& out);

uint32_t checkpointChunkCount(uint32_t byteLength);

}
