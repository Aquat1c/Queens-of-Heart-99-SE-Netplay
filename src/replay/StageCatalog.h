#pragma once

#include "ReplayTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace qreplay {

constexpr uint32_t kSeStageCountV2 = 28u;
constexpr uint32_t kNoLogicalStageIndexV2 = 0xFFFFFFFFu;

struct StageCatalogEntryV2 {
    uint32_t logicalStageIndex = kNoLogicalStageIndexV2;
    uint32_t nativeStageId = 0u;
    uint64_t catalogEntryId = 0u;
};

class StageCatalogV2 {
public:
    static const StageCatalogV2& forSeBuild();

    uint64_t catalogId() const { return catalogId_; }
    size_t entryCount() const { return entries_.size(); }

    const StageCatalogEntryV2* findByLogicalStageIndex(
        uint32_t logicalStageIndex) const;
    const StageCatalogEntryV2* findByNativeStageId(
        uint32_t nativeStageId) const;

    bool nativeStageIdForLogicalStageIndex(
        uint32_t logicalStageIndex, uint32_t& out) const;
    bool logicalStageIndexForNativeStageId(
        uint32_t nativeStageId, uint32_t& out) const;

    bool makeIdentity(
        uint32_t logicalStageIndex, StageIdentityV2& out) const;

    bool validateIdentity(const StageIdentityV2& identity) const;

    const StageCatalogEntryV2& entryAt(size_t index) const;

private:
    StageCatalogV2();

    std::array<StageCatalogEntryV2, kSeStageCountV2> entries_{};
    uint64_t catalogId_ = 0u;
};

}
