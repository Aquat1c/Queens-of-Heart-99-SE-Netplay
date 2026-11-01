/* Encoding is explicit little-endian; `out` is left untouched on every failure path. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../compat/CompatibilityIdentity.h"

namespace qnetplay::spectator {

// Little-endian bytes spell "QSK1".
constexpr uint32_t kCheckpointMagic = 0x314B5351u;
constexpr uint32_t kLegacyCheckpointVersion = 1u;
constexpr uint32_t kCheckpointVersion = 2u;
// V3 chain position is (recordCursor, eventCursor); a v2 artifact decodes with
// eventCursorPresent == false. All writers emit v3.
constexpr uint32_t kCheckpointVersion3 = 3u;
constexpr size_t kLegacyCheckpointHeaderBytes = 104u;
constexpr size_t kCheckpointHeaderBytes =
    kLegacyCheckpointHeaderBytes + compat::kCompatibilityIdentityWireBytes;
constexpr size_t kCheckpointHeaderBytesV3 = kCheckpointHeaderBytes + 4u;
constexpr size_t kCheckpointRegionEntryBytes = 16u;
constexpr size_t kCheckpointTrailerBytes = 28u;

constexpr size_t kMaxCheckpointRegions = 64u;
constexpr size_t kMaxCheckpointRegionBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxCheckpointTotalBytes = 64u * 1024u * 1024u;
constexpr size_t kMaxCheckpointChunkCount = 4096u;

enum class CheckpointCompression : uint8_t {
    None = 1,
};

struct CheckpointRegion {
    uint32_t id = 0;
    uint32_t byteLength = 0;
    uint64_t hash = 0;
    std::vector<uint8_t> bytes;

    bool operator==(const CheckpointRegion& other) const {
        return id == other.id
            && byteLength == other.byteLength
            && hash == other.hash
            && bytes == other.bytes;
    }
};

struct CheckpointIdentity {
    uint64_t sessionId = 0;
    uint64_t gameId = 0;
    uint64_t checkpointId = 0;
    uint32_t timelineFrame = 0;
    uint32_t recordCursor = 0;
};

struct Checkpoint {
    CheckpointIdentity identity{};
    uint64_t simulationContract = 0;
    compat::CompatibilityIdentityWire compatibilityIdentity{};
    bool contentIdentityAttested = false;
    uint64_t bootstrapFingerprint = 0;
    uint32_t regionSchema = 0;
    uint32_t regionSchemaVersion = 0;
    uint64_t digestChainPrefix = 0;
    uint32_t eventCursor = 0;
    bool eventCursorPresent = false;
    uint64_t stateHash = 0;
    CheckpointCompression compression = CheckpointCompression::None;
    uint32_t chunkBytes = 0;
    uint64_t hostMacLow = 0;
    uint64_t hostMacHigh = 0;
    uint64_t digest = 0;
    std::vector<CheckpointRegion> regions;
};

enum class CheckpointError {
    None,
    TooShort,
    TooLarge,
    BadMagic,
    BadVersion,
    BadHeader,
    BadCompression,
    RegionLimit,
    RegionTooLarge,
    TotalTooLarge,
    ChunkLimit,
    Truncated,
    BadCrc,
    BadRegionHash,
    BadDigest,
    InvalidArgument,
};

uint64_t checkpointRegionHash(uint32_t id,
                              const uint8_t* bytes,
                              size_t length);
uint64_t checkpointDigest(const Checkpoint& checkpoint);
uint64_t checkpointDigestV3(const Checkpoint& checkpoint);

bool checkpointMatchesCompatibilityIdentity(
    const Checkpoint& checkpoint,
    const compat::CompatibilityIdentityWire& expectedIdentity);

size_t checkpointEncodedSize(const Checkpoint& checkpoint);

bool encodeCheckpoint(const Checkpoint& checkpoint,
                      std::vector<uint8_t>& out);

CheckpointError decodeCheckpoint(const uint8_t* bytes,
                                 size_t length,
                                 Checkpoint& out);

struct CheckpointChunkPlan {
    uint32_t totalBytes = 0;
    uint32_t chunkBytes = 0;
    uint32_t chunkCount = 0;
};

bool planCheckpointChunks(size_t totalBytes,
                          uint32_t chunkBytes,
                          CheckpointChunkPlan& out);

class CheckpointReassembler {
public:
    CheckpointReassembler() = default;

    bool begin(uint64_t checkpointId, const CheckpointChunkPlan& plan);

    bool active() const { return _active; }
    uint64_t checkpointId() const { return _checkpointId; }
    uint32_t receivedChunks() const { return _received; }
    bool complete() const;

    CheckpointError acceptChunk(uint64_t checkpointId,
                                uint32_t chunkIndex,
                                uint32_t byteOffset,
                                const uint8_t* bytes,
                                size_t length);

    bool finish(std::vector<uint8_t>& out);
    void reset();

private:
    bool _active = false;
    uint64_t _checkpointId = 0;
    CheckpointChunkPlan _plan{};
    uint32_t _received = 0;
    std::vector<uint8_t> _bytes;
    std::vector<bool> _have;
};

}
