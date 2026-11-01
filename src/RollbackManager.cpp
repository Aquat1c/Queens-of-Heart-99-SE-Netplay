// Save layout: [ fixed regions... ] then for each pointer-child region:
//     [ uint32 present ][ size bytes of *(fighterBase+ptrOffset) ]
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "RollbackManager.h"
#include "game/StateProofDigest.h"
#include "Log.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace qoh99 {

namespace {
constexpr uintptr_t kAiVmAddress = 0x62EB80u;
constexpr size_t kAiVmHeaderSize = 0x20u;
constexpr size_t kAiVmStackBytes = 0x40000u;
constexpr size_t kAiVmSize = 0x40024u;
constexpr size_t kAiVmCursorOffset = 0x40020u;
constexpr size_t kDescriptorFamilyCount = 6u;
constexpr size_t kMaxDescriptorFamilyBytes = 64u * 1024u * 1024u;
constexpr std::array<uint32_t, kDescriptorFamilyCount>
    kDescriptorPointerOffsets = { 0u, 8u, 16u, 24u, 32u, 40u };
constexpr std::array<uint32_t, kDescriptorFamilyCount>
    kDescriptorCountOffsets = { 4u, 12u, 20u, 28u, 36u, 44u };
constexpr std::array<uint32_t, kDescriptorFamilyCount>
    kDescriptorStrides = { 260u, 12u, 24u, 64u, 8u, 24u };
constexpr std::array<uintptr_t, 10> kDescriptorRoots = {
    addr::kFighterP1,
    addr::kFighterP2,
    addr::kFighterP3,
    addr::kFighterP4,
    addr::kHelperCloneP1,
    addr::kHelperCloneP2,
    addr::kHelperCloneP3,
    addr::kHelperCloneP4,
    0x624EE0u,
    0x8A3740u,
};
}

