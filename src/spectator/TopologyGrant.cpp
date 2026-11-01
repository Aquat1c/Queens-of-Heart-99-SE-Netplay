#include "TopologyGrant.h"

namespace qnetplay::spectator {
namespace {

// Little-endian bytes spell "QTG1".
constexpr uint32_t kTopologyGrantMagic = 0x31475451u;
constexpr uint16_t kTopologyGrantVersion = 1u;

void writeLe16(uint8_t* p, uint16_t value) {
    for (unsigned i = 0; i < 2; ++i)
        p[i] = static_cast<uint8_t>(value >> (i * 8));
}

void writeLe32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = static_cast<uint8_t>(value >> (i * 8));
}

void writeLe64(uint8_t* p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>(value >> (i * 8));
}

uint64_t readLe64(const uint8_t* p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(p[i]) << (i * 8);
    return value;
}

bool equalConstantTime(uint64_t leftLow, uint64_t leftHigh,
                       uint64_t rightLow, uint64_t rightHigh) {
    const uint64_t difference =
        (leftLow ^ rightLow) | (leftHigh ^ rightHigh);
    return difference == 0;
}

bool validGrantScalars(const TopologyGrant& grant) {
    return grant.grantId != 0
        && grant.nodeId != 0
        && grant.sessionId != 0
        && grant.expiryUnix != 0
        && grant.authorityGeneration != 0;
}

}

bool encodeTopologyGrantBody(
    const TopologyGrant& grant,
    std::array<uint8_t, kTopologyGrantBodyBytes>& out) {
    if (!validGrantScalars(grant)) return false;
    std::array<uint8_t, kTopologyGrantBodyBytes> encoded{};
    writeLe32(encoded.data(), kTopologyGrantMagic);
    writeLe16(encoded.data() + 4, kTopologyGrantVersion);
    // Bytes [6,8) are reserved and zero.
    writeLe64(encoded.data() + 8, grant.grantId);
    writeLe64(encoded.data() + 16, grant.nodeId);
    writeLe64(encoded.data() + 24, grant.sessionId);
    writeLe64(encoded.data() + 32, grant.expiryUnix);
    writeLe32(encoded.data() + 40, grant.authorityGeneration);
    out = encoded;
    return true;
}

bool issueTopologyGrant(const TopologyGrantKey& key, TopologyGrant& grant) {
    std::array<uint8_t, kTopologyGrantBodyBytes> body{};
    if (!encodeTopologyGrantBody(grant, body)) return false;
    std::array<uint8_t, 32> mac{};
    hmacSha256(key.data(), key.size(), body.data(), body.size(), mac);
    grant.macLow = readLe64(mac.data());
    grant.macHigh = readLe64(mac.data() + 8);
    return true;
}

GrantVerdict verifyTopologyGrant(const TopologyGrantKey& key,
                                 const TopologyGrant& grant,
                                 uint64_t expectedNodeId,
                                 uint64_t expectedSessionId,
                                 uint64_t nowUnix) {
    std::array<uint8_t, kTopologyGrantBodyBytes> body{};
    if (!encodeTopologyGrantBody(grant, body))
        return GrantVerdict::Malformed;
    std::array<uint8_t, 32> mac{};
    hmacSha256(key.data(), key.size(), body.data(), body.size(), mac);
    if (!equalConstantTime(grant.macLow, grant.macHigh,
                           readLe64(mac.data()),
                           readLe64(mac.data() + 8))) {
        return GrantVerdict::BadMac;
    }
    if (expectedNodeId != 0 && grant.nodeId != expectedNodeId)
        return GrantVerdict::WrongViewer;
    if (expectedSessionId != 0 && grant.sessionId != expectedSessionId)
        return GrantVerdict::WrongSession;
    if (nowUnix >= grant.expiryUnix) return GrantVerdict::Expired;
    return GrantVerdict::Ok;
}

}
