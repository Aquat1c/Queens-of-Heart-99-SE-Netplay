#include "PortMapper.h"

#include "Sockets.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace qnet::portmap {
namespace wire {
namespace {

char lowerAscii(const char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowerCopy(const std::string& in) {
    std::string out(in);
    for (char& c : out) c = lowerAscii(c);
    return out;
}

size_t findNoCase(const std::string& haystack, const char* needle,
                  const size_t from = 0) {
    const std::string lowered = lowerCopy(haystack);
    std::string lowerNeedle(needle);
    for (char& c : lowerNeedle) c = lowerAscii(c);
    return lowered.find(lowerNeedle, from);
}

void appendUInt(std::string& out, const uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%u", value);
    out += text;
}

std::string escapeXml(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20u) out += c;
                break;
        }
    }
    return out;
}

bool elementText(const std::string& xml, const std::string& tag,
                 const size_t from, std::string& out, size_t& afterEnd) {
    const std::string open = "<" + tag;
    const std::string close = "</" + tag;
    const size_t openAt = findNoCase(xml, open.c_str(), from);
    if (openAt == std::string::npos) return false;
    const size_t openEnd = xml.find('>', openAt);
    if (openEnd == std::string::npos) return false;
    const size_t closeAt = findNoCase(xml, close.c_str(), openEnd);
    if (closeAt == std::string::npos) return false;
    out.assign(xml, openEnd + 1, closeAt - openEnd - 1);
    const size_t first = out.find_first_not_of(" \t\r\n");
    const size_t last = out.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) out.clear();
    else out = out.substr(first, last - first + 1);
    afterEnd = closeAt;
    return true;
}

}

void encodeNatPmpUdpRequest(const uint16_t internalPort,
                            const uint16_t suggestedExternal,
                            const uint32_t lifetimeSeconds,
                            uint8_t out[kNatPmpRequestBytes]) {
    std::memset(out, 0, kNatPmpRequestBytes);
    out[0] = 0u;
    out[1] = 1u;   // opcode 1 = map UDP
    out[4] = static_cast<uint8_t>(internalPort >> 8);
    out[5] = static_cast<uint8_t>(internalPort & 0xFFu);
    out[6] = static_cast<uint8_t>(suggestedExternal >> 8);
    out[7] = static_cast<uint8_t>(suggestedExternal & 0xFFu);
    out[8] = static_cast<uint8_t>((lifetimeSeconds >> 24) & 0xFFu);
    out[9] = static_cast<uint8_t>((lifetimeSeconds >> 16) & 0xFFu);
    out[10] = static_cast<uint8_t>((lifetimeSeconds >> 8) & 0xFFu);
    out[11] = static_cast<uint8_t>(lifetimeSeconds & 0xFFu);
}

bool decodeNatPmpReply(const uint8_t* bytes, const size_t length,
                       NatPmpReply& out) {
    out = NatPmpReply{};
    if (bytes == nullptr || length < kNatPmpResponseBytes) return false;
    if (bytes[0] != 0u) return false;
    if (bytes[1] != 129u) return false;        // opcode 1 | 0x80
    out.resultCode = static_cast<uint16_t>(
        (static_cast<uint16_t>(bytes[2]) << 8) | bytes[3]);
    out.internalPort = static_cast<uint16_t>(
        (static_cast<uint16_t>(bytes[8]) << 8) | bytes[9]);
    out.externalPort = static_cast<uint16_t>(
        (static_cast<uint16_t>(bytes[10]) << 8) | bytes[11]);
    out.lifetimeSeconds = (static_cast<uint32_t>(bytes[12]) << 24)
        | (static_cast<uint32_t>(bytes[13]) << 16)
        | (static_cast<uint32_t>(bytes[14]) << 8)
        | static_cast<uint32_t>(bytes[15]);
    return true;
}

std::string buildSsdpSearch(const char* searchTarget, const unsigned mx) {
    std::string out = "M-SEARCH * HTTP/1.1\r\n";
    out += "HOST: ";
    out += kSsdpMulticast;
    out += ":1900\r\n";
    out += "MAN: \"ssdp:discover\"\r\n";
    out += "MX: ";
    appendUInt(out, mx);
    out += "\r\nST: ";
    out += (searchTarget != nullptr && *searchTarget != '\0') ? searchTarget
                                                              : "ssdp:all";
    out += "\r\n\r\n";
    return out;
}

