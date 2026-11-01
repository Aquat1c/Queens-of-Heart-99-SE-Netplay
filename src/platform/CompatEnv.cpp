#include "CompatEnv.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <utility>

namespace qplat {
namespace {

wchar_t asciiLower(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

bool asciiIEqual(const wchar_t* a, size_t aLen, const wchar_t* b) {
    size_t i = 0;
    for (; i < aLen; ++i) {
        if (b[i] == L'\0') return false;
        if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    }
    return b[i] == L'\0';
}

bool asciiIEqual(const std::wstring& a, const wchar_t* b) {
    return asciiIEqual(a.c_str(), a.size(), b);
}

bool asciiIStartsWith(const std::wstring& s, const wchar_t* prefix) {
    size_t i = 0;
    for (; prefix[i] != L'\0'; ++i) {
        if (i >= s.size()) return false;
        if (asciiLower(s[i]) != asciiLower(prefix[i])) return false;
    }
    return true;
}

bool isAsciiSpace(wchar_t c) { return c == L' ' || c == L'\t'; }

std::wstring trimAscii(const std::wstring& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && isAsciiSpace(s[begin])) ++begin;
    while (end > begin && isAsciiSpace(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

bool isBlank(const std::wstring& s) {
    for (const wchar_t c : s) {
        if (!isAsciiSpace(c)) return false;
    }
    return true;
}

bool isDdrawModuleName(const std::wstring& name) {
    return asciiIEqual(name, L"ddraw") || asciiIEqual(name, L"ddraw.dll");
}

struct EnvKey {
    bool drive = false;
    std::wstring name;
};

EnvKey envKeyOf(const std::wstring& entry) {
    EnvKey key;
    if (!entry.empty() && entry[0] == L'=') {
        key.drive = true;
        const size_t eq = entry.find(L'=', 1);
        key.name = (eq == std::wstring::npos) ? entry : entry.substr(0, eq);
        return key;
    }
    const size_t eq = entry.find(L'=');
    key.name = (eq == std::wstring::npos) ? entry : entry.substr(0, eq);
    return key;
}

int compareNamesCi(const std::wstring& a, const std::wstring& b) {
    const int r = CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                       b.c_str(), static_cast<int>(b.size()),
                                       TRUE);
    if (r == 0) return _wcsicmp(a.c_str(), b.c_str());
    return r - CSTR_EQUAL;
}

typedef const char* (__cdecl* WineGetVersionFn)(void);
typedef const char* (__cdecl* WineGetBuildIdFn)(void);
typedef void (__cdecl* WineGetHostVersionFn)(const char** sysname,
                                             const char** release);

bool envPresent(const wchar_t* name) {
    std::wstring value;
    return getEnvW(name, value);
}

bool moduleLoaded(const wchar_t* name) {
    return GetModuleHandleW(name) != nullptr;
}

std::string safeAnsi(const char* text) {
    if (!text) return std::string();
    for (size_t i = 0; i < 256; ++i) {
        if (text[i] == '\0') return std::string(text, i);
    }
    return std::string(text, 256);
}

CompatInfo computeCompat() {
    CompatInfo info;

    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        const auto getVersion = reinterpret_cast<WineGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version")));
        const auto getBuildId = reinterpret_cast<WineGetBuildIdFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_build_id")));
        const auto getHost = reinterpret_cast<WineGetHostVersionFn>(
            reinterpret_cast<void*>(
                GetProcAddress(ntdll, "wine_get_host_version")));
        if (getVersion) {
            info.wine = true;
            info.wineVersion = safeAnsi(getVersion());
        }
        if (getBuildId) info.wineBuildId = safeAnsi(getBuildId());
        if (getHost) {
            const char* sysname = nullptr;
            const char* release = nullptr;
            getHost(&sysname, &release);
            info.wineHostOs = safeAnsi(sysname);
            const std::string releaseText = safeAnsi(release);
            if (!releaseText.empty()) {
                if (!info.wineHostOs.empty()) info.wineHostOs += ' ';
                info.wineHostOs += releaseText;
            }
        }
    }

    std::vector<std::wstring> entries;
    if (readParentEnvironmentW(entries)) {
        for (const std::wstring& entry : entries) {
            if (entry.empty() || entry[0] == L'=') continue;
            if (asciiIStartsWith(entry, L"STEAM_COMPAT_")
                || asciiIStartsWith(entry, L"PROTON_")) {
                info.proton = true;
                break;
            }
        }
    }

    std::wstring appId;
    if (getEnvW(L"SteamAppId", appId) || getEnvW(L"SteamGameId", appId)) {
        for (const wchar_t c : appId) {
            if (c < 128) info.protonAppId.push_back(static_cast<char>(c));
        }
    }

    info.steamOverlayPresent = moduleLoaded(L"gameoverlayrenderer.dll")
                            || envPresent(L"SteamOverlayGameId");
    info.steamInputPresent = moduleLoaded(L"SteamInput.dll")
                          || moduleLoaded(L"steam_api.dll")
                          || envPresent(L"SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT");
    return info;
}

}

const CompatInfo& detectCompat() {
    static const CompatInfo cached = []() -> CompatInfo {
        try {
            return computeCompat();
        } catch (...) {
            return CompatInfo{};
        }
    }();
    return cached;
}

bool getEnvW(const wchar_t* name, std::wstring& out) {
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
            return true;
        }
        needed = written;
    }
    return false;
}

