#include "ReplayBattleLogFile.h"

#include <windows.h>

#include <vector>

namespace qclient {
namespace replay {
namespace {

constexpr wchar_t kBattleLogFileName[] = L"battles.q99log";

constexpr uint64_t kMaxReadableBytes = 16ull * 1024ull * 1024ull;

}

std::wstring battleLogPathFor(const std::wstring& replayRoot) {
    if (replayRoot.empty()) return std::wstring();
    std::wstring path = replayRoot;
    const wchar_t last = path.back();
    if (last != L'\\' && last != L'/') path.push_back(L'\\');
    path += kBattleLogFileName;
    return path;
}

bool appendBattleLogRow(const std::wstring& replayRoot,
                        const BattleLogRowV1& row) {
    const std::wstring path = battleLogPathFor(replayRoot);
    if (path.empty()) return false;

    // FILE_APPEND_DATA, never GENERIC_WRITE: the append offset must be set inside the write itself.
    HANDLE file = ::CreateFileW(
        path.c_str(), FILE_APPEND_DATA | FILE_READ_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    std::string payload;
    LARGE_INTEGER size{};
    const bool haveSize = ::GetFileSizeEx(file, &size) != FALSE;
    if (haveSize && size.QuadPart == 0) {
        payload = battleLogHeaderLineV1();
    } else if (haveSize && size.QuadPart > 0) {
        // Never append onto a torn line: the merged line would still decode as a valid row.
        LARGE_INTEGER back{};
        back.QuadPart = size.QuadPart - 1;
        char last = 0;
        DWORD got = 0u;
        if (::SetFilePointerEx(file, back, nullptr, FILE_BEGIN)
            && ::ReadFile(file, &last, 1u, &got, nullptr) && got == 1u
            && last != 0x0A) {
            payload.push_back(0x0A);
        }
    }
    payload += encodeBattleLogRowV1(row);

    DWORD written = 0u;
    const BOOL ok = ::WriteFile(
        file, payload.data(), static_cast<DWORD>(payload.size()), &written,
        nullptr);
    const bool complete = ok != FALSE && written == payload.size();
    ::CloseHandle(file);
    return complete;
}

bool readBattleLogFile(const std::wstring& replayRoot, std::string& text) {
    text.clear();
    const std::wstring path = battleLogPathFor(replayRoot);
    if (path.empty()) return false;

    HANDLE file = ::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0
        || static_cast<uint64_t>(size.QuadPart) > kMaxReadableBytes) {
        ::CloseHandle(file);
        return false;
    }
    text.resize(static_cast<size_t>(size.QuadPart));

    size_t done = 0u;
    while (done < text.size()) {
        const DWORD want = static_cast<DWORD>(
            (text.size() - done) > 0x40000000u ? 0x40000000u
                                               : (text.size() - done));
        DWORD got = 0u;
        if (!::ReadFile(file, &text[done], want, &got, nullptr)) {
            ::CloseHandle(file);
            text.clear();
            return false;
        }
        if (got == 0u) break;
        done += got;
    }
    text.resize(done);
    ::CloseHandle(file);
    return true;
}

}
}
