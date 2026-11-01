#include "TestHarness.h"

#include "replay/CharacterCatalog.h"

#include <string>

using namespace qreplay;

namespace {

constexpr uint8_t kExpectedGrid[kSelectorCells] = {
    0u,  1u,  2u,  3u,  4u,  5u,  6u,  30u,
    7u,  8u,  9u,  10u, 11u, 12u, 13u, 32u,
    14u, 15u, 16u, 17u, 18u, 19u, 20u, 33u,
};

struct AlternatePair {
    uint8_t baseId;
    uint8_t resolvedId;
};

constexpr AlternatePair kExpectedAlternates[] = {
    {  1u, 24u },
    {  3u, 29u },
    {  6u, 23u },
    {  8u, 21u },
    {  9u, 25u },
    { 11u, 26u },
    { 12u, 28u },
    { 15u, 22u },
    { 16u, 27u },
    { 19u, 31u },
};

constexpr size_t kExpectedAlternateCount =
    sizeof(kExpectedAlternates) / sizeof(kExpectedAlternates[0]);

bool baseHasAlternate(uint8_t baseId) {
    for (size_t i = 0; i < kExpectedAlternateCount; ++i) {
        if (kExpectedAlternates[i].baseId == baseId) return true;
    }
    return false;
}

SelectorProofV1 proofOf(uint8_t cursorCell, uint8_t alternateFlag) {
    SelectorProofV1 proof{};
    proof.cursorCell = cursorCell;
    proof.alternateFlag = alternateFlag;
    proof.confirmState = 4u;
    return proof;
}

size_t searchHitCount(const CharacterCatalog& catalog, const char* query) {
    size_t hits = 0;
    for (uint8_t id = 0u; id <= kMaxSelectableCharacterId; ++id) {
        if (catalog.matchesSearch(id, query)) ++hits;
    }
    return hits;
}

}

QNP_TEST(CharacterCatalogGridMatchesAuditedSeTable) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();
    QNP_REQUIRE(catalog.cellCount() == kSelectorCells);

    for (uint8_t cell = 0u; cell < kSelectorCells; ++cell) {
        QNP_CHECK(catalog.baseIdForCell(cell) == kExpectedGrid[cell]);
        QNP_CHECK(catalog.cellForBaseId(kExpectedGrid[cell]) == cell);
    }

    QNP_CHECK(catalog.baseIdForCell(kSelectorCells) == kNoCharacterId);
    QNP_CHECK(catalog.baseIdForCell(200u) == kNoCharacterId);
    QNP_CHECK(catalog.baseIdForCell(kInactiveCharacterId) == kNoCharacterId);

    QNP_CHECK(catalog.cellForBaseId(24u) == kNoCharacterId);
    QNP_CHECK(catalog.cellForBaseId(29u) == kNoCharacterId);
    QNP_CHECK(catalog.cellForBaseId(35u) == kNoCharacterId);
}

QNP_TEST(CharacterCatalogForwardStandAlternates) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();

    for (size_t i = 0; i < kExpectedAlternateCount; ++i) {
        const uint8_t base = kExpectedAlternates[i].baseId;
        const uint8_t alternate = kExpectedAlternates[i].resolvedId;
        QNP_CHECK(catalog.resolveAlternate(base, true) == alternate);
        QNP_CHECK(catalog.resolveAlternate(base, false) == base);

        const CharacterEntry* entry = catalog.find(alternate);
        QNP_REQUIRE(entry != nullptr);
        QNP_CHECK(entry->standAlternate);
        QNP_CHECK(entry->baseId == base);
        QNP_CHECK(!entry->hiddenColumn);

        QNP_CHECK(catalog.resolveAlternate(alternate, true) == alternate);
    }

    size_t standCount = 0;
    for (size_t i = 0; i < catalog.entryCount(); ++i) {
        if (catalog.entryAt(i).standAlternate) ++standCount;
    }
    QNP_CHECK(standCount == kExpectedAlternateCount);
}

