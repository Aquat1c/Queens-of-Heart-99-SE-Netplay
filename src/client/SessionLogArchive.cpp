#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../third_party/miniz/miniz.h"
#include "../platform/WidePath.h"
#include "SessionLogArchive.h"

namespace qclient {
namespace {

constexpr const wchar_t* kWriterMarkerPrefix = L"._qoh99_writer_";
constexpr const wchar_t* kArchiveMarkerInfix = L"._qoh99_archive_";
constexpr const wchar_t* kWriterMarkerSuffix = L".lock";

enum class WriterMarkerState : uint8_t {
    Absent,
    Inactive,
    ActiveOrUnknown,
};

bool removeTree(const std::wstring& dir, uint32_t depth);

bool isInternalMarkerLeaf(const std::wstring& leaf) {
    return leaf.rfind(L"._qoh99_", 0u) == 0u;
}

std::wstring appendChild(const std::wstring& parent, const wchar_t* child) {
    if (parent.empty()) return {};
    if (parent.back() == L'\\' || parent.back() == L'/')
        return parent + child;
    return parent + L"\\" + child;
}

struct SessionFile {
    std::wstring fullPath;
    std::string entryName;
    uint64_t size = 0u;
};

struct HandleReader {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool failed = false;
};

size_t handleReadCallback(void* opaque, mz_uint64 offset, void* buf, size_t n) {
    auto* reader = static_cast<HandleReader*>(opaque);
    if (reader == nullptr || reader->handle == INVALID_HANDLE_VALUE) return 0u;
    LARGE_INTEGER move{};
    move.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(reader->handle, move, nullptr, FILE_BEGIN)) {
        reader->failed = true;
        return 0u;
    }
    size_t total = 0u;
    while (total < n) {
        const DWORD want = static_cast<DWORD>(
            (std::min)(static_cast<size_t>(1u << 20), n - total));
        DWORD got = 0u;
        if (!ReadFile(reader->handle, static_cast<char*>(buf) + total, want,
                      &got, nullptr)) {
            reader->failed = true;
            return total;
        }
        if (got == 0u) break;
        total += got;
    }
    return total;
}

struct VerifySink {
    HANDLE source = INVALID_HANDLE_VALUE;
    uint64_t compared = 0u;
    bool mismatch = false;
};

size_t verifyWriteCallback(void* opaque, mz_uint64 offset, const void* buf,
                           size_t n) {
    auto* sink = static_cast<VerifySink*>(opaque);
    if (sink == nullptr || sink->mismatch) return 0u;
    if (offset != sink->compared) {
        sink->mismatch = true;
        return 0u;
    }
    std::vector<char> expected(n);
    size_t total = 0u;
    while (total < n) {
        DWORD got = 0u;
        if (!ReadFile(sink->source, expected.data() + total,
                      static_cast<DWORD>(n - total), &got, nullptr)
            || got == 0u) {
            sink->mismatch = true;
            return 0u;
        }
        total += got;
    }
    if (std::memcmp(expected.data(), buf, n) != 0) {
        sink->mismatch = true;
        return 0u;
    }
    sink->compared += n;
    return n;
}

HANDLE openForRead(const std::wstring& path) {
    return CreateFileW(qplat::longPathW(path).c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                       nullptr);
}

bool enumerateSessionFiles(const std::wstring& root,
                           const std::wstring& relative,
                           std::vector<SessionFile>& out) {
    const std::wstring dir = relative.empty() ? root : root + L"\\" + relative;
    WIN32_FIND_DATAW find{};
    const HANDLE search = FindFirstFileExW(
        qplat::longPathW(dir + L"\\*").c_str(), FindExInfoBasic, &find,
        FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    bool ok = true;
    do {
        const std::wstring leaf = find.cFileName;
        if (leaf == L"." || leaf == L"..") continue;
        const std::wstring childRelative =
            relative.empty() ? leaf : relative + L"\\" + leaf;
        if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
            // Reparse points are not followed: a junction would archive, then DELETE, its target.
            if ((find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
                continue;
            }
            if (!enumerateSessionFiles(root, childRelative, out)) ok = false;
            continue;
        }
        if (isInternalMarkerLeaf(leaf)) continue;
        SessionFile entry{};
        entry.fullPath = root + L"\\" + childRelative;
        std::wstring forward = childRelative;
        for (wchar_t& ch : forward) {
            if (ch == L'\\') ch = L'/';
        }
        entry.entryName = qplat::narrow(forward);
        if (entry.entryName.empty()) {
            ok = false;
            continue;
        }
        entry.size = (static_cast<uint64_t>(find.nFileSizeHigh) << 32)
            | find.nFileSizeLow;
        out.push_back(std::move(entry));
    } while (FindNextFileW(search, &find));
    const DWORD closeError = GetLastError();
    FindClose(search);
    if (closeError != ERROR_NO_MORE_FILES) ok = false;
    return ok;
}

bool hasSessionLeafName(const std::wstring& path) {
    const std::wstring leaf = qplat::baseNameW(path);
    return leaf.size() > 8u && leaf.compare(0u, 8u, L"session_") == 0;
}

std::wstring sessionSourcePath(const std::string& sessionDirUtf8) {
    return qplat::absolutePathW(qplat::widen(sessionDirUtf8));
}

bool processCreationStamp(const uint32_t requestedPid, uint64_t& stamp) {
    const DWORD pid = requestedPid == 0u ? GetCurrentProcessId() : requestedPid;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, pid);
    if (process == nullptr) return false;
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user)
        != FALSE;
    CloseHandle(process);
    if (!ok) return false;
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    stamp = value.QuadPart;
    return stamp != 0u;
}

