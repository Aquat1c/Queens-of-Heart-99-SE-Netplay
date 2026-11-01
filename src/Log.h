#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cwchar>
#include <string>
#include <utility>

namespace qoh99 {

class Log {
public:
    static Log& get() { static Log inst; return inst; }

    void openUtf8(const std::string& path, bool continuous = false,
                  size_t maxFileBytes = 0) {
        if (_open || _writerThread) return;
        _readIndex = _writeIndex = _flushedIndex = 0;
        _dropped = _reportedDropped = 0;
        _persist = false;
        configure(path, false, continuous, maxFileBytes);
    }

    void reopenUtf8(const std::string& path, bool continuous = false,
                    size_t maxFileBytes = 0) {
        configure(path, true, continuous, maxFileBytes);
    }
    void open(const char* path) { if (path && *path) openUtf8(path); }
    void reopen(const char* path) { if (path && *path) reopenUtf8(path); }

    void issue() {
        if (!_open) return;
        _persist = true;
        if (_issueEvent) SetEvent(_issueEvent);
        if (_wake) SetEvent(_wake);
    }

    bool diagnosticsEnabled() const {
        return _persist || (_issueEvent
            && WaitForSingleObject(_issueEvent, 0) == WAIT_OBJECT_0);
    }

    static bool fullLoggingRequested() {
        char value[16]{};
        const DWORD n = GetEnvironmentVariableA("QOH99_LOG", value, sizeof(value));
        return n && n < sizeof(value)
            && (!lstrcmpiA(value, "1") || !lstrcmpiA(value, "on")
                || !lstrcmpiA(value, "true"));
    }

