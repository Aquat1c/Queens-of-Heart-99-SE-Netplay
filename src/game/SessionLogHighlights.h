#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace qgame {

constexpr const char* kDllLogLaunchBanner = "qoh99_netplay.dll  built";

inline std::vector<std::string> selectHighlightLines(const std::string& text,
                                                     const char* startMarker,
                                                     const char* const* patterns,
                                                     std::size_t patternCount,
                                                     std::size_t maxLines) {
    std::size_t begin = 0u;
    if (startMarker != nullptr && *startMarker != '\0') {
        const std::size_t at = text.rfind(startMarker);
        if (at != std::string::npos) begin = at;
        const std::size_t process = text.rfind("[logger] process-start ");
        if (process != std::string::npos && process > begin) begin = process;
    }
    std::vector<std::string> picked;
    std::size_t pos = begin;
    while (pos < text.size()) {
        const std::size_t newline = text.find('\n', pos);
        std::string line = text.substr(pos, newline == std::string::npos
                                                ? std::string::npos
                                                : newline - pos);
        pos = newline == std::string::npos ? text.size() : newline + 1u;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty()) continue;
        for (std::size_t i = 0; i < patternCount; ++i) {
            if (patterns[i] != nullptr && line.find(patterns[i]) != std::string::npos) {
                picked.push_back(line);
                break;
            }
        }
    }
    if (maxLines != 0u && picked.size() > maxLines)
        picked.erase(picked.begin(),
                     picked.begin() + static_cast<std::ptrdiff_t>(picked.size() - maxLines));
    return picked;
}

inline std::string readLogTail(const std::string& path) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::string();
    LARGE_INTEGER size{};
    std::string text;
    if (GetFileSizeEx(file, &size)) {
        const LONGLONG cap = 256LL * 1024LL;
        LARGE_INTEGER start{};
        start.QuadPart = size.QuadPart > cap ? size.QuadPart - cap : 0;
        if (SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) {
            const DWORD want = static_cast<DWORD>(size.QuadPart - start.QuadPart);
            text.assign(want, '\0');
            DWORD read = 0;
            if (want == 0 || ReadFile(file, &text[0], want, &read, nullptr))
                text.resize(read);
            else
                text.clear();
        }
    }
    CloseHandle(file);
    return text;
}

inline std::vector<std::string> readLogHighlights(const std::string& path,
                                                  const char* startMarker,
                                                  const char* const* patterns,
                                                  std::size_t patternCount,
                                                  std::size_t maxLines) {
    return selectHighlightLines(readLogTail(path), startMarker, patterns,
                                patternCount, maxLines);
}

}
