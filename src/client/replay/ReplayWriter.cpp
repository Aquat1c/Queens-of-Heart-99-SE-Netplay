#include "ReplayWriter.h"

#include "ReplayParser.h"
#include "platform/WidePath.h"
#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace qclient::replay {

using namespace qreplay;

namespace {

constexpr size_t kHeaderCrcFieldsOffset = 40u;

bool addChecked64(uint64_t a, uint64_t b, uint64_t& out) {
    if (a > (~static_cast<uint64_t>(0)) - b) return false;
    out = a + b;
    return true;
}

WriterError mapWin32Error(DWORD code) {
    switch (code) {
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return WriterError::DiskFull;
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT:
        case ERROR_SHARING_VIOLATION:
            return WriterError::AccessDenied;
        default:
            return WriterError::WriteFailed;
    }
}

}

const char* writerErrorText(WriterError error) {
    switch (error) {
        case WriterError::None: return "ok";
        case WriterError::AlreadyOpen: return "a recording is already open";
        case WriterError::NotOpen: return "no recording is open";
        case WriterError::Finalized: return "the recording is finalized";
        case WriterError::RootUnusable: return "replay root is not writable";
        case WriterError::NameExhausted: return "no free replay filename";
        case WriterError::CreateFailed: return "could not publish the replay file";
        case WriterError::WriteFailed: return "write failed";
        case WriterError::FlushFailed: return "flush failed";
        case WriterError::RenameFailed: return "final rename failed";
        case WriterError::DiskFull: return "disk full";
        case WriterError::AccessDenied: return "access denied";
        case WriterError::InvalidRecord: return "record rejected by the codec";
        case WriterError::LimitExceeded: return "a container limit was reached";
        case WriterError::WrongOrder: return "chunk written out of order";
    }
    return "unknown";
}

Win32FileSink::~Win32FileSink() { close(); }

bool Win32FileSink::createNew(const std::wstring& path) {
    close();
    error_ = WriterError::None;
    const std::wstring wide = qplat::longPathW(path);
    HANDLE handle = ::CreateFileW(
        wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        error_ = (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS)
            ? WriterError::CreateFailed
            : mapWin32Error(code);
        if (error_ == WriterError::WriteFailed)
            error_ = WriterError::CreateFailed;
        return false;
    }
    handle_ = handle;
    path_ = path;
    bytes_ = 0u;
    return true;
}

bool Win32FileSink::append(const uint8_t* data, size_t bytes) {
    if (handle_ == nullptr) {
        error_ = WriterError::NotOpen;
        return false;
    }
    size_t done = 0u;
    while (done < bytes) {
        const DWORD ask = static_cast<DWORD>(
            std::min<size_t>(bytes - done, 1u * 1024u * 1024u));
        DWORD wrote = 0u;
        if (::WriteFile(static_cast<HANDLE>(handle_), data + done, ask, &wrote,
                        nullptr) == 0) {
            error_ = mapWin32Error(::GetLastError());
            return false;
        }
        if (wrote == 0u) {
            error_ = WriterError::DiskFull;
            return false;
        }
        done += static_cast<size_t>(wrote);
        bytes_ += static_cast<uint64_t>(wrote);
    }
    return true;
}

bool Win32FileSink::flush() {
    if (handle_ == nullptr) {
        error_ = WriterError::NotOpen;
        return false;
    }
    if (::FlushFileBuffers(static_cast<HANDLE>(handle_)) == 0) {
        error_ = WriterError::FlushFailed;
        return false;
    }
    return true;
}

bool Win32FileSink::close() {
    if (handle_ == nullptr) return true;
    const BOOL ok = ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    if (ok == 0) {
        error_ = WriterError::FlushFailed;
        return false;
    }
    return true;
}

bool Win32FileSink::discard() {
    close();
    if (path_.empty()) return true;
    const BOOL ok = ::DeleteFileW(qplat::longPathW(path_).c_str());
    if (ok == 0 && ::GetLastError() != ERROR_FILE_NOT_FOUND) {
        error_ = mapWin32Error(::GetLastError());
        return false;
    }
    path_.clear();
    bytes_ = 0u;
    return true;
}

