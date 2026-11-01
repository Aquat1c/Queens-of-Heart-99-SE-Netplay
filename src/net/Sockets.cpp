#include "Sockets.h"
#include "MonotonicTime.h"
#include "Protocol.h"
#include "RouteCandidates.h"
#include "../Log.h"
#include <iphlpapi.h>
#include <mswsock.h>
#include <process.h>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace qnet {

static LONG g_wsaRefs = 0;

bool netInit() {
    if (InterlockedIncrement(&g_wsaRefs) == 1) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
            InterlockedDecrement(&g_wsaRefs);
            return false;
        }
    }
    return true;
}

void netShutdown() {
    if (InterlockedDecrement(&g_wsaRefs) == 0)
        WSACleanup();
}

uint64_t nowUs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(
        qnet::ticksToMicros(c.QuadPart, freq.QuadPart));
}

size_t collectLocalIpv4BE(uint32_t* out, size_t cap) {
    if (out == nullptr || cap == 0) return 0;
    ULONG bytes = 0;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST
                      | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &bytes)
            != ERROR_BUFFER_OVERFLOW || bytes == 0) {
        return 0;
    }
    std::vector<uint8_t> buffer(bytes);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bytes)
            != ERROR_SUCCESS) {
        return 0;
    }
    size_t used = 0;
    for (const IP_ADAPTER_ADDRESSES* adapter = adapters;
         adapter != nullptr && used < cap; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        for (const IP_ADAPTER_UNICAST_ADDRESS* unicast =
                 adapter->FirstUnicastAddress;
             unicast != nullptr && used < cap; unicast = unicast->Next) {
            const sockaddr* sa = unicast->Address.lpSockaddr;
            if (sa == nullptr || sa->sa_family != AF_INET) continue;
            out[used++] = reinterpret_cast<const sockaddr_in*>(sa)
                              ->sin_addr.s_addr;
        }
    }
    return used;
}

static bool setNonBlocking(SOCKET s) {
    u_long nb = 1;
    return ioctlsocket(s, FIONBIO, &nb) == 0;
}

void disableUdpConnectionReset(SOCKET socket) {
    BOOL enabled = FALSE;
    DWORD returned = 0;
    (void)WSAIoctl(socket, SIO_UDP_CONNRESET,
                   &enabled, sizeof(enabled), nullptr, 0,
                   &returned, nullptr, nullptr);
}

bool Endpoint::parse(const std::string& ip, uint16_t port, Endpoint& out) {
    out.sa.sin_family = AF_INET;
    out.sa.sin_port = htons(port);
    return inet_pton(AF_INET, ip.c_str(), &out.sa.sin_addr) == 1;
}

std::string Endpoint::str() const {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, const_cast<in_addr*>(&sa.sin_addr), ip, sizeof(ip));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s:%u", ip, ntohs(sa.sin_port));
    return buf;
}

namespace {

constexpr char kIpv6UnsupportedDetail[] =
    "IPv6-only host: the QOH99 player, relay, and spectator protocols are "
    "IPv4-only in this build";

bool normalizeHostText(const std::string& raw, std::string& out,
                       std::string& detail) {
    out.clear();
    if (raw.find('\0') != std::string::npos) {
        detail = "address contains an embedded NUL character";
        return false;
    }
    if (raw.size() > kMaxHostTextLength * 2) {
        detail = "address text is too long";
        return false;
    }
    size_t first = 0;
    size_t last = raw.size();
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (first < last && isSpace(raw[first])) ++first;
    while (last > first && isSpace(raw[last - 1])) --last;
    std::string text = raw.substr(first, last - first);
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']')
        text = text.substr(1, text.size() - 2);
    if (text.empty()) {
        detail = "address is empty";
        return false;
    }
    if (text.size() > kMaxHostTextLength) {
        detail = "address text is longer than " +
                 std::to_string(kMaxHostTextLength) + " characters";
        return false;
    }
    out.swap(text);
    return true;
}