std::wstring writerMarkerPath(const std::wstring& source, const uint32_t pid,
                              const uint64_t stamp) {
    return appendChild(source, (std::wstring(kWriterMarkerPrefix)
        + std::to_wstring(pid) + L"_" + std::to_wstring(stamp)
        + kWriterMarkerSuffix).c_str());
}

std::wstring archiveMarkerPath(const std::wstring& source, const uint32_t pid,
                               const uint64_t stamp) {
    return source + kArchiveMarkerInfix + std::to_wstring(pid) + L"_"
        + std::to_wstring(stamp) + kWriterMarkerSuffix;
}

bool parseUnsignedDecimal(const std::wstring& text, uint64_t& out) {
    if (text.empty()) return false;
    uint64_t value = 0u;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
        const uint64_t digit = static_cast<uint64_t>(ch - L'0');
        if (value > (UINT64_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

bool parseProcessMarker(const std::wstring& leaf, const std::wstring& prefixText,
                        uint32_t& pid, uint64_t& stamp) {
    const size_t prefix = prefixText.size();
    const size_t suffix = std::wcslen(kWriterMarkerSuffix);
    if (leaf.size() <= prefix + suffix
        || leaf.compare(0u, prefix, prefixText) != 0
        || leaf.compare(leaf.size() - suffix, suffix, kWriterMarkerSuffix)
            != 0) {
        return false;
    }
    const std::wstring body = leaf.substr(prefix, leaf.size() - prefix - suffix);
    const size_t divider = body.find(L'_');
    if (divider == std::wstring::npos || body.find(L'_', divider + 1u)
        != std::wstring::npos) {
        return false;
    }
    uint64_t parsedPid = 0u;
    uint64_t parsedStamp = 0u;
    if (!parseUnsignedDecimal(body.substr(0u, divider), parsedPid)
        || !parseUnsignedDecimal(body.substr(divider + 1u), parsedStamp)
        || parsedPid == 0u || parsedPid > MAXDWORD || parsedStamp == 0u) {
        return false;
    }
    pid = static_cast<uint32_t>(parsedPid);
    stamp = parsedStamp;
    return true;
}

bool writerProcessIsActiveOrUnknown(const uint32_t pid, const uint64_t stamp) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, pid);
    if (process == nullptr) {
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }
    DWORD exitCode = STILL_ACTIVE;
    bool activeOrUnknown = GetExitCodeProcess(process, &exitCode) == FALSE
        || exitCode == STILL_ACTIVE;
    if (activeOrUnknown) {
        uint64_t actualStamp = 0u;
        if (processCreationStamp(pid, actualStamp) && actualStamp != stamp) {
            activeOrUnknown = false;
        }
    }
    CloseHandle(process);
    return activeOrUnknown;
}

WriterMarkerState writerMarkerState(const std::wstring& source) {
    const std::wstring pattern = appendChild(source, L"._qoh99_writer_*");
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(qplat::longPathW(pattern).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND
            ? WriterMarkerState::Absent
            : WriterMarkerState::ActiveOrUnknown;
    }
    WriterMarkerState state = WriterMarkerState::Inactive;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
            state = WriterMarkerState::ActiveOrUnknown;
            break;
        }
        uint32_t pid = 0u;
        uint64_t stamp = 0u;
        if (!parseProcessMarker(found.cFileName, kWriterMarkerPrefix, pid, stamp)
            || writerProcessIsActiveOrUnknown(pid, stamp)) {
            state = WriterMarkerState::ActiveOrUnknown;
            break;
        }
    } while (FindNextFileW(search, &found) != FALSE);
    const DWORD searchError = GetLastError();
    FindClose(search);
    if (state != WriterMarkerState::ActiveOrUnknown
        && searchError != ERROR_NO_MORE_FILES) {
        return WriterMarkerState::ActiveOrUnknown;
    }
    return state;
}