bool parseSsdpLocation(const std::string& response, std::string& location) {
    location.clear();
    size_t at = 0;
    for (;;) {
        const size_t found = findNoCase(response, "location:", at);
        if (found == std::string::npos) return false;
        if (found != 0 && response[found - 1] != '\n'
            && response[found - 1] != '\r') {
            at = found + 1;
            continue;
        }
        size_t valueAt = found + 9;
        while (valueAt < response.size()
               && (response[valueAt] == ' ' || response[valueAt] == '\t')) {
            ++valueAt;
        }
        size_t end = valueAt;
        while (end < response.size() && response[end] != '\r'
               && response[end] != '\n') {
            ++end;
        }
        std::string candidate = response.substr(valueAt, end - valueAt);
        if (lowerCopy(candidate).compare(0, 7, "http://") == 0) {
            location = candidate;
            return true;
        }
        at = end;
    }
}

bool parseHttpUrl(const std::string& url, HttpUrl& out) {
    out = HttpUrl{};
    const std::string lowered = lowerCopy(url);
    if (lowered.compare(0, 7, "http://") != 0) return false;
    const size_t hostAt = 7;
    size_t hostEnd = url.find_first_of("/", hostAt);
    std::string authority = hostEnd == std::string::npos
        ? url.substr(hostAt)
        : url.substr(hostAt, hostEnd - hostAt);
    if (authority.empty()) return false;
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        const std::string portText = authority.substr(colon + 1);
        if (portText.empty()) return false;
        uint32_t value = 0;
        for (const char c : portText) {
            if (c < '0' || c > '9') return false;
            value = value * 10u + static_cast<uint32_t>(c - '0');
            if (value > 65535u) return false;
        }
        if (value == 0u) return false;
        out.port = static_cast<uint16_t>(value);
        authority = authority.substr(0, colon);
    }
    if (authority.empty()) return false;
    out.host = authority;
    out.path = hostEnd == std::string::npos ? "/" : url.substr(hostEnd);
    if (out.path.empty()) out.path = "/";
    return true;
}

bool resolveControlUrl(const HttpUrl& base, const std::string& controlUrl,
                       HttpUrl& out) {
    out = HttpUrl{};
    if (controlUrl.empty()) return false;
    if (lowerCopy(controlUrl).compare(0, 7, "http://") == 0) {
        if (!parseHttpUrl(controlUrl, out)) return false;
        return out.host == base.host && out.port == base.port;
    }
    out.host = base.host;
    out.port = base.port;
    if (controlUrl[0] == '/') {
        out.path = controlUrl;
        return true;
    }
    const size_t lastSlash = base.path.rfind('/');
    out.path = lastSlash == std::string::npos
        ? "/" + controlUrl
        : base.path.substr(0, lastSlash + 1) + controlUrl;
    return true;
}