bool Win32FileSink::renameNoReplace(const std::wstring& from,
                                    const std::wstring& to) {
    const std::wstring wideFrom = qplat::longPathW(from);
    const std::wstring wideTo = qplat::longPathW(to);
    // No MOVEFILE_REPLACE_EXISTING: a name collision must never destroy an existing replay.
    if (::MoveFileExW(wideFrom.c_str(), wideTo.c_str(),
                      MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    error_ = WriterError::RenameFailed;
    return false;
}

bool MemoryFileSink::exists(const std::wstring& path) const {
    for (const std::wstring& existing : faults.existingPaths) {
        if (existing == path) return true;
    }
    return false;
}

bool MemoryFileSink::createNew(const std::wstring& path) {
    error_ = WriterError::None;
    if (faults.failRename) {
        error_ = WriterError::RenameFailed;
        return false;
    }
    if (faults.failCreate) {
        error_ = faults.createError;
        return false;
    }
    if (exists(path)) {
        error_ = WriterError::CreateFailed;
        return false;
    }
    data_.clear();
    path_ = path;
    renamedTo_ = path;
    open_ = true;
    closed_ = false;
    return true;
}

bool MemoryFileSink::append(const uint8_t* data, size_t bytes) {
    if (!open_) {
        error_ = WriterError::NotOpen;
        return false;
    }
    if (faults.failAppendAfterBytes != 0u
        && data_.size() + bytes > faults.failAppendAfterBytes) {
        const size_t room = faults.failAppendAfterBytes > data_.size()
            ? static_cast<size_t>(faults.failAppendAfterBytes - data_.size())
            : 0u;
        if (room != 0u && data != nullptr)
            data_.insert(data_.end(), data, data + room);
        error_ = faults.appendError;
        return false;
    }
    if (bytes != 0u && data != nullptr)
        data_.insert(data_.end(), data, data + bytes);
    return true;
}

bool MemoryFileSink::flush() {
    if (!open_) {
        error_ = WriterError::NotOpen;
        return false;
    }
    if (faults.failFlush) {
        error_ = WriterError::FlushFailed;
        return false;
    }
    ++flushCount_;
    return true;
}

bool MemoryFileSink::close() {
    open_ = false;
    closed_ = true;
    return true;
}

bool MemoryFileSink::discard() {
    open_ = false;
    closed_ = true;
    data_.clear();
    path_.clear();
    renamedTo_.clear();
    return true;
}

bool MemoryFileSink::renameNoReplace(const std::wstring&,
                                     const std::wstring& to) {
    if (faults.failRename || exists(to)) {
        error_ = WriterError::RenameFailed;
        return false;
    }
    renamedTo_ = to;
    return true;
}

struct ReplayWriter::DigestState {
    Sha256 sha;
};

std::string ReplayWriter::sanitizeNameToken(const std::string& text,
                                            size_t maxChars) {
    const std::string safe = sanitizeForDisplay(text, 512u);
    std::string out;
    bool lastWasSeparator = true;
    for (char raw : safe) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        const bool keep = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
            || (ch >= '0' && ch <= '9');
        if (keep) {
            out.push_back(static_cast<char>(ch));
            lastWasSeparator = false;
        } else if (!lastWasSeparator && out.size() + 1u < maxChars) {
            out.push_back('-');
            lastWasSeparator = true;
        }
        if (out.size() >= maxChars) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "session";
    return out;
}

std::wstring ReplayWriter::buildBaseName(int64_t unixSeconds,
                                         const std::string& nameHint,
                                         uint64_t sessionUuidHigh) {
    std::tm local{};
    const __time64_t stamp = static_cast<__time64_t>(unixSeconds);
    if (_localtime64_s(&local, &stamp) != 0) std::memset(&local, 0, sizeof(local));

    char buffer[192];
    const std::string token = sanitizeNameToken(nameHint);
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d_%02d-%02d-%02d_%s_%08X",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec, token.c_str(),
                  static_cast<unsigned>(sessionUuidHigh >> 32));
    return qplat::widen(std::string(buffer));
}

ReplayWriter::ReplayWriter() : digest_(new DigestState()) {}

ReplayWriter::~ReplayWriter() {
    if (open_) abandon();
}

void ReplayWriter::setSink(std::unique_ptr<ReplayFileSink> sink) {
    if (open_) return;
    sink_ = std::move(sink);
}

void ReplayWriter::failWith(WriterError error) {
    if (error_ == WriterError::None) error_ = error;
    closeSink();
    discardSink();
    stagedBytes_.clear();
    open_ = false;
}

void ReplayWriter::closeSink() {
    if (sink_) sink_->close();
}

void ReplayWriter::discardSink() {
    if (sink_) sink_->discard();
}

bool ReplayWriter::rawWrite(const uint8_t* data, size_t bytes) {
    if (data == nullptr || bytes == 0u) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    if (bytesWritten_ > kMaxFileBytes
        || bytes > kMaxFileBytes - bytesWritten_) {
        failWith(WriterError::LimitExceeded);
        return false;
    }
    stagedBytes_.insert(stagedBytes_.end(), data, data + bytes);
    bytesWritten_ += static_cast<uint64_t>(bytes);
    return true;
}

bool ReplayWriter::maybeFlush(bool force) {
    if (!open_) return false;
    const uint64_t now = ::GetTickCount64();
    const bool byCount = config_.flushEveryChunks != 0u
        && chunksSinceFlush_ >= config_.flushEveryChunks;
    const bool byTime = config_.flushEveryMs != 0u
        && now - lastFlushTick_ >= static_cast<uint64_t>(config_.flushEveryMs);
    if (!force && !byCount && !byTime) return true;
    ++flushCount_;
    chunksSinceFlush_ = 0u;
    lastFlushTick_ = now;
    return true;
}

bool ReplayWriter::appendChunk(ChunkKind kind, uint32_t matchId,
                               uint32_t firstFrame, uint32_t lastFrame,
                               uint32_t recordCount,
                               const std::vector<uint8_t>& payload) {
    if (!open_) {
        error_ = WriterError::NotOpen;
        return false;
    }
    if (payload.size() > kMaxChunkPayloadBytes) {
        failWith(WriterError::LimitExceeded);
        return false;
    }
    if (sequence_ >= kMaxChunksPerFile) {
        failWith(WriterError::LimitExceeded);
        return false;
    }
    uint64_t projected = 0u;
    if (!addChecked64(bytesWritten_, kChunkHeaderBytes + payload.size(),
                      projected)
        || projected > kMaxFileBytes) {
        failWith(WriterError::LimitExceeded);
        return false;
    }

    ChunkHeader header{};
    header.kind = kind;
    header.schemaVersion = 1u;
    header.critical = isCriticalChunkKind(kind) ? 1u : 0u;
    header.codec = ChunkCodec::Raw;
    header.sequence = sequence_;
    header.matchId = matchId;
    header.firstTimelineFrame = firstFrame;
    header.lastTimelineFrame = lastFrame;
    header.recordCount = recordCount;
    header.payloadBytes = static_cast<uint32_t>(payload.size());
    header.decodedBytes = static_cast<uint32_t>(payload.size());
    header.payloadCrc32c = crc32c(payload.data(), payload.size());

    std::vector<uint8_t> headerBytes;
    encodeChunkHeader(header, headerBytes);
    if (headerBytes.size() != kChunkHeaderBytes) {
        failWith(WriterError::InvalidRecord);
        return false;
    }

    ChunkHeader verify{};
    if (decodeChunkHeader(headerBytes.data(), headerBytes.size(), verify)
        != ParseStatus::Ok) {
        failWith(WriterError::InvalidRecord);
        return false;
    }

    if (!rawWrite(headerBytes.data(), headerBytes.size())) return false;
    if (!payload.empty() && !rawWrite(payload.data(), payload.size()))
        return false;

    digest_->sha.update(headerBytes.data(), headerBytes.size());
    if (!payload.empty())
        digest_->sha.update(payload.data(), payload.size());

    ++sequence_;
    ++chunksSinceFlush_;
    return true;
}

bool ReplayWriter::begin(const WriterConfig& config, const ReplayPrefix& prefix,
                         const SessionManifest& manifest) {
    if (open_ || finalized_) {
        error_ = open_ ? WriterError::AlreadyOpen : WriterError::Finalized;
        return false;
    }
    error_ = WriterError::None;
    config_ = config;
    prefix_ = prefix;
    summary_ = WriterSummary{};
    stagedBytes_.clear();
    partPath_.clear();
    finalPath_.clear();
    index_.clear();
    openMatchIds_.clear();
    digest_.reset(new DigestState());
    sequence_ = 0u;
    frameCount_ = 0u;
    nextFrame_ = 0u;
    lastEventFrame_ = 0u;
    bytesWritten_ = 0u;
    chunksSinceFlush_ = 0u;
    flushCount_ = 0u;
    sessionStartWritten_ = false;
    timelineStarted_ = false;
    terminalWritten_ = false;
    finalized_ = false;

    if (!sink_) sink_.reset(new Win32FileSink());

    if (config_.rootDirectory.empty()) {
        error_ = WriterError::RootUnusable;
        return false;
    }
    std::wstring root = qplat::absolutePathW(config_.rootDirectory);
    if (root.empty()) root = config_.rootDirectory;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();
    if (root.empty()) {
        error_ = WriterError::RootUnusable;
        return false;
    }

    const int64_t stamp = config_.recordedUnixSeconds != 0
        ? config_.recordedUnixSeconds
        : static_cast<int64_t>(::_time64(nullptr));
    const std::wstring base =
        buildBaseName(stamp, config_.nameHint, prefix.sessionUuidHigh);

    bool created = false;
    const uint32_t attempts = config_.maxNameAttempts != 0u
        ? config_.maxNameAttempts
        : 1u;
    for (uint32_t attempt = 0; attempt < attempts; ++attempt) {
        std::wstring candidate = root + L"\\" + base;
        if (attempt != 0u) {
            wchar_t suffix[16];
            _snwprintf_s(suffix, 16, _TRUNCATE, L"_%u", attempt + 1u);
            candidate += suffix;
        }
        finalPath_ = candidate + L".q99r";
        created = true;
        break;
    }
    if (!created) {
        error_ = WriterError::NameExhausted;
        return false;
    }
    open_ = true;
    lastFlushTick_ = ::GetTickCount64();

    std::vector<uint8_t> prefixBytes;
    encodePrefix(prefix_, prefixBytes);
    ReplayPrefix verifyPrefix{};
    if (prefixBytes.size() != kPrefixBytes
        || decodePrefix(prefixBytes.data(), prefixBytes.size(), verifyPrefix)
            != ParseStatus::Ok) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    if (!rawWrite(prefixBytes.data(), prefixBytes.size())) return false;
    digest_->sha.update(prefixBytes.data(), prefixBytes.size());

    std::vector<uint8_t> payload;
    if (!encodeSessionManifest(manifest, payload)) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    SessionManifest verifyManifest{};
    if (decodeSessionManifest(payload.data(), payload.size(), verifyManifest)
        != ParseStatus::Ok) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    if (!appendChunk(ChunkKind::SessionManifest, 0u, 0u, 0u, 1u, payload))
        return false;

    summary_.sessionUuidLow = prefix_.sessionUuidLow;
    summary_.sessionUuidHigh = prefix_.sessionUuidHigh;
    summary_.partPath.clear();
    summary_.finalPath.clear();
    return maybeFlush(true);
}

bool ReplayWriter::writeSessionStart(const DeterministicStart& start) {
    if (!open_) { error_ = WriterError::NotOpen; return false; }
    if (sessionStartWritten_ || sequence_ != 1u) {
        failWith(WriterError::WrongOrder);
        return false;
    }
    std::vector<uint8_t> payload;
    if (!encodeDeterministicStart(start, payload)) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    if (!appendChunk(ChunkKind::SessionStart, 0u, start.firstTimelineFrame,
                     start.firstTimelineFrame, 1u, payload)) {
        return false;
    }
    sessionStartWritten_ = true;
    timelineStarted_ = true;
    nextFrame_ = start.firstTimelineFrame;
    lastEventFrame_ = start.firstTimelineFrame;
    return maybeFlush(false);
}

bool ReplayWriter::writeMatchStart(const MatchStartRecord& record) {
    if (!open_) { error_ = WriterError::NotOpen; return false; }
    if (!sessionStartWritten_ || terminalWritten_) {
        failWith(WriterError::WrongOrder);
        return false;
    }
    if (record.firstTimelineFrame != nextFrame_) {
        failWith(WriterError::WrongOrder);
        return false;
    }
    if (index_.size() >= kMaxMatchesPerSet) {
        failWith(WriterError::LimitExceeded);
        return false;
    }
    for (const MatchIndexEntry& entry : index_) {
        if (entry.matchId == record.matchId
            || entry.ordinal >= record.ordinal) {
            failWith(WriterError::WrongOrder);
            return false;
        }
    }
    if (record.dependsOnMatchId != 0u) {
        bool found = false;
        for (const MatchIndexEntry& entry : index_) {
            if (entry.matchId == record.dependsOnMatchId) { found = true; break; }
        }
        if (!found) {
            failWith(WriterError::WrongOrder);
            return false;
        }
    }

    std::vector<uint8_t> payload;
    if (!encodeMatchStart(record, payload)) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    const uint64_t offset = bytesWritten_;
    if (!appendChunk(ChunkKind::MatchStart, record.matchId,
                     record.firstTimelineFrame, record.firstTimelineFrame, 1u,
                     payload)) {
        return false;
    }

    MatchIndexEntry entry{};
    entry.matchId = record.matchId;
    entry.ordinal = record.ordinal;
    entry.matchStartOffset = offset;
    entry.firstInputOffset = 0u;
    entry.firstTimelineFrame = record.firstTimelineFrame;
    entry.lastTimelineFrame = record.firstTimelineFrame;
    entry.frameCount = 0u;
    entry.roundCount = 0u;
    entry.winningSideMask = 0u;
    entry.independentBootstrap = record.independentBootstrap;
    for (size_t i = 0; i < kFighterSlots; ++i) {
        entry.roster[i] = record.slots[i].identity;
        entry.ownership[i] = record.slots[i].ownership;
    }
    index_.push_back(entry);
    openMatchIds_.push_back(record.matchId);
    return maybeFlush(false);
}

bool ReplayWriter::writeInputBlock(const ReplayFrameV1* frames, size_t count) {
    if (!open_) { error_ = WriterError::NotOpen; return false; }
    if (!timelineStarted_ || terminalWritten_) {
        failWith(WriterError::WrongOrder);
        return false;
    }
    if (count == 0u || count > kMaxRecordsPerInputBlock) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    if (frames == nullptr || frames[0].timelineFrame != nextFrame_) {
        failWith(WriterError::WrongOrder);
        return false;
    }
    const uint32_t lastFrame =
        frames[0].timelineFrame + static_cast<uint32_t>(count) - 1u;
    if (lastFrame > kMaxTimelineFrames) {
        failWith(WriterError::LimitExceeded);
        return false;
    }

    std::vector<uint8_t> payload;
    if (!encodeInputBlock(frames, count, payload)) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    const uint32_t matchId =
        openMatchIds_.empty() ? 0u : openMatchIds_.back();
    const uint64_t offset = bytesWritten_;
    if (!appendChunk(ChunkKind::InputBlock, matchId, frames[0].timelineFrame,
                     lastFrame, static_cast<uint32_t>(count), payload)) {
        return false;
    }
    if (matchId != 0u && !index_.empty()) {
        MatchIndexEntry& entry = index_.back();
        if (entry.firstInputOffset == 0u) entry.firstInputOffset = offset;
        entry.frameCount += static_cast<uint32_t>(count);
        entry.lastTimelineFrame = lastFrame;
    }
    frameCount_ += static_cast<uint32_t>(count);
    nextFrame_ = lastFrame + 1u;
    return maybeFlush(false);
}

bool ReplayWriter::writeEventBlock(const ReplayEvent* events, size_t count) {
    if (!open_) { error_ = WriterError::NotOpen; return false; }
    if (!timelineStarted_ || terminalWritten_) {
        failWith(WriterError::WrongOrder);
        return false;
    }
    if (count == 0u || count > kMaxEventsPerBlock || events == nullptr) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    const uint32_t head = nextFrame_;
    uint32_t first = events[0].timelineFrame;
    uint32_t last = events[0].timelineFrame;
    const uint32_t matchId = events[0].matchId;
    for (size_t i = 0; i < count; ++i) {
        if (events[i].matchId != matchId) {
            failWith(WriterError::InvalidRecord);
            return false;
        }
        if (events[i].timelineFrame > head
            || events[i].timelineFrame < lastEventFrame_) {
            failWith(WriterError::WrongOrder);
            return false;
        }
        if (i != 0u && events[i].timelineFrame < events[i - 1].timelineFrame) {
            failWith(WriterError::WrongOrder);
            return false;
        }
        first = std::min(first, events[i].timelineFrame);
        last = std::max(last, events[i].timelineFrame);
    }
    if (matchId != 0u) {
        bool known = false;
        for (const MatchIndexEntry& entry : index_) {
            if (entry.matchId == matchId) { known = true; break; }
        }
        if (!known) {
            failWith(WriterError::WrongOrder);
            return false;
        }
    }

    std::vector<uint8_t> payload;
    if (!encodeEventBlock(events, count, payload)) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    if (!appendChunk(ChunkKind::EventBlock, matchId, first, last,
                     static_cast<uint32_t>(count), payload)) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (events[i].kind == EventKind::RoundEnded && matchId != 0u) {
            for (MatchIndexEntry& entry : index_) {
                if (entry.matchId == matchId) ++entry.roundCount;
            }
        }
    }
    lastEventFrame_ = last;
    return maybeFlush(false);
}

