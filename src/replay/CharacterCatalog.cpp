#include "CharacterCatalog.h"

#include <cstring>

namespace qreplay {
namespace {

constexpr uint8_t kSeGrid[kSelectorCells] = {
    0u,  1u,  2u,  3u,  4u,  5u,  6u,  30u,
    7u,  8u,  9u,  10u, 11u, 12u, 13u, 32u,
    14u, 15u, 16u, 17u, 18u, 19u, 20u, 33u,
};

constexpr CharacterEntry kSeEntries[] = {
    {  0u, "Saori",                "Saori",      false, false,  0u },
    {  1u, "Mizuho",               "Mizuho",     false, false,  1u },
    {  2u, "Ruriko",               "Ruriko",     false, false,  2u },
    {  3u, "Hatsune",              "Hatune",     false, false,  3u },
    {  4u, "Kaede",                "Kaede",      false, false,  4u },
    {  5u, "Azusa",                "Azusa",      false, false,  5u },
    {  6u, "Chizuru",              "Chizuru",    false, false,  6u },
    {  7u, "Mizuki",               "Mizuki",     false, false,  7u },
    {  8u, "Akari",                "Akari",      false, false,  8u },
    {  9u, "Serika",               "Serika",     false, false,  9u },
    { 10u, "Tomoko",               "Tomoko",     false, false, 10u },
    { 11u, "Shiho",                "Siho",       false, false, 11u },
    { 12u, "Aoi",                  "Aoi",        false, false, 12u },
    { 13u, "Yuki",                 "Yuki",       false, false, 13u },
    { 14u, "Serio",                "Serio",      false, false, 14u },
    { 15u, "Multi",                "Multi",      false, false, 15u },
    { 16u, "Kotone",               "Kotone",     false, false, 16u },
    { 17u, "Lemmy",                "Remy",       false, false, 17u },
    { 18u, "Rio",                  "Rio",        false, false, 18u },
    { 19u, "Ayaka",                "Ayaka",      false, false, 19u },
    { 20u, "Rina",                 "rina",       false, false, 20u },
    // Stand/D alternates.  Base -> result: 8->21, 15->22, 6->23, 1->24,
    // 9->25, 11->26, 16->27, 12->28, 3->29, 19->31.
    { 21u, "Young Akari",          "o_akari",    false, true,   8u },
    { 22u, "HM-12",                "m_multi",    false, true,  15u },
    { 23u, "Demon Chizuru",        "o_chizuru",  false, true,   6u },
    { 24u, "Kanako",               "kanako",     false, true,   1u },
    { 25u, "Komode Serika",        "c_serika",   false, true,   9u },
    { 26u, "Arrange Shiho",        "s_siho",     false, true,  11u },
    { 27u, "True Heroine Kotone",  "s_kotone",   false, true,  16u },
    { 28u, "LF'97 Aoi",            "LF97_aoi",   false, true,  12u },
    { 29u, "Badness Hatsune",      "H_Hatune",   false, true,   3u },
    { 30u, "Tiria",                "Tiria",      true,  false, 30u },
    { 31u, "Rapid Ayaka",          "R_Ayaka",    false, true,  19u },
    { 32u, "Corin",                "Corin",      true,  false, 32u },
    { 33u, "Yoshie",               "Yosie",      true,  false, 33u },
};

constexpr size_t kSeEntryCount = sizeof(kSeEntries) / sizeof(kSeEntries[0]);
static_assert(kSeEntryCount == kMaxSelectableCharacterId + 1u,
              "SE catalog must hold exactly the 34 selectable identities");

constexpr CharacterAlias kSeAliases[] = {
    {  3u, "Hatune"   },
    {  6u, "Chitzuru" },
    { 11u, "Siho"     },
    { 17u, "Remy"     },
    { 33u, "Yosie"    },
    { 30u, "Thiria"   },
    { 25u, "Comede"   },
    { 25u, "Comode"   },
    { 15u, "HMX-12"   },
};

constexpr size_t kSeAliasCount = sizeof(kSeAliases) / sizeof(kSeAliases[0]);

constexpr char kCatalogDomain[] = "qoh99.se.character-catalog.v1";

constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint8_t kFieldSeparator = 0x1Fu;

uint64_t fnvBytes(uint64_t hash, const void* data, size_t length) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

uint64_t fnvByte(uint64_t hash, uint8_t value) {
    return fnvBytes(hash, &value, 1u);
}

uint64_t fnvText(uint64_t hash, const char* text) {
    if (text == nullptr) return fnvByte(hash, 0u);
    return fnvBytes(hash, text, std::strlen(text));
}

uint64_t computeCatalogId(const uint8_t* grid,
                          uint8_t gridCells,
                          const CharacterEntry* entries,
                          size_t entryCount) {
    uint64_t hash = kFnvOffsetBasis;
    hash = fnvBytes(hash, kCatalogDomain, std::strlen(kCatalogDomain));
    hash = fnvByte(hash, kFieldSeparator);

    hash = fnvByte(hash, gridCells);
    hash = fnvBytes(hash, grid, gridCells);
    hash = fnvByte(hash, kFieldSeparator);

    hash = fnvByte(hash, static_cast<uint8_t>(entryCount));
    for (size_t i = 0; i < entryCount; ++i) {
        const CharacterEntry& entry = entries[i];
        const uint8_t flags = static_cast<uint8_t>(
            (entry.hiddenColumn ? 0x01u : 0x00u) |
            (entry.standAlternate ? 0x02u : 0x00u));
        hash = fnvByte(hash, entry.id);
        hash = fnvByte(hash, entry.baseId);
        hash = fnvByte(hash, flags);
        hash = fnvText(hash, entry.canonicalLabel);
        hash = fnvByte(hash, kFieldSeparator);
        hash = fnvText(hash, entry.rawToken);
        hash = fnvByte(hash, kFieldSeparator);
    }
    return hash;
}

char foldAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool containsFolded(const char* haystack, const std::string& needle) {
    if (haystack == nullptr) return false;
    if (needle.empty()) return true;
    const size_t hayLength = std::strlen(haystack);
    if (needle.size() > hayLength) return false;
    for (size_t start = 0; start + needle.size() <= hayLength; ++start) {
        size_t i = 0;
        for (; i < needle.size(); ++i) {
            if (foldAscii(haystack[start + i]) != needle[i]) break;
        }
        if (i == needle.size()) return true;
    }
    return false;
}

const CharacterEntry& placeholderEntry() {
    static const CharacterEntry placeholder{
        kInactiveCharacterId, "", "", false, false, kInactiveCharacterId };
    return placeholder;
}

}

CharacterCatalog::CharacterCatalog(const uint8_t* grid,
                                   uint8_t gridCells,
                                   const CharacterEntry* entries,
                                   size_t entryCount,
                                   const CharacterAlias* aliases,
                                   size_t aliasCount)
    : grid_(grid),
      entries_(entries),
      aliases_(aliases),
      entryCount_(entryCount),
      aliasCount_(aliasCount),
      gridCells_(gridCells),
      catalogId_(computeCatalogId(grid, gridCells, entries, entryCount)) {}

const CharacterCatalog& CharacterCatalog::forSeBuild() {
    static const CharacterCatalog catalog(kSeGrid,
                                          kSelectorCells,
                                          kSeEntries,
                                          kSeEntryCount,
                                          kSeAliases,
                                          kSeAliasCount);
    return catalog;
}

uint8_t CharacterCatalog::baseIdForCell(uint8_t cell) const {
    if (cell >= gridCells_) return kNoCharacterId;
    return grid_[cell];
}

uint8_t CharacterCatalog::cellForBaseId(uint8_t baseId) const {
    for (uint8_t cell = 0u; cell < gridCells_; ++cell) {
        if (grid_[cell] == baseId) return cell;
    }
    return kNoCharacterId;
}

uint8_t CharacterCatalog::resolveAlternate(uint8_t baseId,
                                           bool alternateFlag) const {
    if (!alternateFlag) return baseId;
    for (size_t i = 0; i < entryCount_; ++i) {
        const CharacterEntry& entry = entries_[i];
        if (entry.standAlternate && entry.baseId == baseId) return entry.id;
    }
    return baseId;
}

bool CharacterCatalog::inverseAlternate(uint8_t resolvedId,
                                        uint8_t& baseId,
                                        bool& alternateFlag) const {
    const CharacterEntry* entry = find(resolvedId);
    if (entry == nullptr) return false;
    if (entry->standAlternate) {
        baseId = entry->baseId;
        alternateFlag = true;
    } else {
        baseId = entry->id;
        alternateFlag = false;
    }
    return true;
}

const CharacterEntry* CharacterCatalog::find(uint8_t resolvedId) const {
    for (size_t i = 0; i < entryCount_; ++i) {
        if (entries_[i].id == resolvedId) return &entries_[i];
    }
    return nullptr;
}

std::string CharacterCatalog::displayName(uint8_t resolvedId) const {
    const CharacterEntry* entry = find(resolvedId);
    if (entry != nullptr) return std::string(entry->canonicalLabel);
    return "Unknown character #" +
           std::to_string(static_cast<unsigned>(resolvedId));
}

bool CharacterCatalog::isSelectable(uint8_t resolvedId) const {
    if (resolvedId > kMaxSelectableCharacterId) return false;
    return find(resolvedId) != nullptr;
}

bool CharacterCatalog::isInternalFormId(uint8_t id) const {
    return id >= kFirstInternalFormId && id <= kLastInternalFormId;
}

bool CharacterCatalog::verifySelectorProof(const SelectorProofV1& proof,
                                            uint8_t resolvedId) const {
    if (proof.alternateFlag > 1u) return false;
    const uint8_t baseId = baseIdForCell(proof.cursorCell);
    if (baseId == kNoCharacterId) return false;
    return resolveAlternate(baseId, proof.alternateFlag != 0u) == resolvedId;
}

bool CharacterCatalog::verifySelectorProof(const SelectorProofV2& proof,
                                            uint8_t resolvedId) const {
    if (proof.cursorCell >= gridCells_ || proof.alternateFlag > 1u
        || proof.confirmState > 4u || !isSelectable(resolvedId)) {
        return false;
    }
    const uint8_t base = baseIdForCell(proof.cursorCell);
    if (base == kNoCharacterId) return false;
    return resolveAlternate(base, proof.alternateFlag != 0u) == resolvedId;
}

bool CharacterCatalog::matchesSearch(uint8_t resolvedId,
                                     const std::string& lowerQuery) const {
    const CharacterEntry* entry = find(resolvedId);
    if (entry == nullptr) return false;

    std::string needle;
    needle.reserve(lowerQuery.size());
    for (char c : lowerQuery) needle.push_back(foldAscii(c));
    if (needle.empty()) return true;

    if (containsFolded(entry->canonicalLabel, needle)) return true;
    if (containsFolded(entry->rawToken, needle)) return true;
    for (size_t i = 0; i < aliasCount_; ++i) {
        if (aliases_[i].id != resolvedId) continue;
        if (containsFolded(aliases_[i].alias, needle)) return true;
    }
    return false;
}

const CharacterEntry& CharacterCatalog::entryAt(size_t index) const {
    if (index >= entryCount_) return placeholderEntry();
    return entries_[index];
}

}