bool findWanConnectionService(const std::string& xml, IgdService& out) {
    out = IgdService{};
    static const char* kWanted[] = {
        "urn:schemas-upnp-org:service:WANIPConnection:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };
    for (const char* wanted : kWanted) {
        size_t search = 0;
        for (;;) {
            std::string type;
            size_t after = 0;
            if (!elementText(xml, "serviceType", search, type, after)) break;
            search = after;
            if (type != wanted) continue;
            std::string control;
            size_t controlAfter = 0;
            if (!elementText(xml, "controlURL", after, control, controlAfter))
                break;
            if (control.empty()) break;
            out.serviceType = type;
            out.controlUrl = control;
            return true;
        }
    }
    return false;
}

std::string buildAddPortMappingBody(const std::string& serviceType,
                                    const AddMapping& mapping) {
    std::string out =
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:AddPortMapping xmlns:u=\"";
    out += escapeXml(serviceType);
    out += "\">";
    out += "<NewRemoteHost></NewRemoteHost><NewExternalPort>";
    appendUInt(out, mapping.externalPort);
    out += "</NewExternalPort><NewProtocol>UDP</NewProtocol>"
           "<NewInternalPort>";
    appendUInt(out, mapping.internalPort);
    out += "</NewInternalPort><NewInternalClient>";
    out += escapeXml(mapping.internalClient);
    out += "</NewInternalClient><NewEnabled>1</NewEnabled>"
           "<NewPortMappingDescription>";
    out += escapeXml(mapping.description);
    out += "</NewPortMappingDescription><NewLeaseDuration>";
    appendUInt(out, mapping.leaseSeconds);
    out += "</NewLeaseDuration></u:AddPortMapping></s:Body></s:Envelope>";
    return out;
}

std::string buildDeletePortMappingBody(const std::string& serviceType,
                                       const uint16_t externalPort) {
    std::string out =
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:DeletePortMapping xmlns:u=\"";
    out += escapeXml(serviceType);
    out += "\">";
    out += "<NewRemoteHost></NewRemoteHost><NewExternalPort>";
    appendUInt(out, externalPort);
    out += "</NewExternalPort><NewProtocol>UDP</NewProtocol>"
           "</u:DeletePortMapping></s:Body></s:Envelope>";
    return out;
}

std::string buildSoapRequest(const HttpUrl& target,
                             const std::string& serviceType,
                             const char* action, const std::string& body) {
    std::string out = "POST ";
    out += target.path;
    out += " HTTP/1.1\r\nHOST: ";
    out += target.host;
    out += ":";
    appendUInt(out, target.port);
    out += "\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\nSOAPACTION: \"";
    out += serviceType;
    out += "#";
    out += action;
    out += "\"\r\nCONNECTION: close\r\nCONTENT-LENGTH: ";
    appendUInt(out, static_cast<uint32_t>(body.size()));
    out += "\r\n\r\n";
    out += body;
    return out;
}

int parseHttpStatus(const std::string& response) {
    if (response.size() < 12) return 0;
    if (lowerCopy(response).compare(0, 5, "http/") != 0) return 0;
    const size_t space = response.find(' ');
    if (space == std::string::npos) return 0;
    int code = 0;
    size_t at = space + 1;
    size_t digits = 0;
    while (at < response.size() && response[at] >= '0' && response[at] <= '9') {
        code = code * 10 + (response[at] - '0');
        ++at;
        if (++digits > 3) return 0;
    }
    return digits == 3 ? code : 0;
}

int parseUpnpErrorCode(const std::string& response) {
    std::string value;
    size_t after = 0;
    if (!elementText(response, "errorCode", 0, value, after)) return 0;
    int code = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') return 0;
        code = code * 10 + (c - '0');
        if (code > 100000) return 0;
    }
    return code;
}

bool splitHttpBody(const std::string& response, std::string& body) {
    body.clear();
    size_t at = response.find("\r\n\r\n");
    size_t skip = 4;
    if (at == std::string::npos) {
        at = response.find("\n\n");
        skip = 2;
    }
    if (at == std::string::npos) return false;
    body = response.substr(at + skip);
    return true;
}

}

namespace {

uint64_t nowMs() {
    return static_cast<uint64_t>(qnet::nowUs() / 1000ull);
}

constexpr size_t kMaxHttpResponseBytes = 64u * 1024u;

bool gatewayAndLocalAddress(Endpoint& gateway, std::string& localAddress) {
    gateway = Endpoint{};
    localAddress.clear();

    in_addr probe{};
    probe.s_addr = htonl(0x08080808u);
    MIB_IPFORWARDROW row{};
    if (GetBestRoute(probe.s_addr, 0, &row) == NO_ERROR
        && row.dwForwardNextHop != 0
        && row.dwForwardNextHop != htonl(INADDR_LOOPBACK)) {
        gateway.sa.sin_family = AF_INET;
        gateway.sa.sin_addr.s_addr = row.dwForwardNextHop;
        gateway.sa.sin_port = 0;
    }

    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr = probe;
    target.sin_port = htons(53);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&target),
                  sizeof(target)) == 0) {
        sockaddr_in mine{};
        int length = static_cast<int>(sizeof(mine));
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&mine), &length) == 0
            && mine.sin_addr.s_addr != 0) {
            char text[INET_ADDRSTRLEN]{};
            if (::inet_ntop(AF_INET, &mine.sin_addr, text, sizeof(text))
                != nullptr) {
                localAddress = text;
            }
        }
    }
    ::closesocket(s);
    return !localAddress.empty();
}

