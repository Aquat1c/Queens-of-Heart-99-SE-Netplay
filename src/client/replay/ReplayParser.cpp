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
#include <cstring>

namespace qclient::replay {
namespace {

using namespace qreplay;

constexpr size_t kHeaderCrcFieldsOffset = 40u;

constexpr uint16_t kSupportedSchemaVersion = 1u;

constexpr uint64_t kUnixEpochInFileTimeTicks = 116444736000000000ull;

int64_t fileTimeToUnixSeconds(const FILETIME& ft) {
    uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32)
        | static_cast<uint64_t>(ft.dwLowDateTime);
    if (ticks < kUnixEpochInFileTimeTicks) return 0;
    return static_cast<int64_t>((ticks - kUnixEpochInFileTimeTicks)
                                / 10000000ull);
}

bool addChecked64(uint64_t a, uint64_t b, uint64_t& out) {
    if (a > (~static_cast<uint64_t>(0)) - b) return false;
    out = a + b;
    return true;
}

ParseResult fail(ParseStatus status, uint64_t offset, const char* detail,
                 uint32_t sequence = 0u) {
    ParseResult result{};
    result.status = status;
    result.offset = offset;
    result.chunkSequence = sequence;
    result.detail = detail != nullptr ? detail : "";
    return result;
}

class FileDigestHasher {
public:
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    void feed(const uint8_t* data, size_t len) {
        if (enabled_ && len != 0u) sha_.update(data, len);
    }

    void feedFooterHeader(const uint8_t* header48) {
        if (!enabled_) return;
        uint8_t scratch[kChunkHeaderBytes];
        std::memcpy(scratch, header48, kChunkHeaderBytes);
        std::memset(scratch + kHeaderCrcFieldsOffset, 0,
                    kChunkHeaderBytes - kHeaderCrcFieldsOffset);
        sha_.update(scratch, kChunkHeaderBytes);
    }

    void finish(std::array<uint8_t, 32>& out) { sha_.finish(out); }

private:
    Sha256 sha_;
    bool enabled_ = true;
};

struct OpenMatch {
    uint32_t matchId = 0u;
    uint32_t ordinal = 0u;
    uint32_t firstFrame = 0u;
    bool ended = false;
};

struct WalkState {
    uint32_t chunkOrdinal = 0u;
    uint32_t expectedSequence = 0u;
    bool sawManifest = false;
    bool sawSessionStart = false;
    bool sawSessionEnd = false;
    bool sawFooter = false;
    uint32_t matchCount = 0u;
    uint32_t lastOrdinal = 0u;
    bool haveLastOrdinal = false;
    uint32_t nextFrame = 0u;
    bool timelineStarted = false;
    uint32_t lastEventFrame = 0u;
    bool haveEventFrame = false;
    std::vector<OpenMatch> matches;

    OpenMatch* findMatch(uint32_t matchId) {
        for (OpenMatch& match : matches) {
            if (match.matchId == matchId) return &match;
        }
        return nullptr;
    }
};

}

std::string sanitizeForDisplay(const std::string& text, size_t maxBytes) {
    std::string out;
    out.reserve(std::min(text.size(), maxBytes));
    const uint8_t* data = reinterpret_cast<const uint8_t*>(text.data());
    const size_t len = text.size();
    size_t i = 0;
    bool truncated = false;

    auto append = [&](const char* bytes, size_t count) -> bool {
        if (out.size() + count > maxBytes) {
            truncated = true;
            return false;
        }
        out.append(bytes, count);
        return true;
    };
    const char placeholder = '?';

    while (i < len) {
        const uint8_t lead = data[i];
        size_t sequence = 0u;
        uint32_t code = 0u;
        if (lead < 0x80u) {
            sequence = 1u;
            code = lead;
        } else if ((lead & 0xE0u) == 0xC0u) {
            sequence = 2u;
            code = lead & 0x1Fu;
        } else if ((lead & 0xF0u) == 0xE0u) {
            sequence = 3u;
            code = lead & 0x0Fu;
        } else if ((lead & 0xF8u) == 0xF0u) {
            sequence = 4u;
            code = lead & 0x07u;
        } else {
            if (!append(&placeholder, 1u)) break;
            ++i;
            continue;
        }
        if (i + sequence > len) {
            if (!append(&placeholder, 1u)) break;
            ++i;
            continue;
        }
        bool valid = true;
        for (size_t k = 1; k < sequence; ++k) {
            const uint8_t cont = data[i + k];
            if ((cont & 0xC0u) != 0x80u) { valid = false; break; }
            code = (code << 6) | static_cast<uint32_t>(cont & 0x3Fu);
        }
        if (!valid) {
            if (!append(&placeholder, 1u)) break;
            ++i;
            continue;
        }
        const bool overlong = (sequence == 2u && code < 0x80u)
            || (sequence == 3u && code < 0x800u)
            || (sequence == 4u && code < 0x10000u);
        const bool surrogate = code >= 0xD800u && code <= 0xDFFFu;
        const bool tooLarge = code > 0x10FFFFu;
        const bool control = code < 0x20u || code == 0x7Fu
            || (code >= 0x80u && code <= 0x9Fu);
        if (overlong || surrogate || tooLarge || control) {
            if (!append(&placeholder, 1u)) break;
            i += sequence;
            continue;
        }
        if (!append(reinterpret_cast<const char*>(data + i), sequence)) break;
        i += sequence;
    }
    if (truncated) {
        while (!out.empty() && out.size() + 3u > maxBytes) {
            while (!out.empty()
                   && (static_cast<unsigned char>(out.back()) & 0xC0u)
                       == 0x80u) {
                out.pop_back();
            }
            if (!out.empty()) out.pop_back();
        }
        if (out.size() + 3u <= maxBytes) out.append("...");
    }
    return out;
}