WriterMarkerState archiveMarkerState(const std::wstring& source) {
    const std::wstring prefix = qplat::baseNameW(source) + kArchiveMarkerInfix;
    const std::wstring pattern = source + kArchiveMarkerInfix + L"*";
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(qplat::longPathW(pattern).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND
            ? WriterMarkerState::Absent
            : WriterMarkerState::ActiveOrUnknown;
    }
    WriterMarkerState state = WriterMarkerState::Inactive;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
            state = WriterMarkerState::ActiveOrUnknown;
            break;
        }
        uint32_t pid = 0u;
        uint64_t stamp = 0u;
        if (!parseProcessMarker(found.cFileName, prefix, pid, stamp)
            || writerProcessIsActiveOrUnknown(pid, stamp)) {
            state = WriterMarkerState::ActiveOrUnknown;
            break;
        }
    } while (FindNextFileW(search, &found) != FALSE);
    const DWORD searchError = GetLastError();
    FindClose(search);
    if (state != WriterMarkerState::ActiveOrUnknown
        && searchError != ERROR_NO_MORE_FILES) {
        return WriterMarkerState::ActiveOrUnknown;
    }
    return state;
}

bool writeLeaseMarker(const std::wstring& marker) {
    HANDLE file = CreateFileW(qplat::longPathW(marker).c_str(), GENERIC_WRITE,
                              0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const char payload[] = "QOH99 session writer lease\r\n";
    DWORD written = 0u;
    const bool wrote = WriteFile(file, payload,
                                 static_cast<DWORD>(sizeof(payload) - 1u),
                                 &written, nullptr) != FALSE
        && written == sizeof(payload) - 1u;
    CloseHandle(file);
    if (!wrote) DeleteFileW(qplat::longPathW(marker).c_str());
    return wrote;
}

SessionLogWriterLease acquireArchiveLease(const std::wstring& source,
                                          const uint32_t processId) {
    SessionLogWriterLease lease{};
    uint64_t stamp = 0u;
    if (!processCreationStamp(processId, stamp)) return lease;
    const std::wstring marker = archiveMarkerPath(source, processId, stamp);
    if (!writeLeaseMarker(marker)) return lease;
    lease.markerPath = qplat::narrow(marker);
    return lease;
}

bool writeArchivePart(const std::wstring& partPath,
                      const std::vector<SessionFile>& files) {
    FILE* out = _wfopen(qplat::longPathW(partPath).c_str(), L"wb");
    if (out == nullptr) return false;

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_cfile(&zip, out, 0)) {
        std::fclose(out);
        return false;
    }
    bool ok = true;
    for (const SessionFile& file : files) {
        HandleReader reader{};
        reader.handle = openForRead(file.fullPath);
        if (reader.handle == INVALID_HANDLE_VALUE) {
            ok = false;
            break;
        }
        const mz_bool added = mz_zip_writer_add_read_buf_callback(
            &zip, file.entryName.c_str(), &handleReadCallback, &reader,
            file.size, nullptr, nullptr, 0, MZ_BEST_COMPRESSION, nullptr, 0,
            nullptr, 0);
        CloseHandle(reader.handle);
        if (!added || reader.failed) {
            ok = false;
            break;
        }
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&zip) != MZ_FALSE;
    if (!mz_zip_writer_end(&zip)) ok = false;
    if (std::fclose(out) != 0) ok = false;
    return ok;
}