std::string httpExchange(const wire::HttpUrl& target,
                         const std::string& request) {
    Endpoint endpoint{};
    if (!Endpoint::parse(target.host, target.port, endpoint)) return {};
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return {};

    std::string response;
    u_long nonBlocking = 1;
    ::ioctlsocket(s, FIONBIO, &nonBlocking);
    const uint64_t deadline = nowMs() + kHttpTimeoutMs;
    bool connected =
        ::connect(s, reinterpret_cast<const sockaddr*>(&endpoint.sa),
                  sizeof(endpoint.sa)) == 0;
    if (!connected && ::WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(s, &writable);
        timeval tv{};
        tv.tv_sec = static_cast<long>(kHttpTimeoutMs / 1000ull);
        tv.tv_usec = static_cast<long>((kHttpTimeoutMs % 1000ull) * 1000ull);
        connected = ::select(0, nullptr, &writable, nullptr, &tv) == 1;
    }
    if (!connected) {
        ::closesocket(s);
        return {};
    }

    size_t sent = 0;
    while (sent < request.size() && nowMs() < deadline) {
        const int wrote = ::send(s, request.data() + sent,
                                 static_cast<int>(request.size() - sent), 0);
        if (wrote > 0) {
            sent += static_cast<size_t>(wrote);
            continue;
        }
        if (wrote < 0 && ::WSAGetLastError() == WSAEWOULDBLOCK) {
            ::Sleep(5);
            continue;
        }
        ::closesocket(s);
        return {};
    }

    char buffer[4096];
    while (nowMs() < deadline && response.size() < kMaxHttpResponseBytes) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        const int ready = ::select(0, &readable, nullptr, nullptr, &tv);
        if (ready == 0) continue;
        if (ready < 0) break;
        const int got = ::recv(s, buffer, sizeof(buffer), 0);
        if (got == 0) break;
        if (got < 0) {
            if (::WSAGetLastError() == WSAEWOULDBLOCK) continue;
            break;
        }
        response.append(buffer, static_cast<size_t>(got));
    }
    ::closesocket(s);
    return response;
}

void bindMulticastInterface(const UdpSock& socket,
                            const std::string& localAddress) {
    if (localAddress.empty()) return;
    in_addr chosen{};
    if (::inet_pton(AF_INET, localAddress.c_str(), &chosen) != 1) return;
    (void)::setsockopt(socket.raw(), IPPROTO_IP, IP_MULTICAST_IF,
                       reinterpret_cast<const char*>(&chosen), sizeof(chosen));
    const DWORD ttl = 2;
    (void)::setsockopt(socket.raw(), IPPROTO_IP, IP_MULTICAST_TTL,
                       reinterpret_cast<const char*>(&ttl), sizeof(ttl));
}

}

struct PortMapper::Impl {
    mutable std::mutex mutex;
    MappingStatus status;
    std::thread worker;
    std::atomic<bool> stop{false};

    bool mappedByUpnp = false;
    wire::HttpUrl control{};
    std::string serviceType;
    uint16_t mappedExternal = 0;
    uint16_t internalPort = 0;
    bool mappedByNatPmp = false;

    void setStatus(const MappingState state, const char* method,
                   const std::string& detail, const uint16_t external) {
        std::lock_guard<std::mutex> lock(mutex);
        status.state = state;
        status.method = method != nullptr ? method : "";
        status.detail = detail;
        status.externalPort = external;
    }
};

PortMapper::~PortMapper() {
    release();
}

MappingStatus PortMapper::status() const {
    if (_impl == nullptr) return MappingStatus{};
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->status;
}

