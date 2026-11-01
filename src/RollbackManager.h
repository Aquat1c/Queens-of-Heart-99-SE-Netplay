#pragma once
#include "qoh99_game.h"
#include <cstdint>
#include <cstddef>
#include <limits>
#include <vector>
#include <memory>

namespace qoh99 {

class RollbackManager {
public:
    struct QohMatchConfigProof {
        uint32_t domain = 0x31474643u; // "CFG1", little-endian
        uint32_t timerMode = 0;
        uint32_t aiDifficulty = 0;
        uint32_t lifeMultiplier = 0;
        uint32_t roundsToWinMinusOne = 0;
    };
    static constexpr uintptr_t kQohConfigRegionAddress = 0x613000u;
    static constexpr size_t kQohConfigRegionSize = 0x47C0u;
    static constexpr uintptr_t kQohActiveConfigPointerAddress = 0x613DE0u;
    static constexpr uintptr_t kQohConfigImageAddress = 0x615860u;
    static constexpr size_t kQohConfigImageSize = 0x1F6Cu;
    static constexpr uintptr_t kQohConfigRecordBase = 0x615864u;
    static constexpr size_t kQohConfigRecordStride = 0x2FCu;
    static constexpr size_t kQohConfigRecordCount = 5u;
    // Vanilla message type 6 syncs these two frontend-persistence blocks host -> client; neither block nor the 0xD34-byte gap is part of gameplay hashes.
    static constexpr uintptr_t kQohConfigSharedHeaderAddress = 0x616750u;
    static constexpr size_t kQohConfigSharedHeaderSize = 0x14u;
    static constexpr uintptr_t kQohConfigSharedBlockAddress = 0x617498u;
    static constexpr size_t kQohConfigSharedBlockSize = 0x334u;
    static constexpr uintptr_t kQohConfigSharedBlockTailAddress =
        kQohConfigRegionAddress + kQohConfigRegionSize;
    static constexpr size_t kQohConfigSharedBlockTailSize =
        kQohConfigSharedBlockAddress + kQohConfigSharedBlockSize
        - kQohConfigSharedBlockTailAddress;
    static constexpr size_t kQohConfigGameplayPrefixSize = 0x17Cu;
    static constexpr size_t kQohConfigBindingOffset = 0x17Cu;
    static constexpr size_t kQohNetSchedulerConfigOffsetCount = 5u;
    static constexpr size_t kQohNetSchedulerNormalizedOffsets[kQohNetSchedulerConfigOffsetCount] = {
        64u, 84u, 88u, 92u, 96u,
    };
    static constexpr size_t kQohNetNormalizedOffsets[kQohNetSchedulerConfigOffsetCount] = {
        64u, 84u, 88u, 92u, 96u,
    };
    static constexpr uintptr_t kQohConfigSnapshotAddress = 0x623520u;
    static constexpr size_t kQohConfigSnapshotRegionSize = 0x1D0u;
    static constexpr uintptr_t kQohBindingPointerAddress = 0x626860u;
    // The main match loop updates 30 effect actors at 0x618160 (sub_421EE0).
    static constexpr size_t kQohEffectActorCount = 30u;

