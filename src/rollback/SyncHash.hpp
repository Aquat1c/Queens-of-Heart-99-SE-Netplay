#pragma once
#include <cstdint>
#include <vector>
#include "SavestateProfile.hpp"

namespace qoh99 { namespace netplay {

enum class HashProfile { GameplaySync, RenderCosmetic, FullDebugWide };

struct RegionHash {
    const char* region;
    std::uint64_t hash;
};

class RegionHasher {
public:
    static constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
    static constexpr std::uint64_t kFnvPrime  = 1099511628211ull;

    static std::uint64_t fnv1a(const void* data, std::size_t n,
                               std::uint64_t seed = kFnvOffset) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        std::uint64_t h = seed;
        while (n--) { h ^= *p++; h *= kFnvPrime; }
        return h;
    }
};

class SyncHash {
public:
    explicit SyncHash(const SavestateProfile& profile) : profile_(profile) {}

    std::vector<RegionHash> regionHashes(const Savestate& s, HashProfile p) const;
    std::uint64_t combined(const Savestate& s, HashProfile p) const;

    struct FirstDiff {
        bool differs = false;
        const char* region = nullptr;
        std::uint32_t offset = 0;
    };
    FirstDiff firstDiff(const Savestate& a, const Savestate& b, HashProfile p) const;

private:
    bool includes(bool inGameplayHash, bool inRenderHash, HashProfile p) const;
    const SavestateProfile& profile_;
};

}}
