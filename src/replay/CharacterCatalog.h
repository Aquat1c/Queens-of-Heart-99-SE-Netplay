/* IDs 34..36 are internal partner/form actors, never catalog entries; selectable Kanako 24 and partner Kanako 35 must not be conflated. */
#pragma once

#include "ReplayTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace qreplay {

struct CharacterEntry {
    uint8_t id = kInactiveCharacterId;
    const char* canonicalLabel = "";
    const char* rawToken = "";
    bool hiddenColumn = false;
    bool standAlternate = false;
    uint8_t baseId = kInactiveCharacterId;
};

struct CharacterAlias {
    uint8_t id = kInactiveCharacterId;
    const char* alias = "";
};

constexpr uint8_t kFirstInternalFormId = 34u;
constexpr uint8_t kLastInternalFormId = 36u;

constexpr uint8_t kNoCharacterId = 0xFFu;

class CharacterCatalog {
public:
    static const CharacterCatalog& forSeBuild();

    // catalogId: FNV-1a/64 over domain string, 0x1F, cell count, grid bytes, 0x1F, entry count, then per entry:
    // id, baseId, flags(bit0 hiddenColumn, bit1 standAlternate), canonicalLabel, 0x1F, rawToken, 0x1F. Aliases are not hashed.
    uint64_t catalogId() const { return catalogId_; }

    uint8_t baseIdForCell(uint8_t cell) const;

    uint8_t cellForBaseId(uint8_t baseId) const;

    uint8_t cellCount() const { return gridCells_; }

    uint8_t resolveAlternate(uint8_t baseId, bool alternateFlag) const;

    bool inverseAlternate(uint8_t resolvedId,
                          uint8_t& baseId,
                          bool& alternateFlag) const;

    const CharacterEntry* find(uint8_t resolvedId) const;

    std::string displayName(uint8_t resolvedId) const;

    bool isSelectable(uint8_t resolvedId) const;

    bool isInternalFormId(uint8_t id) const;

    bool verifySelectorProof(const SelectorProofV1& proof,
                             uint8_t resolvedId) const;
    bool verifySelectorProof(const SelectorProofV2& proof,
                             uint8_t resolvedId) const;

    bool matchesSearch(uint8_t resolvedId, const std::string& lowerQuery) const;

    size_t entryCount() const { return entryCount_; }
    const CharacterEntry& entryAt(size_t index) const;

private:
    CharacterCatalog(const uint8_t* grid,
                     uint8_t gridCells,
                     const CharacterEntry* entries,
                     size_t entryCount,
                     const CharacterAlias* aliases,
                     size_t aliasCount);

    const uint8_t* grid_;
    const CharacterEntry* entries_;
    const CharacterAlias* aliases_;
    size_t entryCount_;
    size_t aliasCount_;
    uint8_t gridCells_;
    uint64_t catalogId_;
};

}
