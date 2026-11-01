#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdio>

#include "../platform/WidePath.h"
#include "AntivirusPreflight.h"

namespace qclient {
namespace {

constexpr const wchar_t* kRequiredLeaves[] = {
    L"qoh99caster.exe",
    L"qoh99_netplay.dll",
    L"ddraw.dll",
};

constexpr const wchar_t* kRunMarkerLeaf = L".qoh99_run_in_progress";
constexpr const wchar_t* kExclusionHelperLeaf = L"defender_exclusions.cmd";

std::wstring casterDir() {
    std::wstring dir = qplat::exeDirW();
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
        dir.pop_back();
    return dir;
}

std::wstring child(const std::wstring& dir, const wchar_t* leaf) {
    return dir.empty() ? std::wstring(leaf) : dir + L"\\" + leaf;
}

AntivirusFileState probeFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(qplat::longPathW(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return AntivirusFileState::Missing;
        return AntivirusFileState::Unreadable;
    }
    const HANDLE handle = CreateFileW(
        qplat::longPathW(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return AntivirusFileState::Unreadable;
    CloseHandle(handle);
    return AntivirusFileState::Present;
}

}

AntivirusPreflightReport preflightAntivirus() {
    AntivirusPreflightReport report{};
    const std::wstring dir = casterDir();
    for (const wchar_t* leaf : kRequiredLeaves) {
        const std::wstring path = child(dir, leaf);
        AntivirusFileReport file{};
        file.path = qplat::narrow(path);
        file.leaf = qplat::narrow(leaf);
        file.state = probeFile(path);
        if (file.state != AntivirusFileState::Present) report.degraded = true;
        report.files.push_back(std::move(file));
    }
    report.lastRunEndedAbruptly =
        qplat::fileExistsW(child(dir, kRunMarkerLeaf));
    return report;
}

bool antivirusPreflightShouldSkip(int argc, char** argv) {
    if (argv == nullptr) return false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) continue;
        const std::string arg = argv[i];
        if (arg == "--dll" || arg == "--game") return true;
        if (arg.rfind("--dll=", 0u) == 0u) return true;
        if (arg.rfind("--game=", 0u) == 0u) return true;
        if (arg == "--version" || arg == "--tests" || arg == "help"
            || arg == "--help" || arg == "-h" || arg == "relay"
            || arg == "--wine-fixture" || arg == "--fixture-child") {
            return true;
        }
        if (arg.rfind("analyze-", 0u) == 0u) return true;
    }
    return false;
}

std::vector<std::string> antivirusPreflightMessages(
    const AntivirusPreflightReport& report) {
    std::vector<std::string> lines;
    if (!report.degraded) return lines;

    for (const AntivirusFileReport& file : report.files) {
        if (file.state == AntivirusFileState::Missing) {
            lines.push_back(
                file.leaf + " is missing from the install folder.");
        } else if (file.state == AntivirusFileState::Unreadable) {
            lines.push_back(
                file.leaf + " is present but cannot be opened - something "
                "else is holding it.");
        }
    }

    lines.push_back(
        "The most common cause is antivirus quarantine. This mod injects a "
        "DLL into the game, which is what netplay and training require, and "
        "heuristic scanners flag that pattern as a false positive.");
    if (report.lastRunEndedAbruptly) {
        lines.push_back(
            "The previous session also did not shut down cleanly, which fits "
            "a file being removed while it was running.");
    }
    lines.push_back(
        "Fix: restore the file from quarantine and add this folder to your "
        "antivirus exclusions - " + antivirusExclusionPath());
    return lines;
}

void markRunStarted() {
    const std::wstring marker = child(casterDir(), kRunMarkerLeaf);
    const HANDLE handle = CreateFileW(
        qplat::longPathW(marker).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
}

void markRunEndedCleanly() {
    const std::wstring marker = child(casterDir(), kRunMarkerLeaf);
    DeleteFileW(qplat::longPathW(marker).c_str());
}

std::string antivirusExclusionPath() {
    return qplat::narrow(casterDir());
}

bool launchDefenderExclusionHelper(std::string& error) {
    const std::wstring helper = child(casterDir(), kExclusionHelperLeaf);
    if (!qplat::fileExistsW(helper)) {
        error = "defender_exclusions.cmd is not in the install folder.";
        return false;
    }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = helper.c_str();
    info.lpDirectory = casterDir().c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        const DWORD failure = GetLastError();
        if (failure == ERROR_CANCELLED) {
            error = "Elevation was declined, so nothing was changed.";
        } else {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer),
                          "Could not start the helper (error %lu).", failure);
            error = buffer;
        }
        return false;
    }
    if (info.hProcess != nullptr) {
        WaitForSingleObject(info.hProcess, 120000u);
        CloseHandle(info.hProcess);
    }
    return true;
}

}