bool looksLikeHostname(const std::string& host, std::string& detail) {
    size_t labelLen = 0;
    for (size_t i = 0; i < host.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(host[i]);
        if (c == '.') {
            if (labelLen == 0) {
                detail = "hostname has an empty label";
                return false;
            }
            labelLen = 0;
            continue;
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            detail = "'" + host + "' is neither a numeric IPv4 address nor a "
                                  "plain hostname";
            return false;
        }
        if (++labelLen > 63) {
            detail = "hostname label is longer than 63 characters";
            return false;
        }
    }
    if (host.front() == '-' || host.front() == '.') {
        detail = "hostname starts with '" + std::string(1, host.front()) + "'";
        return false;
    }
    return true;
}

bool isIpv6Literal(const std::string& text) {
    in6_addr address{};
    return InetPtonA(AF_INET6, text.c_str(), &address) == 1;
}

struct ResolveJob {
    volatile LONG refs = 2;
    HANDLE finished = nullptr;
    int status = 0;
    std::vector<ResolvedSample> samples;
    std::wstring host;
};

void releaseJob(ResolveJob* job) {
    if (job == nullptr) return;
    if (InterlockedDecrement(&job->refs) != 0) return;
    if (job->finished) CloseHandle(job->finished);
    delete job;
}

unsigned __stdcall resolveWorker(void* context) {
    auto* job = static_cast<ResolveJob*>(context);
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    PADDRINFOW result = nullptr;
    const int status = GetAddrInfoW(job->host.c_str(), nullptr, &hints,
                                    &result);
    std::vector<ResolvedSample> samples;
    if (status == 0) {
        for (const ADDRINFOW* it = result;
             it != nullptr && samples.size() < 64; it = it->ai_next) {
            ResolvedSample sample{};
            sample.family = it->ai_family;
            if (it->ai_family == AF_INET && it->ai_addr != nullptr
                && it->ai_addrlen >= sizeof(sockaddr_in)) {
                sample.ipv4BE = reinterpret_cast<const sockaddr_in*>(it->ai_addr)
                                    ->sin_addr.s_addr;
            }
            samples.push_back(sample);
        }
    }
    if (result != nullptr) FreeAddrInfoW(result);

    job->status = status;
    job->samples.swap(samples);
    SetEvent(job->finished);
    releaseJob(job);
    netShutdown();
    return 0;
}

}

const char* resolveErrorText(ResolveError error) {
    switch (error) {
    case ResolveError::None:         return "ok";
    case ResolveError::InvalidInput: return "invalid address text";
    case ResolveError::NotFound:     return "name not found";
    case ResolveError::NoAddress:    return "no usable address";
    case ResolveError::Timeout:      return "name lookup timed out";
    case ResolveError::SystemError:  return "name lookup failed";
    }
    return "unknown resolve error";
}

const char* resolveIpv6UnsupportedText() { return kIpv6UnsupportedDetail; }

bool resolveFamilySupported(int family) {
    return family == AF_INET;
}