QNP_TEST(CharacterCatalogInverseStandAlternates) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();

    for (size_t i = 0; i < kExpectedAlternateCount; ++i) {
        const uint8_t base = kExpectedAlternates[i].baseId;
        const uint8_t alternate = kExpectedAlternates[i].resolvedId;

        uint8_t inverseBase = kInactiveCharacterId;
        bool flag = false;
        QNP_REQUIRE(catalog.inverseAlternate(alternate, inverseBase, flag));
        QNP_CHECK(inverseBase == base);
        QNP_CHECK(flag);

        uint8_t baseInverse = kInactiveCharacterId;
        bool baseFlag = true;
        QNP_REQUIRE(catalog.inverseAlternate(base, baseInverse, baseFlag));
        QNP_CHECK(baseInverse == base);
        QNP_CHECK(!baseFlag);
    }

    uint8_t unusedBase = 7u;
    bool unusedFlag = true;
    QNP_CHECK(!catalog.inverseAlternate(35u, unusedBase, unusedFlag));
    QNP_CHECK(!catalog.inverseAlternate(200u, unusedBase, unusedFlag));
    QNP_CHECK(!catalog.inverseAlternate(kInactiveCharacterId, unusedBase,
                                        unusedFlag));
}

QNP_TEST(CharacterCatalogOrdinaryCellsIgnoreStand) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();

    for (uint8_t cell = 0u; cell < kSelectorCells; ++cell) {
        const uint8_t base = catalog.baseIdForCell(cell);
        QNP_REQUIRE(base != kNoCharacterId);
        if (baseHasAlternate(base)) continue;
        QNP_CHECK(catalog.resolveAlternate(base, true) == base);
        QNP_CHECK(catalog.resolveAlternate(base, false) == base);
    }

    QNP_CHECK(catalog.resolveAlternate(30u, true) == 30u);
    QNP_CHECK(catalog.resolveAlternate(32u, true) == 32u);
    QNP_CHECK(catalog.resolveAlternate(33u, true) == 33u);
}

QNP_TEST(CharacterCatalogRandomInverseMappingIsConsistent) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();

    for (uint8_t id = 0u; id <= kMaxSelectableCharacterId; ++id) {
        QNP_REQUIRE(catalog.isSelectable(id));

        uint8_t base = kInactiveCharacterId;
        bool flag = false;
        QNP_REQUIRE(catalog.inverseAlternate(id, base, flag));

        const uint8_t cell = catalog.cellForBaseId(base);
        QNP_REQUIRE(cell != kNoCharacterId);
        QNP_CHECK(cell < kSelectorCells);
        QNP_CHECK(catalog.baseIdForCell(cell) == base);
        QNP_CHECK(catalog.resolveAlternate(base, flag) == id);

        QNP_CHECK(catalog.verifySelectorProof(
            proofOf(cell, flag ? 1u : 0u), id));
    }
}

QNP_TEST(CharacterCatalogHiddenColumnCells) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();

    QNP_CHECK(catalog.baseIdForCell(7u) == 30u);
    QNP_CHECK(catalog.baseIdForCell(15u) == 32u);
    QNP_CHECK(catalog.baseIdForCell(23u) == 33u);

    const uint8_t hidden[] = { 30u, 32u, 33u };
    for (uint8_t id : hidden) {
        const CharacterEntry* entry = catalog.find(id);
        QNP_REQUIRE(entry != nullptr);
        QNP_CHECK(entry->hiddenColumn);
        QNP_CHECK(!entry->standAlternate);
        QNP_CHECK(entry->baseId == id);
    }
    QNP_CHECK(catalog.displayName(30u) == "Tiria");
    QNP_CHECK(catalog.displayName(32u) == "Corin");
    QNP_CHECK(catalog.displayName(33u) == "Yoshie");

    size_t hiddenCount = 0;
    for (size_t i = 0; i < catalog.entryCount(); ++i) {
        if (catalog.entryAt(i).hiddenColumn) ++hiddenCount;
    }
    QNP_CHECK(hiddenCount == 3u);
}