bool verifyArchivePart(const std::wstring& partPath,
                       const std::vector<SessionFile>& files) {
    FILE* in = _wfopen(qplat::longPathW(partPath).c_str(), L"rb");
    if (in == nullptr) return false;
    LARGE_INTEGER size{};
    {
        const HANDLE probe = openForRead(partPath);
        if (probe == INVALID_HANDLE_VALUE || !GetFileSizeEx(probe, &size)) {
            if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
            std::fclose(in);
            return false;
        }
        CloseHandle(probe);
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, in,
                                  static_cast<mz_uint64>(size.QuadPart), 0)) {
        std::fclose(in);
        return false;
    }
    bool ok = true;
    for (const SessionFile& file : files) {
        const int index = mz_zip_reader_locate_file(
            &zip, file.entryName.c_str(), nullptr, 0);
        if (index < 0) {
            ok = false;
            break;
        }
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(index), &stat)
            || stat.m_uncomp_size != file.size) {
            ok = false;
            break;
        }
        VerifySink sink{};
        sink.source = openForRead(file.fullPath);
        if (sink.source == INVALID_HANDLE_VALUE) {
            ok = false;
            break;
        }
        const mz_bool extracted = mz_zip_reader_extract_to_callback(
            &zip, static_cast<mz_uint>(index), &verifyWriteCallback, &sink, 0);
        CloseHandle(sink.source);
        if (!extracted || sink.mismatch || sink.compared != file.size) {
            ok = false;
            break;
        }
    }
    if (!mz_zip_reader_end(&zip)) ok = false;
    std::fclose(in);
    return ok;
}

void writeArchiveFailureNote(const std::wstring& source,
                             const std::string& reason) {
    const std::wstring notePath = appendChild(source, L"archive_failure.txt");
    const HANDLE note = CreateFileW(
        qplat::longPathW(notePath).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (note == INVALID_HANDLE_VALUE) return;
    DWORD written = 0u;
    (void)WriteFile(note, reason.data(), static_cast<DWORD>(reason.size()),
                    &written, nullptr);
    CloseHandle(note);
}

SessionLogArchiveResult result(SessionLogArchiveStatus status,
                               const std::wstring& archive = {}) {
    SessionLogArchiveResult out{};
    out.status = status;
    out.archivePath = qplat::narrow(archive);
    return out;
}

}

SessionLogWriterLease acquireSessionLogWriterLease(
    const std::string& sessionDirUtf8, const uint32_t requestedPid) {
    SessionLogWriterLease lease{};
    const std::wstring source = sessionSourcePath(sessionDirUtf8);
    if (source.empty() || !hasSessionLeafName(source)
        || !qplat::directoryExistsW(source)) {
        return lease;
    }
    const uint32_t pid = requestedPid == 0u
        ? GetCurrentProcessId() : requestedPid;
    uint64_t stamp = 0u;
    if (!processCreationStamp(pid, stamp)) return lease;
    const std::wstring marker = writerMarkerPath(source, pid, stamp);
    if (!writeLeaseMarker(marker)) return lease;
    lease.markerPath = qplat::narrow(marker);
    return lease;
}

SessionLogWriterLease acquirePendingSessionLogWriterLease(
    const std::string& sessionDirUtf8) {
    SessionLogWriterLease lease{};
    const std::wstring source = sessionSourcePath(sessionDirUtf8);
    if (source.empty() || !hasSessionLeafName(source)
        || !qplat::directoryExistsW(source)) {
        return lease;
    }
    const std::wstring marker = appendChild(
        source, L"._qoh99_writer_launch_pending.lock");
    if (!writeLeaseMarker(marker)) return lease;
    lease.markerPath = qplat::narrow(marker);
    return lease;
}

void releaseSessionLogWriterLease(SessionLogWriterLease& lease) {
    if (!lease.markerPath.empty()) {
        const std::wstring marker = qplat::widen(lease.markerPath);
        if (!marker.empty())
            DeleteFileW(qplat::longPathW(marker).c_str());
    }
    lease.markerPath.clear();
}

bool sessionLogArchiveAvailable() {
    return true;
}