bool filterResolvedSamples(const ResolvedSample* samples, size_t count,
                           uint16_t port, std::vector<ResolvedEndpoint>& out,
                           ResolveError& err, std::string& detail) {
    out.clear();
    err = ResolveError::None;
    detail.clear();
    if (samples == nullptr && count != 0) {
        err = ResolveError::SystemError;
        detail = "resolver produced no candidate array";
        return false;
    }

    size_t unsupportedFamily = 0;
    size_t ipv6Seen = 0;
    size_t unusableAddress = 0;
    for (size_t i = 0; i < count && out.size() < kMaxResolvedEndpoints; ++i) {
        const ResolvedSample& sample = samples[i];
        if (!resolveFamilySupported(sample.family)) {
            ++unsupportedFamily;
            if (sample.family == AF_INET6) ++ipv6Seen;
            continue;
        }
        if (sample.ipv4BE == 0 || isNonUnicastBE(sample.ipv4BE)) {
            ++unusableAddress;
            continue;
        }
        bool duplicate = false;
        for (const ResolvedEndpoint& kept : out) {
            if (kept.endpoint.sa.sin_addr.s_addr == sample.ipv4BE) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        ResolvedEndpoint resolved;
        resolved.endpoint.sa.sin_family = AF_INET;
        resolved.endpoint.sa.sin_addr.s_addr = sample.ipv4BE;
        resolved.endpoint.sa.sin_port = htons(port);
        resolved.wasHostname = true;
        resolved.canonicalText = resolved.endpoint.str();
        out.push_back(std::move(resolved));
    }

    if (!out.empty()) return true;

    if (ipv6Seen > 0) {
        err = ResolveError::NoAddress;
        detail = kIpv6UnsupportedDetail;
        return false;
    }
    if (unsupportedFamily > 0 || unusableAddress > 0) {
        err = ResolveError::NoAddress;
        detail = "the name resolved, but to no usable IPv4 address";
        return false;
    }
    err = ResolveError::NotFound;
    detail = "the name did not resolve to any address";
    return false;
}

bool resolveEndpoints(const std::string& hostOrIp, uint16_t port,
                      unsigned timeoutMs, std::vector<ResolvedEndpoint>& out,
                      ResolveError& err, std::string& detail) {
    out.clear();
    err = ResolveError::None;
    detail.clear();

    std::string host;
    if (!normalizeHostText(hostOrIp, host, detail)) {
        err = ResolveError::InvalidInput;
        return false;
    }

    Endpoint numeric;
    if (Endpoint::parse(host, port, numeric)) {
        ResolvedEndpoint resolved;
        resolved.endpoint = numeric;
        resolved.wasHostname = false;
        resolved.canonicalText = numeric.str();
        out.push_back(std::move(resolved));
        return true;
    }

    if (isIpv6Literal(host)) {
        err = ResolveError::NoAddress;
        detail = kIpv6UnsupportedDetail;
        QLOG("[resolve] '%s' rejected: %s", host.c_str(), detail.c_str());
        return false;
    }

    if (!looksLikeHostname(host, detail)) {
        err = ResolveError::InvalidInput;
        return false;
    }

    unsigned budgetMs = timeoutMs == 0 ? kDefaultResolveTimeoutMs : timeoutMs;
    if (budgetMs > kMaxResolveTimeoutMs) budgetMs = kMaxResolveTimeoutMs;

    if (!netInit()) {
        err = ResolveError::SystemError;
        detail = "WSAStartup failed before name resolution";
        return false;
    }

    auto* job = new (std::nothrow) ResolveJob();
    if (job == nullptr) {
        netShutdown();
        err = ResolveError::SystemError;
        detail = "out of memory starting the bounded resolver";
        return false;
    }
    job->finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    job->host.assign(host.begin(), host.end());
    if (job->finished == nullptr) {
        job->refs = 1;
        releaseJob(job);
        netShutdown();
        err = ResolveError::SystemError;
        detail = "could not create the resolver completion event";
        return false;
    }

    const uintptr_t thread = _beginthreadex(nullptr, 0, &resolveWorker, job,
                                            0, nullptr);
    if (thread == 0) {
        job->refs = 1;
        releaseJob(job);
        netShutdown();
        err = ResolveError::SystemError;
        detail = "could not start the bounded resolver worker";
        return false;
    }
    CloseHandle(reinterpret_cast<HANDLE>(thread));

    const DWORD waited = WaitForSingleObject(job->finished, budgetMs);
    if (waited != WAIT_OBJECT_0) {
        releaseJob(job);
        err = ResolveError::Timeout;
        detail = "'" + host + "' did not resolve within "
               + std::to_string(budgetMs) + " ms";
        QLOG("[resolve] %s", detail.c_str());
        return false;
    }

    const int status = job->status;
    std::vector<ResolvedSample> samples;
    samples.swap(job->samples);
    releaseJob(job);

    if (status != 0) {
        err = (status == WSAHOST_NOT_FOUND || status == WSANO_DATA)
            ? ResolveError::NotFound
            : ResolveError::SystemError;
        detail = "could not resolve '" + host + "' (winsock error "
               + std::to_string(status) + ")";
        QLOG("[resolve] %s", detail.c_str());
        return false;
    }

    if (!filterResolvedSamples(samples.data(), samples.size(), port, out, err,
                               detail)) {
        QLOG("[resolve] '%s' -> %s: %s", host.c_str(), resolveErrorText(err),
             detail.c_str());
        return false;
    }

    QLOG("[resolve] '%s' -> %zu IPv4 candidate(s); using %s",
         host.c_str(), out.size(), out.front().canonicalText.c_str());
    return true;
}

bool resolveEndpoint(const std::string& hostOrIp, uint16_t port,
                     unsigned timeoutMs, ResolvedEndpoint& out,
                     ResolveError& err, std::string& detail) {
    std::vector<ResolvedEndpoint> all;
    if (!resolveEndpoints(hostOrIp, port, timeoutMs, all, err, detail))
        return false;
    if (all.empty()) {
        err = ResolveError::NoAddress;
        detail = "resolver returned success with no candidate";
        return false;
    }
    out = all.front();
    return true;
}

bool UdpSock::open(uint16_t port) {
    close();
    _lastErr = 0;
    _s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_s == INVALID_SOCKET) { _lastErr = WSAGetLastError(); return false; }
    disableUdpConnectionReset(_s);
    BOOL excl = TRUE;
    setsockopt(_s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&excl), sizeof(excl));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(port);
    if (bind(_s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        _lastErr = WSAGetLastError();
        close();
        return false;
    }
    if (!setNonBlocking(_s)) { _lastErr = WSAGetLastError(); close(); return false; }
    return true;
}