QNP_TEST(CharacterCatalogSelectableKanakoIsNotPartnerKanako) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();

    const CharacterEntry* selectable = catalog.find(24u);
    QNP_REQUIRE(selectable != nullptr);
    QNP_CHECK(std::string(selectable->canonicalLabel) == "Kanako");
    QNP_CHECK(std::string(selectable->rawToken) == "kanako");
    QNP_CHECK(selectable->standAlternate);
    QNP_CHECK(selectable->baseId == 1u);
    QNP_CHECK(catalog.isSelectable(24u));
    QNP_CHECK(!catalog.isInternalFormId(24u));

    QNP_CHECK(catalog.find(35u) == nullptr);
    QNP_CHECK(!catalog.isSelectable(35u));
    QNP_CHECK(catalog.isInternalFormId(35u));

    QNP_CHECK(catalog.isInternalFormId(34u));
    QNP_CHECK(catalog.isInternalFormId(36u));
    QNP_CHECK(!catalog.isSelectable(34u));
    QNP_CHECK(!catalog.isSelectable(36u));
    QNP_CHECK(catalog.find(34u) == nullptr);
    QNP_CHECK(catalog.find(36u) == nullptr);

    QNP_CHECK(!catalog.isInternalFormId(33u));
    QNP_CHECK(!catalog.isInternalFormId(37u));
    QNP_CHECK(!catalog.isInternalFormId(kInactiveCharacterId));

    for (uint8_t cell = 0u; cell < kSelectorCells; ++cell) {
        const uint8_t base = catalog.baseIdForCell(cell);
        QNP_CHECK(!catalog.isInternalFormId(base));
        QNP_CHECK(!catalog.isInternalFormId(catalog.resolveAlternate(base, true)));
    }
}

QNP_TEST(CharacterCatalogUnknownIdDisplayName) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();

    QNP_CHECK(catalog.displayName(200u) == "Unknown character #200");
    QNP_CHECK(catalog.find(200u) == nullptr);
    QNP_CHECK(!catalog.isSelectable(200u));
    QNP_CHECK(!catalog.matchesSearch(200u, "unknown"));

    QNP_CHECK(catalog.displayName(34u) == "Unknown character #34");
    QNP_CHECK(catalog.displayName(255u) == "Unknown character #255");
    QNP_CHECK(catalog.displayName(kInactiveCharacterId)
              == "Unknown character #255");

    QNP_CHECK(catalog.displayName(0u) == "Saori");
    QNP_CHECK(catalog.displayName(20u) == "Rina");
    QNP_CHECK(catalog.displayName(22u) == "HM-12");
    QNP_CHECK(catalog.displayName(25u) == "Komode Serika");
    QNP_CHECK(catalog.displayName(26u) == "Arrange Shiho");
    QNP_CHECK(catalog.displayName(27u) == "True Heroine Kotone");
    QNP_CHECK(catalog.displayName(28u) == "LF'97 Aoi");
}

