#pragma once

#include "replay/ReplayTypes.h"
#include "replay/ReplayCompatibilityIdentityV3.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qclient::replay {

std::string sanitizeForDisplay(const std::string& text, size_t maxBytes = 256u);

std::string sanitizeForDisplay(const std::wstring& text,
                               size_t maxBytes = 256u);

bool isSafeReplayPath(const std::wstring& path);

class ReplayByteSource {
public:
    virtual ~ReplayByteSource() = default;
    virtual uint64_t size() const = 0;
    virtual bool read(uint64_t offset, uint8_t* dst, size_t bytes) = 0;
};

class MemoryByteSource final : public ReplayByteSource {
public:
    MemoryByteSource() = default;
    MemoryByteSource(const uint8_t* data, size_t len);
    explicit MemoryByteSource(std::vector<uint8_t> bytes);

    uint64_t size() const override;
    bool read(uint64_t offset, uint8_t* dst, size_t bytes) override;

    uint64_t readCalls() const { return readCalls_; }
    uint64_t bytesRead() const { return bytesRead_; }

private:
    std::vector<uint8_t> owned_;
    const uint8_t* data_ = nullptr;
    size_t len_ = 0;
    uint64_t readCalls_ = 0;
    uint64_t bytesRead_ = 0;
};

struct FileIdentity {
    uint64_t volumeId = 0u;
    uint64_t fileId = 0u;
    uint64_t sizeBytes = 0u;
    int64_t modifiedUnixSeconds = 0;
    bool known = false;

    bool sameFile(const FileIdentity& other) const {
        return known && other.known && volumeId == other.volumeId
            && fileId == other.fileId;
    }
    bool unchanged(const FileIdentity& other) const {
        return sameFile(other) && sizeBytes == other.sizeBytes
            && modifiedUnixSeconds == other.modifiedUnixSeconds;
    }
};

enum class OpenError : uint8_t {
    None = 0,
    InvalidPath,
    NotFound,
    AccessDenied,
    ReparsePoint,
    TooLarge,
    IoError,
};

const char* openErrorText(OpenError error);

class FileByteSource final : public ReplayByteSource {
public:
    FileByteSource() = default;
    ~FileByteSource() override;
    FileByteSource(const FileByteSource&) = delete;
    FileByteSource& operator=(const FileByteSource&) = delete;

    bool open(const std::wstring& path);
    void close();
    bool isOpen() const { return handle_ != nullptr; }
    OpenError error() const { return error_; }
    const FileIdentity& identity() const { return identity_; }
    const std::wstring& path() const { return path_; }

    uint64_t size() const override;
    bool read(uint64_t offset, uint8_t* dst, size_t bytes) override;

private:
    void* handle_ = nullptr;
    uint64_t size_ = 0u;
    OpenError error_ = OpenError::None;
    FileIdentity identity_{};
    std::wstring path_;
};

bool queryFileIdentity(const std::wstring& path, FileIdentity& out);

struct ChunkLocation {
    uint64_t headerOffset = 0u;
    uint64_t payloadOffset = 0u;
    qreplay::ChunkHeader header{};

    uint64_t endOffset() const {
        return payloadOffset + static_cast<uint64_t>(header.payloadBytes);
    }
};

struct DecodedChunk {
    const qreplay::SessionManifest* manifest = nullptr;
    const qreplay::DeterministicStart* sessionStart = nullptr;
    const qreplay::MatchStartRecord* matchStart = nullptr;
    const qreplay::MatchEndRecord* matchEnd = nullptr;
    const qreplay::SessionEndRecord* sessionEnd = nullptr;
    const qreplay::FooterIndex* footer = nullptr;
    const std::vector<qreplay::ReplayFrameV1>* frames = nullptr;
    const std::vector<qreplay::ReplayEvent>* events = nullptr;
    bool skippedOptional = false;
};

class ReplayChunkVisitor {
public:
    virtual ~ReplayChunkVisitor() = default;

    virtual bool onChunk(const ChunkLocation& location,
                         const uint8_t* payload,
                         size_t payloadBytes,
                         const DecodedChunk& decoded) = 0;
};

struct WalkOptions {
    bool decodeRecords = true;
    bool verifyFileDigest = true;
    bool stopAfterSessionStart = false;
    size_t readChunkBytes = 64u * 1024u;
};

enum class ParseMode : uint8_t {
    Metadata = 0,
    Verify = 1,
    Full = 2,
};

struct ParseOptions {
    ParseMode mode = ParseMode::Metadata;
    uint32_t matchIdFilter = 0u;
    size_t maxRetainedFrames = 300000u;
    size_t maxRetainedEvents = 200000u;
    bool collectChunkIndex = false;
    bool requireFooter = false;
    size_t readChunkBytes = 64u * 1024u;
};

struct MatchSpan {
    uint32_t matchId = 0u;
    uint32_t ordinal = 0u;
    uint32_t firstTimelineFrame = 0u;
    uint32_t lastTimelineFrame = 0u;
    uint32_t frameCount = 0u;
    uint32_t roundCount = 0u;
    uint64_t matchStartOffset = 0u;
    uint64_t firstInputOffset = 0u;
    bool hasEnd = false;
    bool independentBootstrap = false;
    uint32_t dependsOnMatchId = 0u;
    uint8_t winningSideMask = 0u;
    qreplay::TerminalReason endReason = qreplay::TerminalReason::Completed;
    std::array<qreplay::FighterIdentityV1, qreplay::kFighterSlots> roster{};
    std::array<qreplay::SlotOwnership, qreplay::kFighterSlots> ownership{};
};

struct ParsedReplay {
    qreplay::ReplayFile file{};

    qreplay::ReplayCompatibilityIdentityEnvelopeV3 compatibilityIdentity{
        qreplay::ReplayCompatibilityIdentityStatusV3::LegacyUnattested, {}};

    bool hasManifest = false;
    bool hasSessionStart = false;
    bool hasFooter = false;

    uint32_t chunkCount = 0u;
    uint32_t skippedOptionalChunks = 0u;
    uint64_t contentBytes = 0u;

    uint32_t totalFrames = 0u;
    uint32_t firstTimelineFrame = 0u;
    uint32_t lastTimelineFrame = 0u;

    std::vector<MatchSpan> matches;
    std::vector<ChunkLocation> chunkIndex;

    bool fileDigestChecked = false;
    bool fileDigestMatched = false;
    std::array<uint8_t, 32> computedFileDigest{};

    uint64_t lastValidChunkEnd = 0u;
    uint32_t lastValidSequence = 0u;
    bool anyChunkValidated = false;

    FileIdentity identity{};

    void reset();
};

class ReplayParser {
public:
    static qreplay::ParseResult walk(ReplayByteSource& source,
                                     const WalkOptions& options,
                                     ReplayChunkVisitor& visitor);

    static qreplay::ParseResult parse(ReplayByteSource& source,
                                      const ParseOptions& options,
                                      ParsedReplay& out);

    static qreplay::ParseResult parseFile(const std::wstring& path,
                                          const ParseOptions& options,
                                          ParsedReplay& out);

    static bool findFooter(ReplayByteSource& source,
                           ChunkLocation& locationOut,
                           qreplay::FooterIndex& footerOut);

    static size_t footerSearchWindowBytes();
};

}
