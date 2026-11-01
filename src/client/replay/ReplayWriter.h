#pragma once

#include "replay/ReplayTypes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qclient::replay {

enum class WriterError : uint16_t {
    None = 0,
    AlreadyOpen,
    NotOpen,
    Finalized,
    RootUnusable,
    NameExhausted,
    CreateFailed,
    WriteFailed,
    FlushFailed,
    RenameFailed,
    DiskFull,
    AccessDenied,
    InvalidRecord,
    LimitExceeded,
    WrongOrder,
};

const char* writerErrorText(WriterError error);

class ReplayFileSink {
public:
    virtual ~ReplayFileSink() = default;

    virtual bool createNew(const std::wstring& path) = 0;
    virtual bool append(const uint8_t* data, size_t bytes) = 0;
    virtual bool flush() = 0;
    virtual bool close() = 0;
    virtual bool discard() = 0;
    virtual bool renameNoReplace(const std::wstring& from,
                                 const std::wstring& to) = 0;
    virtual uint64_t bytes() const = 0;
    virtual WriterError lastError() const = 0;
};

class Win32FileSink final : public ReplayFileSink {
public:
    Win32FileSink() = default;
    ~Win32FileSink() override;
    Win32FileSink(const Win32FileSink&) = delete;
    Win32FileSink& operator=(const Win32FileSink&) = delete;

    bool createNew(const std::wstring& path) override;
    bool append(const uint8_t* data, size_t bytes) override;
    bool flush() override;
    bool close() override;
    bool discard() override;
    bool renameNoReplace(const std::wstring& from,
                         const std::wstring& to) override;
    uint64_t bytes() const override { return bytes_; }
    WriterError lastError() const override { return error_; }

private:
    void* handle_ = nullptr;
    std::wstring path_;
    uint64_t bytes_ = 0u;
    WriterError error_ = WriterError::None;
};

class MemoryFileSink final : public ReplayFileSink {
public:
    struct Faults {
        uint64_t failAppendAfterBytes = 0u;
        WriterError appendError = WriterError::DiskFull;
        bool failCreate = false;
        WriterError createError = WriterError::AccessDenied;
        bool failFlush = false;
        bool failRename = false;
        std::vector<std::wstring> existingPaths;
    };

    Faults faults;

    bool createNew(const std::wstring& path) override;
    bool append(const uint8_t* data, size_t bytes) override;
    bool flush() override;
    bool close() override;
    bool discard() override;
    bool renameNoReplace(const std::wstring& from,
                         const std::wstring& to) override;
    uint64_t bytes() const override { return data_.size(); }
    WriterError lastError() const override { return error_; }

    const std::vector<uint8_t>& data() const { return data_; }
    const std::wstring& createdPath() const { return path_; }
    const std::wstring& renamedTo() const { return renamedTo_; }
    uint32_t flushCount() const { return flushCount_; }
    bool closed() const { return closed_; }

private:
    bool exists(const std::wstring& path) const;

    std::vector<uint8_t> data_;
    std::wstring path_;
    std::wstring renamedTo_;
    WriterError error_ = WriterError::None;
    uint32_t flushCount_ = 0u;
    bool open_ = false;
    bool closed_ = false;
};

struct WriterConfig {
    std::wstring rootDirectory;
    std::string nameHint;
    int64_t recordedUnixSeconds = 0;
    uint32_t flushEveryChunks = 64u;
    uint32_t flushEveryMs = 2000u;
    uint32_t maxNameAttempts = 64u;
};

struct WriterSummary {
    std::wstring finalPath;
    std::wstring partPath;
    uint64_t sessionUuidLow = 0u;
    uint64_t sessionUuidHigh = 0u;
    std::array<uint8_t, 32> fileDigest{};
    uint32_t chunkCount = 0u;
    uint32_t matchCount = 0u;
    uint32_t frameCount = 0u;
    uint64_t bytes = 0u;
    bool finalized = false;
};

class ReplayWriter {
public:
    ReplayWriter();
    ~ReplayWriter();
    ReplayWriter(const ReplayWriter&) = delete;
    ReplayWriter& operator=(const ReplayWriter&) = delete;

    void setSink(std::unique_ptr<ReplayFileSink> sink);

    bool begin(const WriterConfig& config,
               const qreplay::ReplayPrefix& prefix,
               const qreplay::SessionManifest& manifest);

    bool writeSessionStart(const qreplay::DeterministicStart& start);
    bool writeMatchStart(const qreplay::MatchStartRecord& record);
    bool writeInputBlock(const qreplay::ReplayFrameV1* frames, size_t count);
    bool writeEventBlock(const qreplay::ReplayEvent* events, size_t count);
    bool writeMatchEnd(const qreplay::MatchEndRecord& record);

    bool writeTerminalAndAbandon(const qreplay::SessionEndRecord& record);

    bool finalize(const qreplay::SessionEndRecord& record);

    void abandon();

    bool isOpen() const { return open_; }
    bool isFinalized() const { return finalized_; }
    WriterError error() const { return error_; }
    const char* errorText() const { return writerErrorText(error_); }

    const std::wstring& partPath() const { return partPath_; }
    const std::wstring& finalPath() const { return finalPath_; }
    const WriterSummary& summary() const { return summary_; }

    uint32_t chunkCount() const { return sequence_; }
    uint32_t frameCount() const { return frameCount_; }
    uint32_t matchCount() const { return static_cast<uint32_t>(index_.size()); }
    uint64_t bytesWritten() const { return bytesWritten_; }
    const std::vector<uint8_t>& stagedBytesForTesting() const {
        return stagedBytes_;
    }
    uint32_t flushCount() const { return flushCount_; }
    const std::vector<qreplay::MatchIndexEntry>& matchIndex() const {
        return index_;
    }
    uint32_t nextTimelineFrame() const { return nextFrame_; }

    static std::string sanitizeNameToken(const std::string& text,
                                         size_t maxChars = 48u);
    static std::wstring buildBaseName(int64_t unixSeconds,
                                      const std::string& nameHint,
                                      uint64_t sessionUuidHigh);

private:
    bool appendChunk(qreplay::ChunkKind kind,
                     uint32_t matchId,
                     uint32_t firstFrame,
                     uint32_t lastFrame,
                     uint32_t recordCount,
                     const std::vector<uint8_t>& payload);
    bool rawWrite(const uint8_t* data, size_t bytes);
    bool maybeFlush(bool force);
    bool writeFooter();
    void failWith(WriterError error);
    void closeSink();
    void discardSink();

    std::unique_ptr<ReplayFileSink> sink_;
    WriterConfig config_{};
    qreplay::ReplayPrefix prefix_{};

    std::wstring partPath_;
    std::wstring finalPath_;
    WriterSummary summary_{};
    std::vector<uint8_t> stagedBytes_;

    struct DigestState;
    std::unique_ptr<DigestState> digest_;

    std::vector<qreplay::MatchIndexEntry> index_;
    std::vector<uint32_t> openMatchIds_;

    uint32_t sequence_ = 0u;
    uint32_t frameCount_ = 0u;
    uint64_t finalRollingDigest_ = 0u;
    uint32_t nextFrame_ = 0u;
    uint32_t lastEventFrame_ = 0u;
    uint64_t bytesWritten_ = 0u;
    uint32_t chunksSinceFlush_ = 0u;
    uint32_t flushCount_ = 0u;
    uint64_t lastFlushTick_ = 0u;

    bool open_ = false;
    bool sessionStartWritten_ = false;
    bool timelineStarted_ = false;
    bool finalized_ = false;
    bool terminalWritten_ = false;
    WriterError error_ = WriterError::None;
};

}