std::string sanitizeForDisplay(const std::wstring& text, size_t maxBytes) {
    std::string narrow = qplat::narrow(text);
    if (narrow.empty() && !text.empty()) {
        narrow.reserve(text.size());
        for (wchar_t ch : text) {
            narrow.push_back(
                (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '?');
        }
    }
    return sanitizeForDisplay(narrow, maxBytes);
}

bool isSafeReplayPath(const std::wstring& path) {
    if (path.empty() || path.size() > 32768u) return false;
    if (path.find(L'\0') != std::wstring::npos) return false;
    if (path.find(L'*') != std::wstring::npos) return false;
    if (path.find(L'?') != std::wstring::npos) return false;
    if (path.find(L'|') != std::wstring::npos) return false;
    if (path.find(L'<') != std::wstring::npos) return false;
    if (path.find(L'>') != std::wstring::npos) return false;

    size_t colonStart = 0u;
    if (path.size() >= 2u && path[1] == L':') colonStart = 2u;
    if (path.find(L':', colonStart) != std::wstring::npos) return false;

    size_t start = 0u;
    while (start <= path.size()) {
        size_t end = path.find_first_of(L"\\/", start);
        if (end == std::wstring::npos) end = path.size();
        const std::wstring component = path.substr(start, end - start);
        if (component == L"." || component == L"..") return false;
        if (end == path.size()) break;
        start = end + 1u;
    }
    return true;
}

MemoryByteSource::MemoryByteSource(const uint8_t* data, size_t len)
    : data_(data), len_(data != nullptr ? len : 0u) {}

MemoryByteSource::MemoryByteSource(std::vector<uint8_t> bytes)
    : owned_(std::move(bytes)) {
    data_ = owned_.data();
    len_ = owned_.size();
}

uint64_t MemoryByteSource::size() const {
    return static_cast<uint64_t>(len_);
}

bool MemoryByteSource::read(uint64_t offset, uint8_t* dst, size_t bytes) {
    ++readCalls_;
    if (bytes == 0u) return true;
    if (dst == nullptr) return false;
    if (offset > static_cast<uint64_t>(len_)) return false;
    uint64_t end = 0u;
    if (!addChecked64(offset, static_cast<uint64_t>(bytes), end)) return false;
    if (end > static_cast<uint64_t>(len_)) return false;
    std::memcpy(dst, data_ + static_cast<size_t>(offset), bytes);
    bytesRead_ += bytes;
    return true;
}

const char* openErrorText(OpenError error) {
    switch (error) {
        case OpenError::None: return "ok";
        case OpenError::InvalidPath: return "unsafe or malformed path";
        case OpenError::NotFound: return "file not found";
        case OpenError::AccessDenied: return "access denied";
        case OpenError::ReparsePoint: return "refused a reparse point";
        case OpenError::TooLarge: return "file exceeds the replay size limit";
        case OpenError::IoError: return "read error";
    }
    return "unknown";
}

FileByteSource::~FileByteSource() { close(); }

bool FileByteSource::open(const std::wstring& path) {
    close();
    error_ = OpenError::None;
    if (!isSafeReplayPath(path)) {
        error_ = OpenError::InvalidPath;
        return false;
    }
    const std::wstring wide = qplat::longPathW(path);

    HANDLE handle = ::CreateFileW(
        wide.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT
            | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            error_ = OpenError::NotFound;
        else if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION)
            error_ = OpenError::AccessDenied;
        else
            error_ = OpenError::IoError;
        return false;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileInformationByHandle(handle, &info) == 0) {
        ::CloseHandle(handle);
        error_ = OpenError::IoError;
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u
        || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
        ::CloseHandle(handle);
        error_ = OpenError::ReparsePoint;
        return false;
    }

    const uint64_t bytes = (static_cast<uint64_t>(info.nFileSizeHigh) << 32)
        | static_cast<uint64_t>(info.nFileSizeLow);
    if (bytes > kMaxFileBytes) {
        ::CloseHandle(handle);
        error_ = OpenError::TooLarge;
        return false;
    }

    handle_ = handle;
    size_ = bytes;
    path_ = path;
    identity_.volumeId = static_cast<uint64_t>(info.dwVolumeSerialNumber);
    identity_.fileId = (static_cast<uint64_t>(info.nFileIndexHigh) << 32)
        | static_cast<uint64_t>(info.nFileIndexLow);
    identity_.sizeBytes = bytes;
    identity_.modifiedUnixSeconds = fileTimeToUnixSeconds(info.ftLastWriteTime);
    identity_.known = true;
    return true;
}

void FileByteSource::close() {
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    size_ = 0u;
    identity_ = FileIdentity{};
    path_.clear();
}

uint64_t FileByteSource::size() const { return size_; }

bool FileByteSource::read(uint64_t offset, uint8_t* dst, size_t bytes) {
    if (bytes == 0u) return true;
    if (handle_ == nullptr || dst == nullptr) return false;
    uint64_t end = 0u;
    if (!addChecked64(offset, static_cast<uint64_t>(bytes), end)) return false;
    if (end > size_) return false;

    size_t done = 0u;
    while (done < bytes) {
        OVERLAPPED overlapped{};
        const uint64_t at = offset + static_cast<uint64_t>(done);
        overlapped.Offset = static_cast<DWORD>(at & 0xFFFFFFFFull);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
        const size_t remaining = bytes - done;
        const DWORD ask = static_cast<DWORD>(
            std::min<size_t>(remaining, 1u * 1024u * 1024u));
        DWORD got = 0u;
        if (::ReadFile(static_cast<HANDLE>(handle_), dst + done, ask, &got,
                       &overlapped) == 0) {
            return false;
        }
        if (got == 0u) return false;
        done += static_cast<size_t>(got);
    }
    return true;
}

