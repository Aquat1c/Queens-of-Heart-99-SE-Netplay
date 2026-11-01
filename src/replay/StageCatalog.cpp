#include "StageCatalog.h"

#include <cstring>

namespace qreplay {
namespace {

constexpr char kCatalogDomain[] = "qoh99.se.stage-catalog.v1";
constexpr char kEntryDomain[] = "qoh99.se.stage-entry.v1";
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint8_t kFieldSeparator = 0x1Fu;

constexpr std::array<uint32_t, kSeStageCountV2>
    kNativeStageIdByLogicalIndex{{
        1u,  2u,  3u,  4u,  5u,  6u,  7u,
        8u,  9u,  10u, 11u, 12u, 13u, 14u,
        15u, 16u, 17u, 18u, 19u, 20u, 21u,
        22u, 23u, 24u, 25u, 26u, 27u, 28u,
    }};

uint64_t fnvByte(uint64_t hash, uint8_t value) {
    hash ^= value;
    hash *= kFnvPrime;
    return hash;
}

uint64_t fnvBytes(
    uint64_t hash, const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0u; i < length; ++i)
        hash = fnvByte(hash, bytes[i]);
    return hash;
}

uint64_t fnvU32(uint64_t hash, uint32_t value) {
    for (unsigned shift = 0u; shift != 32u; shift += 8u)
        hash = fnvByte(hash, static_cast<uint8_t>(value >> shift));
    return hash;
}

uint64_t catalogId() {
    uint64_t hash = kFnvOffsetBasis;
    hash = fnvBytes(
        hash, kCatalogDomain, std::strlen(kCatalogDomain));
    hash = fnvByte(hash, kFieldSeparator);
    hash = fnvU32(hash, kSeStageCountV2);
    for (uint32_t logical = 0u; logical < kSeStageCountV2; ++logical) {
        hash = fnvU32(hash, logical);
        hash = fnvU32(
            hash, kNativeStageIdByLogicalIndex[logical]);
    }
    return hash;
}

uint64_t entryId(
    uint64_t completeCatalogId, uint32_t logical, uint32_t native) {
    uint64_t hash = kFnvOffsetBasis;
    hash = fnvBytes(hash, kEntryDomain, std::strlen(kEntryDomain));
    hash = fnvByte(hash, kFieldSeparator);
    for (unsigned shift = 0u; shift != 64u; shift += 8u)
        hash = fnvByte(
            hash, static_cast<uint8_t>(completeCatalogId >> shift));
    hash = fnvU32(hash, logical);
    hash = fnvU32(hash, native);
    return hash;
}

const StageCatalogEntryV2& invalidEntry() {
    static const StageCatalogEntryV2 entry{};
    return entry;
}

}

StageCatalogV2::StageCatalogV2()
    : catalogId_(qreplay::catalogId()) {
    for (uint32_t logical = 0u; logical < kSeStageCountV2; ++logical) {
        StageCatalogEntryV2& entry = entries_[logical];
        entry.logicalStageIndex = logical;
        entry.nativeStageId =
            kNativeStageIdByLogicalIndex[logical];
        entry.catalogEntryId = entryId(
            catalogId_, entry.logicalStageIndex, entry.nativeStageId);
    }
}

const StageCatalogV2& StageCatalogV2::forSeBuild() {
    static const StageCatalogV2 catalog;
    return catalog;
}

const StageCatalogEntryV2*
StageCatalogV2::findByLogicalStageIndex(
    uint32_t logicalStageIndex) const {
    if (logicalStageIndex >= entries_.size()) return nullptr;
    return &entries_[logicalStageIndex];
}

const StageCatalogEntryV2* StageCatalogV2::findByNativeStageId(
    uint32_t nativeStageId) const {
    for (const StageCatalogEntryV2& entry : entries_)
        if (entry.nativeStageId == nativeStageId) return &entry;
    return nullptr;
}

bool StageCatalogV2::nativeStageIdForLogicalStageIndex(
    uint32_t logicalStageIndex, uint32_t& out) const {
    const StageCatalogEntryV2* entry =
        findByLogicalStageIndex(logicalStageIndex);
    if (entry == nullptr) return false;
    out = entry->nativeStageId;
    return true;
}

bool StageCatalogV2::logicalStageIndexForNativeStageId(
    uint32_t nativeStageId, uint32_t& out) const {
    const StageCatalogEntryV2* entry =
        findByNativeStageId(nativeStageId);
    if (entry == nullptr) return false;
    out = entry->logicalStageIndex;
    return true;
}

bool StageCatalogV2::makeIdentity(
    uint32_t logicalStageIndex, StageIdentityV2& out) const {
    const StageCatalogEntryV2* entry =
        findByLogicalStageIndex(logicalStageIndex);
    if (entry == nullptr) return false;
    StageIdentityV2 identity{};
    identity.nativeStageId = entry->nativeStageId;
    identity.verified = true;
    identity.catalogEntryId = entry->catalogEntryId;
    out = identity;
    return true;
}

bool StageCatalogV2::validateIdentity(
    const StageIdentityV2& identity) const {
    if (!identity.verified
        || identity.nativeStageId < kMinNativeStageIdV2
        || identity.nativeStageId > kMaxNativeStageIdV2
        || identity.catalogEntryId == 0u) {
        return false;
    }
    const StageCatalogEntryV2* entry =
        findByNativeStageId(identity.nativeStageId);
    return entry != nullptr
        && entry->catalogEntryId == identity.catalogEntryId;
}

const StageCatalogEntryV2& StageCatalogV2::entryAt(
    size_t index) const {
    return index < entries_.size()
        ? entries_[index] : invalidEntry();
}

}