static inline uint64_t fnv1a(
    const void* data, size_t n,
    uint64_t h = 1469598103934665603ull) {
    const uint8_t* b = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static inline uint64_t hashBulk(
    const RollbackManager::HashAlgorithm algorithm,
    const uint8_t* bytes, const size_t n, const uint64_t h) {
    if (algorithm == RollbackManager::HashAlgorithm::Fnv1a64)
        return fnv1a(bytes, n, h);
    const uint64_t digest = qgame::stateProofDigest64(bytes, n);
    return fnv1a(&digest, sizeof(digest), h);
}

static bool protectionIsReadable(DWORD protect) {
    if (protect & PAGE_GUARD) return false;
    switch (protect & 0xFFu) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}
static bool protectionIsWritable(DWORD protect) {
    if (protect & PAGE_GUARD) return false;
    switch (protect & 0xFFu) {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}
static const char* protectionName(DWORD protect) {
    switch (protect & 0xFFu) {
        case PAGE_NOACCESS:           return "NOACCESS";
        case PAGE_READONLY:           return "R";
        case PAGE_READWRITE:          return "RW";
        case PAGE_WRITECOPY:          return "WC";
        case PAGE_EXECUTE:            return "X(execute-only)";
        case PAGE_EXECUTE_READ:       return "RX";
        case PAGE_EXECUTE_READWRITE:  return "RWX";
        case PAGE_EXECUTE_WRITECOPY:  return "WCX";
        default:                      return "unknown";
    }
}

static bool memReadable(const void* p, size_t n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    uintptr_t end = 0;
    if (!RollbackManager::checkedAddressEnd(a, n, end)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < end) {
        if (!VirtualQuery(
                reinterpret_cast<const void*>(a), &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT || !protectionIsReadable(mbi.Protect)) {
            static uint64_t s_unreadable = 0;
            if (++s_unreadable <= 16 || s_unreadable % 500 == 0) {
                QLOG("[rollback] unreadable page #%llu at %p: state=%08lX "
                     "protect=%08lX (%s) regionSize=%zu",
                     static_cast<unsigned long long>(s_unreadable),
                     reinterpret_cast<void*>(a),
                     static_cast<unsigned long>(mbi.State),
                     static_cast<unsigned long>(mbi.Protect),
                     protectionName(mbi.Protect),
                     static_cast<size_t>(mbi.RegionSize));
            }
            return false;
        }
        const uintptr_t base =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t next = 0;
        if (base > a
            || !RollbackManager::checkedAddressEnd(
                base, mbi.RegionSize, next)
            || next <= a) {
            return false;
        }
        a = next;
    }
    return true;
}
static bool memWritable(void* p, size_t n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    uintptr_t end = 0;
    if (!RollbackManager::checkedAddressEnd(a, n, end)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < end) {
        if (!VirtualQuery(
                reinterpret_cast<const void*>(a), &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT || !protectionIsWritable(mbi.Protect)) {
            static uint64_t s_unwritable = 0;
            if (++s_unwritable <= 16 || s_unwritable % 500 == 0) {
                QLOG("[rollback] unwritable page #%llu at %p: state=%08lX "
                     "protect=%08lX (%s) regionSize=%zu",
                     static_cast<unsigned long long>(s_unwritable),
                     reinterpret_cast<void*>(a),
                     static_cast<unsigned long>(mbi.State),
                     static_cast<unsigned long>(mbi.Protect),
                     protectionName(mbi.Protect),
                     static_cast<size_t>(mbi.RegionSize));
            }
            return false;
        }
        const uintptr_t base =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t next = 0;
        if (base > a
            || !RollbackManager::checkedAddressEnd(
                base, mbi.RegionSize, next)
            || next <= a) {
            return false;
        }
        a = next;
    }
    return true;
}

enum class ChildResolutionKind {
    Inactive,
    ActiveValid,
    Invalid,
};

struct ChildResolution {
    ChildResolutionKind kind = ChildResolutionKind::Invalid;
    uintptr_t address = 0;
};

static ChildResolution resolveChild(const PtrRegion& pr) {
    uint32_t activationPointerValue = 0;
    if (pr.activationPointerAddress != 0u) {
        if (!memReadable(
                reinterpret_cast<const void*>(
                    pr.activationPointerAddress),
                sizeof(activationPointerValue))) {
            return {};
        }
        std::memcpy(
            &activationPointerValue,
            reinterpret_cast<const void*>(
                pr.activationPointerAddress),
            sizeof(activationPointerValue));
    }
    if (!pointerChildParticipates(pr, activationPointerValue)) {
        return {
            ChildResolutionKind::Inactive,
            0u,
        };
    }
    const uintptr_t owner = pr.fighterBase + pr.ptrOffset;
    if (!memReadable(
            reinterpret_cast<const void*>(owner),
            sizeof(uint32_t))) {
        return {};
    }
    const uintptr_t child =
        *reinterpret_cast<volatile uint32_t*>(owner);
    if (!child
        || !memReadable(
            reinterpret_cast<const void*>(child), pr.size)) {
        return {};
    }
    return {
        ChildResolutionKind::ActiveValid,
        child,
    };
}

bool RollbackManager::resolveRegions() {
    size_t count = 0;
    const Region* r = saveRegions(count);
    _regions.clear();
    _totalSize = 0;
    bool allOk = true;
    auto addSnapshotBytes = [&](size_t bytes, const char* name) {
        size_t next = 0;
        if (!checkedSizeAdd(_totalSize, bytes, next)) {
            QLOG("[rollback] snapshot layout overflow adding '%s': "
                 "total=%zu contribution=%zu",
                 name ? name : "<unnamed>", _totalSize, bytes);
            allOk = false;
            return false;
        }
        _totalSize = next;
        return true;
    };
    auto addPointerSnapshotBytes = [&](size_t bytes, const char* name) {
        size_t contribution = 0;
        if (!checkedSizeAdd(sizeof(uint32_t), bytes, contribution)) {
            QLOG("[rollback] snapshot pointer contribution overflow for '%s': "
                 "marker=%zu bytes=%zu",
                 name ? name : "<unnamed>", sizeof(uint32_t), bytes);
            allOk = false;
            return false;
        }
        return addSnapshotBytes(contribution, name);
    };
    for (size_t i = 0; i < count; ++i) {
        bool ok = memReadable(reinterpret_cast<void*>(r[i].addr), r[i].size);
        if (!ok) {
            QLOG("[rollback] region '%s' @%08X size=%zu NOT readable yet",
                 r[i].name, (unsigned)r[i].addr, r[i].size);
            allOk = false;
        }
        _regions.push_back(r[i]);
        addSnapshotBytes(r[i].size, r[i].name);
    }
    for (const Region& e : _extraRegions) {
        bool ok = memReadable(reinterpret_cast<void*>(e.addr), e.size);
        if (!ok) QLOG("[rollback] EXTRA region '%s' @%08X size=%zu NOT readable yet",
                      e.name, (unsigned)e.addr, e.size);
        _regions.push_back(e);
        addSnapshotBytes(e.size, e.name);
        allOk = allOk && ok;
    }
    size_t pc = 0;
    const PtrRegion* pr = pointerRegions(pc);
    _ptrRegions.clear();
    _ptrScriptBanks.clear();
    _ptrAliases.clear();
    std::vector<std::pair<uintptr_t, size_t>> accepted;
    auto aliasOf = [&](uintptr_t address, size_t size) -> const char* {
        if (!address) return nullptr;
        for (size_t k = 0; k < accepted.size(); ++k) {
            if (accepted[k].first == address && accepted[k].second == size)
                return _ptrRegions[k].name;
        }
        return nullptr;
    };
    for (size_t i = 0; i < pc; ++i) {
        const ChildResolution child = resolveChild(pr[i]);
        const uintptr_t address =
            child.kind == ChildResolutionKind::ActiveValid ? child.address : 0u;
        if (const char* covers = aliasOf(address, pr[i].size)) {
            QLOG("[rollback] ptr region '%s' aliases '%s' at %08X (%zu bytes) "
                 "- registered once, not twice",
                 pr[i].name, covers, static_cast<unsigned>(address),
                 pr[i].size);
            _ptrAliases.push_back({ pr[i], address, covers });
            continue;
        }
        accepted.push_back({ address, pr[i].size });
        _ptrRegions.push_back(pr[i]);
        _ptrScriptBanks.push_back(-1);
        addPointerSnapshotBytes(pr[i].size, pr[i].name);
    }
    for (size_t i = 0; i < _extraPtrRegions.size(); ++i) {
        const PtrRegion& e = _extraPtrRegions[i];
        uintptr_t c = e.fighterBase + e.ptrOffset;
        bool ok = memReadable(reinterpret_cast<void*>(c), 4);
        if (ok) { uintptr_t child = *reinterpret_cast<volatile uint32_t*>(c);
                  ok = child && memReadable(reinterpret_cast<void*>(child), e.size); }
        if (!ok) QLOG("[rollback] EXTRA ptr region '%s' fighter=%08X+%zu size=%zu NOT readable yet",
                      e.name, (unsigned)e.fighterBase, e.ptrOffset, e.size);
        uintptr_t resolved = 0;
        if (ok) {
            resolved = *reinterpret_cast<volatile uint32_t*>(c);
        }
        if (const char* covers = aliasOf(resolved, e.size)) {
            QLOG("[rollback] EXTRA ptr region '%s' aliases '%s' at %08X "
                 "(%zu bytes) - registered once, not twice",
                 e.name, covers, static_cast<unsigned>(resolved), e.size);
            _ptrAliases.push_back({ e, resolved, covers });
            allOk = allOk && ok;
            continue;
        }
        accepted.push_back({ resolved, e.size });
        _ptrRegions.push_back(e);
        _ptrScriptBanks.push_back(_extraPtrScriptBanks[i]);
        addPointerSnapshotBytes(e.size, e.name);
        allOk = allOk && ok;
    }
    if (!_ptrAliases.empty()) {
        QLOG("[rollback] %zu aliased pointer region(s) collapsed; the "
             "savestate carries each distinct heap exactly once",
             _ptrAliases.size());
    }
    return allOk;
}

bool RollbackManager::buildCanonicalPointerMaps() {
    _fixedCanonicalPointerOffsets.assign(_regions.size(), {});
    _ptrCanonicalPointerOffsets.assign(_ptrRegions.size(), {});
    _fixedCanonicalRelativePointers.assign(_regions.size(), {});
    _fixedCanonicalDescriptorPointers.assign(_regions.size(), {});
    _descriptorIdentities.clear();

    for (const uintptr_t root : kDescriptorRoots) {
        if (!memReadable(reinterpret_cast<const void*>(root), 48u)) {
            QLOG("[rollback] descriptor root %08X is unreadable",
                 static_cast<unsigned>(root));
            return false;
        }
        for (size_t family = 0; family < kDescriptorFamilyCount; ++family) {
            const uint32_t raw = *reinterpret_cast<volatile uint32_t*>(
                root + kDescriptorPointerOffsets[family]);
            const uint32_t count = *reinterpret_cast<volatile uint32_t*>(
                root + kDescriptorCountOffsets[family]);
            if (raw == 0 && count == 0) continue;
            if (raw == 0) {
                QLOG("[rollback] descriptor root=%08X family=%zu has "
                     "partial owner/count state ptr=%08X count=%u",
                     static_cast<unsigned>(root), family, raw, count);
                return false;
            }
            const uint64_t byteCount64 =
                static_cast<uint64_t>(count) * kDescriptorStrides[family];
            if (byteCount64 > kMaxDescriptorFamilyBytes
                || (byteCount64 != 0
                    && !memReadable(
                    reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(raw)),
                    static_cast<size_t>(byteCount64)))) {
                QLOG("[rollback] descriptor root=%08X family=%zu has "
                     "invalid extent ptr=%08X count=%u bytes=%llu",
                     static_cast<unsigned>(root), family, raw, count,
                     static_cast<unsigned long long>(byteCount64));
                return false;
            }
            const DescriptorIdentity candidate{
                raw,
                static_cast<uint32_t>(family),
                count,
                static_cast<uint32_t>(byteCount64),
                byteCount64 == 0
                    ? fnv1a(nullptr, 0)
                    : fnv1a(
                        reinterpret_cast<const void*>(
                            static_cast<uintptr_t>(raw)),
                        static_cast<size_t>(byteCount64)),
            };
            const auto existing = std::find_if(
                _descriptorIdentities.begin(), _descriptorIdentities.end(),
                [&](const DescriptorIdentity& identity) {
                    return identity.rawPointer == candidate.rawPointer
                        && identity.family == candidate.family;
                });
            if (existing != _descriptorIdentities.end()) {
                if (existing->count != candidate.count
                    || existing->byteCount != candidate.byteCount
                    || existing->contentHash != candidate.contentHash) {
                    QLOG("[rollback] descriptor alias collision ptr=%08X "
                         "family=%zu has inconsistent identity",
                         raw, family);
                    return false;
                }
                continue;
            }
            _descriptorIdentities.push_back(candidate);
        }
    }

    size_t fixedTotal = 0;
    for (size_t ri = 0; ri < _regions.size(); ++ri) {
        const Region& region = _regions[ri];
        std::vector<size_t>& offsets =
            _fixedCanonicalPointerOffsets[ri];
        const uintptr_t end = region.addr + region.size;

        const uintptr_t localConfigOwners[] = {
            kQohActiveConfigPointerAddress,
            kQohBindingPointerAddress,
        };
        for (const uintptr_t address : localConfigOwners) {
            if (address >= region.addr
                && address + sizeof(uint32_t) <= end) {
                offsets.push_back(
                    static_cast<size_t>(address - region.addr));
            }
        }

        for (const PtrRegion& pr : _ptrRegions) {
            const uintptr_t address =
                pr.fighterBase + pr.ptrOffset;
            if (address >= region.addr && address + 4u <= end)
                offsets.push_back(
                    static_cast<size_t>(address - region.addr));
        }

        std::sort(offsets.begin(), offsets.end());
        offsets.erase(
            std::unique(offsets.begin(), offsets.end()),
            offsets.end());
        fixedTotal += offsets.size();
    }

    auto addActor = [&](uintptr_t base) {
        for (size_t ri = 0; ri < _regions.size(); ++ri) {
            const Region& region = _regions[ri];
            // The 30th effect actor's last eight bytes overlap the render-only span at 0x6234E0; canonical identity fields end at +0x59C, so key registration must use those fields.
            if (base >= region.addr
                && base + 0x59Cu
                    <= region.addr + region.size) {
                const size_t activeOffset =
                    static_cast<size_t>(base + 0x0AEu - region.addr);
                for (size_t family = 0;
                     family < kDescriptorFamilyCount; ++family) {
                    _fixedCanonicalDescriptorPointers[ri].push_back(
                        DescriptorPointer{
                            static_cast<size_t>(
                                base + kDescriptorPointerOffsets[family]
                                - region.addr),
                            activeOffset,
                            static_cast<uint32_t>(family),
                        });
                }
                _fixedCanonicalRelativePointers[ri].push_back(
                    RelativePointer{
                        static_cast<size_t>(base + 0x598u - region.addr),
                        static_cast<size_t>(base + 0x018u - region.addr),
                        static_cast<size_t>(base + 0x01Cu - region.addr),
                        activeOffset,
                        64u,
                    });
                return;
            }
        }
    };
    const std::array<uintptr_t, 10> standaloneActors = {
        addr::kFighterP1,
        addr::kFighterP2,
        addr::kFighterP3,
        addr::kFighterP4,
        addr::kHelperCloneP1,
        addr::kHelperCloneP2,
        addr::kHelperCloneP3,
        addr::kHelperCloneP4,
        0x624EE0u,
    };
    for (const uintptr_t base : standaloneActors) addActor(base);
    auto addPool = [&](uintptr_t base, size_t count) {
        for (size_t index = 0; index < count; ++index)
            addActor(base + index * addr::kFighterSize);
    };
    addPool(addr::kEffectPool, kQohEffectActorCount);
    addPool(addr::kActorClonePool, 120);
    addPool(addr::kShadowPool, 16);

    // match_aux_actor_pool_slots_05_29: 0x8737CC..0x87CD68, 25 x 0x5FC.
    {
        const Region* auxPool = nullptr;
        for (const Region& region : _regions) {
            if (std::strcmp(region.name,
                            "match_aux_actor_pool_slots_05_29") == 0) {
                auxPool = &region;
                break;
            }
        }
        if (auxPool == nullptr
            || auxPool->addr != 0x8737CCu
            || auxPool->size != 25u * addr::kFighterSize
            || !inGameplayHash(auxPool->role)) {
            QLOG("[rollback] match auxiliary actor region contract mismatch "
                 "(name/addr/size/gameplay-hash)");
            return false;
        }
        addPool(auxPool->addr, 25u);
    }

    // rina_crash_actors: the 0x8A3740 root plus three more 0x5FC records, one per player slot.
    {
        const Region* rina = nullptr;
        for (const Region& region : _regions) {
            if (std::strcmp(region.name, "rina_crash_actors") == 0) {
                rina = &region;
                break;
            }
        }
        if (rina == nullptr
            || rina->addr != 0x8A3740u
            || rina->size != 4u * addr::kFighterSize
            || !inGameplayHash(rina->role)) {
            QLOG("[rollback] rina crash actor region contract mismatch "
                 "(name/addr/size/gameplay-hash)");
            return false;
        }
        addPool(rina->addr, 4u);
    }

    fixedTotal = 0;
    size_t descriptorTotal = 0;
    for (size_t ri = 0; ri < _fixedCanonicalPointerOffsets.size(); ++ri) {
        std::vector<size_t>& offsets = _fixedCanonicalPointerOffsets[ri];
        std::sort(offsets.begin(), offsets.end());
        offsets.erase(
            std::unique(offsets.begin(), offsets.end()),
            offsets.end());
        std::vector<DescriptorPointer>& descriptors =
            _fixedCanonicalDescriptorPointers[ri];
        std::sort(
            descriptors.begin(), descriptors.end(),
            [](const DescriptorPointer& a, const DescriptorPointer& b) {
                return a.pointerOffset < b.pointerOffset;
            });
        descriptors.erase(
            std::unique(
                descriptors.begin(), descriptors.end(),
                [](const DescriptorPointer& a,
                   const DescriptorPointer& b) {
                    return a.pointerOffset == b.pointerOffset
                        && a.family == b.family;
                }),
            descriptors.end());
        for (const DescriptorPointer& descriptor : descriptors) {
            offsets.erase(
                std::remove(
                    offsets.begin(), offsets.end(),
                    descriptor.pointerOffset),
                offsets.end());
        }
        fixedTotal += offsets.size();
        descriptorTotal += descriptors.size();
    }
    for (std::vector<RelativePointer>& pointers :
         _fixedCanonicalRelativePointers) {
        std::sort(
            pointers.begin(), pointers.end(),
            [](const RelativePointer& a, const RelativePointer& b) {
                return a.pointerOffset < b.pointerOffset;
            });
    }

    QLOG("[rollback] canonical identity map: generic=%zu descriptor=%zu "
         "descriptorAssets=%zu child=0 (ten RE-verified roots; "
         "actor+598 remains frame-relative)",
         fixedTotal, descriptorTotal, _descriptorIdentities.size());
    return true;
}

void RollbackManager::addExtraPtrRegion(uintptr_t fighterBase, size_t ptrOffset, size_t size,
                                        const char* name, RegionRole role) {
    _extraPtrRegions.push_back(
        PtrRegion{
            fighterBase, ptrOffset, size, name, role, true, 0u
        });
    _extraPtrScriptBanks.push_back(-1);
    QLOG("[rollback] EXTRA ptr region added: %s fighter=%08X+%zu size=%zu role=%s",
         name, (unsigned)fighterBase, ptrOffset, size,
         inGameplayHash(role) ? "GameplayRequired" : "RenderCosmetic");
}

void RollbackManager::addExternalOwnerPtrRegion(
    uintptr_t ownerAddress, size_t size, const char* name, RegionRole role,
    uint8_t scriptBank) {
    _extraPtrRegions.push_back(
        PtrRegion{
            ownerAddress, 0, size, name, role, false, 0u
        });
    _extraPtrScriptBanks.push_back(static_cast<int>(scriptBank));
    QLOG("[rollback] external-owner ptr region added: %s owner=%08X "
         "size=%zu bank=%u role=%s",
         name, static_cast<unsigned>(ownerAddress), size,
         static_cast<unsigned>(scriptBank),
         inGameplayHash(role) ? "GameplayRequired" : "RenderCosmetic");
}

void RollbackManager::clearExtraPtrRegions() {
    if (!_extraPtrRegions.empty())
        QLOG("[rollback] clearing %zu EXTRA ptr regions (re-registration)", _extraPtrRegions.size());
    invalidateSlots();
    _pool.reset();
    _numStates = 0;
    _poolSize = 0;
    _totalSize = 0;
    _slotTags.clear();
    _regions.clear();
    _ptrRegions.clear();
    _ptrAliases.clear();
    _ptrExpectedPresent.clear();
    _ptrExpectedAddress.clear();
    _ptrScriptBanks.clear();
    _scriptRanges.clear();
    _fixedCanonicalPointerOffsets.clear();
    _ptrCanonicalPointerOffsets.clear();
    _fixedCanonicalRelativePointers.clear();
    _fixedCanonicalDescriptorPointers.clear();
    _descriptorIdentities.clear();
    _extraPtrRegions.clear();
    _extraPtrScriptBanks.clear();
    _immutableAssetFingerprint = 0;
}

void RollbackManager::addExtraRegion(uintptr_t addr, size_t size, const char* name, RegionRole role) {
    _extraRegions.push_back(Region{ addr, size, name, role });
    QLOG("[rollback] candidate region added: %s @%08X size=%zu role=%s",
         name, (unsigned)addr, size, inGameplayHash(role) ? "GameplayRequired" : "RenderCosmetic");
}

bool RollbackManager::poolSlotOffset(size_t slot, size_t& offset) const {
    if (!_pool || slot >= _numStates || _totalSize == 0u
        || _poolSize < _totalSize) {
        return false;
    }
    size_t candidate = 0;
    if (!checkedSizeMultiply(slot, _totalSize, candidate)
        || candidate > _poolSize - _totalSize) {
        return false;
    }
    offset = candidate;
    return true;
}

bool RollbackManager::snapshotFixedU32(
    const uint8_t* snapshot, uintptr_t address, uint32_t& value) const {
    if (!snapshot) return false;
    size_t snapshotOffset = 0;
    for (const Region& region : _regions) {
        uintptr_t regionEnd = 0;
        if (!checkedAddressEnd(region.addr, region.size, regionEnd))
            return false;
        if (region.size >= sizeof(uint32_t)
            && address >= region.addr
            && address <= regionEnd - sizeof(uint32_t)) {
            std::memcpy(
                &value,
                snapshot + snapshotOffset + (address - region.addr),
                sizeof(value));
            return true;
        }
        if (!checkedSizeAdd(
                snapshotOffset, region.size, snapshotOffset)) {
            return false;
        }
    }
    return false;
}

bool RollbackManager::verifyAgainstSlot(size_t slot, Diff* out, Profile profile) const {
    size_t offset = 0;
    if (!poolSlotOffset(slot, offset)) {
        if (out) {
            out->region = "bad-slot";
            out->offsetInRegion = 0;
            out->diffBytes = 1;
        }
        return false;
    }
    return compareWith(_pool.get() + offset, out, profile);
}

bool RollbackManager::bufferOffsetOf(const char* name, size_t* off, bool* isPtrChild) const {
    size_t o = 0;
    for (const Region& r : _regions) {
        if (std::strcmp(r.name, name) == 0) {
            if (off) *off = o;
            if (isPtrChild) *isPtrChild = false;
            return true;
        }
        o += r.size;
    }
    for (const PtrRegion& pr : _ptrRegions) {
        o += 4;
        if (std::strcmp(pr.name, name) == 0) {
            if (off) *off = o;
            if (isPtrChild) *isPtrChild = true;
            return true;
        }
        o += pr.size;
    }
    return false;
}

size_t RollbackManager::diffBuffers(const uint8_t* a, const uint8_t* b, RangeCb cb, void* user) const {
    size_t total = 0, off = 0;
    auto scan = [&](const char* name, uintptr_t baseVa, size_t size) {
        size_t rs = 0; bool in = false;
        for (size_t i = 0; i <= size; ++i) {
            bool d = (i < size) && (a[off + i] != b[off + i]);
            if (d && !in) { in = true; rs = i; }
            else if (!d && in) {
                in = false;
                total += i - rs;
                if (cb) cb(name, rs, baseVa ? baseVa + rs : 0, i - rs, user);
            }
        }
        off += size;
    };
    for (const Region& r : _regions) scan(r.name, r.addr, r.size);
    for (const PtrRegion& pr : _ptrRegions) {
        off += 4;
        scan(pr.name, 0, pr.size);
    }
    return total;
}

bool RollbackManager::init(size_t numStates) {
    bool allReadable = resolveRegions();
    _ptrExpectedPresent.clear();
    _ptrExpectedPresent.reserve(_ptrRegions.size());
    _ptrExpectedAddress.clear();
    _ptrExpectedAddress.reserve(_ptrRegions.size());
    for (const PtrRegion& pr : _ptrRegions) {
        const ChildResolution child = resolveChild(pr);
        const bool valid =
            child.kind != ChildResolutionKind::Invalid;
        const bool present =
            child.kind == ChildResolutionKind::ActiveValid;
        _ptrExpectedPresent.push_back(present ? 1u : 0u);
        _ptrExpectedAddress.push_back(
            present ? child.address : 0u);
        if (!valid) {
            QLOG("[rollback] ptr region '%s' has an invalid active "
                 "gate/owner/child at init",
                 pr.name);
            allReadable = false;
        }
        if (!present && pr.role == RegionRole::GameplayRequired) {
            QLOG("[rollback] REQUIRED ptr region '%s' is absent at init", pr.name);
            allReadable = false;
        }
    }
    _scriptRanges.clear();
    for (size_t i = 0; i < _ptrRegions.size(); ++i) {
        if (_ptrScriptBanks[i] < 0 || !_ptrExpectedPresent[i])
            continue;
        const int scriptBank = _ptrScriptBanks[i];
        const bool duplicateBank = std::any_of(
            _scriptRanges.begin(), _scriptRanges.end(),
            [&](const ScriptRange& range) {
                return range.bank == static_cast<uint32_t>(scriptBank);
            });
        if (scriptBank >= 20 || duplicateBank
            || _ptrExpectedAddress[i] > 0xFFFFFFFFull
            || _ptrRegions[i].size < 2u
            || _ptrRegions[i].size > 0xFFFFFFFFull
            || static_cast<uint64_t>(_ptrExpectedAddress[i])
                + static_cast<uint64_t>(_ptrRegions[i].size)
                > 0x100000000ull) {
            QLOG("[rollback] invalid AI script range bank=%d base=%08X "
                 "bytes=%zu duplicate=%u",
                 scriptBank,
                 static_cast<unsigned>(_ptrExpectedAddress[i]),
                 _ptrRegions[i].size,
                 duplicateBank ? 1u : 0u);
            allReadable = false;
            continue;
        }
        _scriptRanges.push_back(ScriptRange{
            static_cast<uint32_t>(_ptrExpectedAddress[i]),
            static_cast<uint32_t>(_ptrRegions[i].size),
            static_cast<uint32_t>(scriptBank),
        });
    }
    if (!allReadable || numStates == 0 || _totalSize == 0) {
        _pool.reset();
        _numStates = 0;
        _poolSize = 0;
        _slotTags.clear();
        QLOG("[rollback] init deferred: layout is not fully readable yet");
        return false;
    }
    size_t poolSize = 0;
    if (!checkedSizeMultiply(numStates, _totalSize, poolSize)) {
        _pool.reset();
        _numStates = 0;
        _poolSize = 0;
        _slotTags.clear();
        QLOG("[rollback] init rejected: snapshot pool size overflow "
             "(%zu states x %zu bytes)",
             numStates, _totalSize);
        return false;
    }
    if (poolSize > maxSnapshotPoolBytes()) {
        _pool.reset();
        _numStates = 0;
        _poolSize = 0;
        _slotTags.clear();
        QLOG("[rollback] init rejected: snapshot pool %zu bytes exceeds "
             "the Win32 practical limit of %zu bytes "
             "(%zu states x %zu bytes)",
             poolSize, maxSnapshotPoolBytes(), numStates, _totalSize);
        return false;
    }
    if (!buildCanonicalPointerMaps()) {
        _pool.reset();
        _numStates = 0;
        _poolSize = 0;
        _slotTags.clear();
        QLOG("[rollback] init deferred: descriptor identity map is invalid");
        return false;
    }
    std::unique_ptr<uint8_t[]> newPool(
        new (std::nothrow) uint8_t[poolSize]);
    if (!newPool) {
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        const BOOL haveMemory = GlobalMemoryStatusEx(&memory);
        size_t largestFree = 0;
        {
            SYSTEM_INFO info{};
            GetSystemInfo(&info);
            uintptr_t cursor =
                reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
            const uintptr_t limit =
                reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
            MEMORY_BASIC_INFORMATION mbi{};
            while (cursor < limit
                   && VirtualQuery(reinterpret_cast<void*>(cursor), &mbi,
                                   sizeof(mbi)) == sizeof(mbi)) {
                if (mbi.State == MEM_FREE && mbi.RegionSize > largestFree)
                    largestFree = mbi.RegionSize;
                const uintptr_t next =
                    reinterpret_cast<uintptr_t>(mbi.BaseAddress)
                    + mbi.RegionSize;
                if (next <= cursor) break;
                cursor = next;
            }
        }
        QLOG("[rollback] FAILED to allocate snapshot pool: stateBytes=%zu "
             "slots=%zu poolBytes=%zu (%.1f MiB) fixedRegions=%zu "
             "ptrRegions=%zu largestFreeVaRun=%zu (%.1f MiB) "
             "availVirtual=%llu availPhys=%llu reason=contiguous 32-bit "
             "reservation refused",
             _totalSize, numStates, poolSize,
             static_cast<double>(poolSize) / (1024.0 * 1024.0),
             _regions.size(), _ptrRegions.size(), largestFree,
             static_cast<double>(largestFree) / (1024.0 * 1024.0),
             haveMemory
                 ? static_cast<unsigned long long>(memory.ullAvailVirtual) : 0ull,
             haveMemory
                 ? static_cast<unsigned long long>(memory.ullAvailPhys) : 0ull);
        _pool.reset();
        _numStates = 0;
        _poolSize = 0;
        _slotTags.clear();
        return false;
    }
    std::vector<SlotTag> newSlotTags;
    try {
        newSlotTags.assign(numStates, SlotTag{});
    } catch (...) {
        QLOG("[rollback] FAILED to allocate %zu snapshot slot tags",
             numStates);
        _pool.reset();
        _numStates = 0;
        _poolSize = 0;
        _slotTags.clear();
        return false;
    }
    _pool = std::move(newPool);
    _numStates = numStates;
    _poolSize = poolSize;
    ++_layoutGeneration;
    if (_layoutGeneration == 0) ++_layoutGeneration;
    _slotTags = std::move(newSlotTags);
    std::memset(_pool.get(), 0, _poolSize);
    QLOG("[rollback] pool allocated: stateBytes=%zu slots=%zu poolBytes=%zu "
         "(%.1f MiB) base=%p fixedRegions=%zu ptrRegions=%zu "
         "layoutGeneration=%u limit=%zu",
         _totalSize, numStates, _poolSize,
         static_cast<double>(_poolSize) / (1024.0 * 1024.0),
         static_cast<void*>(_pool.get()), _regions.size(),
         _ptrRegions.size(), _layoutGeneration, maxSnapshotPoolBytes());
    return true;
}

bool RollbackManager::snapshotTo(uint8_t* dst) const {
    if (!dst || !_pool) return false;
    for (const Region& r : _regions) {
        if (!memReadable(reinterpret_cast<const void*>(r.addr), r.size))
            return false;
        if (r.addr == kAiVmAddress && r.size == kAiVmSize
            && !validateAiVm(
                reinterpret_cast<const uint8_t*>(r.addr))) {
            return false;
        }
    }
    for (size_t i = 0; i < _ptrRegions.size(); ++i) {
        const PtrRegion& pr = _ptrRegions[i];
        const ChildResolution child = resolveChild(pr);
        if (child.kind == ChildResolutionKind::Invalid)
            return false;
        const bool present =
            child.kind == ChildResolutionKind::ActiveValid;
        if (present != (_ptrExpectedPresent[i] != 0)
            || (present
                && child.address != _ptrExpectedAddress[i]))
            return false;
    }
    for (const AliasedPtrRegion& alias : _ptrAliases) {
        const ChildResolution child = resolveChild(alias.region);
        const uintptr_t address =
            child.kind == ChildResolutionKind::ActiveValid ? child.address : 0u;
        if (address != alias.sharedAddress)
            return false;
    }
    uint8_t* p = dst;
    for (const Region& r : _regions) {
        std::memcpy(p, reinterpret_cast<const void*>(r.addr), r.size);
        if (r.addr == kAiVmAddress && r.size == kAiVmSize) {
            normalizeAiVmInactive(p);
        }
        p += r.size;
    }
    for (size_t i = 0; i < _ptrRegions.size(); ++i) {
        const PtrRegion& pr = _ptrRegions[i];
        uint32_t present = _ptrExpectedPresent[i] ? 1u : 0u;
        std::memcpy(p, &present, 4); p += 4;
        if (present)
            std::memcpy(
                p,
                reinterpret_cast<const void*>(
                    _ptrExpectedAddress[i]),
                pr.size);
        else
            std::memset(p, 0, pr.size);
        p += pr.size;
    }
    return true;
}

bool RollbackManager::saveToSlot(size_t slot) {
    size_t offset = 0;
    if (!poolSlotOffset(slot, offset)) return false;
    const bool ok = snapshotTo(_pool.get() + offset);
    if (!ok && slot < _slotTags.size()) _slotTags[slot].valid = false;
    return ok;
}

bool RollbackManager::copySlotTo(size_t slot, uint8_t* out) const {
    size_t offset = 0;
    if (out == nullptr || !poolSlotOffset(slot, offset)) return false;
    std::memcpy(out, _pool.get() + offset, _totalSize);
    return true;
}

bool RollbackManager::loadFromSlot(size_t slot) {
    size_t offset = 0;
    if (!poolSlotOffset(slot, offset)) return false;
    return loadFrom(_pool.get() + offset);
}

bool RollbackManager::loadFrom(const uint8_t* src) {
    return loadFromInternal(src, false);
}

bool RollbackManager::loadFromForeign(const uint8_t* src) {
    return loadFromInternal(src, true);
}

bool RollbackManager::loadFromInternal(const uint8_t* src, bool foreign) {
    if (!src || !_pool) return false;
    std::vector<uintptr_t> savedChildren(_ptrRegions.size(), 0);
    std::vector<uint32_t> foreignActivationWords;
    if (foreign) {
        foreignActivationWords.assign(_ptrRegions.size(), 0u);
        for (size_t i = 0; i < _ptrRegions.size(); ++i) {
            const PtrRegion& pr = _ptrRegions[i];
            if (pr.activationPointerAddress != 0u
                && memReadable(reinterpret_cast<const void*>(
                                   pr.activationPointerAddress), 4)) {
                std::memcpy(&foreignActivationWords[i],
                            reinterpret_cast<const void*>(
                                pr.activationPointerAddress), 4);
            }
        }
    }
    struct ForeignWordRewrite { uintptr_t address; uint32_t value; };
    std::vector<ForeignWordRewrite> foreignWordRewrites;
    if (foreign) {
        struct DescriptorRelocation {
            uint32_t family;
            uint32_t foreignPointer;
            uint32_t livePointer;
        };
        std::vector<DescriptorRelocation> descriptorRelocations;
        for (const uintptr_t root : kDescriptorRoots) {
            for (size_t family = 0; family < kDescriptorFamilyCount;
                 ++family) {
                const uintptr_t address =
                    root + kDescriptorPointerOffsets[family];
                uint32_t foreignPtr = 0;
                if (!snapshotFixedU32(src, address, foreignPtr))
                    continue;
                if (!memReadable(
                        reinterpret_cast<const void*>(address), 4u)) {
                    QLOG("[rollback] foreign restore: live descriptor root "
                         "%08X is unreadable",
                         static_cast<unsigned>(address));
                    return false;
                }
                uint32_t livePtr = 0;
                std::memcpy(&livePtr,
                            reinterpret_cast<const void*>(address), 4u);
                if ((foreignPtr == 0u) != (livePtr == 0u)) {
                    QLOG("[rollback] foreign restore: root=%08X family=%zu "
                         "presence differs foreign=%08X live=%08X",
                         static_cast<unsigned>(root), family,
                         foreignPtr, livePtr);
                    return false;
                }
                if (foreignPtr == 0u) continue;
                bool conflict = false;
                bool known = false;
                for (const DescriptorRelocation& reloc :
                     descriptorRelocations) {
                    if (reloc.family == family
                        && reloc.foreignPointer == foreignPtr) {
                        known = true;
                        conflict = reloc.livePointer != livePtr;
                        break;
                    }
                }
                if (conflict) {
                    QLOG("[rollback] foreign restore: descriptor alias "
                         "family=%zu foreign=%08X maps to two live values",
                         family, foreignPtr);
                    return false;
                }
                if (!known) {
                    descriptorRelocations.push_back(DescriptorRelocation{
                        static_cast<uint32_t>(family), foreignPtr,
                        livePtr });
                }
            }
        }
        auto mappedLive = [&](uint32_t family, uint32_t foreignPtr,
                              uint32_t* liveOut) {
            for (const DescriptorRelocation& reloc : descriptorRelocations) {
                if (reloc.family == family
                    && reloc.foreignPointer == foreignPtr) {
                    *liveOut = reloc.livePointer;
                    return true;
                }
            }
            return false;
        };
        size_t unmappedActive = 0;
        const uint8_t* snapRegion = src;
        for (size_t ri = 0; ri < _regions.size(); ++ri) {
            const Region& r = _regions[ri];
            if (!safeToRestore(r.role)) {
                snapRegion += r.size;
                continue;
            }
            for (const size_t offset : _fixedCanonicalPointerOffsets[ri]) {
                if (offset + 4u > r.size) continue;
                bool inactiveOwnerSlot = false;
                for (size_t pi = 0; pi < _ptrRegions.size(); ++pi) {
                    const PtrRegion& pr = _ptrRegions[pi];
                    if (pr.fighterBase + pr.ptrOffset == r.addr + offset
                        && _ptrExpectedPresent[pi] == 0u) {
                        inactiveOwnerSlot = true;
                        break;
                    }
                }
                if (inactiveOwnerSlot) {
                    continue;
                }
                if (!memReadable(reinterpret_cast<const void*>(
                                     r.addr + offset), 4u)) {
                    QLOG("[rollback] foreign restore: live canonical word "
                         "%08X is unreadable",
                         static_cast<unsigned>(r.addr + offset));
                    return false;
                }
                uint32_t liveWord = 0;
                std::memcpy(&liveWord, reinterpret_cast<const void*>(
                                            r.addr + offset), 4u);
                foreignWordRewrites.push_back(
                    ForeignWordRewrite{ r.addr + offset, liveWord });
            }
            for (const DescriptorPointer& spec :
                 _fixedCanonicalDescriptorPointers[ri]) {
                if (spec.pointerOffset + 4u > r.size
                    || spec.activeMarkerOffset >= r.size) {
                    continue;
                }
                uint32_t foreignWord = 0;
                std::memcpy(&foreignWord,
                            snapRegion + spec.pointerOffset, 4u);
                if (foreignWord == 0u
                    || snapRegion[spec.activeMarkerOffset] == 0xFFu) {
                    continue;
                }
                uint32_t liveWord = 0;
                if (mappedLive(spec.family, foreignWord, &liveWord)) {
                    foreignWordRewrites.push_back(ForeignWordRewrite{
                        r.addr + spec.pointerOffset, liveWord });
                } else if (unmappedActive++ < 8u) {
                    QLOG("[rollback] foreign restore: unmapped active "
                         "descriptor region=%s offset=0x%zX family=%u "
                         "value=%08X (raw retained)",
                         r.name, spec.pointerOffset, spec.family,
                         foreignWord);
                }
            }
            for (const RelativePointer& spec :
                 _fixedCanonicalRelativePointers[ri]) {
                if (spec.pointerOffset + 4u > r.size
                    || spec.baseOffset + 4u > r.size
                    || spec.countOffset + 4u > r.size
                    || spec.activeMarkerOffset >= r.size) {
                    continue;
                }
                uint32_t raw = 0;
                std::memcpy(&raw, snapRegion + spec.pointerOffset, 4u);
                if (raw == 0u
                    || snapRegion[spec.activeMarkerOffset] == 0xFFu) {
                    continue;
                }
                uint32_t base = 0;
                uint32_t count = 0;
                std::memcpy(&base, snapRegion + spec.baseOffset, 4u);
                std::memcpy(&count, snapRegion + spec.countOffset, 4u);
                const uint64_t extent =
                    static_cast<uint64_t>(count) * spec.elementSize;
                const bool interior =
                    base != 0u && spec.elementSize != 0u && raw >= base
                    && static_cast<uint64_t>(raw - base) < extent
                    && raw - base < 0x01000000u;
                if (!interior) continue;
                uint32_t liveBase = 0;
                if (mappedLive(3u, base, &liveBase)) {
                    foreignWordRewrites.push_back(ForeignWordRewrite{
                        r.addr + spec.pointerOffset,
                        liveBase + (raw - base) });
                } else if (unmappedActive++ < 8u) {
                    QLOG("[rollback] foreign restore: unmapped interior "
                         "base region=%s offset=0x%zX base=%08X "
                         "(raw retained)",
                         r.name, spec.pointerOffset, base);
                }
            }
            snapRegion += r.size;
        }
        if (unmappedActive != 0u) {
            QLOG("[rollback] foreign restore: %zu active canonical words "
                 "kept foreign raw values",
                 unmappedActive);
        }
    }
    const uint8_t* check = src;
    for (const Region& r : _regions) {
        if (r.addr == kAiVmAddress && r.size == kAiVmSize
            && !validateAiVm(check)) {
            return false;
        }
        if (safeToRestore(r.role)
            && !memWritable(reinterpret_cast<void*>(r.addr), r.size))
            return false;
        check += r.size;
    }
    for (size_t i = 0; i < _ptrRegions.size(); ++i) {
        const PtrRegion& pr = _ptrRegions[i];
        uint32_t present = 0;
        std::memcpy(&present, check, 4);
        if (!validPresenceMarker(present)) return false;
        const bool expected = _ptrExpectedPresent[i] != 0;
        const ChildResolution child = resolveChild(pr);
        if (child.kind == ChildResolutionKind::Invalid)
            return false;
        const bool current =
            child.kind == ChildResolutionKind::ActiveValid;
        if (present != (expected ? 1u : 0u) || current != expected
            || (current
                && child.address != _ptrExpectedAddress[i])) {
            return false;
        }
        if (foreign) {
            uintptr_t liveChild = expected ? _ptrExpectedAddress[i] : 0;
            if (present && safeToRestore(pr.role)) {
                if (!liveChild
                    || !memWritable(
                        reinterpret_cast<void*>(liveChild), pr.size)) {
                    return false;
                }
            }
            savedChildren[i] = liveChild;
            check += 4 + pr.size;
            continue;
        }
        if (pr.activationPointerAddress != 0u) {
            uint32_t savedActivation = 0;
            if (!snapshotFixedU32(
                    src, pr.activationPointerAddress,
                    savedActivation)
                || pointerChildParticipates(
                       pr, savedActivation) != expected) {
                return false;
            }
        }
        uintptr_t savedChild = 0;
        if (pr.ownerCaptured) {
            uint32_t savedChild32 = 0;
            if (!snapshotFixedU32(
                    src, pr.fighterBase + pr.ptrOffset,
                    savedChild32)) {
                return false;
            }
            savedChild = static_cast<uintptr_t>(savedChild32);
            if (expected
                && savedChild != _ptrExpectedAddress[i]) {
                return false;
            }
            if (!expected)
                savedChild = 0;
        } else {
            savedChild = _ptrExpectedAddress[i];
        }
        savedChildren[i] = savedChild;
        if (present && safeToRestore(pr.role)) {
            if (!savedChild
                || !memWritable(
                    reinterpret_cast<void*>(savedChild), pr.size)) {
                return false;
            }
        }
        check += 4 + pr.size;
    }

    struct StagedChild {
        void* destination = nullptr;
        const uint8_t* source = nullptr;
        size_t size = 0;
    };
    static thread_local std::vector<StagedChild> stagedChildren;
    stagedChildren.clear();
    stagedChildren.reserve(_ptrRegions.size());
    {
        const uint8_t* walk = src;
        for (const Region& r : _regions) walk += r.size;
        for (size_t i = 0; i < _ptrRegions.size(); ++i) {
            const PtrRegion& pr = _ptrRegions[i];
            uint32_t present = 0;
            std::memcpy(&present, walk, 4);
            walk += 4;
            if (present && safeToRestore(pr.role)) {
                StagedChild staged{};
                staged.destination =
                    reinterpret_cast<void*>(savedChildren[i]);
                staged.source = walk;
                staged.size = pr.size;
                stagedChildren.push_back(staged);
            }
            walk += pr.size;
        }
    }

    const uint8_t* p = src;
    for (const Region& r : _regions) {
        if (safeToRestore(r.role)) {
            std::memcpy(reinterpret_cast<void*>(r.addr), p, r.size);
            if (r.addr == kAiVmAddress && r.size == kAiVmSize) {
                normalizeAiVmInactive(
                    reinterpret_cast<uint8_t*>(r.addr));
            }
        }
        p += r.size;
    }
    if (foreign) {
        // The fixed-region copy planted the other process's pointers in live memory: restore this process's pointers before anything reads them.
        for (const ForeignWordRewrite& rewrite : foreignWordRewrites) {
            std::memcpy(reinterpret_cast<void*>(rewrite.address),
                        &rewrite.value, 4u);
        }
        for (size_t i = 0; i < _ptrRegions.size(); ++i) {
            const PtrRegion& pr = _ptrRegions[i];
            if (pr.ownerCaptured && _ptrExpectedPresent[i] != 0) {
                const uint32_t liveChild =
                    static_cast<uint32_t>(_ptrExpectedAddress[i]);
                std::memcpy(
                    reinterpret_cast<void*>(pr.fighterBase + pr.ptrOffset),
                    &liveChild, 4);
            }
            if (pr.activationPointerAddress != 0u
                && i < foreignActivationWords.size()) {
                std::memcpy(
                    reinterpret_cast<void*>(pr.activationPointerAddress),
                    &foreignActivationWords[i], 4);
            }
        }
    }
    for (const StagedChild& staged : stagedChildren)
        std::memcpy(staged.destination, staged.source, staged.size);
    return true;
}

bool RollbackManager::saveTaggedToSlot(size_t slot, uint32_t epoch,
                                       uint32_t frame, uint32_t phase) {
    if (!saveToSlot(slot)) return false;
    _slotTags[slot] = SlotTag{
        true, _layoutGeneration, epoch, frame, phase
    };
    return true;
}

bool RollbackManager::hasTaggedSlot(size_t slot, uint32_t epoch,
                                    uint32_t frame, uint32_t phase) const {
    if (!_pool || slot >= _slotTags.size()) return false;
    const SlotTag& tag = _slotTags[slot];
    return tag.valid
        && tag.layoutGeneration == _layoutGeneration
        && tag.epoch == epoch
        && tag.frame == frame
        && tag.phase == phase;
}

bool RollbackManager::loadTaggedFromSlot(size_t slot, uint32_t epoch,
                                         uint32_t frame, uint32_t phase) {
    if (!hasTaggedSlot(slot, epoch, frame, phase)) return false;
    return loadFromSlot(slot);
}

const RollbackManager::SlotTag* RollbackManager::slotTag(size_t slot) const {
    return slot < _slotTags.size() ? &_slotTags[slot] : nullptr;
}

bool RollbackManager::invalidateTaggedSlot(
    size_t slot, uint32_t epoch, uint32_t frame) {
    if (slot >= _slotTags.size())
        return false;
    SlotTag& tag = _slotTags[slot];
    if (!tag.valid || tag.epoch != epoch || tag.frame != frame)
        return false;
    tag.valid = false;
    return true;
}

void RollbackManager::invalidateSlots() {
    for (SlotTag& tag : _slotTags) tag.valid = false;
}

bool RollbackManager::compareWith(const uint8_t* snap, Diff* out, Profile profile) const {
    if (!snap || !_pool) {
        if (out) {
            out->region = "invalid-snapshot";
            out->offsetInRegion = 0;
            out->diffBytes = 1;
        }
        return false;
    }
    const uint8_t* p = snap;
    bool identical = true;
    if (out) { out->region = nullptr; out->offsetInRegion = 0; out->diffBytes = 0; }
    auto wanted = [&](RegionRole role) {
        switch (profile) {
            case Profile::Gameplay: return inGameplayHash(role);
            case Profile::Render:   return inRenderHash(role);
            default:                return true;
        }
    };
    auto noteDifference = [&](const char* name, size_t offset,
                              size_t bytes) {
        identical = false;
        if (out) {
            if (!out->region) {
                out->region = name;
                out->offsetInRegion = offset;
            }
            out->diffBytes += bytes;
        }
    };
    auto diff = [&](const char* name, const uint8_t* live,
                    const uint8_t* saved, size_t n, size_t baseOffset) {
        for (size_t i = 0; i < n; ++i) {
            if (live[i] != saved[i]) {
                noteDifference(name, baseOffset + i, 1);
            }
        }
    };
    for (const Region& r : _regions) {
        if (wanted(r.role)) {
            const uint8_t* live =
                reinterpret_cast<const uint8_t*>(r.addr);
            if (r.addr == kAiVmAddress && r.size == kAiVmSize) {
                if (!validateAiVm(live) || !validateAiVm(p)) {
                    noteDifference(r.name, 0, 1);
                } else {
                    uint32_t liveDepth = 0;
                    uint32_t savedDepth = 0;
                    std::memcpy(&liveDepth, live, sizeof(liveDepth));
                    std::memcpy(&savedDepth, p, sizeof(savedDepth));
                    const size_t activeBytes =
                        (static_cast<size_t>(
                            (std::max)(liveDepth, savedDepth)) + 1u)
                        * sizeof(uint32_t);
                    diff(r.name, live, p, kAiVmHeaderSize, 0);
                    diff(
                        r.name,
                        live + kAiVmHeaderSize,
                        p + kAiVmHeaderSize,
                        activeBytes,
                        kAiVmHeaderSize);
                    diff(
                        r.name,
                        live + kAiVmCursorOffset,
                        p + kAiVmCursorOffset,
                        sizeof(uint32_t),
                        kAiVmCursorOffset);
                }
            } else {
                diff(r.name, live, p, r.size, 0);
            }
        }
        p += r.size;
    }
    for (size_t i = 0; i < _ptrRegions.size(); ++i) {
        const PtrRegion& pr = _ptrRegions[i];
        uint32_t present; std::memcpy(&present, p, 4); const uint8_t* savedBytes = p + 4;
        if (wanted(pr.role)) {
            const ChildResolution child = resolveChild(pr);
            const bool currentPresent =
                child.kind == ChildResolutionKind::ActiveValid;
            const bool expectedPresent =
                _ptrExpectedPresent[i] != 0;
            const bool liveLayoutValid =
                child.kind != ChildResolutionKind::Invalid
                && currentPresent == expectedPresent
                && (!currentPresent
                    || child.address == _ptrExpectedAddress[i]);
            bool savedGateValid = true;
            if (pr.activationPointerAddress != 0u) {
                uint32_t savedActivation = 0;
                savedGateValid =
                    snapshotFixedU32(
                        snap, pr.activationPointerAddress,
                        savedActivation)
                    && pointerChildParticipates(
                           pr, savedActivation)
                        == expectedPresent;
            }
            bool savedOwnerValid = true;
            if (pr.ownerCaptured && expectedPresent) {
                uint32_t savedChild = 0;
                savedOwnerValid =
                    snapshotFixedU32(
                        snap,
                        pr.fighterBase + pr.ptrOffset,
                        savedChild)
                    && savedChild
                        == static_cast<uint32_t>(
                            _ptrExpectedAddress[i]);
            }
            if (!validPresenceMarker(present)
                || !liveLayoutValid
                || !savedGateValid
                || !savedOwnerValid
                || present != (expectedPresent ? 1u : 0u)) {
                noteDifference(pr.name, 0, 1);
            } else if (present == 1u) {
                diff(
                    pr.name,
                    reinterpret_cast<const uint8_t*>(
                        child.address),
                    savedBytes, pr.size, 0);
            }
        }
        p += 4 + pr.size;
    }
    return identical;
}

bool RollbackManager::validateAiVm(const uint8_t* bytes) const {
    if (!bytes) return false;
    uint32_t depth = 0;
    uint32_t selectedBank = 0;
    uint32_t selectedBase = 0;
    uint32_t cursor = 0;
    std::memcpy(&depth, bytes, sizeof(depth));
    std::memcpy(&selectedBank, bytes + 4, sizeof(selectedBank));
    std::memcpy(&selectedBase, bytes + 8, sizeof(selectedBase));
    std::memcpy(
        &cursor, bytes + kAiVmCursorOffset, sizeof(cursor));
    return validAiVmSelection(
        depth, selectedBank, selectedBase, cursor,
        _scriptRanges.data(), _scriptRanges.size());
}

void RollbackManager::normalizeAiVmInactive(uint8_t* bytes) const {
    uint32_t depth = 0;
    std::memcpy(&depth, bytes, sizeof(depth));
    if (!validAiVmDepth(depth)) return;
    const size_t activeBytes =
        (static_cast<size_t>(depth) + 1u) * sizeof(uint32_t);
    if (activeBytes < kAiVmStackBytes) {
        std::memset(
            bytes + kAiVmHeaderSize + activeBytes, 0,
            kAiVmStackBytes - activeBytes);
    }
}

uint64_t RollbackManager::hashScriptValue(
    uint32_t value, uint32_t selectedBank, uint64_t hash) const {
    uint32_t offset = 0;
    if (canonicalScriptPointerForBank(
            value, _scriptRanges.data(), _scriptRanges.size(),
            selectedBank, offset)) {
        const uint8_t tag = 1u;
        hash = fnv1a(&tag, sizeof(tag), hash);
        hash = fnv1a(&selectedBank, sizeof(selectedBank), hash);
        return fnv1a(&offset, sizeof(offset), hash);
    }
    const uint8_t tag = 0u;
    hash = fnv1a(&tag, sizeof(tag), hash);
    return fnv1a(&value, sizeof(value), hash);
}

uint64_t RollbackManager::hashAiVmCanonical(const uint8_t* bytes) const {
    uint64_t hash = 1469598103934665603ull;
    uint32_t depth = 0;
    uint32_t selectedBank = 0;
    std::memcpy(&depth, bytes, sizeof(depth));
    std::memcpy(&selectedBank, bytes + 4, sizeof(selectedBank));
    if (!validateAiVm(bytes)) {
        static constexpr uint32_t invalid = 0xBAD0A17Eu;
        hash = fnv1a(&invalid, sizeof(invalid), hash);
        hash = fnv1a(bytes, kAiVmHeaderSize, hash);
        return fnv1a(
            bytes + kAiVmCursorOffset, sizeof(uint32_t), hash);
    }

    hash = fnv1a(bytes, 8, hash);

    uint32_t selectedBase = 0;
    std::memcpy(&selectedBase, bytes + 8, sizeof(selectedBase));
    hash = hashScriptValue(selectedBase, selectedBank, hash);
    hash = fnv1a(bytes + 12, kAiVmHeaderSize - 12, hash);

    const uint8_t* stack = bytes + kAiVmHeaderSize;
    for (uint32_t i = 0; i <= depth; ++i) {
        uint32_t value = 0;
        std::memcpy(
            &value, stack + static_cast<size_t>(i) * sizeof(value),
            sizeof(value));
        hash = hashScriptValue(value, selectedBank, hash);
    }
    return fnv1a(
        bytes + kAiVmCursorOffset, sizeof(uint32_t), hash);
}

template<typename RelativePointers, typename DescriptorPointers,
         typename DescriptorIdentities>
static uint64_t hashCanonical(RollbackManager::HashAlgorithm algorithm,
                              const uint8_t* bytes, size_t size,
                              const std::vector<size_t>& pointerOffsets,
                              const RelativePointers& relativePointers,
                              const DescriptorPointers& descriptorPointers,
                              const DescriptorIdentities& descriptorIdentities) {
    uint64_t h = 1469598103934665603ull;
    size_t cursor = 0;
    static const uint32_t canonicalPointer = 0xF1F1F1F1u;
    size_t pointerIndex = 0;
    size_t relativeIndex = 0;
    size_t descriptorIndex = 0;
    while (pointerIndex < pointerOffsets.size()
           || relativeIndex < relativePointers.size()
           || descriptorIndex < descriptorPointers.size()) {
        const size_t genericOffset =
            pointerIndex < pointerOffsets.size()
            ? pointerOffsets[pointerIndex] : static_cast<size_t>(-1);
        const size_t relativeOffset =
            relativeIndex < relativePointers.size()
            ? relativePointers[relativeIndex].pointerOffset
            : static_cast<size_t>(-1);
        const size_t descriptorOffset =
            descriptorIndex < descriptorPointers.size()
            ? descriptorPointers[descriptorIndex].pointerOffset
            : static_cast<size_t>(-1);
        const size_t offset =
            (std::min)(genericOffset,
                       (std::min)(relativeOffset, descriptorOffset));
        const bool useDescriptor = descriptorOffset == offset;
        const bool useRelative =
            !useDescriptor && relativeOffset == offset;
        if (useDescriptor)
            ++descriptorIndex;
        else if (useRelative)
            ++relativeIndex;
        else
            ++pointerIndex;
        if (useDescriptor) {
            while (pointerIndex < pointerOffsets.size()
                   && pointerOffsets[pointerIndex] == offset)
                ++pointerIndex;
            while (relativeIndex < relativePointers.size()
                   && relativePointers[relativeIndex].pointerOffset == offset)
                ++relativeIndex;
        } else if (useRelative) {
            while (pointerIndex < pointerOffsets.size()
                   && pointerOffsets[pointerIndex] == offset)
                ++pointerIndex;
        }
        if (offset + sizeof(uint32_t) > size) continue;
        if (offset > cursor)
            h = hashBulk(algorithm, bytes + cursor, offset - cursor, h);
        uint32_t rawPointer = 0;
        std::memcpy(&rawPointer, bytes + offset, sizeof(rawPointer));
        if (useDescriptor) {
            const auto& spec =
                descriptorPointers[descriptorIndex - 1];
            const bool inactive =
                bytes[spec.activeMarkerOffset] == 0xFFu;
            const auto identity = std::find_if(
                descriptorIdentities.begin(),
                descriptorIdentities.end(),
                [&](const auto& item) {
                    return item.rawPointer == rawPointer
                        && item.family == spec.family;
                });
            const uint8_t tag =
                inactive ? 3u
                : rawPointer == 0u ? 0u
                : identity != descriptorIdentities.end() ? 1u : 2u;
            h = fnv1a(&tag, sizeof(tag), h);
            h = fnv1a(&spec.family, sizeof(spec.family), h);
            if (!inactive && identity != descriptorIdentities.end()) {
                h = fnv1a(&identity->count, sizeof(identity->count), h);
                h = fnv1a(
                    &identity->byteCount,
                    sizeof(identity->byteCount), h);
                h = fnv1a(
                    &identity->contentHash,
                    sizeof(identity->contentHash), h);
            } else if (!inactive && rawPointer != 0u) {
                h = fnv1a(
                    &rawPointer, sizeof(rawPointer), h);
            }
        } else {
            uint32_t canonical = 0;
            if (useRelative) {
                const auto& spec =
                    relativePointers[relativeIndex - 1];
                if (bytes[spec.activeMarkerOffset] == 0xFFu) {
                    canonical = 0xA5FFFFFEu;
                } else {
                    uint32_t base = 0;
                    uint32_t count = 0;
                    std::memcpy(
                        &base, bytes + spec.baseOffset, sizeof(base));
                    std::memcpy(
                        &count, bytes + spec.countOffset, sizeof(count));
                    canonical =
                        RollbackManager::canonicalActorInteriorPointer(
                            rawPointer, base, count, spec.elementSize);
                }
            } else {
                canonical =
                    rawPointer == 0 ? 0u : canonicalPointer;
            }
            h = fnv1a(&canonical, sizeof(canonical), h);
        }
        cursor = offset + sizeof(uint32_t);
    }
    if (cursor < size)
        h = hashBulk(algorithm, bytes + cursor, size - cursor, h);
    return h;
}

uint64_t RollbackManager::hashLive(std::vector<RegionHash>* perRegion,
                                   Profile profile) const {
    uint64_t acc = 1469598103934665603ull;
    std::array<uint8_t, kQohConfigRegionSize> qohConfigScratch{};
    static const std::vector<RelativePointer> kNoRelativePointers;
    static const std::vector<DescriptorPointer> kNoDescriptorPointers;
    static const std::vector<DescriptorIdentity> kNoDescriptorIdentities;
    if (perRegion) perRegion->clear();
    auto wanted = [&](RegionRole role) {
        switch (profile) {
            case Profile::Gameplay: return inGameplayHash(role);
            case Profile::Render:   return inRenderHash(role);
            default:                return true;
        }
    };
    if (wanted(RegionRole::GameplayRequired)) {
        static constexpr uint64_t kAssetDomain =
            0x514F483939415353ull; // "QOH99ASS"
        uint64_t assetHash =
            fnv1a(&kAssetDomain, sizeof(kAssetDomain));
        assetHash = fnv1a(
            &_immutableAssetFingerprint,
            sizeof(_immutableAssetFingerprint), assetHash);
        acc = fnv1a(&assetHash, sizeof(assetHash), acc);
        if (perRegion) {
            perRegion->push_back({
                "immutable_layout_assets", assetHash,
                sizeof(_immutableAssetFingerprint),
                _immutableAssetFingerprint != 0,
            });
        }
    }
    for (size_t i = 0; i < _regions.size(); ++i) {
        const Region& r = _regions[i];
        const bool readable =
            memReadable(reinterpret_cast<void*>(r.addr), r.size);
        uint64_t h = 0;
        if (readable) {
            const uint8_t* hashBytes =
                reinterpret_cast<const uint8_t*>(r.addr);
            bool canonicalConfigValid = true;
            if ((r.addr == kQohConfigRegionAddress
                    && r.size == kQohConfigRegionSize)
                || (r.addr == kQohConfigSnapshotAddress
                    && r.size == kQohConfigSnapshotRegionSize)) {
                std::memcpy(
                    qohConfigScratch.data(), hashBytes, r.size);
                canonicalConfigValid =
                    canonicalizeQohPeerLocalConfigForHash(
                        r.addr, qohConfigScratch.data(), r.size);
                hashBytes = qohConfigScratch.data();
            }
            h = !canonicalConfigValid
                ? 0xBAD0C0F16F1A0001ull
                : r.addr == kAiVmAddress && r.size == kAiVmSize
                    ? hashAiVmCanonical(hashBytes)
                    : hashCanonical(
                          _hashAlgorithm, hashBytes, r.size,
                          _fixedCanonicalPointerOffsets[i],
                          _fixedCanonicalRelativePointers[i],
                          _fixedCanonicalDescriptorPointers[i],
                          _descriptorIdentities);
        }
        if (wanted(r.role)) {
            acc = fnv1a(&h, sizeof(h), acc);
            if (perRegion)
                perRegion->push_back({ r.name, h, r.size, readable });
        }
    }
    for (size_t i = 0; i < _ptrRegions.size(); ++i) {
        const PtrRegion& pr = _ptrRegions[i];
        const ChildResolution child = resolveChild(pr);
        const bool expected =
            _ptrExpectedPresent[i] != 0;
        const bool current =
            child.kind == ChildResolutionKind::ActiveValid;
        const bool layoutValid =
            child.kind != ChildResolutionKind::Invalid
            && current == expected
            && (!current
                || child.address == _ptrExpectedAddress[i]);
        const uint32_t marker =
            layoutValid ? (expected ? 1u : 0u) : 2u;
        const uint64_t h = marker == 1u
             ? hashCanonical(
                   _hashAlgorithm,
                   reinterpret_cast<const uint8_t*>(
                       child.address),
                   pr.size,
                   _ptrCanonicalPointerOffsets[i], kNoRelativePointers,
                   kNoDescriptorPointers, kNoDescriptorIdentities)
             : marker == 0u ? 0u : 0xBAD0F135E11CE001ull;
        if (wanted(pr.role)) {
            acc = fnv1a(&marker, sizeof(marker), acc);
            acc = fnv1a(&h, sizeof(h), acc);
            if (perRegion)
                perRegion->push_back({
                    pr.name, h, pr.size, marker == 1u
                });
        }
    }
    return acc;
}

uint64_t RollbackManager::hashSlot(
    size_t slot, Profile profile,
    std::vector<RegionHash>* perRegion) const {
    size_t offset = 0;
    if (!poolSlotOffset(slot, offset)) return 0;
    const uint8_t* const snapshot = _pool.get() + offset;
    const uint8_t* p = snapshot;
    uint64_t acc = 1469598103934665603ull;
    std::array<uint8_t, kQohConfigRegionSize> qohConfigScratch{};
    static const std::vector<RelativePointer> kNoRelativePointers;
    static const std::vector<DescriptorPointer> kNoDescriptorPointers;
    static const std::vector<DescriptorIdentity> kNoDescriptorIdentities;
    if (perRegion) perRegion->clear();
    auto wanted = [&](RegionRole role) {
        switch (profile) {
            case Profile::Gameplay: return inGameplayHash(role);
            case Profile::Render:   return inRenderHash(role);
            default:                return true;
        }
    };
    if (wanted(RegionRole::GameplayRequired)) {
        static constexpr uint64_t kAssetDomain =
            0x514F483939415353ull; // "QOH99ASS"
        uint64_t assetHash =
            fnv1a(&kAssetDomain, sizeof(kAssetDomain));
        assetHash = fnv1a(
            &_immutableAssetFingerprint,
            sizeof(_immutableAssetFingerprint), assetHash);
        acc = fnv1a(&assetHash, sizeof(assetHash), acc);
        if (perRegion) {
            perRegion->push_back({
                "immutable_layout_assets", assetHash,
                sizeof(_immutableAssetFingerprint),
                _immutableAssetFingerprint != 0,
            });
        }

    }
    for (size_t i = 0; i < _regions.size(); ++i) {
        const Region& r = _regions[i];
        if (wanted(r.role)) {
            const uint8_t* hashBytes = p;
            bool canonicalConfigValid = true;
            if ((r.addr == kQohConfigRegionAddress
                    && r.size == kQohConfigRegionSize)
                || (r.addr == kQohConfigSnapshotAddress
                    && r.size == kQohConfigSnapshotRegionSize)) {
                std::memcpy(
                    qohConfigScratch.data(), hashBytes, r.size);
                canonicalConfigValid =
                    canonicalizeQohPeerLocalConfigForHash(
                        r.addr, qohConfigScratch.data(), r.size);
                hashBytes = qohConfigScratch.data();
            }
            const uint64_t h =
                !canonicalConfigValid
                    ? 0xBAD0C0F16F1A0001ull
                    : r.addr == kAiVmAddress && r.size == kAiVmSize
                        ? hashAiVmCanonical(hashBytes)
                        : hashCanonical(
                              _hashAlgorithm, hashBytes, r.size,
                              _fixedCanonicalPointerOffsets[i],
                              _fixedCanonicalRelativePointers[i],
                              _fixedCanonicalDescriptorPointers[i],
                              _descriptorIdentities);
            acc = fnv1a(&h, sizeof(h), acc);
            if (perRegion) {
                perRegion->push_back({
                    r.name, h, r.size, canonicalConfigValid,
                });
            }
        }
        p += r.size;
    }
    for (size_t i = 0; i < _ptrRegions.size(); ++i) {
        const PtrRegion& pr = _ptrRegions[i];
        uint32_t present = 0;
        std::memcpy(&present, p, 4);
        p += 4;
        if (wanted(pr.role)) {
            const bool expected =
                _ptrExpectedPresent[i] != 0;
            bool validMarker =
                validPresenceMarker(present)
                && present == (expected ? 1u : 0u);
            if (pr.activationPointerAddress != 0u) {
                uint32_t savedActivation = 0;
                validMarker =
                    validMarker
                    && snapshotFixedU32(
                           snapshot,
                           pr.activationPointerAddress,
                           savedActivation)
                    && pointerChildParticipates(
                           pr, savedActivation) == expected;
            }
            if (pr.ownerCaptured && expected) {
                uint32_t savedChild = 0;
                validMarker =
                    validMarker
                    && snapshotFixedU32(
                           snapshot,
                           pr.fighterBase + pr.ptrOffset,
                           savedChild)
                    && savedChild
                        == static_cast<uint32_t>(
                            _ptrExpectedAddress[i]);
            }
            const uint64_t h = validMarker && present == 1u
                ? hashCanonical(
                      _hashAlgorithm,
                      p, pr.size, _ptrCanonicalPointerOffsets[i],
                      kNoRelativePointers, kNoDescriptorPointers,
                      kNoDescriptorIdentities)
                : validMarker && present == 0u
                    ? 0u
                    : 0xBAD0F135E11CE001ull;
            acc = fnv1a(&present, sizeof(present), acc);
            acc = fnv1a(&h, sizeof(h), acc);
            if (perRegion) {
                perRegion->push_back({
                    pr.name, h, pr.size,
                    validMarker && present == 1u,
                });
            }
        }
        p += pr.size;
    }
    return acc;
}

bool RollbackManager::validateLive(const char** why) const {
    return validateLive(why,
                        LiveValidationPolicy::NetplayDeterministic);
}

bool RollbackManager::validateLive(
    const char** why,
    LiveValidationPolicy policy) const {
    auto fail = [&](const char* m){ if (why) *why = m; return false; };
    const bool checkSchedulerFields =
        policy == LiveValidationPolicy::NetplayDeterministic
        || policy == LiveValidationPolicy::LocalTrainingSnapshot;
    QohMatchConfigProof activeConfig{};
    QohMatchConfigProof snapshotConfig{};
    bool foundActiveConfig = false;
    bool foundSnapshotConfig = false;
    for (size_t regionIndex = 0;
         regionIndex < _regions.size(); ++regionIndex) {
        const Region& r = _regions[regionIndex];
        if (!memReadable(reinterpret_cast<void*>(r.addr), r.size)) return fail(r.name);
        if (r.addr == kAiVmAddress && r.size == kAiVmSize
            && !validateAiVm(
                reinterpret_cast<const uint8_t*>(r.addr))) {
            return fail("ai_script_vm");
        }
        const uint8_t* bytes =
            reinterpret_cast<const uint8_t*>(r.addr);
        if (r.addr == kQohConfigRegionAddress
            && r.size == kQohConfigRegionSize) {
            if (!qohMatchConfigProof(
                    r.addr, bytes, r.size, activeConfig)) {
                return fail("active QOH config pointer is invalid");
            }
            foundActiveConfig = true;
        } else if (r.addr == kQohConfigSnapshotAddress
                   && r.size == kQohConfigSnapshotRegionSize) {
            if (!qohMatchConfigProof(
                    r.addr, bytes, r.size, snapshotConfig)) {
                return fail("QOH match-config snapshot is invalid");
            }
            foundSnapshotConfig = true;
        }
        for (const DescriptorPointer& spec :
             _fixedCanonicalDescriptorPointers[regionIndex]) {
            // sub_42DBE0 marks unused actor slots with actor+0xAE == 0xFF.
            if (bytes[spec.activeMarkerOffset] == 0xFFu) continue;
            uint32_t raw = 0;
            std::memcpy(
                &raw, bytes + spec.pointerOffset, sizeof(raw));
            if (raw == 0) continue;
            const bool known = std::any_of(
                _descriptorIdentities.begin(),
                _descriptorIdentities.end(),
                [&](const DescriptorIdentity& identity) {
                    return identity.rawPointer == raw
                        && identity.family == spec.family;
                });
            if (!known) return fail(r.name);
        }
    }
    if (!foundActiveConfig || !foundSnapshotConfig)
        return fail("QOH match-config regions are missing");
    if (!_matchRulesOverridden
        && (activeConfig.timerMode != snapshotConfig.timerMode
        || activeConfig.aiDifficulty != snapshotConfig.aiDifficulty
        || activeConfig.lifeMultiplier != snapshotConfig.lifeMultiplier
        || activeConfig.roundsToWinMinusOne
            != snapshotConfig.roundsToWinMinusOne)) {
        QLOG("[rollback] match-config mismatch (match RULES only - not a "
             "state hash, not the recording): timerMode=%u/%u%s "
             "aiDifficulty=%u/%u%s lifeMultiplier=%u/%u%s "
             "roundsToWinMinusOne=%u/%u%s  (active/snapshot)",
             activeConfig.timerMode, snapshotConfig.timerMode,
             activeConfig.timerMode != snapshotConfig.timerMode ? " <-" : "",
             activeConfig.aiDifficulty, snapshotConfig.aiDifficulty,
             activeConfig.aiDifficulty != snapshotConfig.aiDifficulty
                 ? " <-" : "",
             activeConfig.lifeMultiplier, snapshotConfig.lifeMultiplier,
             activeConfig.lifeMultiplier != snapshotConfig.lifeMultiplier
                 ? " <-" : "",
             activeConfig.roundsToWinMinusOne,
             snapshotConfig.roundsToWinMinusOne,
             activeConfig.roundsToWinMinusOne
                     != snapshotConfig.roundsToWinMinusOne ? " <-" : "");
        return fail("active and snapshotted QOH gameplay config differ");
    }
    const uint32_t activeConfigPointer =
        *reinterpret_cast<volatile uint32_t*>(
            kQohActiveConfigPointerAddress);
    const uint32_t bindingPointer =
        *reinterpret_cast<volatile uint32_t*>(
            kQohBindingPointerAddress);
    if (bindingPointer
        != expectedQohBindingPointer(activeConfigPointer)) {
        return fail("QOH binding pointer does not match active profile");
    }
    if (checkSchedulerFields) {
        if (*reinterpret_cast<volatile uint32_t*>(0x4AAA50u) != 0u)
            return fail("QOH M2 pipeline is not in local mode");
        if (!qohNetSchedulerConfigValid(
            reinterpret_cast<const uint8_t*>(
                static_cast<uintptr_t>(activeConfigPointer)),
            kQohConfigGameplayPrefixSize)
            || !qohNetSchedulerConfigValid(
            reinterpret_cast<const uint8_t*>(
                kQohConfigSnapshotAddress),
            kQohConfigGameplayPrefixSize)) {
            return fail("QOH local scheduler config is not normalized");
        }
    }
    for (size_t i = 0; i < _ptrRegions.size(); ++i) {
        const ChildResolution child =
            resolveChild(_ptrRegions[i]);
        const bool required =
            _ptrRegions[i].role == RegionRole::GameplayRequired;
        const bool expected = _ptrExpectedPresent[i] != 0;
        const bool present =
            child.kind == ChildResolutionKind::ActiveValid;
        if (child.kind == ChildResolutionKind::Invalid
            || present != expected
            || (present
                && child.address != _ptrExpectedAddress[i])
            || (expected
                && !memReadable(
                    reinterpret_cast<void*>(child.address),
                    _ptrRegions[i].size))
            || (required && !expected)) {
            return fail(_ptrRegions[i].name);
        }
    }
    uintptr_t active = *reinterpret_cast<volatile uint32_t*>(kActiveBlockPtr);
    if (active != kBlock0Addr && active != kBlock1Addr) return fail("activeBlock!=block0/block1");
    if (why) *why = nullptr;
    return true;
}

}
