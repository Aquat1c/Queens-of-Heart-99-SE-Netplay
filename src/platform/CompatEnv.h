// QOH's ddraw wrapper exports a private pacing ABI, so the child must be told `ddraw=n,b`; Wine's builtin DirectDraw is never a substitute.
#pragma once

#include <string>
#include <vector>

namespace qplat {

struct CompatInfo {
    bool wine = false;
    std::string wineVersion;
    std::string wineBuildId;
    std::string wineHostOs;
    bool proton = false;
    std::string protonAppId;
    bool steamOverlayPresent = false;
    bool steamInputPresent = false;
};

const CompatInfo& detectCompat();

bool readParentEnvironmentW(std::vector<std::wstring>& entries);

bool mergeDdrawOverride(std::vector<std::wstring>& entries);

bool mergeDdrawOverrideValue(const std::wstring& in, std::wstring& out);

bool buildEnvironmentBlockW(const std::vector<std::wstring>& entries,
                            std::vector<wchar_t>& block);

bool getEnvW(const wchar_t* name, std::wstring& out);

}
