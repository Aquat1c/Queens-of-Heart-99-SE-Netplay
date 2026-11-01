#pragma once

#include <cstddef>
#include <cstdint>

namespace qnet::autopunch {

inline constexpr const char* kRelayHost = "delthas.fr";
inline constexpr uint16_t kRelayPort = 14763;

inline constexpr size_t kRegisterBytes = 2;
inline constexpr size_t kLookupBytes = 8;
inline constexpr size_t kMappingBytes = 8;
inline constexpr size_t kPunchBytes = 1;

inline constexpr size_t kMaxPacketBytes = kLookupBytes;

bool encodeRegister(uint16_t localPort, uint8_t out[kRegisterBytes]);

bool encodeLookup(uint16_t localPort,
                  uint32_t targetIpv4NetworkOrder,
                  uint16_t targetPort,
                  uint8_t out[kLookupBytes]);

inline constexpr uint8_t kPunchByte = 0x00u;

struct Mapping {
    uint16_t internalPort = 0;
    uint16_t natPort = 0;
    uint32_t ipv4NetworkOrder = 0;
};

// The caller must establish that the datagram came from the relay first; a forged mapping would redirect punch bursts.
bool decodeMapping(const uint8_t* bytes, size_t length, Mapping& out);

bool isPunchProbe(const uint8_t* bytes, size_t length);

}
