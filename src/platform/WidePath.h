#pragma once

#include <string>

namespace qplat {

std::wstring widen(const std::string& utf8);
std::string  narrow(const std::wstring& utf16);

std::wstring widenAcp(const std::string& acp);

std::wstring exeDirW();
std::wstring exePathW();

std::wstring absolutePathW(const std::wstring& path);

std::wstring dirOfW(const std::wstring& path);
std::wstring baseNameW(const std::wstring& path);

bool fileExistsW(const std::wstring& path);
bool directoryExistsW(const std::wstring& path);

bool ensureDirW(const std::wstring& path);

bool isDirWritableW(const std::wstring& dir);

std::wstring longPathW(const std::wstring& path);

std::wstring stateRootW();
bool stateRootUsedFallback();
const wchar_t* stateRootSourceName();

std::wstring logsRootW();
std::wstring replayRootW();

std::wstring makeSessionDirW();

bool atomicReplaceFileW(const std::wstring& from, const std::wstring& to);

bool moveToTrashDirW(const std::wstring& file, const std::wstring& trashDir,
                     std::wstring& finalPath);

bool originalNameFromTrashLeafW(const std::wstring& leaf,
                                const std::wstring& extension,
                                std::wstring& original);

}