bool UdpSock::adopt(SOCKET s) {
    close();
    _s = s;
    if (_s == INVALID_SOCKET) return false;
    disableUdpConnectionReset(_s);
    return setNonBlocking(_s);
}

SOCKET UdpSock::release() {
    const SOCKET s = _s;
    _s = INVALID_SOCKET;
    return s;
}

void UdpSock::close() {
    if (_s != INVALID_SOCKET) { closesocket(_s); _s = INVALID_SOCKET; }
}

uint16_t UdpSock::boundPort() const {
    if (_s == INVALID_SOCKET) return 0;
    sockaddr_in sa{}; int len = sizeof(sa);
    if (getsockname(_s, reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0;
    return ntohs(sa.sin_port);
}

bool UdpSock::sendTo(const Endpoint& to, const void* data, size_t len) {
    if (_s == INVALID_SOCKET) return false;
    int r = ::sendto(_s, static_cast<const char*>(data), static_cast<int>(len), 0,
                     reinterpret_cast<const sockaddr*>(&to.sa), sizeof(to.sa));
    if (r == SOCKET_ERROR) _lastErr = WSAGetLastError();
    return r == static_cast<int>(len);
}

int UdpSock::recvFrom(void* buf, size_t cap, Endpoint& from) {
    if (_s == INVALID_SOCKET) return -1;
    for (uint32_t resetCount = 0; resetCount < 256u; ++resetCount) {
        int fromLen = sizeof(from.sa);
        const int r = ::recvfrom(
            _s, static_cast<char*>(buf), static_cast<int>(cap), 0,
            reinterpret_cast<sockaddr*>(&from.sa), &fromLen);
        if (r != SOCKET_ERROR) return r;
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) return 0;
        if (error == WSAECONNRESET) continue;
        _lastErr = error;
        return -1;
    }
    return 0;
}

bool UdpSock::waitReadable(int timeoutMs) {
    if (_s == INVALID_SOCKET) return false;
    fd_set rd; FD_ZERO(&rd); FD_SET(_s, &rd);
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    return select(0, &rd, nullptr, nullptr, &tv) == 1;
}

int UdpSock::drain() {
    if (_s == INVALID_SOCKET) return 0;
    char buf[2048]; Endpoint from; int n = 0;
    while (recvFrom(buf, sizeof(buf), from) > 0) ++n;
    return n;
}

bool TcpConn::connectTo(const std::string& ip, uint16_t port, int timeoutMs) {
    close();
    _lastErr = 0;
    _s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_s == INVALID_SOCKET) { _lastErr = WSAGetLastError(); return false; }
    setNonBlocking(_s);
    Endpoint ep;
    if (!Endpoint::parse(ip, port, ep)) { _lastErr = WSAEINVAL; close(); return false; }
    int r = ::connect(_s, reinterpret_cast<sockaddr*>(&ep.sa), sizeof(ep.sa));
    if (r != 0 && WSAGetLastError() != WSAEWOULDBLOCK) { _lastErr = WSAGetLastError(); close(); return false; }
    fd_set wr, ex; FD_ZERO(&wr); FD_ZERO(&ex); FD_SET(_s, &wr); FD_SET(_s, &ex);
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    int sel = select(0, nullptr, &wr, &ex, &tv);
    if (sel != 1 || FD_ISSET(_s, &ex)) {
        if (sel == 0) _lastErr = WSAETIMEDOUT;
        else {
            int soErr = 0, len = sizeof(soErr);
            getsockopt(_s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
            _lastErr = soErr ? soErr : WSAGetLastError();
        }
        close();
        return false;
    }
    BOOL nd = TRUE;
    setsockopt(_s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&nd), sizeof(nd));
    return true;
}