bool readParentEnvironmentW(std::vector<std::wstring>& entries) {
    entries.clear();
    LPWCH block = GetEnvironmentStringsW();
    if (!block) return false;
    for (const wchar_t* cursor = block; *cursor != L'\0'; ) {
        const size_t length = std::wcslen(cursor);
        entries.emplace_back(cursor, length);
        cursor += length + 1;
    }
    FreeEnvironmentStringsW(block);
    return true;
}

bool mergeDdrawOverrideValue(const std::wstring& in, std::wstring& out) {
    std::vector<std::wstring> kept;

    size_t pos = 0;
    while (pos <= in.size()) {
        const size_t sep = in.find(L';', pos);
        const std::wstring rule = (sep == std::wstring::npos)
            ? in.substr(pos)
            : in.substr(pos, sep - pos);
        pos = (sep == std::wstring::npos) ? in.size() + 1 : sep + 1;

        if (isBlank(rule)) continue;

        const size_t eq = rule.find(L'=');
        if (eq == std::wstring::npos) return false;
        const std::wstring names = rule.substr(0, eq);
        const std::wstring value = rule.substr(eq + 1);

        std::vector<std::wstring> keptNames;
        size_t namePos = 0;
        for (;;) {
            const size_t comma = names.find(L',', namePos);
            const std::wstring name = (comma == std::wstring::npos)
                ? names.substr(namePos)
                : names.substr(namePos, comma - namePos);
            const std::wstring trimmed = trimAscii(name);
            if (trimmed.empty()) return false;
            if (!isDdrawModuleName(trimmed)) keptNames.push_back(name);
            if (comma == std::wstring::npos) break;
            namePos = comma + 1;
        }
        if (keptNames.empty()) continue;

        std::wstring rebuilt;
        for (size_t i = 0; i < keptNames.size(); ++i) {
            if (i != 0) rebuilt += L',';
            rebuilt += keptNames[i];
        }
        rebuilt += L'=';
        rebuilt += value;
        kept.push_back(std::move(rebuilt));
    }

    std::wstring result;
    for (const std::wstring& rule : kept) {
        if (!result.empty()) result += L';';
        result += rule;
    }
    if (!result.empty()) result += L';';
    result += L"ddraw=n,b";
    out = std::move(result);
    return true;
}

bool mergeDdrawOverride(std::vector<std::wstring>& entries) {
    static const wchar_t kName[] = L"WINEDLLOVERRIDES";
    for (std::wstring& entry : entries) {
        if (entry.empty() || entry[0] == L'=') continue;   // drive entry
        const size_t eq = entry.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (!asciiIEqual(entry.c_str(), eq, kName)) continue;

        std::wstring merged;
        if (!mergeDdrawOverrideValue(entry.substr(eq + 1), merged)) return false;
        entry = entry.substr(0, eq + 1) + merged;
        return true;
    }
    entries.push_back(std::wstring(kName) + L"=ddraw=n,b");
    return true;
}

bool buildEnvironmentBlockW(const std::vector<std::wstring>& entries,
                            std::vector<wchar_t>& block) {
    std::vector<std::pair<EnvKey, const std::wstring*>> ordered;
    ordered.reserve(entries.size());
    for (const std::wstring& entry : entries) {
        if (entry.empty()) return false;
        if (entry.find(L'\0') != std::wstring::npos) return false;
        ordered.emplace_back(envKeyOf(entry), &entry);
    }

    std::stable_sort(
        ordered.begin(), ordered.end(),
        [](const std::pair<EnvKey, const std::wstring*>& a,
           const std::pair<EnvKey, const std::wstring*>& b) {
            if (a.first.drive != b.first.drive) return a.first.drive;
            const int c = compareNamesCi(a.first.name, b.first.name);
            return c < 0;
        });

    block.clear();
    size_t total = 1;
    for (const auto& item : ordered) total += item.second->size() + 1;
    block.reserve(total + 1);
    for (const auto& item : ordered) {
        block.insert(block.end(), item.second->begin(), item.second->end());
        block.push_back(L'\0');
    }
    if (ordered.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return true;
}

}