    static constexpr bool validQohActiveConfigPointer(uint32_t pointer) {
        if (pointer < kQohConfigRecordBase)
            return false;
        const uintptr_t delta =
            static_cast<uintptr_t>(pointer) - kQohConfigRecordBase;
        return delta % kQohConfigRecordStride == 0u
            && delta / kQohConfigRecordStride < kQohConfigRecordCount;
    }
    static constexpr uint32_t expectedQohBindingPointer(
        uint32_t activeConfigPointer) {
        return validQohActiveConfigPointer(activeConfigPointer)
            ? activeConfigPointer
                + static_cast<uint32_t>(kQohConfigBindingOffset)
            : 0u;
    }
    static inline uint32_t readCanonicalU32(
        const uint8_t* bytes, size_t offset) {
        return static_cast<uint32_t>(bytes[offset])
            | (static_cast<uint32_t>(bytes[offset + 1u]) << 8u)
            | (static_cast<uint32_t>(bytes[offset + 2u]) << 16u)
            | (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
    }
    static inline void writeCanonicalU32(
        uint8_t* bytes, size_t offset, uint32_t value) {
        bytes[offset] = static_cast<uint8_t>(value);
        bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<uint8_t>(value >> 24u);
    }
    static inline bool qohMatchConfigProof(
        uintptr_t regionAddress, const uint8_t* bytes, size_t size,
        QohMatchConfigProof& proof) {
        if (!bytes)
            return false;
        size_t recordOffset = 0;
        if (regionAddress == kQohConfigRegionAddress) {
            const size_t pointerOffset =
                kQohActiveConfigPointerAddress - regionAddress;
            if (size < kQohConfigRegionSize
                || pointerOffset + sizeof(uint32_t) > size) {
                return false;
            }
            const uint32_t pointer =
                readCanonicalU32(bytes, pointerOffset);
            if (!validQohActiveConfigPointer(pointer))
                return false;
            recordOffset =
                static_cast<size_t>(pointer - regionAddress);
        } else if (regionAddress == kQohConfigSnapshotAddress) {
            if (size < kQohConfigGameplayPrefixSize)
                return false;
        } else {
            return false;
        }
        if (recordOffset + 52u > size)
            return false;
        proof = QohMatchConfigProof{};
        proof.timerMode = readCanonicalU32(bytes, recordOffset + 36u);
        proof.aiDifficulty = readCanonicalU32(bytes, recordOffset + 40u);
        proof.lifeMultiplier = readCanonicalU32(
            bytes, recordOffset + 44u);
        proof.roundsToWinMinusOne = readCanonicalU32(
            bytes, recordOffset + 48u);
        return true;
    }
    static inline bool qohNetSchedulerConfigValid(
        const uint8_t* record, size_t size) {
        if (!record)
            return false;
        for (const size_t offset : kQohNetSchedulerNormalizedOffsets) {
            if (offset + sizeof(uint32_t) > size
                || readCanonicalU32(record, offset) != 0u) {
                return false;
            }
        }
        return true;
    }

    static inline bool qohNetNormalizedConfigValid(
        const uint8_t* record, size_t size) {
        return qohNetSchedulerConfigValid(record, size);
    }

    enum class LiveValidationPolicy : uint8_t {
        NetplayDeterministic,
        LocalTrainingSnapshot,
        RawStructuralOnly,
    };
    static inline bool canonicalizeQohPeerLocalConfigForHash(
        uintptr_t regionAddress, uint8_t* bytes, size_t size) {
        QohMatchConfigProof proof{};
        if (!qohMatchConfigProof(
                regionAddress, bytes, size, proof)) {
            return false;
        }
        size_t localOffset = 0;
        size_t localSize = 0;
        if (regionAddress == kQohConfigRegionAddress) {
            localOffset =
                static_cast<size_t>(
                    kQohConfigImageAddress - regionAddress);
            localSize = size - localOffset;
            writeCanonicalU32(
                bytes,
                static_cast<size_t>(
                    kQohActiveConfigPointerAddress - regionAddress),
                0xF1F1F1F1u);
        } else {
            localOffset = 0;
            localSize = kQohConfigGameplayPrefixSize;
        }
        for (size_t i = 0; i < localSize; ++i)
            bytes[localOffset + i] = 0u;
        writeCanonicalU32(bytes, localOffset + 0u, proof.domain);
        writeCanonicalU32(bytes, localOffset + 4u, proof.timerMode);
        writeCanonicalU32(bytes, localOffset + 8u, proof.aiDifficulty);
        writeCanonicalU32(
            bytes, localOffset + 12u, proof.lifeMultiplier);
        writeCanonicalU32(
            bytes, localOffset + 16u, proof.roundsToWinMinusOne);
        return true;
    }

    struct ScriptRange {
        uint32_t base = 0;
        uint32_t size = 0;
        uint32_t bank = 0;
    };

    static constexpr bool validPresenceMarker(uint32_t marker) {
        return marker == 0u || marker == 1u;
    }
    static constexpr bool checkedSizeAdd(
        size_t left, size_t right, size_t& result) {
        if (right > (std::numeric_limits<size_t>::max)() - left)
            return false;
        result = left + right;
        return true;
    }
    static constexpr bool checkedSizeMultiply(
        size_t left, size_t right, size_t& result) {
        if (left != 0u
            && right > (std::numeric_limits<size_t>::max)() / left)
            return false;
        result = left * right;
        return true;
    }
    static constexpr bool checkedAddressEnd(
        uintptr_t start, size_t bytes, uintptr_t& endExclusive) {
        if (start == 0u || bytes == 0u)
            return false;
        const uintptr_t remaining =
            (std::numeric_limits<uintptr_t>::max)() - start;
        if (bytes > static_cast<size_t>(remaining))
            return false;
        endExclusive = start + static_cast<uintptr_t>(bytes);
        return endExclusive > start;
    }
    static constexpr size_t maxSnapshotPoolBytes() {
        return size_t{512u} * 1024u * 1024u;
    }
    static constexpr bool validAiVmDepth(uint32_t depth) {
        // The stack has exactly 65,536 DWORD slots and depth is an inclusive index, so 0x10000 is one past the allocation.
        return depth < 0x10000u;
    }
    static constexpr bool validAiVmCursor(uint32_t cursor,
                                          uint32_t scriptBytes) {
        // Each dispatch reads a WORD and the allocation includes two zero sentinels, so the last legal cursor is two bytes before the end.
        return scriptBytes >= 2u
            && (cursor & 1u) == 0u
            && cursor <= scriptBytes - 2u;
    }
    static inline uint32_t canonicalActorInteriorPointer(
        uint32_t raw, uint32_t base, uint32_t count, uint32_t stride) {
        if (raw == 0) return 0;
        const uint64_t extent =
            static_cast<uint64_t>(count) * stride;
        if (base != 0 && stride != 0 && raw >= base
            && static_cast<uint64_t>(raw - base) < extent
            && raw - base < 0x01000000u) {
            return 0xA5000000u | (raw - base);
        }
        return raw;
    }
    static inline bool canonicalScriptPointer(
        uint32_t raw, const ScriptRange* ranges, size_t count,
        uint32_t& bank, uint32_t& offset) {
        if (!ranges) return false;
        for (size_t i = 0; i < count; ++i) {
            if (raw >= ranges[i].base
                && static_cast<uint64_t>(raw - ranges[i].base)
                    < ranges[i].size) {
                bank = ranges[i].bank;
                offset = raw - ranges[i].base;
                return true;
            }
        }
        return false;
    }
    static inline bool canonicalScriptPointerForBank(
        uint32_t raw, const ScriptRange* ranges, size_t count,
        uint32_t selectedBank, uint32_t& offset) {
        if (!ranges) return false;
        for (size_t i = 0; i < count; ++i) {
            if (ranges[i].bank != selectedBank) continue;
            if (raw >= ranges[i].base
                && static_cast<uint64_t>(raw - ranges[i].base)
                    < ranges[i].size) {
                offset = raw - ranges[i].base;
                return true;
            }
        }
        return false;
    }
    static inline bool validAiVmSelection(
        uint32_t depth, uint32_t selectedBank, uint32_t selectedBase,
        uint32_t cursor, const ScriptRange* ranges, size_t count) {
        if (!validAiVmDepth(depth) || selectedBank >= 20u)
            return false;
        if (selectedBase == 0u) {
            return depth == 0u && selectedBank == 0u && cursor == 0u;
        }
        if (!ranges) return false;
        for (size_t i = 0; i < count; ++i) {
            if (ranges[i].bank == selectedBank
                && ranges[i].base == selectedBase) {
                return validAiVmCursor(cursor, ranges[i].size);
            }
        }
        return false;
    }
    static inline uint64_t descriptorIdentityFingerprint(
        uint32_t family, uint32_t count, uint32_t byteCount,
        uint64_t contentHash) {
        uint64_t hash = 1469598103934665603ull;
        const uint64_t domain = 0x514F483939444553ull; // "QOH99DES"
        const uint8_t* fields[] = {
            reinterpret_cast<const uint8_t*>(&domain),
            reinterpret_cast<const uint8_t*>(&family),
            reinterpret_cast<const uint8_t*>(&count),
            reinterpret_cast<const uint8_t*>(&byteCount),
            reinterpret_cast<const uint8_t*>(&contentHash),
        };
        const size_t sizes[] = {
            sizeof(domain), sizeof(family), sizeof(count),
            sizeof(byteCount), sizeof(contentHash),
        };
        for (size_t field = 0; field < 5; ++field) {
            for (size_t i = 0; i < sizes[field]; ++i) {
                hash ^= fields[field][i];
                hash *= 1099511628211ull;
            }
        }
        return hash;
    }

    bool init(size_t numStates);

    size_t totalSize() const { return _totalSize; }
    bool   ready()     const { return _pool != nullptr; }

    bool saveToSlot(size_t slot);
    bool loadFromSlot(size_t slot);
    bool copySlotTo(size_t slot, uint8_t* out) const;
private:
    bool loadFromInternal(const uint8_t* src, bool foreign);
public:

    bool loadFrom(const uint8_t* src);
    bool loadFromForeign(const uint8_t* src);

    bool snapshotTo(uint8_t* dst) const;

    struct SlotTag {
        bool     valid = false;
        uint32_t layoutGeneration = 0;
        uint32_t epoch = 0;
        uint32_t frame = 0;
        uint32_t phase = 0;
    };
    bool saveTaggedToSlot(size_t slot, uint32_t epoch, uint32_t frame,
                          uint32_t phase);
    bool loadTaggedFromSlot(size_t slot, uint32_t epoch, uint32_t frame,
                            uint32_t phase);
    bool hasTaggedSlot(size_t slot, uint32_t epoch, uint32_t frame,
                       uint32_t phase) const;
    const SlotTag* slotTag(size_t slot) const;
    bool invalidateTaggedSlot(size_t slot, uint32_t epoch, uint32_t frame);
    void invalidateSlots();
    uint32_t layoutGeneration() const { return _layoutGeneration; }

    struct Diff { const char* region; size_t offsetInRegion; size_t diffBytes; };
    enum class Profile { Gameplay, Render, All };
    bool compareWith(const uint8_t* snap, Diff* out, Profile profile = Profile::All) const;

    // The bulk-hash primitive is baked into recorded .q99r proofs and exchanged hashes: Fnv1a64 must stay reachable for playback of older files.
    enum class HashAlgorithm : uint8_t { Fnv1a64 = 1, Block64 = 2 };
    static constexpr HashAlgorithm kHashAlgorithmCurrent =
        HashAlgorithm::Block64;

    void setHashAlgorithm(HashAlgorithm algorithm) {
        _hashAlgorithm = algorithm;
    }
    HashAlgorithm hashAlgorithm() const { return _hashAlgorithm; }

    void setMatchRulesOverridden(bool overridden) {
        _matchRulesOverridden = overridden;
    }
    bool matchRulesOverridden() const { return _matchRulesOverridden; }

    struct RegionHash { const char* name; uint64_t hash; size_t size; bool present; };
    uint64_t hashLive(std::vector<RegionHash>* perRegion,
                      Profile profile = Profile::All) const;
    uint64_t hashSlot(
        size_t slot, Profile profile = Profile::Gameplay,
        std::vector<RegionHash>* perRegion = nullptr) const;

    bool validateLive(const char** why) const;
    bool validateLive(const char** why, LiveValidationPolicy policy) const;

    size_t fixedRegionCount() const { return _regions.size(); }
    size_t ptrRegionCount()   const { return _ptrRegions.size(); }

    void addExtraRegion(uintptr_t addr, size_t size, const char* name, RegionRole role);

    void addExtraPtrRegion(uintptr_t fighterBase, size_t ptrOffset, size_t size,
                           const char* name, RegionRole role);

    void addExternalOwnerPtrRegion(uintptr_t ownerAddress, size_t size,
                                   const char* name, RegionRole role,
                                   uint8_t scriptBank);

    bool setImmutableAssetFingerprint(uint64_t fingerprint) {
        if (_pool && fingerprint != _immutableAssetFingerprint)
            return false;
        _immutableAssetFingerprint = fingerprint;
        return true;
    }
    uint64_t immutableAssetFingerprint() const {
        return _immutableAssetFingerprint;
    }

    void clearExtraPtrRegions();

    bool verifyAgainstSlot(size_t slot, Diff* out, Profile profile = Profile::All) const;

    using RangeCb = void(*)(const char* region, size_t offsetInRegion,
                            uintptr_t va, size_t len, void* user);
    size_t diffBuffers(const uint8_t* a, const uint8_t* b, RangeCb cb, void* user) const;

    const std::vector<Region>&    fixedRegions()        const { return _regions; }
    const std::vector<PtrRegion>& pointerChildRegions() const { return _ptrRegions; }
    const uint8_t* slotData(size_t slot) const {
        size_t offset = 0;
        return poolSlotOffset(slot, offset) ? _pool.get() + offset : nullptr;
    }
    bool bufferOffsetOf(const char* name, size_t* off, bool* isPtrChild) const;

private:
    size_t                     _totalSize = 0;
    size_t                     _numStates = 0;
    size_t                     _poolSize = 0;
    std::unique_ptr<uint8_t[]> _pool;
    std::vector<SlotTag>       _slotTags;
    uint32_t                   _layoutGeneration = 0;
    std::vector<Region>        _regions;
    struct AliasedPtrRegion {
        PtrRegion region;
        uintptr_t sharedAddress = 0;
        const char* coversName = nullptr;
    };
    std::vector<AliasedPtrRegion> _ptrAliases;
    std::vector<PtrRegion>     _ptrRegions;
    std::vector<uint8_t>       _ptrExpectedPresent;
    std::vector<uintptr_t>     _ptrExpectedAddress;
    std::vector<std::vector<size_t>> _fixedCanonicalPointerOffsets;
    std::vector<std::vector<size_t>> _ptrCanonicalPointerOffsets;
    struct RelativePointer {
        size_t pointerOffset = 0;
        size_t baseOffset = 0;
        size_t countOffset = 0;
        size_t activeMarkerOffset = 0;
        uint32_t elementSize = 0;
    };
    struct DescriptorPointer {
        size_t pointerOffset = 0;
        size_t activeMarkerOffset = 0;
        uint32_t family = 0;
    };
    struct DescriptorIdentity {
        uint32_t rawPointer = 0;
        uint32_t family = 0;
        uint32_t count = 0;
        uint32_t byteCount = 0;
        uint64_t contentHash = 0;
    };
    std::vector<std::vector<RelativePointer>>
        _fixedCanonicalRelativePointers;
    std::vector<std::vector<DescriptorPointer>>
        _fixedCanonicalDescriptorPointers;
    std::vector<DescriptorIdentity> _descriptorIdentities;
    std::vector<int>           _ptrScriptBanks;
    std::vector<ScriptRange>   _scriptRanges;
    std::vector<Region>        _extraRegions;
    std::vector<PtrRegion>     _extraPtrRegions;
    std::vector<int>           _extraPtrScriptBanks;
    uint64_t                   _immutableAssetFingerprint = 0;
    HashAlgorithm              _hashAlgorithm = kHashAlgorithmCurrent;
    bool                       _matchRulesOverridden = false;
    bool poolSlotOffset(size_t slot, size_t& offset) const;
    bool snapshotFixedU32(const uint8_t* snapshot, uintptr_t address,
                          uint32_t& value) const;
    bool resolveRegions();
    bool buildCanonicalPointerMaps();
    bool validateAiVm(const uint8_t* bytes) const;
    void normalizeAiVmInactive(uint8_t* bytes) const;
    uint64_t hashAiVmCanonical(const uint8_t* bytes) const;
    uint64_t hashScriptValue(uint32_t value, uint32_t selectedBank,
                             uint64_t hash) const;
};

}
