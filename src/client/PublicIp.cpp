#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wininet.h>

#include "PublicIp.h"
#include "../Log.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace qclient::public_ip {
namespace {

struct State {
    std::atomic<bool> started{false};
    std::atomic<bool> inFlight{false};
    std::mutex mutex;
    std::string result;
};

State& state() {
    static State* const instance = new State();
    return *instance;
}

struct WinInet {
    HMODULE module = nullptr;
    HINTERNET(WINAPI* open)(LPCSTR, DWORD, LPCSTR, LPCSTR, DWORD) = nullptr;
    BOOL(WINAPI* setOption)(HINTERNET, DWORD, LPVOID, DWORD) = nullptr;
    HINTERNET(WINAPI* openUrl)(HINTERNET, LPCSTR, LPCSTR, DWORD, DWORD,
                               DWORD_PTR) = nullptr;
    BOOL(WINAPI* readFile)(HINTERNET, LPVOID, DWORD, LPDWORD) = nullptr;
    BOOL(WINAPI* closeHandle)(HINTERNET) = nullptr;
    BOOL(WINAPI* queryInfo)(HINTERNET, DWORD, LPVOID, LPDWORD,
                            LPDWORD) = nullptr;

    bool complete() const {
        return module && open && setOption && openUrl && readFile
            && closeHandle && queryInfo;
    }
};

template <typename Fn>
void bindProc(HMODULE module, const char* name, Fn& slot) {
    slot = reinterpret_cast<Fn>(::GetProcAddress(module, name));
}

const WinInet& wininet() {
    static const WinInet resolved = [] {
        WinInet found;
        found.module = ::LoadLibraryW(L"wininet.dll");
        if (!found.module) return found;
        bindProc(found.module, "InternetOpenA", found.open);
        bindProc(found.module, "InternetSetOptionA", found.setOption);
        bindProc(found.module, "InternetOpenUrlA", found.openUrl);
        bindProc(found.module, "InternetReadFile", found.readFile);
        bindProc(found.module, "InternetCloseHandle", found.closeHandle);
        bindProc(found.module, "HttpQueryInfoA", found.queryInfo);
        return found;
    }();
    return resolved;
}

std::string fetchOne(const char* urlText) {
    const WinInet& api = wininet();
    if (!api.complete()) return {};

    HINTERNET internet = api.open(
        "qoh99caster/1.0", INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr, nullptr, 0);
    if (!internet) return {};

    DWORD timeoutMs = 3000;
    api.setOption(internet, INTERNET_OPTION_CONNECT_TIMEOUT,
                  &timeoutMs, sizeof(timeoutMs));
    api.setOption(internet, INTERNET_OPTION_SEND_TIMEOUT,
                  &timeoutMs, sizeof(timeoutMs));
    api.setOption(internet, INTERNET_OPTION_RECEIVE_TIMEOUT,
                  &timeoutMs, sizeof(timeoutMs));

    HINTERNET request = api.openUrl(
        internet, urlText, nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE
            | INTERNET_FLAG_PRAGMA_NOCACHE
            | INTERNET_FLAG_NO_UI,
        0);
    std::string body;
    DWORD statusCode = 0;
    bool haveStatus = false;
    bool cleanEof = false;
    if (request) {
        DWORD statusBytes = sizeof(statusCode);
        haveStatus = api.queryInfo(
            request,
            HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
            &statusCode, &statusBytes, nullptr) != FALSE;
        char buffer[128] = {};
        while (body.size() < 256) {
            DWORD got = 0;
            if (!api.readFile(request, buffer, sizeof(buffer), &got))
                break;
            if (got == 0) {
                cleanEof = true;
                break;
            }
            body.append(buffer, got);
        }
        api.closeHandle(request);
    }
    api.closeHandle(internet);

    std::string canonical;
    const std::uint32_t effectiveStatus =
        haveStatus ? static_cast<std::uint32_t>(statusCode) : 0u;
    return normalizeHttpIpv4Response(
               effectiveStatus, cleanEof, body, canonical)
        ? canonical : std::string{};
}

}

bool advisoryOnly() {
    return true;
}

bool httpBackendAvailable() { return wininet().complete(); }

const char* httpBackendStatus() {
    const WinInet& api = wininet();
    if (!api.module) return "wininet.dll not present (advisory lookup disabled)";
    if (!api.complete())
        return "wininet.dll is missing required entry points "
               "(advisory lookup disabled)";
    return "wininet.dll loaded dynamically";
}

bool normalizeIpv4(const std::string& text, std::string& canonical) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    const std::string trimmed = text.substr(first, last - first + 1);

    IN_ADDR address{};
    if (InetPtonA(AF_INET, trimmed.c_str(), &address) != 1) return false;
    char out[INET_ADDRSTRLEN] = {};
    if (!InetNtopA(AF_INET, &address, out, sizeof(out))) return false;
    canonical = out;
    return true;
}

bool normalizeHttpIpv4Response(std::uint32_t statusCode, bool cleanEof,
                               const std::string& body,
                               std::string& canonical) {
    if (!cleanEof || statusCode < 200u || statusCode >= 300u)
        return false;
    return normalizeIpv4(body, canonical);
}

namespace {

void beginLookup(bool refreshExisting) {
    State& shared = state();
    if (!refreshExisting && shared.started.exchange(true)) return;
    shared.started.store(true);
    bool expected = false;
    if (!shared.inFlight.compare_exchange_strong(expected, true)) return;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.result.clear();
    }

    char overrideText[64] = {};
    const DWORD overrideLength = GetEnvironmentVariableA(
        "QOH99_PUBLIC_IP_OVERRIDE", overrideText,
        static_cast<DWORD>(sizeof(overrideText)));
    std::string canonicalOverride;
    if (overrideLength > 0 && overrideLength < sizeof(overrideText)
        && normalizeIpv4(overrideText, canonicalOverride)) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.result = canonicalOverride;
        shared.inFlight.store(false);
        QLOG("[public-ip] using validated environment override %s",
             canonicalOverride.c_str());
        return;
    }

    if (!httpBackendAvailable()) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.result = "n/a";
        shared.inFlight.store(false);
        QLOG("[public-ip] advisory lookup unavailable: %s",
             httpBackendStatus());
        return;
    }

    try {
        std::thread([&shared]() {
            std::string found = fetchOne("https://api.ipify.org/");
            if (found.empty())
                found = fetchOne("https://ipv4.icanhazip.com/");
            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                shared.result = found.empty() ? "n/a" : found;
            }
            shared.inFlight.store(false);
            // Detached worker: do not touch function-static logging state here, it may already be in destruction.
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.result = "n/a";
        shared.inFlight.store(false);
        QLOG("[public-ip] could not start discovery worker");
    }
}

}

void start() {
    beginLookup(false);
}

void refresh() {
    beginLookup(true);
}

bool pending() {
    return state().inFlight.load();
}

std::string value() {
    State& shared = state();
    std::lock_guard<std::mutex> lock(shared.mutex);
    return shared.result;
}

}