void TcpConn::close() {
    if (_s != INVALID_SOCKET) { closesocket(_s); _s = INVALID_SOCKET; }
    _rxBuf.clear();
}

void TcpConn::closeGraceful(int capMs) {
    if (_s == INVALID_SOCKET) return;
    shutdown(_s, SD_SEND);
    uint64_t deadline = nowUs() + (uint64_t)capMs * 1000;
    char tmp[1024];
    for (;;) {
        int64_t remainUs = (int64_t)deadline - (int64_t)nowUs();
        if (remainUs <= 0) break;
        fd_set rd; FD_ZERO(&rd); FD_SET(_s, &rd);
        timeval tv{ (long)(remainUs / 1000000), (long)(remainUs % 1000000) };
        if (select(0, &rd, nullptr, nullptr, &tv) != 1) break;
        int r = ::recv(_s, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
    }
    close();
}

bool TcpConn::sendMsg(uint8_t type, const void* body, size_t bodyLen) {
    if (_s == INVALID_SOCKET) { _lastErr = WSAENOTSOCK; return false; }
    uint8_t msg[kMaxMsgSize];
    size_t msgLen = packMsg(type, body, bodyLen, msg, sizeof(msg));
    if (!msgLen || msgLen > 0xFFFF) { _lastErr = WSAEMSGSIZE; return false; }
    uint8_t frame[kMaxMsgSize + 2];
    frame[0] = static_cast<uint8_t>(msgLen & 0xFF);
    frame[1] = static_cast<uint8_t>(msgLen >> 8);
    std::memcpy(frame + 2, msg, msgLen);
    size_t total = msgLen + 2, sent = 0;
    const uint64_t deadline = nowUs() + 5000000ull;
    while (sent < total) {
        int r = ::send(_s, reinterpret_cast<const char*>(frame) + sent,
                       static_cast<int>(total - sent), 0);
        if (r == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                if (nowUs() >= deadline) { _lastErr = WSAETIMEDOUT; return false; }
                Sleep(1); continue;
            }
            _lastErr = WSAGetLastError();
            return false;
        }
        sent += r;
    }
    return true;
}