QNP_TEST(CharacterCatalogAliasSearchKeepsIdentitiesSeparate) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();

    QNP_CHECK(catalog.matchesSearch(3u, "hatune"));
    QNP_CHECK(catalog.matchesSearch(6u, "chitzuru"));
    QNP_CHECK(catalog.matchesSearch(11u, "siho"));
    QNP_CHECK(catalog.matchesSearch(17u, "remy"));
    QNP_CHECK(catalog.matchesSearch(33u, "yosie"));
    QNP_CHECK(catalog.matchesSearch(30u, "thiria"));
    QNP_CHECK(catalog.matchesSearch(25u, "comede"));
    QNP_CHECK(catalog.matchesSearch(25u, "comode"));
    QNP_CHECK(catalog.matchesSearch(15u, "hmx-12"));

    QNP_CHECK(catalog.displayName(3u) == "Hatsune");
    QNP_CHECK(catalog.displayName(6u) == "Chizuru");
    QNP_CHECK(catalog.displayName(11u) == "Shiho");
    QNP_CHECK(catalog.displayName(17u) == "Lemmy");
    QNP_CHECK(catalog.displayName(33u) == "Yoshie");
    QNP_CHECK(catalog.displayName(30u) == "Tiria");
    QNP_CHECK(catalog.displayName(15u) == "Multi");

    QNP_CHECK(searchHitCount(catalog, "chitzuru") == 1u);
    QNP_CHECK(searchHitCount(catalog, "thiria") == 1u);
    QNP_CHECK(searchHitCount(catalog, "comode") == 1u);
    QNP_CHECK(searchHitCount(catalog, "comede") == 1u);
    QNP_CHECK(searchHitCount(catalog, "hmx-12") == 1u);
    QNP_CHECK(!catalog.matchesSearch(23u, "chitzuru"));
    QNP_CHECK(!catalog.matchesSearch(32u, "thiria"));
    QNP_CHECK(!catalog.matchesSearch(9u, "comode"));
    QNP_CHECK(!catalog.matchesSearch(22u, "hmx-12"));
    QNP_CHECK(!catalog.matchesSearch(18u, "remy"));

    QNP_CHECK(catalog.matchesSearch(11u, "shiho"));
    QNP_CHECK(catalog.matchesSearch(21u, "young akari"));
    QNP_CHECK(catalog.matchesSearch(21u, "o_akari"));
    QNP_CHECK(catalog.matchesSearch(21u, "YOUNG AKARI"));
    QNP_CHECK(catalog.matchesSearch(28u, "lf97_aoi"));
    QNP_CHECK(catalog.matchesSearch(20u, "rina"));

    QNP_CHECK(catalog.matchesSearch(0u, ""));
    QNP_CHECK(searchHitCount(catalog, "")
              == static_cast<size_t>(kMaxSelectableCharacterId) + 1u);
    QNP_CHECK(!catalog.matchesSearch(35u, "kanako"));
    QNP_CHECK(!catalog.matchesSearch(35u, ""));
}

QNP_TEST(CharacterCatalogVerifySelectorProof) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();

    QNP_CHECK(catalog.verifySelectorProof(proofOf(1u, 0u), 1u));
    QNP_CHECK(catalog.verifySelectorProof(proofOf(1u, 1u), 24u));
    QNP_CHECK(catalog.verifySelectorProof(proofOf(0u, 0u), 0u));
    QNP_CHECK(catalog.verifySelectorProof(proofOf(7u, 0u), 30u));
    QNP_CHECK(catalog.verifySelectorProof(proofOf(23u, 0u), 33u));

    QNP_CHECK(!catalog.verifySelectorProof(proofOf(2u, 1u), 24u));
    QNP_CHECK(!catalog.verifySelectorProof(proofOf(9u, 1u), 24u));

    QNP_CHECK(!catalog.verifySelectorProof(proofOf(1u, 0u), 24u));
    QNP_CHECK(!catalog.verifySelectorProof(proofOf(1u, 1u), 1u));

    QNP_CHECK(!catalog.verifySelectorProof(proofOf(0u, 0u), 5u));
    QNP_CHECK(!catalog.verifySelectorProof(proofOf(7u, 0u), 32u));

    QNP_CHECK(!catalog.verifySelectorProof(proofOf(kSelectorCells, 0u), 0u));
    QNP_CHECK(!catalog.verifySelectorProof(proofOf(200u, 0u), 0u));
    QNP_CHECK(!catalog.verifySelectorProof(proofOf(1u, 2u), 24u));
    QNP_CHECK(!catalog.verifySelectorProof(proofOf(1u, 255u), 1u));

    QNP_CHECK(!catalog.verifySelectorProof(proofOf(1u, 1u), 35u));
    QNP_CHECK(!catalog.verifySelectorProof(proofOf(0u, 0u), 200u));

    QNP_CHECK(catalog.verifySelectorProof(proofOf(7u, 1u), 30u));
    QNP_CHECK(catalog.verifySelectorProof(proofOf(0u, 1u), 0u));
}

