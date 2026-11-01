#include "SpectatorGrant.h"

#include "../replay/ReplayIntegrity.h"

#include <algorithm>

namespace qnetplay::spectator {
namespace {

constexpr size_t kBlockBytes = 64u;

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

bool validGrantScalars(const SpectatorGrant& grant) {
    return grant.grantId != 0
        && grant.viewerId != 0
        && grant.sessionId != 0
        && grant.expiryUnix != 0;
}

}

bool encodeGrantBody(const SpectatorGrant& grant,
                     std::array<uint8_t, kSpectatorGrantBodyBytes>& out) {
    if (!validGrantScalars(grant)) return false;
    std::array<uint8_t, kSpectatorGrantBodyBytes> encoded{};
    writeLe64(encoded.data(), grant.grantId);
    writeLe64(encoded.data() + 8, grant.viewerId);
    writeLe64(encoded.data() + 16, grant.sessionId);
    writeLe64(encoded.data() + 24, grant.expiryUnix);
    writeLe64(encoded.data() + 32, grant.resumeDigest);
    writeLe32(encoded.data() + 40, grant.sourceGeneration);
    writeLe32(encoded.data() + 44, grant.resumeRecord);
    encoded[48] = grant.route;
    // Bytes 49..55 are reserved and zero.
    out = encoded;
    return true;
}

void hmacSha256(const uint8_t* key, size_t keyLength,
                const uint8_t* message, size_t messageLength,
                std::array<uint8_t, 32>& out) {
    uint8_t block[kBlockBytes] = {};
    if (keyLength > kBlockBytes) {
        const std::array<uint8_t, 32> reduced =
            qreplay::sha256(key, keyLength);
        std::copy(reduced.begin(), reduced.end(), block);
    } else if (key && keyLength > 0) {
        std::copy(key, key + keyLength, block);
    }

    uint8_t inner[kBlockBytes];
    uint8_t outer[kBlockBytes];
    for (size_t i = 0; i < kBlockBytes; ++i) {
        inner[i] = static_cast<uint8_t>(block[i] ^ 0x36u);
        outer[i] = static_cast<uint8_t>(block[i] ^ 0x5Cu);
    }

    qreplay::Sha256 innerHash;
    innerHash.update(inner, kBlockBytes);
    if (message && messageLength > 0)
        innerHash.update(message, messageLength);
    std::array<uint8_t, 32> innerDigest{};
    innerHash.finish(innerDigest);

    qreplay::Sha256 outerHash;
    outerHash.update(outer, kBlockBytes);
    outerHash.update(innerDigest.data(), innerDigest.size());
    outerHash.finish(out);
}

bool issueGrant(const SpectatorGrantKey& key, SpectatorGrant& grant) {
    std::array<uint8_t, kSpectatorGrantBodyBytes> body{};
    if (!encodeGrantBody(grant, body)) return false;
    std::array<uint8_t, 32> mac{};
    hmacSha256(key.data(), key.size(), body.data(), body.size(), mac);
    grant.macLow = readLe64(mac.data());
    grant.macHigh = readLe64(mac.data() + 8);
    return true;
}

GrantVerdict verifyGrant(const SpectatorGrantKey& key,
                         const SpectatorGrant& grant,
                         uint64_t expectedViewerId,
                         uint64_t expectedSessionId,
                         uint64_t nowUnix) {
    std::array<uint8_t, kSpectatorGrantBodyBytes> body{};
    if (!encodeGrantBody(grant, body)) return GrantVerdict::Malformed;
    std::array<uint8_t, 32> mac{};
    hmacSha256(key.data(), key.size(), body.data(), body.size(), mac);
    if (!equalConstantTime(grant.macLow, grant.macHigh,
                           readLe64(mac.data()),
                           readLe64(mac.data() + 8))) {
        return GrantVerdict::BadMac;
    }
    if (expectedViewerId != 0 && grant.viewerId != expectedViewerId)
        return GrantVerdict::WrongViewer;
    if (expectedSessionId != 0 && grant.sessionId != expectedSessionId)
        return GrantVerdict::WrongSession;
    if (nowUnix >= grant.expiryUnix) return GrantVerdict::Expired;
    return GrantVerdict::Ok;
}

GrantRegistry::GrantRegistry(const SpectatorGrantKey& key,
                             size_t capacity,
                             size_t rememberedGrants)
    : _key(key),
      _capacity(capacity == 0 ? 1u : capacity),
      _rememberedGrants(rememberedGrants == 0 ? 1u : rememberedGrants) {
}

const GrantRegistry::Redeemed* GrantRegistry::findRedeemed(
    uint64_t grantId) const {
    for (const Redeemed& entry : _reserved) {
        if (entry.grantId == grantId) return &entry;
    }
    for (const Redeemed& entry : _history) {
        if (entry.grantId == grantId) return &entry;
    }
    return nullptr;
}

GrantVerdict GrantRegistry::verify(const SpectatorGrant& grant,
                                   uint64_t expectedViewerId,
                                   uint64_t expectedSessionId,
                                   uint64_t nowUnix) const {
    const GrantVerdict verdict = verifyGrant(
        _key, grant, expectedViewerId, expectedSessionId, nowUnix);
    if (verdict != GrantVerdict::Ok) return verdict;
    if (findRedeemed(grant.grantId)) return GrantVerdict::Replayed;
    return GrantVerdict::Ok;
}

GrantVerdict GrantRegistry::redeem(const SpectatorGrant& grant,
                                   uint64_t expectedViewerId,
                                   uint64_t expectedSessionId,
                                   uint64_t nowUnix) {
    const GrantVerdict verdict =
        verify(grant, expectedViewerId, expectedSessionId, nowUnix);
    if (verdict != GrantVerdict::Ok) return verdict;
    if (_reserved.size() >= _capacity) return GrantVerdict::CapacityReached;
    Redeemed entry{};
    entry.grantId = grant.grantId;
    entry.viewerId = grant.viewerId;
    entry.expiryUnix = grant.expiryUnix;
    entry.held = true;
    _reserved.push_back(entry);
    return GrantVerdict::Ok;
}

bool GrantRegistry::release(uint64_t grantId) {
    for (auto it = _reserved.begin(); it != _reserved.end(); ++it) {
        if (it->grantId != grantId) continue;
        Redeemed entry = *it;
        entry.held = false;
        _reserved.erase(it);
        // Remembering the id makes a grant single-use: releasing the capacity slot must never make a replay admissible.
        _history.push_back(entry);
        while (_history.size() > _rememberedGrants)
            _history.erase(_history.begin());
        return true;
    }
    return false;
}

void GrantRegistry::expire(uint64_t nowUnix) {
    _history.erase(
        std::remove_if(_history.begin(), _history.end(),
                       [&](const Redeemed& entry) {
                           return nowUnix >= entry.expiryUnix;
                       }),
        _history.end());
}

}
