#include "CompatPreamble.h"

#include "CompatEnv.h"
#include "WidePath.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>

namespace qplat {
namespace {

constexpr const char* kTag = "[compat] ";

std::string orNone(const std::string& value) {
    return value.empty() ? std::string("none") : value;
}

std::string quoted(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char c : value) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string quotedW(const std::wstring& value) {
    const std::string text = narrow(value);
    if (text.empty() && !value.empty()) return quoted("<unrepresentable>");
    return quoted(text);
}

const char* yesNo(bool value) { return value ? "yes" : "no"; }

}

void writeSessionPreamble(const PreambleContext& ctx,
                          const std::function<void(const std::string&)>& emit) {
    if (!emit) return;

    const CompatInfo& compat = detectCompat();

    emit(std::string(kTag) + "component=" + orNone(ctx.component)
         + " contract=" + quoted(ctx.buildContract));

    if (compat.wine) {
        emit(std::string(kTag) + "platform wine=yes"
             + " version=" + quoted(compat.wineVersion)
             + " buildid=" + quoted(compat.wineBuildId)
             + " host=" + quoted(compat.wineHostOs));
    } else {
        emit(std::string(kTag) + "platform wine=no host=\"native windows\"");
    }

    emit(std::string(kTag) + "steam proton=" + yesNo(compat.proton)
         + " appid=" + quoted(compat.protonAppId)
         + " overlay=" + yesNo(compat.steamOverlayPresent)
         + " steaminput=" + yesNo(compat.steamInputPresent));

    emit(std::string(kTag) + "state root=" + quotedW(ctx.stateRoot)
         + " fallback=" + yesNo(ctx.stateRootFallback));

    {
        LARGE_INTEGER frequency{};
        LARGE_INTEGER counter{};
        const bool frequencyOk = QueryPerformanceFrequency(&frequency) != FALSE;
        const bool counterOk = QueryPerformanceCounter(&counter) != FALSE;
        const bool sane = frequencyOk && counterOk
                       && frequency.QuadPart >= 1000
                       && frequency.QuadPart <= 1000000000LL
                       && counter.QuadPart > 0;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "timing qpc-frequency=%lld sane=%s",
                      frequencyOk ? static_cast<long long>(frequency.QuadPart)
                                  : 0LL,
                      yesNo(sane));
        emit(std::string(kTag) + buf);
    }

    for (const PreambleModule& module : ctx.modules) {
        emit(std::string(kTag) + "module name=" + quotedW(module.name)
             + " path=" + quotedW(module.path)
             + " sha256=" + orNone(module.sha256OrEmpty));
    }

    if (!ctx.extra.empty())
        emit(std::string(kTag) + "extra " + ctx.extra);
}

bool loadOptionalModule(const wchar_t* name, void*& moduleOut) {
    moduleOut = nullptr;
    if (!name || !*name) return false;

    if (HMODULE existing = GetModuleHandleW(name)) {
        moduleOut = existing;
        return true;
    }

    HMODULE module = LoadLibraryExW(name, nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER)
            module = LoadLibraryW(name);
    }
    if (!module) return false;
    moduleOut = module;
    return true;
}

}
