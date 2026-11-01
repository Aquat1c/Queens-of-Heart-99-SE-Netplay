#pragma once

#include <cstdint>
#include <string>

namespace qclient {

enum class SessionLogArchiveStatus : uint8_t {
    Archived,
    Deferred,
    InUse,
    SourceRetained,
    Unavailable,
    InvalidSource,
    Failed,
};

struct SessionLogArchiveResult {
    SessionLogArchiveStatus status = SessionLogArchiveStatus::InvalidSource;
    std::string archivePath;
};

struct SessionLogWriterLease {
    std::string markerPath;
};

SessionLogWriterLease acquireSessionLogWriterLease(
    const std::string& sessionDirUtf8, uint32_t processId = 0u);

SessionLogWriterLease acquirePendingSessionLogWriterLease(
    const std::string& sessionDirUtf8);

void releaseSessionLogWriterLease(SessionLogWriterLease& lease);

bool sessionLogArchiveAvailable();

SessionLogArchiveResult archiveClosedSessionLogDirectory(
    const std::string& sessionDirUtf8);

struct SessionLogArchiveRecovery {
    uint32_t inspected = 0u;
    uint32_t archived = 0u;
    uint32_t deferred = 0u;
    uint32_t retained = 0u;
};

SessionLogArchiveRecovery recoverAbandonedSessionLogDirectories(
    const std::string& logsRootUtf8);

const char* sessionLogArchiveStatusText(SessionLogArchiveStatus status);

constexpr uint32_t kSessionLogRetentionDays = 2u;

struct SessionLogRetentionResult {
    uint32_t inspected = 0u;
    uint32_t removedDirectories = 0u;
    uint32_t removedArchives = 0u;
    uint32_t keptInUse = 0u;
    uint32_t failed = 0u;
};

SessionLogRetentionResult pruneAgedSessionLogs(
    const std::string& logsRootUtf8,
    uint32_t maxAgeDays = kSessionLogRetentionDays);

}
