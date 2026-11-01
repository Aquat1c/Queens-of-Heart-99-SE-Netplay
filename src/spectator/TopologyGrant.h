#pragma once

#include "SpectatorGrant.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace qnetplay::spectator {

// Layout: domain magic + version + reserved + five bound fields.
constexpr size_t kTopologyGrantBodyBytes = 44u;
using TopologyGrantKey = SpectatorGrantKey;

struct TopologyGrant {
    uint64_t grantId = 0;
    uint64_t nodeId = 0;
    uint64_t sessionId = 0;
    uint64_t expiryUnix = 0;
    uint32_t authorityGeneration = 0;
    uint64_t macLow = 0;
    uint64_t macHigh = 0;

    bool operator==(const TopologyGrant& other) const {
        return grantId == other.grantId
            && nodeId == other.nodeId
            && sessionId == other.sessionId
            && expiryUnix == other.expiryUnix
            && authorityGeneration == other.authorityGeneration
            && macLow == other.macLow
            && macHigh == other.macHigh;
    }
};

bool encodeTopologyGrantBody(
    const TopologyGrant& grant,
    std::array<uint8_t, kTopologyGrantBodyBytes>& out);

bool issueTopologyGrant(const TopologyGrantKey& key, TopologyGrant& grant);

GrantVerdict verifyTopologyGrant(const TopologyGrantKey& key,
                                 const TopologyGrant& grant,
                                 uint64_t expectedNodeId,
                                 uint64_t expectedSessionId,
                                 uint64_t nowUnix);

}