QNP_TEST(CharacterCatalogVerifyV2SelectorProof) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();
    for (uint8_t id = 0u; id <= kMaxSelectableCharacterId; ++id) {
        uint8_t base = kNoCharacterId;
        bool alternate = false;
        QNP_REQUIRE(catalog.inverseAlternate(id, base, alternate));
        const uint8_t cell = catalog.cellForBaseId(base);
        QNP_REQUIRE(cell != kNoCharacterId);
        SelectorProofV2 proof{};
        proof.cursorCell = cell;
        proof.alternateFlag = alternate ? 1u : 0u;
        proof.confirmState = 4u;
        QNP_CHECK(catalog.verifySelectorProof(proof, id));
        proof.cursorCell = static_cast<uint8_t>(
            (cell + 1u) % kSelectorCells);
        QNP_CHECK(!catalog.verifySelectorProof(proof, id));
    }

    SelectorProofV2 invalid{};
    invalid.cursorCell = kSelectorCells;
    QNP_CHECK(!catalog.verifySelectorProof(invalid, 0u));
    invalid.cursorCell = 0u;
    invalid.alternateFlag = 2u;
    QNP_CHECK(!catalog.verifySelectorProof(invalid, 0u));
    invalid.alternateFlag = 0u;
    invalid.confirmState = 5u;
    QNP_CHECK(!catalog.verifySelectorProof(invalid, 0u));
}

QNP_TEST(CharacterCatalogIdentityIsContentDerived) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();

    QNP_CHECK(catalog.catalogId() == 0xB01A6B38033002CEull);
    QNP_CHECK(catalog.catalogId() != 0u);
    QNP_CHECK(CharacterCatalog::forSeBuild().catalogId() == catalog.catalogId());
    QNP_CHECK(&CharacterCatalog::forSeBuild() == &catalog);

    QNP_REQUIRE(catalog.entryCount()
                == static_cast<size_t>(kMaxSelectableCharacterId) + 1u);
    for (size_t i = 0; i < catalog.entryCount(); ++i) {
        const CharacterEntry& entry = catalog.entryAt(i);
        QNP_CHECK(entry.id == static_cast<uint8_t>(i));
        QNP_CHECK(entry.canonicalLabel != nullptr
                  && entry.canonicalLabel[0] != '\0');
        QNP_CHECK(entry.rawToken != nullptr && entry.rawToken[0] != '\0');
        if (entry.standAlternate) {
            QNP_CHECK(entry.baseId != entry.id);
            QNP_CHECK(catalog.cellForBaseId(entry.baseId) != kNoCharacterId);
        } else {
            QNP_CHECK(entry.baseId == entry.id);
        }
    }

    QNP_CHECK(catalog.entryAt(catalog.entryCount()).id == kInactiveCharacterId);
    QNP_CHECK(catalog.entryAt(9999u).id == kInactiveCharacterId);
}

QNP_TEST(CharacterCatalogRawTokensMatchAuditedSpellings) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();

    struct TokenCase { uint8_t id; const char* token; };
    const TokenCase cases[] = {
        {  3u, "Hatune"   }, { 11u, "Siho"      }, { 17u, "Remy"     },
        { 20u, "rina"     }, { 33u, "Yosie"     }, { 21u, "o_akari"  },
        { 22u, "m_multi"  }, { 23u, "o_chizuru" }, { 24u, "kanako"   },
        { 25u, "c_serika" }, { 26u, "s_siho"    }, { 27u, "s_kotone" },
        { 28u, "LF97_aoi" }, { 29u, "H_Hatune"  }, { 31u, "R_Ayaka"  },
        { 30u, "Tiria"    }, { 32u, "Corin"     }, {  0u, "Saori"    },
    };
    for (const TokenCase& c : cases) {
        const CharacterEntry* entry = catalog.find(c.id);
        QNP_REQUIRE(entry != nullptr);
        QNP_CHECK(std::string(entry->rawToken) == c.token);
    }
}