TcpConn::Rs TcpConn::recvMsgEx(Received& out, int timeoutMs) {
    out.type = MSG_NONE;
    out.body.clear();
    if (_s == INVALID_SOCKET) { _lastErr = WSAENOTSOCK; return Rs::Error; }
    const uint64_t budgetUs = timeoutMs > 0
        ? static_cast<uint64_t>(timeoutMs) * 1000
        : 0;
    const uint64_t deadline = nowUs() + budgetUs;
    bool polledSocket = false;

    for (;;) {
        if (_rxBuf.size() >= 2) {
            size_t frameLen = _rxBuf[0] | (_rxBuf[1] << 8);
            if (frameLen > kMaxMsgSize) { close(); return Rs::Corrupt; }
            if (_rxBuf.size() >= 2 + frameLen) {
                uint8_t type; const uint8_t* body; size_t bodyLen;
                bool ok = unpackMsg(_rxBuf.data() + 2, frameLen, type, body, bodyLen);
                if (!ok) { close(); return Rs::Corrupt; }
                out.type = type;
                out.body.assign(body, body + bodyLen);
                _rxBuf.erase(_rxBuf.begin(), _rxBuf.begin() + 2 + frameLen);
                return Rs::Msg;
            }
        }
        int64_t remainUs = static_cast<int64_t>(deadline) - static_cast<int64_t>(nowUs());
        if (remainUs <= 0 && polledSocket) return Rs::Timeout;
        if (remainUs < 0) remainUs = 0;
        fd_set rd; FD_ZERO(&rd); FD_SET(_s, &rd);
        timeval tv{ static_cast<long>(remainUs / 1000000),
                    static_cast<long>(remainUs % 1000000) };
        polledSocket = true;
        int sel = select(0, &rd, nullptr, nullptr, &tv);
        if (sel == 0) return Rs::Timeout;
        if (sel == SOCKET_ERROR) { _lastErr = WSAGetLastError(); close(); return Rs::Error; }
        char tmp[4096];
        int r = ::recv(_s, tmp, sizeof(tmp), 0);
        if (r == 0) { close(); return Rs::Closed; }
        if (r == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
            _lastErr = WSAGetLastError();
            close(); return Rs::Error;
        }
        _rxBuf.insert(_rxBuf.end(), tmp, tmp + r);
    }
}

bool TcpConn::recvMsg(Received& out, int timeoutMs) {
    Rs rs = recvMsgEx(out, timeoutMs);
    return rs == Rs::Msg || rs == Rs::Timeout;
}

bool TcpListener::listen(uint16_t port) {
    close();
    _lastErr = 0;
    _s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_s == INVALID_SOCKET) { _lastErr = WSAGetLastError(); return false; }
    BOOL excl = TRUE;
    setsockopt(_s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&excl), sizeof(excl));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(port);
    if (bind(_s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 ||
        ::listen(_s, 1) != 0) {
        _lastErr = WSAGetLastError();
        close();
        return false;
    }
    if (!setNonBlocking(_s)) { _lastErr = WSAGetLastError(); close(); return false; }
    return true;
}

void TcpListener::close() {
    if (_s != INVALID_SOCKET) { closesocket(_s); _s = INVALID_SOCKET; }
}

TcpConn TcpListener::acceptOne(int timeoutMs, Endpoint* peer) {
    if (_s == INVALID_SOCKET) return TcpConn();
    fd_set rd; FD_ZERO(&rd); FD_SET(_s, &rd);
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    if (select(0, &rd, nullptr, nullptr, &tv) != 1) return TcpConn();
    sockaddr_in from{}; int fromLen = sizeof(from);
    SOCKET c = ::accept(_s, reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (c == INVALID_SOCKET) { _lastErr = WSAGetLastError(); return TcpConn(); }
    if (peer) peer->sa = from;
    setNonBlocking(c);
    BOOL nd = TRUE;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&nd), sizeof(nd));
    return TcpConn(c);
}

}
