#include "WidePath.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <climits>
#include <cwchar>
#include <cwctype>
#include <vector>

namespace qplat {
namespace {

constexpr size_t kLongPathThreshold = 240;

bool isSep(wchar_t c) { return c == L'\\' || c == L'/'; }

bool isAbsoluteW(const std::wstring& p) {
    if (p.size() >= 3 && std::iswalpha(static_cast<wint_t>(p[0]))
        && p[1] == L':' && isSep(p[2])) {
        return true;
    }
    return p.size() >= 2 && isSep(p[0]) && isSep(p[1]);
}

std::wstring stripTrailingSeparators(std::wstring p) {
    while (p.size() > 1 && isSep(p.back())) {
        if (p.size() == 3 && p[1] == L':') break;
        p.pop_back();
    }
    return p;
}

size_t rootPrefixLength(const std::wstring& p) {
    const size_t n = p.size();
    size_t i = 0;

    auto skipComponent = [&](size_t from) {
        while (from < n && !isSep(p[from])) ++from;
        while (from < n && isSep(p[from])) ++from;
        return from;
    };

    if (n >= 4 && p.compare(0, 4, L"\\\\?\\") == 0) {
        i = 4;
        if (n >= 8 && _wcsnicmp(p.c_str() + 4, L"UNC\\", 4) == 0) {
            i = skipComponent(8);
            i = skipComponent(i);
            return i;
        }
    } else if (n >= 2 && isSep(p[0]) && isSep(p[1])) {
        i = skipComponent(2);
        i = skipComponent(i);
        return i;
    }

    if (n >= i + 2 && p[i + 1] == L':') {
        i += 2;
        if (i < n && isSep(p[i])) ++i;
        return i;
    }
    if (i == 0 && n >= 1 && isSep(p[0])) return 1;
    return i;
}

std::wstring joinChild(const std::wstring& dir, const std::wstring& child) {
    std::wstring out = dir;
    if (!out.empty() && !isSep(out.back())) out += L'\\';
    out += child;
    return out;
}

std::wstring expandEnvW(const std::wstring& value) {
    if (value.empty()) return value;
    DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    for (int attempt = 0; attempt < 4 && needed != 0; ++attempt) {
        std::vector<wchar_t> buf(needed + 1, L'\0');
        const DWORD written = ExpandEnvironmentStringsW(
            value.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
        if (written == 0) return value;
        if (written <= buf.size()) return std::wstring(buf.data());
        needed = written;
    }
    return value;
}

bool localEnvW(const wchar_t* name, std::wstring& out) {
    out.clear();
    if (!name || !*name) return false;
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<wchar_t> buf(needed + 1, L'\0');
        const DWORD written = GetEnvironmentVariableW(
            name, buf.data(), static_cast<DWORD>(buf.size()));
        if (written == 0) return false;
        if (written < buf.size()) {
            out.assign(buf.data(), written);
            return !out.empty();
        }
        needed = written;
    }
    return false;
}

std::wstring withTrailingSlash(const std::wstring& dir) {
    if (dir.empty()) return dir;
    if (isSep(dir.back())) return dir;
    return dir + L'\\';
}

struct StateRoot {
    std::wstring root;
    bool fallback = false;
    const wchar_t* source = L"exe";
};

StateRoot computeStateRoot() {
    StateRoot chosen;

    std::wstring env;
    if (localEnvW(L"QOH99_NETPLAY_STATE_DIR", env)) {
        std::wstring dir = absolutePathW(expandEnvW(env));
        if (dir.empty()) dir = expandEnvW(env);
        dir = stripTrailingSeparators(dir);
        if (!dir.empty() && ensureDirW(dir) && isDirWritableW(dir)) {
            chosen.root = withTrailingSlash(dir);
            chosen.source = L"env";
            chosen.fallback = false;
            return chosen;
        }
    }

    const std::wstring exeDir = exeDirW();
    if (!exeDir.empty() && isDirWritableW(exeDir)) {
        chosen.root = withTrailingSlash(exeDir);
        chosen.source = L"exe";
        chosen.fallback = false;
        return chosen;
    }

    std::wstring local;
    std::wstring base;
    if (localEnvW(L"LOCALAPPDATA", local) && !local.empty()) {
        base = joinChild(stripTrailingSeparators(local), L"QOH99Netplay");
    } else {
        std::vector<wchar_t> tmp(512, L'\0');
        DWORD n = GetTempPathW(static_cast<DWORD>(tmp.size()), tmp.data());
        if (n >= tmp.size()) {
            tmp.assign(n + 1, L'\0');
            n = GetTempPathW(static_cast<DWORD>(tmp.size()), tmp.data());
        }
        if (n > 0 && n < tmp.size()) {
            base = joinChild(stripTrailingSeparators(std::wstring(tmp.data(), n)),
                             L"QOH99Netplay");
        }
    }
    if (!base.empty()) ensureDirW(base);
    chosen.root = withTrailingSlash(base);
    chosen.source = L"localappdata";
    chosen.fallback = true;
    return chosen;
}

const StateRoot& stateRootCache() {
    static const StateRoot cached = computeStateRoot();
    return cached;
}

}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    if (utf8.size() > static_cast<size_t>(INT_MAX)) return std::wstring();
    if (utf8.find('\0') != std::string::npos) return std::wstring();
    const int inLen = static_cast<int>(utf8.size());
    const int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         utf8.data(), inLen, nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            utf8.data(), inLen, &out[0], need);
    if (written != need) return std::wstring();
    return out;
}