bool ReplayWriter::writeMatchEnd(const MatchEndRecord& record) {
    if (!open_) { error_ = WriterError::NotOpen; return false; }
    if (terminalWritten_) {
        failWith(WriterError::WrongOrder);
        return false;
    }
    if (openMatchIds_.empty() || openMatchIds_.back() != record.matchId) {
        failWith(WriterError::WrongOrder);
        return false;
    }
    MatchIndexEntry* entry = nullptr;
    for (MatchIndexEntry& candidate : index_) {
        if (candidate.matchId == record.matchId) entry = &candidate;
    }
    if (entry == nullptr) {
        failWith(WriterError::WrongOrder);
        return false;
    }
    if (record.lastTimelineFrame < entry->firstTimelineFrame
        || record.lastTimelineFrame + 1u > nextFrame_) {
        failWith(WriterError::WrongOrder);
        return false;
    }

    std::vector<uint8_t> payload;
    if (!encodeMatchEnd(record, payload)) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    if (!appendChunk(ChunkKind::MatchEnd, record.matchId,
                     record.lastTimelineFrame, record.lastTimelineFrame, 1u,
                     payload)) {
        return false;
    }
    entry->lastTimelineFrame = record.lastTimelineFrame;
    entry->winningSideMask = record.winningSideMask;
    openMatchIds_.pop_back();
    return maybeFlush(true);
}

