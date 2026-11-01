/* QOH writes failure lines to SYSTEM\QOHerror.txt through sub_45C5C0, falling back to <Windows>\QOHerror.txt; text is Shift-JIS (code page 932). */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace qgame {

inline std::vector<std::string> splitGameErrorJournal(const std::string& bytes) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : bytes) {
        if (c == '\n' || c == '\r') {
            if (!current.empty()) lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

inline std::string gameErrorTextUtf8(const char* text) {
    if (text == nullptr) return std::string("(null)");
    std::string out(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    if (out.empty()) return out;
    const int wide = MultiByteToWideChar(932, 0, out.c_str(), -1, nullptr, 0);
    if (wide <= 0) return out;
    std::wstring w(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(932, 0, out.c_str(), -1, &w[0], wide);
    const int narrow = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr,
                                           0, nullptr, nullptr);
    if (narrow <= 0) return out;
    std::string utf8(static_cast<size_t>(narrow), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &utf8[0], narrow, nullptr,
                        nullptr);
    utf8.resize(std::strlen(utf8.c_str()));
    return utf8;
}

struct GameErrorJournalFile {
    std::string path;
    std::vector<std::string> lines;
    bool present = false;
    bool fromThisRun = false;
};

inline GameErrorJournalFile readGameErrorJournalFile(const std::string& path,
                                                     unsigned long long sinceFileTime) {
    GameErrorJournalFile out;
    out.path = path;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attributes)
        || (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return out;
    out.present = true;
    const unsigned long long written =
        (static_cast<unsigned long long>(attributes.ftLastWriteTime.dwHighDateTime) << 32)
        | attributes.ftLastWriteTime.dwLowDateTime;
    const unsigned long long slack = 5ull * 10000000ull;
    if (sinceFileTime != 0ull && written + slack < sinceFileTime) return out;
    out.fromThisRun = true;
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return out;
    std::string bytes(64u * 1024u, '\0');
    DWORD read = 0;
    if (ReadFile(file, &bytes[0], static_cast<DWORD>(bytes.size()), &read, nullptr))
        bytes.resize(read);
    else
        bytes.clear();
    CloseHandle(file);
    for (const std::string& line : splitGameErrorJournal(bytes))
        out.lines.push_back(gameErrorTextUtf8(line.c_str()));
    return out;
}

inline std::vector<std::string> gameErrorJournalCandidates(const std::string& gameDirectory) {
    std::vector<std::string> paths;
    std::string dir = gameDirectory;
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') dir.push_back('\\');
    paths.push_back(dir + "SYSTEM\\QOHerror.txt");
    char windows[MAX_PATH] = {};
    if (GetWindowsDirectoryA(windows, MAX_PATH) != 0)
        paths.push_back(std::string(windows) + "\\QOHerror.txt");
    return paths;
}

}
