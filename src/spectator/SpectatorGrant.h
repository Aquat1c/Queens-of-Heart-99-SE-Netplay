#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qnetplay::spectator {

constexpr size_t kSpectatorGrantBodyBytes = 56u;
using SpectatorGrantKey = std::array<uint8_t, 32>;

struct SpectatorGrant {
    uint64_t grantId = 0;
    uint64_t viewerId = 0;
    uint64_t sessionId = 0;
    uint64_t expiryUnix = 0;
    uint32_t sourceGeneration = 0;
    uint32_t resumeRecord = 0;
    uint64_t resumeDigest = 0;
    uint64_t macLow = 0;
    uint64_t macHigh = 0;
    uint8_t route = 0;

    bool operator==(const SpectatorGrant& other) const {
        return grantId == other.grantId
            && viewerId == other.viewerId
            && sessionId == other.sessionId
            && expiryUnix == other.expiryUnix
            && sourceGeneration == other.sourceGeneration
            && resumeRecord == other.resumeRecord
            && resumeDigest == other.resumeDigest
            && macLow == other.macLow
            && macHigh == other.macHigh
            && route == other.route;
    }
};

enum class GrantVerdict {
    Ok,
    Malformed,
    BadMac,
    Expired,
    WrongViewer,
    WrongSession,
    Replayed,
    CapacityReached,
};

bool encodeGrantBody(const SpectatorGrant& grant,
                     std::array<uint8_t, kSpectatorGrantBodyBytes>& out);

void hmacSha256(const uint8_t* key, size_t keyLength,
                const uint8_t* message, size_t messageLength,
                std::array<uint8_t, 32>& out);

bool issueGrant(const SpectatorGrantKey& key, SpectatorGrant& grant);

GrantVerdict verifyGrant(const SpectatorGrantKey& key,
                         const SpectatorGrant& grant,
                         uint64_t expectedViewerId,
                         uint64_t expectedSessionId,
                         uint64_t nowUnix);

class GrantRegistry {
public:
    GrantRegistry(const SpectatorGrantKey& key,
                  size_t capacity = 32,
                  size_t rememberedGrants = 256);

    GrantVerdict verify(const SpectatorGrant& grant,
                        uint64_t expectedViewerId,
                        uint64_t expectedSessionId,
                        uint64_t nowUnix) const;

    GrantVerdict redeem(const SpectatorGrant& grant,
                        uint64_t expectedViewerId,
                        uint64_t expectedSessionId,
                        uint64_t nowUnix);

    bool release(uint64_t grantId);

    size_t reservedCount() const { return _reserved.size(); }
    size_t capacity() const { return _capacity; }
    void expire(uint64_t nowUnix);

private:
    struct Redeemed {
        uint64_t grantId = 0;
        uint64_t viewerId = 0;
        uint64_t expiryUnix = 0;
        bool held = false;
    };

    SpectatorGrantKey _key{};
    size_t _capacity = 0;
    size_t _rememberedGrants = 0;
    std::vector<Redeemed> _reserved;
    std::vector<Redeemed> _history;

    const Redeemed* findRedeemed(uint64_t grantId) const;
};

}