bool queryFileIdentity(const std::wstring& path, FileIdentity& out) {
    out = FileIdentity{};
    if (!isSafeReplayPath(path)) return false;
    const std::wstring wide = qplat::longPathW(path);
    HANDLE handle = ::CreateFileW(
        wide.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT
            | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = ::GetFileInformationByHandle(handle, &info);
    ::CloseHandle(handle);
    if (ok == 0) return false;
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        return false;
    out.volumeId = static_cast<uint64_t>(info.dwVolumeSerialNumber);
    out.fileId = (static_cast<uint64_t>(info.nFileIndexHigh) << 32)
        | static_cast<uint64_t>(info.nFileIndexLow);
    out.sizeBytes = (static_cast<uint64_t>(info.nFileSizeHigh) << 32)
        | static_cast<uint64_t>(info.nFileSizeLow);
    out.modifiedUnixSeconds = fileTimeToUnixSeconds(info.ftLastWriteTime);
    out.known = true;
    return true;
}

void ParsedReplay::reset() {
    *this = ParsedReplay{};
}

ParseResult ReplayParser::walk(ReplayByteSource& source,
                               const WalkOptions& options,
                               ReplayChunkVisitor& visitor) {
    ParseResult result{};
    const uint64_t fileBytes = source.size();
    if (fileBytes > kMaxFileBytes)
        return fail(ParseStatus::LimitExceeded, 0u, "file exceeds 512 MiB");
    if (fileBytes < kPrefixBytes)
        return fail(ParseStatus::Truncated, 0u, "shorter than the prefix");

    uint8_t prefixBytes[kPrefixBytes];
    if (!source.read(0u, prefixBytes, kPrefixBytes))
        return fail(ParseStatus::IoError, 0u, "prefix read failed");

    ReplayPrefix prefix{};
    const ParseStatus prefixStatus =
        decodePrefix(prefixBytes, kPrefixBytes, prefix);
    if (prefixStatus != ParseStatus::Ok)
        return fail(prefixStatus, 0u, "prefix rejected");

    FileDigestHasher hasher;
    hasher.setEnabled(options.verifyFileDigest);
    hasher.feed(prefixBytes, kPrefixBytes);

    WalkState state{};
    std::vector<uint8_t> payload;
    std::vector<ReplayFrameV1> frames;
    std::vector<ReplayEvent> events;
    uint8_t headerBytes[kChunkHeaderBytes];

    // walk() must be reentrant: no static or thread-local record scratch.
    SessionManifest manifestRecord{};
    DeterministicStart startRecord{};
    MatchStartRecord matchStartRecord{};
    MatchEndRecord matchEndRecord{};
    SessionEndRecord sessionEndRecord{};
    FooterIndex footerRecord{};

    uint64_t offset = kPrefixBytes;
    const size_t sliceBytes = options.readChunkBytes != 0u
        ? std::min<size_t>(options.readChunkBytes, 1u * 1024u * 1024u)
        : 64u * 1024u;

    while (offset < fileBytes) {
        result.offset = offset;
        result.chunkSequence = state.chunkOrdinal;

        if (state.chunkOrdinal >= kMaxChunksPerFile)
            return fail(ParseStatus::LimitExceeded, offset, "too many chunks");
        if (fileBytes - offset < kChunkHeaderBytes) {
            return fail(ParseStatus::Truncated, offset,
                        "chunk header is truncated", state.chunkOrdinal);
        }
        if (!source.read(offset, headerBytes, kChunkHeaderBytes)) {
            return fail(ParseStatus::IoError, offset, "chunk header read",
                        state.chunkOrdinal);
        }

        ChunkLocation location{};
        location.headerOffset = offset;
        const ParseStatus headerStatus =
            decodeChunkHeader(headerBytes, kChunkHeaderBytes, location.header);
        if (headerStatus != ParseStatus::Ok) {
            return fail(headerStatus, offset, "chunk header rejected",
                        state.chunkOrdinal);
        }
        const ChunkHeader& header = location.header;

        uint64_t payloadOffset = 0u;
        uint64_t chunkEnd = 0u;
        if (!addChecked64(offset, kChunkHeaderBytes, payloadOffset)
            || !addChecked64(payloadOffset,
                             static_cast<uint64_t>(header.payloadBytes),
                             chunkEnd)) {
            return fail(ParseStatus::IntegerOverflow, offset,
                        "chunk extent overflows", header.sequence);
        }
        if (chunkEnd > fileBytes) {
            return fail(ParseStatus::Truncated, offset,
                        "chunk payload is truncated", header.sequence);
        }
        location.payloadOffset = payloadOffset;

        if (state.chunkOrdinal == 0u
            && header.kind != ChunkKind::SessionManifest) {
            return fail(ParseStatus::MissingManifest, offset,
                        "first chunk is not the session manifest");
        }

        if (header.sequence != state.expectedSequence) {
            return fail(ParseStatus::SequenceRegression, offset,
                        header.sequence < state.expectedSequence
                            ? "chunk sequence went backwards"
                            : "chunk sequence skipped a value",
                        header.sequence);
        }

        if (isSingletonChunkKind(header.kind)) {
            const bool duplicate =
                (header.kind == ChunkKind::SessionManifest && state.sawManifest)
                || (header.kind == ChunkKind::SessionStart
                    && state.sawSessionStart)
                || (header.kind == ChunkKind::SessionEnd && state.sawSessionEnd)
                || (header.kind == ChunkKind::FooterIndex && state.sawFooter);
            if (duplicate) {
                return fail(ParseStatus::DuplicateSingletonChunk, offset,
                            "duplicate singleton chunk", header.sequence);
            }
        }
        if (state.chunkOrdinal == 1u && header.kind != ChunkKind::SessionStart)
            return fail(ParseStatus::MissingSessionStart, offset,
                        "second chunk is not the session start");
        if (state.sawSessionEnd && header.kind != ChunkKind::FooterIndex) {
            return fail(ParseStatus::FrameConflict, offset,
                        "chunk follows the session terminal", header.sequence);
        }

        payload.clear();
        payload.resize(static_cast<size_t>(header.payloadBytes));
        Crc32c payloadCrc;
        size_t done = 0u;
        while (done < payload.size()) {
            const size_t take = std::min(sliceBytes, payload.size() - done);
            if (!source.read(payloadOffset + static_cast<uint64_t>(done),
                             payload.data() + done, take)) {
                return fail(ParseStatus::IoError, payloadOffset + done,
                            "payload read failed", header.sequence);
            }
            payloadCrc.update(payload.data() + done, take);
            done += take;
        }
        if (payloadCrc.value() != header.payloadCrc32c) {
            return fail(ParseStatus::BadChunkPayloadCrc, payloadOffset,
                        "payload CRC mismatch", header.sequence);
        }

        if (header.kind == ChunkKind::FooterIndex) {
            hasher.feedFooterHeader(headerBytes);
            if (payload.size() >= 32u)
                hasher.feed(payload.data(), payload.size() - 32u);
        } else {
            hasher.feed(headerBytes, kChunkHeaderBytes);
            hasher.feed(payload.data(), payload.size());
        }

        const bool critical = isCriticalChunkKind(header.kind);
        const bool understood = header.schemaVersion == kSupportedSchemaVersion
            && header.kind != ChunkKind::ProofBlock
            && header.kind != ChunkKind::Checkpoint;
        if (!understood && critical) {
            return fail(ParseStatus::UnknownCriticalChunk, offset,
                        "critical chunk uses an unimplemented schema",
                        header.sequence);
        }

        DecodedChunk decoded{};
        if (!understood) {
            decoded.skippedOptional = true;
        } else {
            const uint8_t* body = payload.data();
            const size_t bodyLen = payload.size();
            switch (header.kind) {
                case ChunkKind::SessionManifest: {
                    SessionManifest& manifest = manifestRecord;
                    manifest = SessionManifest{};
                    const ParseStatus status =
                        decodeSessionManifest(body, bodyLen, manifest);
                    if (status != ParseStatus::Ok) {
                        return fail(status, payloadOffset, "manifest rejected",
                                    header.sequence);
                    }
                    if (header.matchId != 0u || header.recordCount != 1u) {
                        return fail(ParseStatus::FrameConflict, offset,
                                    "manifest framing disagrees with its kind",
                                    header.sequence);
                    }
                    state.sawManifest = true;
                    decoded.manifest = &manifest;
                    break;
                }
                case ChunkKind::SessionStart: {
                    DeterministicStart& start = startRecord;
                    start = DeterministicStart{};
                    const ParseStatus status =
                        decodeDeterministicStart(body, bodyLen, start);
                    if (status != ParseStatus::Ok) {
                        return fail(status, payloadOffset,
                                    "session start rejected", header.sequence);
                    }
                    if (header.matchId != 0u
                        || header.firstTimelineFrame != start.firstTimelineFrame) {
                        return fail(ParseStatus::FrameConflict, offset,
                                    "session start framing disagrees",
                                    header.sequence);
                    }
                    state.sawSessionStart = true;
                    state.nextFrame = start.firstTimelineFrame;
                    state.timelineStarted = true;
                    decoded.sessionStart = &start;
                    break;
                }
                case ChunkKind::MatchStart: {
                    MatchStartRecord& match = matchStartRecord;
                    match = MatchStartRecord{};
                    const ParseStatus status =
                        decodeMatchStart(body, bodyLen, match);
                    if (status != ParseStatus::Ok) {
                        return fail(status, payloadOffset,
                                    "match start rejected", header.sequence);
                    }
                    if (!state.sawSessionStart) {
                        return fail(ParseStatus::MissingSessionStart, offset,
                                    "match precedes the session start",
                                    header.sequence);
                    }
                    if (header.matchId != match.matchId
                        || header.firstTimelineFrame != match.firstTimelineFrame) {
                        return fail(ParseStatus::FrameConflict, offset,
                                    "match start framing disagrees",
                                    header.sequence);
                    }
                    if (state.findMatch(match.matchId) != nullptr) {
                        return fail(ParseStatus::FrameConflict, offset,
                                    "duplicate match id", header.sequence);
                    }
                    if (state.haveLastOrdinal
                        && match.ordinal <= state.lastOrdinal) {
                        return fail(ParseStatus::BadMatchIndex, offset,
                                    "match ordinal is not increasing",
                                    header.sequence);
                    }
                    if (match.dependsOnMatchId != 0u
                        && state.findMatch(match.dependsOnMatchId) == nullptr) {
                        return fail(ParseStatus::BadMatchIndex, offset,
                                    "match depends on an unseen match",
                                    header.sequence);
                    }
                    if (match.firstTimelineFrame != state.nextFrame) {
                        return fail(ParseStatus::FrameConflict, offset,
                                    "match start is not at the timeline head",
                                    header.sequence);
                    }
                    if (state.matchCount >= kMaxMatchesPerSet) {
                        return fail(ParseStatus::LimitExceeded, offset,
                                    "too many matches", header.sequence);
                    }
                    OpenMatch open{};
                    open.matchId = match.matchId;
                    open.ordinal = match.ordinal;
                    open.firstFrame = match.firstTimelineFrame;
                    state.matches.push_back(open);
                    state.lastOrdinal = match.ordinal;
                    state.haveLastOrdinal = true;
                    ++state.matchCount;
                    decoded.matchStart = &match;
                    break;
                }
                case ChunkKind::InputBlock: {
                    if (!state.timelineStarted) {
                        return fail(ParseStatus::MissingSessionStart, offset,
                                    "input precedes the session start",
                                    header.sequence);
                    }
                    if (header.recordCount == 0u) {
                        return fail(ParseStatus::FrameConflict, offset,
                                    "empty input block", header.sequence);
                    }
                    if (header.firstTimelineFrame < state.nextFrame) {
                        return fail(ParseStatus::FrameConflict, offset,
                                    "input block overlaps the timeline",
                                    header.sequence);
                    }
                    if (header.firstTimelineFrame > state.nextFrame) {
                        return fail(ParseStatus::FrameGap, offset,
                                    "input block leaves a timeline gap",
                                    header.sequence);
                    }
                    if (header.matchId != 0u) {
                        OpenMatch* open = state.findMatch(header.matchId);
                        if (open == nullptr) {
                            return fail(ParseStatus::BadMatchIndex, offset,
                                        "input block names an unseen match",
                                        header.sequence);
                        }
                        if (open->ended) {
                            return fail(ParseStatus::FrameConflict, offset,
                                        "input block follows the match end",
                                        header.sequence);
                        }
                    }
                    frames.clear();
                    if (options.decodeRecords) {
                        const ParseStatus status = decodeInputBlock(
                            body, bodyLen, header.firstTimelineFrame,
                            header.recordCount, frames);
                        if (status != ParseStatus::Ok) {
                            return fail(status, payloadOffset,
                                        "input block rejected",
                                        header.sequence);
                        }
                        decoded.frames = &frames;
                    }
                    state.nextFrame = header.lastTimelineFrame + 1u;
                    break;
                }
                case ChunkKind::EventBlock: {
                    if (!state.timelineStarted) {
                        return fail(ParseStatus::MissingSessionStart, offset,
                                    "events precede the session start",
                                    header.sequence);
                    }
                    events.clear();
                    const ParseStatus status =
                        decodeEventBlock(body, bodyLen, header.recordCount,
                                         events);
                    if (status != ParseStatus::Ok) {
                        return fail(status, payloadOffset,
                                    "event block rejected", header.sequence);
                    }
                    for (const ReplayEvent& event : events) {
                        if (event.matchId != header.matchId) {
                            return fail(ParseStatus::BadEventPayload,
                                        payloadOffset,
                                        "event match id disagrees with framing",
                                        header.sequence);
                        }
                        if (event.matchId != 0u
                            && state.findMatch(event.matchId) == nullptr) {
                            return fail(ParseStatus::BadMatchIndex,
                                        payloadOffset,
                                        "event names an unseen match",
                                        header.sequence);
                        }
                        if (event.timelineFrame > state.nextFrame) {
                            return fail(ParseStatus::FrameConflict,
                                        payloadOffset,
                                        "event is ahead of the timeline",
                                        header.sequence);
                        }
                        if (state.haveEventFrame
                            && event.timelineFrame < state.lastEventFrame) {
                            return fail(ParseStatus::FrameConflict,
                                        payloadOffset,
                                        "event frames are not ordered",
                                        header.sequence);
                        }
                        if (event.timelineFrame < header.firstTimelineFrame
                            || event.timelineFrame > header.lastTimelineFrame) {
                            return fail(ParseStatus::FrameConflict,
                                        payloadOffset,
                                        "event is outside the block range",
                                        header.sequence);
                        }
                        state.lastEventFrame = event.timelineFrame;
                        state.haveEventFrame = true;
                        if (event.kind == EventKind::RoundEnded
                            && event.matchId != 0u) {
                            OpenMatch* open = state.findMatch(event.matchId);
                            if (open != nullptr && open->ended) {
                                return fail(ParseStatus::FrameConflict,
                                            payloadOffset,
                                            "round ends after the match end",
                                            header.sequence);
                            }
                        }
                    }
                    decoded.events = &events;
                    break;
                }
                case ChunkKind::MatchEnd: {
                    MatchEndRecord& end = matchEndRecord;
                    end = MatchEndRecord{};
                    const ParseStatus status =
                        decodeMatchEnd(body, bodyLen, end);
                    if (status != ParseStatus::Ok) {
                        return fail(status, payloadOffset,
                                    "match end rejected", header.sequence);
                    }
                    if (header.matchId != end.matchId
                        || header.lastTimelineFrame != end.lastTimelineFrame) {
                        return fail(ParseStatus::FrameConflict, offset,
                                    "match end framing disagrees",
                                    header.sequence);
                    }
                    OpenMatch* open = state.findMatch(end.matchId);
                    if (open == nullptr) {
                        return fail(ParseStatus::BadMatchIndex, offset,
                                    "match end without a match start",
                                    header.sequence);
                    }
                    if (open->ended) {
                        return fail(ParseStatus::FrameConflict, offset,
                                    "duplicate match end", header.sequence);
                    }
                    const uint32_t observedLast =
                        state.nextFrame != 0u ? state.nextFrame - 1u : 0u;
                    if (end.lastTimelineFrame < open->firstFrame
                        || end.lastTimelineFrame > observedLast) {
                        return fail(ParseStatus::FrameConflict, offset,
                                    "match end frame is outside the timeline",
                                    header.sequence);
                    }
                    open->ended = true;
                    decoded.matchEnd = &end;
                    break;
                }
                case ChunkKind::SessionEnd: {
                    SessionEndRecord& end = sessionEndRecord;
                    end = SessionEndRecord{};
                    const ParseStatus status =
                        decodeSessionEnd(body, bodyLen, end);
                    if (status != ParseStatus::Ok) {
                        return fail(status, payloadOffset,
                                    "session end rejected", header.sequence);
                    }
                    if (header.matchId != 0u
                        || header.lastTimelineFrame != end.lastTimelineFrame) {
                        return fail(ParseStatus::FrameConflict, offset,
                                    "session end framing disagrees",
                                    header.sequence);
                    }
                    if (end.matchCount != state.matchCount) {
                        return fail(ParseStatus::BadMatchIndex, offset,
                                    "session end match count disagrees",
                                    header.sequence);
                    }
                    const uint32_t observedLast =
                        state.nextFrame != 0u ? state.nextFrame - 1u : 0u;
                    if (state.timelineStarted
                        && end.lastTimelineFrame > observedLast) {
                        return fail(ParseStatus::FrameConflict, offset,
                                    "session end frame is ahead of the timeline",
                                    header.sequence);
                    }
                    state.sawSessionEnd = true;
                    decoded.sessionEnd = &end;
                    break;
                }
                case ChunkKind::FooterIndex: {
                    FooterIndex& footer = footerRecord;
                    footer = FooterIndex{};
                    const ParseStatus status =
                        decodeFooterIndex(body, bodyLen, footer);
                    if (status != ParseStatus::Ok) {
                        return fail(status, payloadOffset, "footer rejected",
                                    header.sequence);
                    }
                    if (chunkEnd != fileBytes) {
                        return fail(ParseStatus::BadMatchIndex, offset,
                                    "footer is not the final chunk",
                                    header.sequence);
                    }
                    if (footer.chunkCount != state.chunkOrdinal + 1u
                        || footer.matchCount != state.matchCount) {
                        return fail(ParseStatus::BadMatchIndex, offset,
                                    "footer counts disagree with the file",
                                    header.sequence);
                    }
                    if (hasher.enabled()) {
                        std::array<uint8_t, 32> computed{};
                        hasher.finish(computed);
                        if (computed != footer.fileDigest) {
                            return fail(ParseStatus::FileDigestMismatch,
                                        payloadOffset,
                                        "file digest mismatch",
                                        header.sequence);
                        }
                    }
                    state.sawFooter = true;
                    decoded.footer = &footer;
                    break;
                }
                default:
                    return fail(ParseStatus::UnknownCriticalChunk, offset,
                                "unhandled chunk kind", header.sequence);
            }
        }

        if (!visitor.onChunk(location, payload.data(), payload.size(),
                             decoded)) {
            result.status = ParseStatus::Ok;
            result.offset = chunkEnd;
            result.chunkSequence = header.sequence;
            result.lastValidTimelineFrame =
                state.timelineStarted && state.nextFrame != 0u
                    ? state.nextFrame - 1u
                    : 0u;
            return result;
        }

        offset = chunkEnd;
        ++state.chunkOrdinal;
        ++state.expectedSequence;
        result.lastValidMatchId =
            state.matches.empty() ? 0u : state.matches.back().matchId;
        result.lastValidTimelineFrame =
            state.timelineStarted && state.nextFrame != 0u
                ? state.nextFrame - 1u
                : 0u;

        if (options.stopAfterSessionStart && state.sawSessionStart) {
            result.status = ParseStatus::Ok;
            result.offset = offset;
            result.chunkSequence = header.sequence;
            return result;
        }
    }

    if (!state.sawManifest)
        return fail(ParseStatus::MissingManifest, offset, "no session manifest");
    if (!state.sawSessionStart) {
        return fail(ParseStatus::MissingSessionStart, offset,
                    "no deterministic session start");
    }

    result.status = ParseStatus::Ok;
    result.offset = offset;
    result.chunkSequence =
        state.chunkOrdinal != 0u ? state.chunkOrdinal - 1u : 0u;
    return result;
}

size_t ReplayParser::footerSearchWindowBytes() {
    return kFooterFixedBytes
        + static_cast<size_t>(kMaxMatchesPerSet) * kMatchIndexEntryBytes
        + kChunkHeaderBytes;
}

namespace {

constexpr uint64_t kFooterWindowStages[] = { 8u * 1024u, 64u * 1024u, 0u };

bool findFooterInWindow(ReplayByteSource& source, uint64_t fileBytes,
                        uint64_t window, ChunkLocation& locationOut,
                        FooterIndex& footerOut) {
    if (window < kChunkHeaderBytes) return false;
    const uint64_t windowStart = fileBytes - window;

    std::vector<uint8_t> tail;
    tail.resize(static_cast<size_t>(window));
    if (!source.read(windowStart, tail.data(), tail.size())) return false;

    for (size_t i = tail.size() - kChunkHeaderBytes + 1u; i-- > 0u;) {
        const uint32_t sync = static_cast<uint32_t>(tail[i])
            | (static_cast<uint32_t>(tail[i + 1u]) << 8)
            | (static_cast<uint32_t>(tail[i + 2u]) << 16)
            | (static_cast<uint32_t>(tail[i + 3u]) << 24);
        if (sync != kChunkSyncWord) continue;

        ChunkHeader header{};
        if (decodeChunkHeader(tail.data() + i, kChunkHeaderBytes, header)
            != ParseStatus::Ok) {
            continue;
        }
        if (header.kind != ChunkKind::FooterIndex) continue;

        const uint64_t headerOffset = windowStart + static_cast<uint64_t>(i);
        uint64_t payloadOffset = 0u;
        uint64_t end = 0u;
        if (!addChecked64(headerOffset, kChunkHeaderBytes, payloadOffset)
            || !addChecked64(payloadOffset,
                             static_cast<uint64_t>(header.payloadBytes), end)) {
            continue;
        }
        if (end != fileBytes) continue;

        std::vector<uint8_t> payload;
        payload.resize(static_cast<size_t>(header.payloadBytes));
        if (!source.read(payloadOffset, payload.data(), payload.size()))
            continue;
        if (crc32c(payload.data(), payload.size()) != header.payloadCrc32c)
            continue;

        FooterIndex footer{};
        if (decodeFooterIndex(payload.data(), payload.size(), footer)
            != ParseStatus::Ok) {
            continue;
        }
        locationOut.headerOffset = headerOffset;
        locationOut.payloadOffset = payloadOffset;
        locationOut.header = header;
        footerOut = footer;
        return true;
    }
    return false;
}

}

bool ReplayParser::findFooter(ReplayByteSource& source,
                              ChunkLocation& locationOut,
                              FooterIndex& footerOut) {
    const uint64_t fileBytes = source.size();
    if (fileBytes < kPrefixBytes + kChunkHeaderBytes) return false;
    const uint64_t available = fileBytes - kPrefixBytes;
    const uint64_t maximum = std::min<uint64_t>(
        static_cast<uint64_t>(footerSearchWindowBytes()), available);

    uint64_t previous = 0u;
    for (size_t stage = 0;
         stage < sizeof(kFooterWindowStages) / sizeof(kFooterWindowStages[0]);
         ++stage) {
        const uint64_t requested = kFooterWindowStages[stage] != 0u
            ? kFooterWindowStages[stage]
            : maximum;
        const uint64_t window = std::min<uint64_t>(requested, maximum);
        if (window <= previous && previous != 0u) continue;
        previous = window;
        if (findFooterInWindow(source, fileBytes, window, locationOut,
                               footerOut)) {
            return true;
        }
        if (window >= maximum) break;
    }
    return false;
}

namespace {

class BuildVisitor final : public ReplayChunkVisitor {
public:
    BuildVisitor(const ParseOptions& options, ParsedReplay& out)
        : options_(options), out_(out) {}

    ParseStatus failure() const { return failure_; }
    const char* failureDetail() const { return failureDetail_; }

    bool onChunk(const ChunkLocation& location, const uint8_t*, size_t,
                 const DecodedChunk& decoded) override {
        if (options_.collectChunkIndex) out_.chunkIndex.push_back(location);
        ++out_.chunkCount;
        out_.contentBytes += static_cast<uint64_t>(location.header.payloadBytes);
        out_.lastValidChunkEnd = location.endOffset();
        out_.lastValidSequence = location.header.sequence;
        out_.anyChunkValidated = true;

        if (decoded.skippedOptional) {
            ++out_.skippedOptionalChunks;
            return true;
        }
        const bool retain = options_.mode == ParseMode::Full;

        if (decoded.manifest != nullptr) {
            out_.file.manifest = *decoded.manifest;
            out_.hasManifest = true;
            return true;
        }
        if (decoded.sessionStart != nullptr) {
            out_.file.sessionStart = *decoded.sessionStart;
            out_.hasSessionStart = true;
            out_.firstTimelineFrame = decoded.sessionStart->firstTimelineFrame;
            out_.lastTimelineFrame = out_.firstTimelineFrame;
            return true;
        }
        if (decoded.matchStart != nullptr) {
            MatchSpan span{};
            span.matchId = decoded.matchStart->matchId;
            span.ordinal = decoded.matchStart->ordinal;
            span.firstTimelineFrame = decoded.matchStart->firstTimelineFrame;
            span.lastTimelineFrame = decoded.matchStart->firstTimelineFrame;
            span.matchStartOffset = location.headerOffset;
            span.independentBootstrap = decoded.matchStart->independentBootstrap;
            span.dependsOnMatchId = decoded.matchStart->dependsOnMatchId;
            for (size_t i = 0; i < kFighterSlots; ++i) {
                span.roster[i] = decoded.matchStart->slots[i].identity;
                span.ownership[i] = decoded.matchStart->slots[i].ownership;
            }
            out_.matches.push_back(span);
            if (retain) out_.file.matchStarts.push_back(*decoded.matchStart);
            return true;
        }
        if (decoded.matchEnd != nullptr) {
            MatchSpan* span = findSpan(decoded.matchEnd->matchId);
            if (span != nullptr) {
                span->hasEnd = true;
                span->lastTimelineFrame = decoded.matchEnd->lastTimelineFrame;
                span->winningSideMask = decoded.matchEnd->winningSideMask;
                span->endReason = decoded.matchEnd->reason;
            }
            if (retain) out_.file.matchEnds.push_back(*decoded.matchEnd);
            return true;
        }
        if (decoded.sessionEnd != nullptr) {
            out_.file.hasSessionEnd = true;
            out_.file.sessionEnd = *decoded.sessionEnd;
            return true;
        }
        if (decoded.footer != nullptr) {
            out_.file.hasFooter = true;
            out_.file.footer = *decoded.footer;
            out_.hasFooter = true;
            return crossCheckFooter(*decoded.footer);
        }
        if (decoded.frames != nullptr) {
            const uint32_t matchId = location.header.matchId;
            MatchSpan* span = matchId != 0u ? findSpan(matchId) : nullptr;
            if (span != nullptr) {
                if (span->firstInputOffset == 0u)
                    span->firstInputOffset = location.headerOffset;
                span->frameCount += location.header.recordCount;
                span->lastTimelineFrame = location.header.lastTimelineFrame;
            }
            out_.totalFrames += location.header.recordCount;
            out_.lastTimelineFrame = location.header.lastTimelineFrame;
            if (retain && wanted(matchId)) {
                if (out_.file.frames.size() + decoded.frames->size()
                    > options_.maxRetainedFrames) {
                    failure_ = ParseStatus::LimitExceeded;
                    failureDetail_ = "frame retention budget exceeded";
                    return false;
                }
                out_.file.frames.insert(out_.file.frames.end(),
                                        decoded.frames->begin(),
                                        decoded.frames->end());
            }
            return true;
        }
        if (decoded.events != nullptr) {
            for (const ReplayEvent& event : *decoded.events) {
                if (event.kind == EventKind::RoundEnded) {
                    MatchSpan* span = findSpan(event.matchId);
                    if (span != nullptr) ++span->roundCount;
                }
            }
            if (retain && wanted(location.header.matchId)) {
                if (out_.file.events.size() + decoded.events->size()
                    > options_.maxRetainedEvents) {
                    failure_ = ParseStatus::LimitExceeded;
                    failureDetail_ = "event retention budget exceeded";
                    return false;
                }
                out_.file.events.insert(out_.file.events.end(),
                                        decoded.events->begin(),
                                        decoded.events->end());
            }
            return true;
        }
        return true;
    }

private:
    bool wanted(uint32_t matchId) const {
        return options_.matchIdFilter == 0u || matchId == 0u
            || matchId == options_.matchIdFilter;
    }

    MatchSpan* findSpan(uint32_t matchId) {
        for (MatchSpan& span : out_.matches) {
            if (span.matchId == matchId) return &span;
        }
        return nullptr;
    }

    bool crossCheckFooter(const FooterIndex& footer) {
        if (footer.totalFrames != out_.totalFrames) {
            failure_ = ParseStatus::BadMatchIndex;
            failureDetail_ = "footer frame total disagrees with the timeline";
            return false;
        }
        if (footer.matches.size() != out_.matches.size()) {
            failure_ = ParseStatus::BadMatchIndex;
            failureDetail_ = "footer match count disagrees";
            return false;
        }
        for (size_t i = 0; i < footer.matches.size(); ++i) {
            const MatchIndexEntry& entry = footer.matches[i];
            const MatchSpan& span = out_.matches[i];
            if (entry.matchId != span.matchId || entry.ordinal != span.ordinal
                || entry.matchStartOffset != span.matchStartOffset
                || entry.firstInputOffset != span.firstInputOffset
                || entry.firstTimelineFrame != span.firstTimelineFrame
                || entry.lastTimelineFrame != span.lastTimelineFrame
                || entry.frameCount != span.frameCount
                || entry.roundCount != span.roundCount
                || entry.winningSideMask != span.winningSideMask
                || entry.independentBootstrap != span.independentBootstrap) {
                failure_ = ParseStatus::BadMatchIndex;
                failureDetail_ = "footer entry disagrees with the match";
                return false;
            }
            for (size_t slot = 0; slot < kFighterSlots; ++slot) {
                if (entry.roster[slot].nativeCharacterId
                        != span.roster[slot].nativeCharacterId
                    || entry.roster[slot].paletteIndex
                        != span.roster[slot].paletteIndex
                    || entry.ownership[slot] != span.ownership[slot]) {
                    failure_ = ParseStatus::BadMatchIndex;
                    failureDetail_ = "footer roster disagrees with the match";
                    return false;
                }
            }
        }
        if (out_.file.hasSessionEnd
            && footer.finalRollingDigest != out_.file.sessionEnd.rollingDigest) {
            failure_ = ParseStatus::DigestMismatch;
            failureDetail_ = "footer digest disagrees with the terminal";
            return false;
        }
        return true;
    }

    const ParseOptions& options_;
    ParsedReplay& out_;
    ParseStatus failure_ = ParseStatus::Ok;
    const char* failureDetail_ = "";
};

}

ParseResult ReplayParser::parse(ReplayByteSource& source,
                                const ParseOptions& options,
                                ParsedReplay& out) {
    out.reset();

    WalkOptions walkOptions{};
    walkOptions.readChunkBytes = options.readChunkBytes;
    walkOptions.decodeRecords = true;
    walkOptions.verifyFileDigest = options.mode != ParseMode::Metadata;
    walkOptions.stopAfterSessionStart = options.mode == ParseMode::Metadata;

    BuildVisitor visitor(options, out);
    ParseResult result = ReplayParser::walk(source, walkOptions, visitor);
    if (visitor.failure() != ParseStatus::Ok) {
        return fail(visitor.failure(), result.offset, visitor.failureDetail(),
                    result.chunkSequence);
    }
    if (!result.ok()) return result;

    if (options.mode == ParseMode::Metadata) {
        ChunkLocation footerLocation{};
        FooterIndex footer{};
        if (ReplayParser::findFooter(source, footerLocation, footer)) {
            out.hasFooter = true;
            out.file.hasFooter = true;
            out.file.footer = footer;
            out.totalFrames = footer.totalFrames;
            out.chunkCount = footer.chunkCount;
            out.matches.clear();
            out.matches.reserve(footer.matches.size());
            for (const MatchIndexEntry& entry : footer.matches) {
                MatchSpan span{};
                span.matchId = entry.matchId;
                span.ordinal = entry.ordinal;
                span.firstTimelineFrame = entry.firstTimelineFrame;
                span.lastTimelineFrame = entry.lastTimelineFrame;
                span.frameCount = entry.frameCount;
                span.roundCount = entry.roundCount;
                span.matchStartOffset = entry.matchStartOffset;
                span.firstInputOffset = entry.firstInputOffset;
                span.winningSideMask = entry.winningSideMask;
                span.independentBootstrap = entry.independentBootstrap;
                span.hasEnd = true;
                span.roster = entry.roster;
                span.ownership = entry.ownership;
                out.matches.push_back(span);
            }
            if (!out.matches.empty())
                out.lastTimelineFrame = out.matches.back().lastTimelineFrame;
        }
    } else if (walkOptions.verifyFileDigest && out.hasFooter) {
        out.fileDigestChecked = true;
        out.fileDigestMatched = true;
        out.computedFileDigest = out.file.footer.fileDigest;
    }

    out.file.prefix = ReplayPrefix{};
    {
        uint8_t prefixBytes[kPrefixBytes];
        if (source.read(0u, prefixBytes, kPrefixBytes))
            decodePrefix(prefixBytes, kPrefixBytes, out.file.prefix);
    }

    if (options.requireFooter && !out.hasFooter) {
        return fail(ParseStatus::MissingFooter, result.offset,
                    "file has no footer index", result.chunkSequence);
    }
    return result;
}

ParseResult ReplayParser::parseFile(const std::wstring& path,
                                    const ParseOptions& options,
                                    ParsedReplay& out) {
    out.reset();
    FileByteSource source;
    if (!source.open(path)) {
        ParseResult result = fail(
            source.error() == OpenError::TooLarge ? ParseStatus::LimitExceeded
                                                  : ParseStatus::IoError,
            0u, openErrorText(source.error()));
        return result;
    }
    ParseResult result = ReplayParser::parse(source, options, out);
    out.identity = source.identity();
    return result;
}

}