bool ReplayWriter::writeTerminalAndAbandon(const SessionEndRecord& record) {
    if (!open_) { error_ = WriterError::NotOpen; return false; }
    SessionEndRecord terminal = record;
    terminal.matchCount = static_cast<uint32_t>(index_.size());
    std::vector<uint8_t> payload;
    if (!encodeSessionEnd(terminal, payload)) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    if (!appendChunk(ChunkKind::SessionEnd, 0u, terminal.lastTimelineFrame,
                     terminal.lastTimelineFrame, 1u, payload)) {
        return false;
    }
    terminalWritten_ = true;
    const bool flushed = maybeFlush(true);
    closeSink();
    discardSink();
    stagedBytes_.clear();
    open_ = false;
    summary_.chunkCount = sequence_;
    summary_.matchCount = static_cast<uint32_t>(index_.size());
    summary_.frameCount = frameCount_;
    summary_.bytes = bytesWritten_;
    summary_.finalized = false;
    return flushed;
}

bool ReplayWriter::writeFooter() {
    FooterIndex footer{};
    footer.chunkCount = sequence_ + 1u;
    footer.matchCount = static_cast<uint32_t>(index_.size());
    footer.totalFrames = frameCount_;
    footer.finalRollingDigest = finalRollingDigest_;
    footer.matches = index_;
    footer.fileDigest.fill(0u);

    std::vector<uint8_t> draft;
    if (!encodeFooterIndex(footer, draft)) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    if (draft.size() < 32u || draft.size() > kMaxChunkPayloadBytes) {
        failWith(WriterError::LimitExceeded);
        return false;
    }

    // The footer header's two trailing CRC words are zeroed for the digest domain, because the CRC covers the digest.
    ChunkHeader header{};
    header.kind = ChunkKind::FooterIndex;
    header.schemaVersion = 1u;
    header.critical = 1u;
    header.codec = ChunkCodec::Raw;
    header.sequence = sequence_;
    header.matchId = 0u;
    header.firstTimelineFrame = 0u;
    header.lastTimelineFrame = 0u;
    header.recordCount = footer.matchCount;
    header.payloadBytes = static_cast<uint32_t>(draft.size());
    header.decodedBytes = static_cast<uint32_t>(draft.size());
    header.payloadCrc32c = 0u;

    std::vector<uint8_t> headerBytes;
    encodeChunkHeader(header, headerBytes);
    if (headerBytes.size() != kChunkHeaderBytes) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    std::vector<uint8_t> domainHeader = headerBytes;
    std::memset(domainHeader.data() + kHeaderCrcFieldsOffset, 0,
                kChunkHeaderBytes - kHeaderCrcFieldsOffset);

    digest_->sha.update(domainHeader.data(), domainHeader.size());
    digest_->sha.update(draft.data(), draft.size() - 32u);
    std::array<uint8_t, 32> fileDigest{};
    digest_->sha.finish(fileDigest);

    footer.fileDigest = fileDigest;
    std::vector<uint8_t> payload;
    if (!encodeFooterIndex(footer, payload) || payload.size() != draft.size()) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    header.payloadCrc32c = crc32c(payload.data(), payload.size());
    headerBytes.clear();
    encodeChunkHeader(header, headerBytes);

    if (!rawWrite(headerBytes.data(), headerBytes.size())) return false;
    if (!rawWrite(payload.data(), payload.size())) return false;
    ++sequence_;
    summary_.fileDigest = fileDigest;
    return true;
}

