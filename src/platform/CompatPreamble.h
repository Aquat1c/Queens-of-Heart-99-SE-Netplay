#pragma once

#include <functional>
#include <string>
#include <vector>

namespace qplat {

struct PreambleModule {
    std::wstring name;
    std::wstring path;
    std::string sha256OrEmpty;
};

struct PreambleContext {
    std::string component;
    std::string buildContract;
    std::wstring stateRoot;
    bool stateRootFallback = false;
    std::vector<PreambleModule> modules;
    std::string extra;
};

void writeSessionPreamble(const PreambleContext& ctx,
                          const std::function<void(const std::string&)>& emit);

bool loadOptionalModule(const wchar_t* name, void*& moduleOut);

}
