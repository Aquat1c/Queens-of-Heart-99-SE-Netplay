#include "qoh99/FighterPool.hpp"

namespace qoh99 {

struct PoolInfo {
    const char* name;
    GameAddr base;
    int count;
};

static const PoolInfo kPools[] = {
    { "effectPool",     addr::kEffectPool,     30  },
    { "actorClonePool", addr::kActorClonePool, 120 },
    { "shadowPool",     addr::kShadowPool,     16  },
};

const PoolInfo* actorPools(std::size_t& count) {
    count = sizeof(kPools) / sizeof(kPools[0]);
    return kPools;
}

}