SessionLogArchiveResult archiveClosedSessionLogDirectory(
    const std::string& sessionDirUtf8) {
    const std::wstring source = sessionSourcePath(sessionDirUtf8);
    if (source.empty() || !hasSessionLeafName(source)
        || !qplat::directoryExistsW(source)) {
        return result(SessionLogArchiveStatus::InvalidSource);
    }

    const std::wstring archive = source + L".zip";
    if (writerMarkerState(source) == WriterMarkerState::ActiveOrUnknown
        || archiveMarkerState(source) == WriterMarkerState::ActiveOrUnknown) {
        return result(SessionLogArchiveStatus::InUse, archive);
    }
    if (qplat::fileExistsW(archive)) {
        return result(SessionLogArchiveStatus::SourceRetained, archive);
    }

    SessionLogWriterLease archiveLease = acquireArchiveLease(
        source, GetCurrentProcessId());
    if (archiveLease.markerPath.empty()) {
        return result(SessionLogArchiveStatus::Failed, archive);
    }

    const std::wstring part = archive + L".part";
    const auto fail = [&](const char* reason) {
        DeleteFileW(qplat::longPathW(part).c_str());
        writeArchiveFailureNote(source, reason);
        releaseSessionLogWriterLease(archiveLease);
        return result(SessionLogArchiveStatus::Failed, archive);
    };

    std::vector<SessionFile> files;
    if (!enumerateSessionFiles(source, std::wstring(), files)) {
        return fail("could not enumerate the session folder");
    }
    DeleteFileW(qplat::longPathW(part).c_str());
    if (!writeArchivePart(part, files)) {
        return fail("could not write the archive");
    }
    if (!verifyArchivePart(part, files)) {
        return fail("the archive did not verify against the source files");
    }
    if (!qplat::atomicReplaceFileW(part, archive)) {
        return fail("could not move the verified archive into place");
    }

    archiveLease.markerPath.clear();
    if (!removeTree(source, 0u)) {
        return result(SessionLogArchiveStatus::SourceRetained, archive);
    }
    return result(SessionLogArchiveStatus::Archived, archive);
}

SessionLogArchiveRecovery recoverAbandonedSessionLogDirectories(
    const std::string& logsRootUtf8) {
    SessionLogArchiveRecovery recovery{};
    const std::wstring root = qplat::absolutePathW(qplat::widen(logsRootUtf8));
    if (root.empty() || !qplat::directoryExistsW(root)) return recovery;

    const std::wstring pattern = appendChild(root, L"session_*");
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(qplat::longPathW(pattern).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return recovery;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) continue;
        const std::wstring source = appendChild(root, found.cFileName);
        if (!hasSessionLeafName(source)) continue;
        const WriterMarkerState writerState = writerMarkerState(source);
        const WriterMarkerState archiveState = archiveMarkerState(source);
        if (writerState == WriterMarkerState::Absent
            && archiveState == WriterMarkerState::Absent) continue;
        ++recovery.inspected;
        if (writerState == WriterMarkerState::ActiveOrUnknown
            || archiveState == WriterMarkerState::ActiveOrUnknown) {
            ++recovery.deferred;
            continue;
        }
        const SessionLogArchiveResult archived =
            archiveClosedSessionLogDirectory(qplat::narrow(source));
        if (archived.status == SessionLogArchiveStatus::Archived) {
            ++recovery.archived;
        } else if (archived.status == SessionLogArchiveStatus::InUse
                   || archived.status == SessionLogArchiveStatus::Deferred) {
            ++recovery.deferred;
        } else {
            ++recovery.retained;
        }
    } while (FindNextFileW(search, &found) != FALSE);
    FindClose(search);
    return recovery;
}

namespace {

constexpr uint64_t kFileTimeTicksPerDay = 24ull * 60ull * 60ull * 10000000ull;
constexpr uint32_t kRetentionMaxDepth = 8u;

uint64_t fileTimeValue(const FILETIME& time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

std::wstring appendChildPath(const std::wstring& parent,
                             const std::wstring& child) {
    return appendChild(parent, child.c_str());
}

bool isDotEntry(const wchar_t* name) {
    return name[0] == L'.'
        && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

bool newestWriteBelow(const std::wstring& dir, uint32_t depth,
                      uint64_t& newest) {
    if (depth > kRetentionMaxDepth) return false;
    WIN32_FIND_DATAW found{};
    const std::wstring pattern = appendChild(dir, L"*");
    HANDLE search = FindFirstFileW(qplat::longPathW(pattern).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    bool ok = true;
    do {
        if (isDotEntry(found.cFileName)) continue;
        const uint64_t stamp = fileTimeValue(found.ftLastWriteTime);
        if (stamp > newest) newest = stamp;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) continue;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
            ok = false;
            continue;
        }
        if (!newestWriteBelow(appendChildPath(dir, found.cFileName),
                              depth + 1u, newest)) {
            ok = false;
        }
    } while (FindNextFileW(search, &found) != FALSE);
    const DWORD searchError = GetLastError();
    FindClose(search);
    return ok && searchError == ERROR_NO_MORE_FILES;
}

bool removeTree(const std::wstring& dir, uint32_t depth) {
    if (depth > kRetentionMaxDepth) return false;
    WIN32_FIND_DATAW found{};
    const std::wstring pattern = appendChild(dir, L"*");
    HANDLE search = FindFirstFileW(qplat::longPathW(pattern).c_str(), &found);
    bool ok = true;
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (isDotEntry(found.cFileName)) continue;
            const std::wstring child =
                appendChildPath(dir, found.cFileName);
            const bool isDir =
                (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
            const bool isLink =
                (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0u) {
                SetFileAttributesW(qplat::longPathW(child).c_str(),
                                   FILE_ATTRIBUTE_NORMAL);
            }
            if (isDir && !isLink) {
                if (!removeTree(child, depth + 1u)) ok = false;
            } else if (isDir) {
                if (RemoveDirectoryW(qplat::longPathW(child).c_str()) == FALSE)
                    ok = false;
            } else if (DeleteFileW(qplat::longPathW(child).c_str()) == FALSE) {
                ok = false;
            }
        } while (FindNextFileW(search, &found) != FALSE);
        FindClose(search);
    }
    if (!ok) return false;
    return RemoveDirectoryW(qplat::longPathW(dir).c_str()) != FALSE;
}

bool hasArchiveExtension(const std::wstring& leaf) {
    if (leaf.size() < 4u) return false;
    const std::wstring tail = leaf.substr(leaf.size() - 4u);
    return _wcsicmp(tail.c_str(), L".zip") == 0;
}

}

