#include "input/DeviceIdentity.h"

#include <cstring>

namespace qinput {

uint64_t identityStringHash(const char* text) {
    if (text == nullptr) return 0;
    uint64_t hash = 1469598103934665603ull;
    for (const char* p = text; *p != '\0'; ++p) {
        hash ^= static_cast<unsigned char>(*p);
        hash *= 1099511628211ull;
    }
    return hash;
}

namespace {

bool physicalClassMatches(const CompiledDeviceMatcher& matcher,
                          const DeviceCandidate& candidate) {
    if (matcher.vendor != candidate.vendor
        || matcher.product != candidate.product) {
        return false;
    }
    if (matcher.version != 0 && candidate.version != 0
        && matcher.version != candidate.version) {
        return false;
    }
    return true;
}

bool classMatches(const CompiledDeviceMatcher& matcher,
                  const DeviceCandidate& candidate) {
    return std::strncmp(matcher.guid, candidate.guid,
                        sizeof(matcher.guid)) == 0
        && matcher.vendor == candidate.vendor
        && matcher.product == candidate.product
        && matcher.version == candidate.version
        && std::strncmp(matcher.normalizedName, candidate.normalizedName,
                        sizeof(matcher.normalizedName)) == 0;
}

}

IdentityResolution resolveDeviceIdentity(
    const CompiledDeviceMatcher& matcher,
    const DeviceCandidate* candidates,
    uint32_t candidateCount) {
    IdentityResolution result{};
    if (candidates == nullptr || candidateCount == 0
        || candidateCount > kMaxIdentityCandidates) {
        return result;
    }

    if (matcher.hasSerial != 0) {
        uint32_t matchIndex = 0;
        uint32_t matches = 0;
        for (uint32_t i = 0; i < candidateCount; ++i) {
            if (candidates[i].hasSerial
                && candidates[i].serialHash == matcher.serialHash) {
                matchIndex = i;
                ++matches;
            }
        }
        if (matches == 1) {
            result.outcome = IdentityOutcome::Unique;
            result.candidateIndex = matchIndex;
            result.tier = IdentityMatchTier::Serial;
            return result;
        }
        if (matches > 1) {
            result.outcome = IdentityOutcome::Ambiguous;
            return result;
        }
    }

    if (matcher.hasPathFingerprint != 0) {
        uint32_t matchIndex = 0;
        uint32_t matches = 0;
        for (uint32_t i = 0; i < candidateCount; ++i) {
            if (candidates[i].hasPathFingerprint
                && candidates[i].pathFingerprint
                       == matcher.pathFingerprint
                && physicalClassMatches(matcher, candidates[i])) {
                matchIndex = i;
                ++matches;
            }
        }
        if (matches == 1) {
            result.outcome = IdentityOutcome::Unique;
            result.candidateIndex = matchIndex;
            result.tier = IdentityMatchTier::PathFingerprint;
            return result;
        }
        if (matches > 1) {
            result.outcome = IdentityOutcome::Ambiguous;
            return result;
        }
    }

    uint32_t matchIndex = 0;
    uint32_t matches = 0;
    for (uint32_t i = 0; i < candidateCount; ++i) {
        if (classMatches(matcher, candidates[i])) {
            matchIndex = i;
            ++matches;
        }
    }
    if (matches == 1) {
        result.outcome = IdentityOutcome::Unique;
        result.candidateIndex = matchIndex;
        result.tier = IdentityMatchTier::Class;
        return result;
    }
    if (matches > 1) {
        result.outcome = IdentityOutcome::Ambiguous;
        return result;
    }

    if (matcher.vendor == 0 && matcher.product == 0) return result;
    matchIndex = 0;
    matches = 0;
    for (uint32_t i = 0; i < candidateCount; ++i) {
        if (matcher.vendor == candidates[i].vendor
            && matcher.product == candidates[i].product) {
            matchIndex = i;
            ++matches;
        }
    }
    if (matches == 1) {
        result.outcome = IdentityOutcome::Unique;
        result.candidateIndex = matchIndex;
        result.tier = IdentityMatchTier::VendorProduct;
        return result;
    }
    if (matches > 1) {
        result.outcome = IdentityOutcome::Ambiguous;
        return result;
    }
    return result;
}

}