std::string narrow(const std::wstring& utf16) {
    if (utf16.empty()) return std::string();
    if (utf16.size() > static_cast<size_t>(INT_MAX)) return std::string();
    if (utf16.find(L'\0') != std::wstring::npos) return std::string();
    const int inLen = static_cast<int>(utf16.size());
    const int need = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                         utf16.data(), inLen, nullptr, 0,
                                         nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                            utf16.data(), inLen, &out[0], need,
                                            nullptr, nullptr);
    if (written != need) return std::string();
    return out;
}

std::wstring widenAcp(const std::string& acp) {
    if (acp.empty()) return std::wstring();
    if (acp.size() > static_cast<size_t>(INT_MAX)) return std::wstring();
    if (acp.find('\0') != std::string::npos) return std::wstring();
    const int inLen = static_cast<int>(acp.size());
    const int need = MultiByteToWideChar(CP_ACP, 0, acp.data(), inLen,
                                         nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    const int written = MultiByteToWideChar(CP_ACP, 0, acp.data(), inLen,
                                            &out[0], need);
    if (written != need) return std::wstring();
    return out;
}

std::wstring exePathW() {
    std::vector<wchar_t> buf(512, L'\0');
    for (int attempt = 0; attempt < 8; ++attempt) {
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                           static_cast<DWORD>(buf.size()));
        if (n == 0) return std::wstring();
        if (n < buf.size() && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return std::wstring(buf.data(), n);
        buf.assign(buf.size() * 2, L'\0');
    }
    return std::wstring();
}

std::wstring exeDirW() {
    const std::wstring path = exePathW();
    if (path.empty()) return std::wstring();
    const size_t sep = path.find_last_of(L"\\/");
    if (sep == std::wstring::npos) return std::wstring();
    return path.substr(0, sep + 1);
}

std::wstring absolutePathW(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    for (int attempt = 0; attempt < 4 && needed != 0; ++attempt) {
        std::vector<wchar_t> buf(needed + 1, L'\0');
        const DWORD written = GetFullPathNameW(
            path.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
        if (written == 0) return std::wstring();
        if (written < buf.size()) return std::wstring(buf.data(), written);
        needed = written;
    }
    return std::wstring();
}

std::wstring dirOfW(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    const std::wstring trimmed = stripTrailingSeparators(path);
    const size_t sep = trimmed.find_last_of(L"\\/");
    if (sep == std::wstring::npos) return std::wstring();
    if (sep == 0) return std::wstring(1, trimmed[0]);
    std::wstring dir = trimmed.substr(0, sep);
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';
    return dir;
}

std::wstring baseNameW(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    const std::wstring trimmed = stripTrailingSeparators(path);
    const size_t sep = trimmed.find_last_of(L"\\/");
    if (sep == std::wstring::npos) return trimmed;
    return trimmed.substr(sep + 1);
}

bool fileExistsW(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(longPathW(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES
        && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool directoryExistsW(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(longPathW(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES
        && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ensureDirW(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring full = absolutePathW(path);
    if (full.empty()) full = path;
    full = stripTrailingSeparators(full);
    if (full.empty()) return false;
    if (directoryExistsW(full)) return true;

    const size_t start = rootPrefixLength(full);
    for (size_t i = start; i <= full.size(); ++i) {
        if (i != full.size() && !isSep(full[i])) continue;
        const std::wstring part = stripTrailingSeparators(full.substr(0, i));
        if (part.empty() || part.size() <= start) continue;
        if (directoryExistsW(part)) continue;
        if (!CreateDirectoryW(longPathW(part).c_str(), nullptr)) {
            const DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                if (!directoryExistsW(part)) break;
            }
        }
    }
    return directoryExistsW(full);
}

bool isDirWritableW(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (!directoryExistsW(dir)) return false;

    const std::wstring base = stripTrailingSeparators(dir);
    wchar_t stem[96];
    std::swprintf(stem, sizeof(stem) / sizeof(stem[0]),
                  L"qoh99np_probe_%lu_%lu",
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long>(GetTickCount()));

    const std::wstring first = joinChild(base, std::wstring(stem) + L".tmp");
    const std::wstring second = joinChild(base, std::wstring(stem) + L".tmp2");

    HANDLE file = CreateFileW(longPathW(first).c_str(), GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const char probe[] = "qoh99np";
    DWORD written = 0;
    const bool wrote = WriteFile(file, probe, sizeof(probe), &written, nullptr)
                    && written == sizeof(probe);
    CloseHandle(file);
    if (!wrote) {
        DeleteFileW(longPathW(first).c_str());
        return false;
    }

    const bool renamed = MoveFileExW(longPathW(first).c_str(),
                                     longPathW(second).c_str(),
                                     MOVEFILE_REPLACE_EXISTING) != FALSE;
    const std::wstring& created = renamed ? second : first;
    const bool deleted = DeleteFileW(longPathW(created).c_str()) != FALSE;
    return renamed && deleted;
}

std::wstring longPathW(const std::wstring& path) {
    if (path.size() < kLongPathThreshold) return path;
    if (path.compare(0, 4, L"\\\\?\\") == 0) return path;
    if (path.compare(0, 4, L"\\\\.\\") == 0) return path;
    if (!isAbsoluteW(path)) return path;
    if (isSep(path[0]) && isSep(path[1]))
        return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring stateRootW()          { return stateRootCache().root; }
bool stateRootUsedFallback()       { return stateRootCache().fallback; }
const wchar_t* stateRootSourceName() { return stateRootCache().source; }

std::wstring logsRootW()   { return stateRootW() + L"logs"; }
std::wstring replayRootW() { return stateRootW() + L"replays"; }

std::wstring makeSessionDirW() {
    const std::wstring logs = logsRootW();
    ensureDirW(logs);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[80];
    std::swprintf(name, sizeof(name) / sizeof(name[0]),
                  L"session_%04d%02d%02d_%02d%02d%02d_%03d_p%lu",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond, st.wMilliseconds,
                  static_cast<unsigned long>(GetCurrentProcessId()));

    const std::wstring dir = joinChild(logs, name);
    ensureDirW(dir);
    return dir;
}

bool atomicReplaceFileW(const std::wstring& from, const std::wstring& to) {
    if (from.empty() || to.empty()) return false;
    return MoveFileExW(longPathW(from).c_str(), longPathW(to).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
           != FALSE;
}

bool moveToTrashDirW(const std::wstring& file, const std::wstring& trashDir,
                     std::wstring& finalPath) {
    finalPath.clear();
    if (file.empty() || trashDir.empty()) return false;
    if (!fileExistsW(file)) return false;
    if (!ensureDirW(trashDir)) return false;

    std::wstring stem = baseNameW(file);
    if (stem.empty()) stem = L"file";

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[64];
    std::swprintf(stamp, sizeof(stamp) / sizeof(stamp[0]),
                  L"%04d%02d%02d_%02d%02d%02d_%03d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond, st.wMilliseconds);

    const std::wstring dir = stripTrailingSeparators(trashDir);
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        std::wstring leaf = stem + L'.' + stamp;
        if (attempt != 0) leaf += L'.' + std::to_wstring(attempt);
        leaf += L".bak";
        const std::wstring candidate = joinChild(dir, leaf);
        if (fileExistsW(candidate) || directoryExistsW(candidate)) continue;
        if (MoveFileExW(longPathW(file).c_str(), longPathW(candidate).c_str(),
                        MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            finalPath = candidate;
            return true;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS)
            return false;
    }
    return false;
}

bool originalNameFromTrashLeafW(const std::wstring& leaf,
                                const std::wstring& extension,
                                std::wstring& original) {
    original.clear();
    if (leaf.empty() || extension.empty()) return false;

    auto lower = [](std::wstring value) {
        for (wchar_t& c : value) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
        }
        return value;
    };
    const std::wstring lowerLeaf = lower(leaf);
    if (lowerLeaf.size() < 5u
        || lowerLeaf.compare(lowerLeaf.size() - 4u, 4u, L".bak") != 0) {
        return false;
    }

    const std::wstring needle = lower(extension);
    const size_t at = lowerLeaf.rfind(needle);
    if (at == std::wstring::npos || at == 0u) return false;
    const size_t end = at + needle.size();
    if (end >= lowerLeaf.size() || lowerLeaf[end] != L'.') return false;
    original = leaf.substr(0, end);
    return true;
}

}