SessionLogRetentionResult pruneAgedSessionLogs(
    const std::string& logsRootUtf8, const uint32_t maxAgeDays) {
    SessionLogRetentionResult retention{};
    if (maxAgeDays == 0u) return retention;
    const std::wstring root = qplat::absolutePathW(qplat::widen(logsRootUtf8));
    if (root.empty() || !qplat::directoryExistsW(root)) return retention;

    FILETIME nowFileTime{};
    GetSystemTimeAsFileTime(&nowFileTime);
    const uint64_t now = fileTimeValue(nowFileTime);
    const uint64_t window =
        static_cast<uint64_t>(maxAgeDays) * kFileTimeTicksPerDay;
    if (now <= window) return retention;
    const uint64_t cutoff = now - window;

    const std::wstring pattern = appendChild(root, L"session_*");
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(qplat::longPathW(pattern).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return retention;
    do {
        if (isDotEntry(found.cFileName)) continue;
        const std::wstring entry = appendChildPath(root, found.cFileName);
        if (!hasSessionLeafName(entry)) continue;
        const bool isDir =
            (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
            continue;
        if (!isDir && !hasArchiveExtension(found.cFileName)) continue;
        ++retention.inspected;

        if (!isDir) {
            if (fileTimeValue(found.ftLastWriteTime) >= cutoff) continue;
            if (DeleteFileW(qplat::longPathW(entry).c_str()) != FALSE)
                ++retention.removedArchives;
            else
                ++retention.failed;
            continue;
        }

        if (writerMarkerState(entry) == WriterMarkerState::ActiveOrUnknown
            || archiveMarkerState(entry)
                   == WriterMarkerState::ActiveOrUnknown) {
            ++retention.keptInUse;
            continue;
        }
        uint64_t newest = fileTimeValue(found.ftLastWriteTime);
        if (!newestWriteBelow(entry, 0u, newest)) {
            ++retention.keptInUse;
            continue;
        }
        if (newest >= cutoff) continue;
        if (removeTree(entry, 0u)) ++retention.removedDirectories;
        else ++retention.failed;
    } while (FindNextFileW(search, &found) != FALSE);
    FindClose(search);
    return retention;
}

const char* sessionLogArchiveStatusText(const SessionLogArchiveStatus status) {
    switch (status) {
    case SessionLogArchiveStatus::Archived:
        return "Session archive saved";
    case SessionLogArchiveStatus::Deferred:
        return "Session archive is finishing in the background";
    case SessionLogArchiveStatus::InUse:
        return "Session folder kept while the game is still writing";
    case SessionLogArchiveStatus::SourceRetained:
        return "Session folder kept beside its archive";
    case SessionLogArchiveStatus::Unavailable:
        return "Session folder kept because ZIP compression is unavailable";
    case SessionLogArchiveStatus::InvalidSource:
        return "Session folder was not eligible for archiving";
    case SessionLogArchiveStatus::Failed:
        return "Session folder kept because ZIP compression failed";
    }
    return "Session folder kept";
}

}
