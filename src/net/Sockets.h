#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>
#include <vector>

namespace qnet {

bool netInit();
void netShutdown();

struct Endpoint {
    sockaddr_in sa{};
    Endpoint() { sa.sin_family = AF_INET; }
    static bool parse(const std::string& ip, uint16_t port, Endpoint& out);
    std::string str() const;
    bool operator==(const Endpoint& o) const {
        return sa.sin_addr.s_addr == o.sa.sin_addr.s_addr && sa.sin_port == o.sa.sin_port;
    }
};

enum class ResolveError : uint8_t {
    None = 0,
    InvalidInput,
    NotFound,
    NoAddress,
    Timeout,
    SystemError,
};

struct ResolvedEndpoint {
    Endpoint endpoint{};
    bool wasHostname = false;
    std::string canonicalText;
};

constexpr size_t   kMaxHostTextLength      = 255;
constexpr size_t   kMaxResolvedEndpoints   = 8;
constexpr unsigned kDefaultResolveTimeoutMs = 4000;
constexpr unsigned kMaxResolveTimeoutMs     = 60000;

bool resolveEndpoints(const std::string& hostOrIp, uint16_t port,
                      unsigned timeoutMs, std::vector<ResolvedEndpoint>& out,
                      ResolveError& err, std::string& detail);

bool resolveEndpoint(const std::string& hostOrIp, uint16_t port,
                     unsigned timeoutMs, ResolvedEndpoint& out,
                     ResolveError& err, std::string& detail);

const char* resolveErrorText(ResolveError error);

const char* resolveIpv6UnsupportedText();

bool resolveFamilySupported(int family);

struct ResolvedSample {
    int family = 0;
    uint32_t ipv4BE = 0;
};

bool filterResolvedSamples(const ResolvedSample* samples, size_t count,
                           uint16_t port, std::vector<ResolvedEndpoint>& out,
                           ResolveError& err, std::string& detail);

class UdpSock {
public:
    UdpSock() = default;
    ~UdpSock() { close(); }
    UdpSock(const UdpSock&) = delete;
    UdpSock& operator=(const UdpSock&) = delete;

    bool open(uint16_t port);
    bool adopt(SOCKET s);
    SOCKET release();
    void close();
    bool isOpen() const { return _s != INVALID_SOCKET; }
    SOCKET raw() const { return _s; }
    uint16_t boundPort() const;
    int  lastError() const { return _lastErr; }

    bool sendTo(const Endpoint& to, const void* data, size_t len);
    int  recvFrom(void* buf, size_t cap, Endpoint& from);
    bool waitReadable(int timeoutMs);
    int  drain();

private:
    SOCKET _s = INVALID_SOCKET;
    int    _lastErr = 0;
};

class TcpConn {
public:
    TcpConn() = default;
    explicit TcpConn(SOCKET s) : _s(s) {}
    ~TcpConn() { close(); }
    TcpConn(TcpConn&& o) noexcept { _s = o._s; _rxBuf = std::move(o._rxBuf); _lastErr = o._lastErr; o._s = INVALID_SOCKET; }
    TcpConn& operator=(TcpConn&& o) noexcept {
        if (this != &o) { close(); _s = o._s; _rxBuf = std::move(o._rxBuf); _lastErr = o._lastErr; o._s = INVALID_SOCKET; }
        return *this;
    }
    TcpConn(const TcpConn&) = delete;
    TcpConn& operator=(const TcpConn&) = delete;

    bool connectTo(const std::string& ip, uint16_t port, int timeoutMs);
    void close();
    void closeGraceful(int capMs = 1000);
    bool isOpen() const { return _s != INVALID_SOCKET; }
    SOCKET raw() const { return _s; }
    int  lastError() const { return _lastErr; }

    // Message framing: [u16 totalLen][header+body].
    bool sendMsg(uint8_t type, const void* body, size_t bodyLen);

    struct Received { uint8_t type = 0; std::vector<uint8_t> body; };
    enum class Rs { Msg, Timeout, Closed, Corrupt, Error };
    Rs recvMsgEx(Received& out, int timeoutMs);
    bool recvMsg(Received& out, int timeoutMs);

private:
    SOCKET _s = INVALID_SOCKET;
    std::vector<uint8_t> _rxBuf;
    int    _lastErr = 0;
};

class TcpListener {
public:
    ~TcpListener() { close(); }
    bool listen(uint16_t port);
    void close();
    bool isOpen() const { return _s != INVALID_SOCKET; }
    int  lastError() const { return _lastErr; }
    TcpConn acceptOne(int timeoutMs, Endpoint* peer = nullptr);

private:
    SOCKET _s = INVALID_SOCKET;
    int    _lastErr = 0;
};

uint64_t nowUs();

size_t collectLocalIpv4BE(uint32_t* out, size_t cap);

}