void PortMapper::requestUdp(const uint16_t port,
                            const std::string& description) {
    if (port == 0) return;
    if (_impl != nullptr) return;
    _impl = new (std::nothrow) Impl();
    if (_impl == nullptr) return;
    _impl->internalPort = port;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->status.state = MappingState::Working;
        _impl->status.internalPort = port;
        _impl->status.detail = "asking the router to open this port";
    }

    Impl* impl = _impl;
    const std::string label = description;
    impl->worker = std::thread([impl, port, label]() {
        Endpoint gateway{};
        std::string localAddress;
        if (!gatewayAndLocalAddress(gateway, localAddress)
            || localAddress.empty()) {
            impl->setStatus(MappingState::Unavailable, nullptr,
                            "no default gateway was found, so no router could "
                            "be asked", 0);
            return;
        }

        if (gateway.sa.sin_addr.s_addr != 0) {
            UdpSock sock;
            if (sock.open(0)) {
                Endpoint pmp = gateway;
                pmp.sa.sin_port = htons(kNatPmpPort);
                uint8_t request[wire::kNatPmpRequestBytes];
                wire::encodeNatPmpUdpRequest(port, port, kLeaseSeconds,
                                             request);
                const uint64_t deadline = nowMs() + 1000ull;
                for (int attempt = 0; attempt < 3 && !impl->stop.load();
                     ++attempt) {
                    sock.sendTo(pmp, request, sizeof(request));
                    uint8_t reply[64];
                    Endpoint from{};
                    const int64_t remain = static_cast<int64_t>(deadline)
                        - static_cast<int64_t>(nowMs());
                    if (remain <= 0) break;
                    sock.waitReadable(static_cast<int>(
                        remain > 300 ? 300 : remain));
                    const int got =
                        sock.recvFrom(reply, sizeof(reply), from);
                    if (got <= 0) continue;
                    if (from.sa.sin_addr.s_addr != gateway.sa.sin_addr.s_addr)
                        continue;
                    wire::NatPmpReply decoded{};
                    if (!wire::decodeNatPmpReply(
                            reply, static_cast<size_t>(got), decoded)) {
                        continue;
                    }
                    if (decoded.resultCode != 0u) break;
                    if (decoded.externalPort == 0u) break;
                    impl->mappedByNatPmp = true;
                    impl->mappedExternal = decoded.externalPort;
                    char text[160];
                    std::snprintf(text, sizeof(text),
                                  "the router opened UDP %u for %u seconds",
                                  static_cast<unsigned>(decoded.externalPort),
                                  static_cast<unsigned>(
                                      decoded.lifetimeSeconds));
                    impl->setStatus(MappingState::Mapped, "NAT-PMP", text,
                                    decoded.externalPort);
                    return;
                }
            }
        }
        if (impl->stop.load()) return;

        std::string location;
        {
            UdpSock sock;
            if (!sock.open(0)) {
                impl->setStatus(MappingState::Unavailable, nullptr,
                                "no local socket for router discovery", 0);
                return;
            }
            bindMulticastInterface(sock, localAddress);
            Endpoint ssdp{};
            if (!Endpoint::parse(kSsdpMulticast, kSsdpPort, ssdp)) {
                impl->setStatus(MappingState::Unavailable, nullptr,
                                "the discovery address could not be formed",
                                0);
                return;
            }
            static const char* kTargets[] = {
                "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                "urn:schemas-upnp-org:service:WANIPConnection:1",
                "ssdp:all",
            };
            const uint64_t deadline = nowMs() + kDiscoveryTimeoutMs;
            for (const char* target : kTargets) {
                if (!location.empty() || impl->stop.load()) break;
                const std::string search = wire::buildSsdpSearch(target, 2);
                sock.sendTo(ssdp, search.data(), search.size());
                while (location.empty() && !impl->stop.load()) {
                    const int64_t remain = static_cast<int64_t>(deadline)
                        - static_cast<int64_t>(nowMs());
                    if (remain <= 0) break;
                    sock.waitReadable(static_cast<int>(
                        remain > 400 ? 400 : remain));
                    uint8_t buffer[2048];
                    Endpoint from{};
                    const int got =
                        sock.recvFrom(buffer, sizeof(buffer), from);
                    if (got <= 0) break;
                    const std::string response(
                        reinterpret_cast<const char*>(buffer),
                        static_cast<size_t>(got));
                    std::string found;
                    if (!wire::parseSsdpLocation(response, found)) continue;
                    wire::HttpUrl url{};
                    if (!wire::parseHttpUrl(found, url)) continue;
                    Endpoint described{};
                    if (!Endpoint::parse(url.host, url.port, described))
                        continue;
                    if (described.sa.sin_addr.s_addr
                        != from.sa.sin_addr.s_addr) {
                        continue;
                    }
                    location = found;
                }
            }
        }
        if (impl->stop.load()) return;
        if (location.empty()) {
            impl->setStatus(MappingState::Unavailable, nullptr,
                            "no router answered UPnP discovery", 0);
            return;
        }

        wire::HttpUrl descriptionUrl{};
        if (!wire::parseHttpUrl(location, descriptionUrl)) {
            impl->setStatus(MappingState::Unavailable, nullptr,
                            "the router's description address was unusable",
                            0);
            return;
        }
        std::string get = "GET ";
        get += descriptionUrl.path;
        get += " HTTP/1.1\r\nHOST: ";
        get += descriptionUrl.host;
        get += "\r\nCONNECTION: close\r\n\r\n";
        const std::string described = httpExchange(descriptionUrl, get);
        std::string body;
        wire::IgdService service{};
        if (described.empty() || !wire::splitHttpBody(described, body)
            || !wire::findWanConnectionService(body, service)) {
            impl->setStatus(MappingState::Unavailable, nullptr,
                            "the router did not describe a mappable internet "
                            "connection", 0);
            return;
        }
        wire::HttpUrl control{};
        if (!wire::resolveControlUrl(descriptionUrl, service.controlUrl,
                                     control)) {
            impl->setStatus(MappingState::Unavailable, nullptr,
                            "the router's control address was not on the "
                            "router", 0);
            return;
        }

        wire::AddMapping mapping{};
        mapping.externalPort = port;
        mapping.internalPort = port;
        mapping.internalClient = localAddress;
        mapping.description = label;
        mapping.leaseSeconds = kLeaseSeconds;
        const std::string add = wire::buildSoapRequest(
            control, service.serviceType, "AddPortMapping",
            wire::buildAddPortMappingBody(service.serviceType, mapping));
        const std::string reply = httpExchange(control, add);
        const int status = wire::parseHttpStatus(reply);
        if (status == 200) {
            impl->mappedByUpnp = true;
            impl->control = control;
            impl->serviceType = service.serviceType;
            impl->mappedExternal = port;
            char text[160];
            std::snprintf(text, sizeof(text),
                          "the router opened UDP %u for this session",
                          static_cast<unsigned>(port));
            impl->setStatus(MappingState::Mapped, "UPnP", text, port);
            return;
        }
        const int upnpError = wire::parseUpnpErrorCode(reply);
        char text[200];
        if (upnpError == 718) {
            std::snprintf(text, sizeof(text),
                          "UDP %u is already mapped to another machine on "
                          "this network",
                          static_cast<unsigned>(port));
        } else if (upnpError != 0) {
            std::snprintf(text, sizeof(text),
                          "the router refused the mapping (UPnP error %d)",
                          upnpError);
        } else if (status != 0) {
            std::snprintf(text, sizeof(text),
                          "the router refused the mapping (HTTP %d)", status);
        } else {
            std::snprintf(text, sizeof(text),
                          "the router did not answer the mapping request");
        }
        impl->setStatus(MappingState::Unavailable, nullptr, text, 0);
    });
}

void PortMapper::release() {
    if (_impl == nullptr) return;
    Impl* impl = _impl;
    _impl = nullptr;
    impl->stop.store(true);
    if (impl->worker.joinable()) impl->worker.join();

    if (impl->mappedByUpnp && impl->mappedExternal != 0) {
        const std::string del = wire::buildSoapRequest(
            impl->control, impl->serviceType, "DeletePortMapping",
            wire::buildDeletePortMappingBody(impl->serviceType,
                                             impl->mappedExternal));
        (void)httpExchange(impl->control, del);
    } else if (impl->mappedByNatPmp && impl->internalPort != 0) {
        Endpoint gateway{};
        std::string ignored;
        if (gatewayAndLocalAddress(gateway, ignored)
            && gateway.sa.sin_addr.s_addr != 0) {
            UdpSock sock;
            if (sock.open(0)) {
                Endpoint pmp = gateway;
                pmp.sa.sin_port = htons(kNatPmpPort);
                uint8_t request[wire::kNatPmpRequestBytes];
                wire::encodeNatPmpUdpRequest(impl->internalPort, 0, 0,
                                             request);
                sock.sendTo(pmp, request, sizeof(request));
            }
        }
    }
    delete impl;
}

}