bool ReplayWriter::finalize(const SessionEndRecord& record) {
    if (!open_) { error_ = WriterError::NotOpen; return false; }
    if (terminalWritten_) {
        failWith(WriterError::WrongOrder);
        return false;
    }
    if (!openMatchIds_.empty()) {
        failWith(WriterError::WrongOrder);
        return false;
    }

    SessionEndRecord terminal = record;
    terminal.matchCount = static_cast<uint32_t>(index_.size());
    std::vector<uint8_t> payload;
    if (!encodeSessionEnd(terminal, payload)) {
        failWith(WriterError::InvalidRecord);
        return false;
    }
    if (!appendChunk(ChunkKind::SessionEnd, 0u, terminal.lastTimelineFrame,
                     terminal.lastTimelineFrame, 1u, payload)) {
        return false;
    }
    terminalWritten_ = true;
    finalRollingDigest_ = terminal.rollingDigest;

    if (!writeFooter()) return false;
    ++flushCount_;
    if (!sink_->close()) {
        failWith(WriterError::FlushFailed);
        return false;
    }
    open_ = false;

    summary_.chunkCount = sequence_;
    summary_.matchCount = static_cast<uint32_t>(index_.size());
    summary_.frameCount = frameCount_;
    summary_.bytes = bytesWritten_;

    const std::wstring stem =
        finalPath_.substr(0, finalPath_.size() - 5u);
    const uint32_t attempts = config_.maxNameAttempts != 0u
        ? config_.maxNameAttempts
        : 1u;
    bool published = false;
    std::wstring target;
    for (uint32_t attempt = 1u; attempt <= attempts; ++attempt) {
        target = stem;
        if (attempt != 1u) {
            wchar_t suffix[16];
            _snwprintf_s(suffix, 16, _TRUNCATE, L"_%u", attempt);
            target += suffix;
        }
        target += L".q99r";
        if (!sink_->createNew(target)) {
            const WriterError sinkError = sink_->lastError();
            if (sinkError == WriterError::AccessDenied
                || sinkError == WriterError::DiskFull
                || sinkError == WriterError::RenameFailed) {
                error_ = sinkError;
                discardSink();
                summary_.finalized = false;
                return false;
            }
            continue;
        }
        if (!stagedBytes_.empty()
            && !sink_->append(stagedBytes_.data(), stagedBytes_.size())) {
            const WriterError sinkError = sink_->lastError();
            error_ = sinkError != WriterError::None
                ? sinkError : WriterError::WriteFailed;
            discardSink();
            summary_.finalized = false;
            return false;
        }
        if (!sink_->flush() || !sink_->close()) {
            error_ = sink_->lastError() != WriterError::None
                ? sink_->lastError() : WriterError::FlushFailed;
            discardSink();
            summary_.finalized = false;
            return false;
        }
        published = true;
        break;
    }
    if (!published) {
        error_ = WriterError::NameExhausted;
        discardSink();
        summary_.finalized = false;
        return false;
    }

    finalPath_ = target;
    summary_.finalPath = target;
    summary_.partPath.clear();
    summary_.finalized = true;
    finalized_ = true;
    stagedBytes_.clear();
    return true;
}

void ReplayWriter::abandon() {
    if (!open_) return;
    closeSink();
    discardSink();
    stagedBytes_.clear();
    open_ = false;
    summary_.chunkCount = sequence_;
    summary_.matchCount = static_cast<uint32_t>(index_.size());
    summary_.frameCount = frameCount_;
    summary_.bytes = bytesWritten_;
    summary_.finalized = false;
}

}