    void printf(const char* fmt, ...) {
        if (!_open) return;
        SYSTEMTIME st; GetLocalTime(&st);
        char line[kRecordBytes];
        const int stamp = std::snprintf(line, sizeof(line),
            "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
        if (stamp <= 0 || static_cast<size_t>(stamp) >= sizeof(line) - 2u) return;
        size_t length = static_cast<size_t>(stamp);
        va_list ap; va_start(ap, fmt);
        const int body = std::vsnprintf(line + length, sizeof(line) - length - 1u,
                                       fmt, ap);
        va_end(ap);
        if (body > 0) {
            const size_t room = sizeof(line) - length - 2u;
            length += static_cast<size_t>(body) < room ? body : room;
        }
        line[length++] = '\n';
        EnterCriticalSection(&_cs);
        if (_writeIndex - _readIndex >= kSlots) {
            ++_dropped;
            ++_readIndex;
        }
        Record& slot = _ring[_writeIndex++ % kSlots];
        std::memcpy(slot.text, line, length);
        slot.length = length;
        LeaveCriticalSection(&_cs);
        if (_persist && _wake) SetEvent(_wake);
    }

    void flushNow(DWORD budgetMs = 500) {
        if (!_open || !_wake || !diagnosticsEnabled()) return;
        EnterCriticalSection(&_cs);
        const size_t target = _writeIndex;
        LeaveCriticalSection(&_cs);
        SetEvent(_wake);
        const DWORD started = GetTickCount();
        for (;;) {
            EnterCriticalSection(&_cs);
            const bool done = _flushedIndex >= target;
            LeaveCriticalSection(&_cs);
            if (done || GetTickCount() - started >= budgetMs) return;
            Sleep(1);
        }
    }

    unsigned long droppedRecords() const { return _dropped; }
    unsigned long longestWriteMs() const { return _longestWriteMs; }

    void close() {
        _open = false;
        if (!stopWriter()) return;
        if (_issueEvent) { CloseHandle(_issueEvent); _issueEvent = nullptr; }
    }

    // DllMain must not wait for a writer or use the CRT under the loader lock.
    void markProcessExit() { _open = false; }

private:
    Log() = default;
    ~Log() = default;
    static constexpr size_t kSlots = 1024;
    static constexpr size_t kRecordBytes = 2048;
    struct Record { size_t length = 0; char text[kRecordBytes]{}; };

    void configure(const std::string& path, bool append, bool continuous,
                   size_t maxFileBytes) {
        if (path.empty() || path.size() > INT_MAX) return;
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            path.data(), static_cast<int>(path.size()), nullptr, 0);
        if (n <= 0) return;
        std::wstring wide(n, L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
            static_cast<int>(path.size()), &wide[0], n) != n) return;
        if (!stopWriter()) return;
        if (!_csInit) { InitializeCriticalSection(&_cs); _csInit = true; }
        if (_issueEvent) CloseHandle(_issueEvent);
        const size_t slash = wide.find_last_of(L"\\/");
        unsigned long long hash = 14695981039346656037ull;
        for (size_t i = 0; slash != std::wstring::npos && i < slash; ++i) {
            wchar_t c = wide[i] == L'/' ? L'\\' : wide[i];
            if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
            hash = (hash ^ static_cast<unsigned>(c)) * 1099511628211ull;
        }
        wchar_t name[64];
        std::swprintf(name, 64, L"Local\\QOH99Diagnostics_%016llX", hash);
        _issueEvent = CreateEventW(nullptr, TRUE, FALSE,
            slash == std::wstring::npos ? nullptr : name);
        if (_persist && _issueEvent) SetEvent(_issueEvent);
        _path = std::move(wide);
        _append = append;
        _maxFileBytes = maxFileBytes;
        _persist = _persist || continuous || fullLoggingRequested();
        _writerStop = false;
        _open = true;
        _wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (_wake) {
            HMODULE owner = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                    | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&Log::writerEntry), &owner))
                _writerThread = CreateThread(nullptr, 0, &Log::writerEntry, this, 0, nullptr);
            if (!_writerThread) { CloseHandle(_wake); _wake = nullptr; }
        }
    }

    bool stopWriter() {
        if (!_writerThread) return true;
        _writerStop = true;
        SetEvent(_wake);
        if (WaitForSingleObject(_writerThread, 2000) != WAIT_OBJECT_0)
            return false;
        CloseHandle(_writerThread); _writerThread = nullptr;
        CloseHandle(_wake); _wake = nullptr;
        return true;
    }
    static DWORD WINAPI writerEntry(LPVOID self) {
        static_cast<Log*>(self)->writerLoop();
        return 0;
    }
    void writerLoop() {
        FILE* file = nullptr;
        bool attempted = false;
        size_t bytes = 0;
        for (;;) {
            HANDLE events[] = { _wake, _issueEvent };
            WaitForMultipleObjects(_persist || !_issueEvent ? 1 : 2,
                                   events, FALSE, 100);
            if (diagnosticsEnabled()) {
                _persist = true;
                if (!attempted) {
                    attempted = true;
                    file = _wfopen(_path.c_str(), _append ? L"ab" : L"wb");
                    if (file && _append && !std::fseek(file, 0, SEEK_END)) {
                        const long pos = std::ftell(file);
                        if (pos > 0) bytes = static_cast<size_t>(pos);
                    }
                    if (file && (!_maxFileBytes || bytes < _maxFileBytes)) {
                        const int written = std::fprintf(file,
                            "[logger] process-start pid=%lu built " __DATE__ " " __TIME__ "\n",
                            GetCurrentProcessId());
                        if (written > 0) bytes += written;
                    }
                }
                drain(file, bytes);
            }
            if (_writerStop) break;
        }
        if (file) std::fclose(file);
    }
    void drain(FILE* file, size_t& bytes) {
        for (;;) {
            Record batch[32];
            size_t count = 0, end = 0;
            unsigned long dropped = 0;
            EnterCriticalSection(&_cs);
            while (count < 32 && _readIndex != _writeIndex) {
                batch[count++] = _ring[_readIndex++ % kSlots];
            }
            end = _readIndex;
            dropped = _dropped - _reportedDropped;
            _reportedDropped = _dropped;
            LeaveCriticalSection(&_cs);
            if (!count) return;
            const DWORD started = GetTickCount();
            if (file && (!_maxFileBytes || bytes < _maxFileBytes)) {
                if (dropped) {
                    const int written = std::fprintf(file,
                        "[logger] omitted %lu older history/overflow record(s)\n", dropped);
                    if (written > 0) bytes += written;
                }
                for (size_t i = 0; i < count; ++i) {
                    if (_maxFileBytes && bytes + batch[i].length > _maxFileBytes) break;
                    bytes += std::fwrite(batch[i].text, 1, batch[i].length, file);
                }
                std::fflush(file);
            }
            const DWORD elapsed = GetTickCount() - started;
            if (elapsed > _longestWriteMs) _longestWriteMs = elapsed;
            EnterCriticalSection(&_cs);
            _flushedIndex = end;
            LeaveCriticalSection(&_cs);
        }
    }

    std::atomic<bool> _open{false}, _persist{false}, _writerStop{false};
    bool _csInit = false, _append = false;
    CRITICAL_SECTION _cs{};
    std::wstring _path;
    size_t _maxFileBytes = 0;
    Record _ring[kSlots];
    size_t _writeIndex = 0, _readIndex = 0, _flushedIndex = 0;
    std::atomic<unsigned long> _dropped{0}, _longestWriteMs{0};
    unsigned long _reportedDropped = 0;
    HANDLE _writerThread = nullptr, _wake = nullptr, _issueEvent = nullptr;
};
}

#define QLOG(...) ::qoh99::Log::get().printf(__VA_ARGS__)
#define QLOG_ISSUE(...) do { ::qoh99::Log::get().issue(); QLOG(__VA_ARGS__); } while (0)
