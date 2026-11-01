#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qnet::portmap {

inline constexpr uint32_t kLeaseSeconds = 3600;
inline constexpr uint64_t kRenewIntervalMs = 900000;
inline constexpr uint64_t kDiscoveryTimeoutMs = 3000;
inline constexpr uint64_t kHttpTimeoutMs = 4000;
inline constexpr uint16_t kNatPmpPort = 5351;
inline constexpr uint16_t kSsdpPort = 1900;
inline constexpr const char* kSsdpMulticast = "239.255.255.250";

namespace wire {

inline constexpr size_t kNatPmpRequestBytes = 12;
inline constexpr size_t kNatPmpResponseBytes = 16;

void encodeNatPmpUdpRequest(uint16_t internalPort, uint16_t suggestedExternal,
                            uint32_t lifetimeSeconds,
                            uint8_t out[kNatPmpRequestBytes]);

struct NatPmpReply {
    uint16_t resultCode = 0;
    uint16_t internalPort = 0;
    uint16_t externalPort = 0;
    uint32_t lifetimeSeconds = 0;
};

bool decodeNatPmpReply(const uint8_t* bytes, size_t length, NatPmpReply& out);

std::string buildSsdpSearch(const char* searchTarget, unsigned mx);

bool parseSsdpLocation(const std::string& response, std::string& location);

struct HttpUrl {
    std::string host;
    uint16_t port = 80;
    std::string path;
};

bool parseHttpUrl(const std::string& url, HttpUrl& out);

bool resolveControlUrl(const HttpUrl& base, const std::string& controlUrl,
                       HttpUrl& out);

struct IgdService {
    std::string serviceType;
    std::string controlUrl;
};

bool findWanConnectionService(const std::string& xml, IgdService& out);

struct AddMapping {
    uint16_t externalPort = 0;
    uint16_t internalPort = 0;
    std::string internalClient;
    std::string description;
    uint32_t leaseSeconds = kLeaseSeconds;
};

std::string buildAddPortMappingBody(const std::string& serviceType,
                                    const AddMapping& mapping);
std::string buildDeletePortMappingBody(const std::string& serviceType,
                                       uint16_t externalPort);

std::string buildSoapRequest(const HttpUrl& target,
                             const std::string& serviceType,
                             const char* action, const std::string& body);

int parseHttpStatus(const std::string& response);

int parseUpnpErrorCode(const std::string& response);

bool splitHttpBody(const std::string& response, std::string& body);

}

enum class MappingState : uint8_t {
    Idle = 0,
    Working,
    Mapped,
    Unavailable,
};

struct MappingStatus {
    MappingState state = MappingState::Idle;
    uint16_t internalPort = 0;
    uint16_t externalPort = 0;
    std::string method;
    std::string detail;
};

class PortMapper {
public:
    PortMapper() = default;
    ~PortMapper();
    PortMapper(const PortMapper&) = delete;
    PortMapper& operator=(const PortMapper&) = delete;

    void requestUdp(uint16_t port, const std::string& description);

    MappingStatus status() const;

    void release();

private:
    struct Impl;
    Impl* _impl = nullptr;
};

}
