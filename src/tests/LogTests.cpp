#include "../Log.h"
#include "../CrashDump.h"
#include "../../ddraw_wrapper/ddraw_log.h"
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static std::string contents(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
static PROCESS_INFORMATION start(const std::wstring& mode, const fs::path& root) {
    wchar_t exe[32768]; GetModuleFileNameW(nullptr, exe, 32768);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + mode + L" \"" + root.wstring() + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    CHECK(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    return pi;
}
static DWORD finish(PROCESS_INFORMATION& pi) {
    const DWORD wait = WaitForSingleObject(pi.hProcess, 10000);
    CHECK(wait == WAIT_OBJECT_0);
    if (wait != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 99);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code;
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    auto& log = qoh99::Log::get();
    if (argc == 3) {
        const fs::path root(argv[2]);
        const std::wstring mode(argv[1]);
        if (mode.find(L"wrapper") == 0) {
            SetEnvironmentVariableW(L"QOH99_NETPLAY_SESSION_DIR", root.c_str());
            if (mode == L"wrapper-full") SetEnvironmentVariableA("QOH99_WRAPPER_LOG", "1");
            if (mode == L"wrapper-off") SetEnvironmentVariableA("QOH99_WRAPPER_LOG", "0");
            qoh99_wlog_write("WRAPPER_CONTEXT");
            if (mode == L"wrapper-issue")
                for (int i = 0; i < 500; ++i) qoh99_wlog_write("burst %d", i);
            if (mode == L"wrapper-issue" || mode == L"wrapper-off")
                WLOG_ISSUE("WRAPPER_FAILURE");
            qoh99_wlog_close();
            return 0;
        }
        log.openUtf8((root / "child.log").u8string());
        QLOG("CHILD_CONTEXT");
        if (mode == L"crash") {
            qcrash::installW(root.wstring(), "logging-test");
            QLOG("BEFORE_CRASH");
            RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
            return 98;
        }
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE,
            (L"Local\\qoh99-log-ready-" + root.filename().wstring()).c_str());
        if (!ready) return 97;
        SetEvent(ready); CloseHandle(ready);
        const DWORD deadline = GetTickCount();
        while (!log.diagnosticsEnabled() && GetTickCount() - deadline < 5000) Sleep(1);
        if (!log.diagnosticsEnabled()) return 96;
        QLOG("CHILD_AFTER_ISSUE");
        log.close();
        return 0;
    }
    SetEnvironmentVariableA("QOH99_LOG", nullptr);
    SetEnvironmentVariableA("QOH99_WRAPPER_LOG", nullptr);
    const fs::path root = fs::temp_directory_path()
        / (L"qoh99-log-test-\u65e5\u672c-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);
    const fs::path quiet = root / "quiet.log";
    log.openUtf8(quiet.u8string());
    for (int i = 0; i < 1200; ++i) QLOG("ordinary frame %d", i);
    Sleep(150);
    CHECK(!fs::exists(quiet));
    log.close();
    CHECK(!fs::exists(quiet));

    const fs::path issue = root / "issue.log";
    log.openUtf8(issue.u8string());
    for (int i = 0; i < 1200; ++i) QLOG("history-%04d", i);
    QLOG_ISSUE("REAL_FAILURE");
    log.flushNow();
    const std::string flushed = contents(issue);
    CHECK(flushed.find("REAL_FAILURE") != std::string::npos);
    CHECK(flushed.find("history-1199") != std::string::npos);
    CHECK(flushed.find("history-0000") == std::string::npos);
    log.close();

    log.openUtf8((root / "early.log").u8string());
    QLOG("EARLY_CONTEXT");
    log.reopenUtf8((root / "repointed.log").u8string());
    CHECK(!fs::exists(root / "early.log"));
    CHECK(!fs::exists(root / "repointed.log"));
    QLOG_ISSUE("AFTER_REPOINT");
    log.close();
    CHECK(contents(root / "repointed.log").find("EARLY_CONTEXT") != std::string::npos);

    SetEnvironmentVariableA("QOH99_LOG", "1");
    log.openUtf8((root / "full.log").u8string());
    QLOG("FULL_DIAGNOSTICS");
    log.close();
    CHECK(contents(root / "full.log").find("FULL_DIAGNOSTICS") != std::string::npos);
    SetEnvironmentVariableA("QOH99_LOG", nullptr);

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE,
        (L"Local\\qoh99-log-ready-" + root.filename().wstring()).c_str());
    log.openUtf8((root / "parent.log").u8string());
    QLOG("PARENT_CONTEXT");
    PROCESS_INFORMATION child = start(L"shared", root);
    CHECK(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
    CHECK(!fs::exists(root / "parent.log"));
    CHECK(!fs::exists(root / "child.log"));
    QLOG_ISSUE("SESSION_FAILURE");
    CHECK(finish(child) == 0);
    log.close(); CloseHandle(ready);
    CHECK(contents(root / "child.log").find("CHILD_CONTEXT") != std::string::npos);
    CHECK(contents(root / "child.log").find("CHILD_AFTER_ISSUE") != std::string::npos);
    CHECK(contents(root / "parent.log").find("SESSION_FAILURE") != std::string::npos);

    for (const wchar_t* mode : {L"wrapper-quiet", L"wrapper-issue", L"wrapper-full", L"wrapper-off", L"crash"}) {
        const fs::path dir = root / mode;
        fs::create_directories(dir);
        child = start(mode, dir);
        const DWORD code = finish(child);
        if (!wcscmp(mode, L"crash")) {
            CHECK(code == EXCEPTION_ACCESS_VIOLATION);
            CHECK(contents(dir / "child.log").find("BEFORE_CRASH") != std::string::npos);
            CHECK(contents(dir / "child.log").find("[CRASH] code=") != std::string::npos);
        } else {
            CHECK(code == 0);
            const bool quietMode = !wcscmp(mode, L"wrapper-quiet") || !wcscmp(mode, L"wrapper-off");
            CHECK(fs::exists(dir / "ddraw_wrapper.log") != quietMode);
            if (!quietMode) CHECK(contents(dir / "ddraw_wrapper.log").find("WRAPPER_CONTEXT") != std::string::npos);
            if (!wcscmp(mode, L"wrapper-issue"))
                CHECK(contents(dir / "ddraw_wrapper.log").find("WRAPPER_FAILURE") != std::string::npos);
        }
    }
    fs::remove_all(root);
    std::printf("Logging checks: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
