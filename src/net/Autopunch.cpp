#include "Autopunch.h"

#include <cstring>

namespace qnet::autopunch {

namespace {

void writeBe16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    out[1] = static_cast<uint8_t>(value & 0xFFu);
}

uint16_t readBe16(const uint8_t* in) {
    return static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8)
                                 | static_cast<uint16_t>(in[1]));
}

}

bool encodeRegister(uint16_t localPort, uint8_t out[kRegisterBytes]) {
    if (out == nullptr || localPort == 0u)
        return false;
    writeBe16(out, localPort);
    return true;
}

bool encodeLookup(uint16_t localPort,
                  uint32_t targetIpv4NetworkOrder,
                  uint16_t targetPort,
                  uint8_t out[kLookupBytes]) {
    if (out == nullptr || localPort == 0u || targetPort == 0u
        || targetIpv4NetworkOrder == 0u) {
        return false;
    }
    writeBe16(out, localPort);
    // Copy without conversion: the address is in network byte order both here and at the relay.
    std::memcpy(out + 2, &targetIpv4NetworkOrder, sizeof(targetIpv4NetworkOrder));
    writeBe16(out + 6, targetPort);
    return true;
}

bool decodeMapping(const uint8_t* bytes, size_t length, Mapping& out) {
    out = Mapping{};
    if (bytes == nullptr || length != kMappingBytes)
        return false;

    Mapping mapping{};
    mapping.internalPort = readBe16(bytes);
    mapping.natPort = readBe16(bytes + 2);
    std::memcpy(&mapping.ipv4NetworkOrder, bytes + 4,
                sizeof(mapping.ipv4NetworkOrder));

    if (mapping.internalPort == 0u || mapping.natPort == 0u
        || mapping.ipv4NetworkOrder == 0u) {
        return false;
    }
    out = mapping;
    return true;
}

bool isPunchProbe(const uint8_t* bytes, size_t length) {
    return bytes != nullptr && length == kPunchBytes && bytes[0] == kPunchByte;
}

}
