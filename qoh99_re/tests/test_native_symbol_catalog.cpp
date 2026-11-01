#include "TestFramework.hpp"
#include "qoh99/generated/AddressConstants.hpp"
#include "qoh99/generated/BuildProfiles.hpp"
#include "qoh99/generated/FunctionCatalog.hpp"
#include "qoh99/generated/NativeSymbolCatalog.hpp"
#include <set>
#include <string>

using namespace qoh99;
using namespace qoh99::generated_model;

TEST(native_catalog_exact_coverage) {
    CHECK_EQ(kNativeSymbolCount, 255u);
    CHECK_EQ(kNativeSymbolCount,
             kFunctionSymbolCount + kDataSymbolCount + 106u);

    std::set<std::string> keys;
    std::set<GameAddr> novemberVas;
    for (const auto& symbol : kNativeSymbols) {
        CHECK(symbol.id != NativeSymbolId::Unknown);
        CHECK(symbol.key != nullptr && symbol.key[0] != '\0');
        CHECK(symbol.note != nullptr && symbol.note[0] != '\0');
        CHECK(keys.insert(symbol.key).second);
        CHECK(novemberVas.insert(symbol.novemberVa).second);
        CHECK(symbol.novemberVa >= kImageBaseVa);
        CHECK(symbol.novemberVa < 0x900000u);
        CHECK(findNativeSymbol(symbol.id) == &symbol);
    }
}

TEST(native_catalog_november_matches_the_runtime_seam) {
    std::size_t linked = 0;
    for (const auto& symbol : kNativeSymbols) {
        if (symbol.functionId == FunctionId::Unknown) continue;
        ++linked;
        CHECK_EQ(symbol.novemberVa,
                 resolveFunctionVa(BuildId::Se2006, symbol.functionId));
        CHECK_EQ(symbol.decemberVa,
                 resolveFunctionVa(BuildId::Game2000, symbol.functionId));
        CHECK_EQ(nativeSymbolVaForBuild(symbol, BuildId::Se2006),
                 symbol.novemberVa);
    }
    CHECK_EQ(linked, kFunctionSymbolCount);

    for (const auto& function : kFunctions) {
        const NativeSymbol* symbol = findNativeSymbolByFunctionId(function.id);
        CHECK(symbol != nullptr);
        if (symbol) CHECK_EQ(symbol->novemberVa, function.se2006Va);
    }
}

TEST(native_catalog_data_addresses_are_shared_and_unshiftable) {
    std::size_t dataRows = 0;
    for (const auto& symbol : kNativeSymbols) {
        const bool isData = symbol.kind == NativeSymbolKind::DataGlobal
            || symbol.kind == NativeSymbolKind::CodePointerStorage
            || symbol.kind == NativeSymbolKind::DataTableWord;
        if (!isData) continue;
        ++dataRows;
        CHECK(symbol.shiftClass == NativeShiftClass::DataUnchanged);
        CHECK(symbol.novemberVa >= kCodeDataBoundaryVa);
        CHECK_EQ(symbol.decemberVa, symbol.novemberVa);
        CHECK_EQ(resolveNativeSymbolVa(BuildId::Game2000, symbol.id),
                 resolveNativeSymbolVa(BuildId::Se2006, symbol.id));
    }
    CHECK(dataRows >= kDataSymbolCount);
}

TEST(native_catalog_layout_rule_is_checked_not_applied) {
    CHECK(nativeCatalogObeysLayoutRule());
    CHECK(nativeCatalogAgreesWithFunctionCatalog());

    std::size_t interior = 0;
    std::size_t edited = 0;
    for (const auto& symbol : kNativeSymbols) {
        switch (symbol.shiftClass) {
        case NativeShiftClass::IdentityLayout:
            CHECK(symbol.novemberVa <= kIdentityLayoutLastEntryVa);
            CHECK_EQ(symbol.decemberVa, symbol.novemberVa);
            break;
        case NativeShiftClass::ShiftedEntry:
            CHECK(symbol.novemberVa >= kShiftedLayoutFirstEntryNovemberVa);
            CHECK_EQ(symbol.decemberVa,
                     symbol.novemberVa + kShiftedLayoutEntryDelta);
            break;
        case NativeShiftClass::InteriorOfIdentityFunction:
            ++interior;
            CHECK(symbol.kind == NativeSymbolKind::InteriorCode);
            CHECK_EQ(symbol.decemberVa, symbol.novemberVa);
            break;
        case NativeShiftClass::SemanticallyEdited:
            ++edited;
            if (symbol.id == NativeSymbolId::EditedActorTransition) {
                CHECK(symbol.runtimeUse == NativeRuntimeUse::SeamResolved);
                CHECK(symbol.novemberPrologue.status
                      == PrologueSignatureStatus::Curated);
                CHECK(symbol.decemberPrologue.status
                      == PrologueSignatureStatus::Curated);
            } else {
                CHECK(symbol.runtimeUse == NativeRuntimeUse::NotCallable);
            }
            break;
        case NativeShiftClass::DataUnchanged:
            break;
        }
    }
    CHECK_EQ(interior, 3u);
    CHECK_EQ(edited, 3u);
}

TEST(native_catalog_carries_per_build_values) {
    const NativeSymbol* revision =
        findNativeSymbol(NativeSymbolId::DataTableWord4A71E0);
    CHECK(revision != nullptr);
    if (revision) {
        CHECK_EQ(revision->novemberVa, 0x4A71E0u);
        CHECK_EQ(revision->decemberVa, 0x4A71E0u);
        CHECK_EQ(revision->novemberValue, 5u);
        CHECK_EQ(revision->decemberValue, 6u);
    }

    std::size_t codePointers = 0;
    for (const auto& symbol : kNativeSymbols) {
        if (symbol.kind != NativeSymbolKind::CodePointerStorage) continue;
        ++codePointers;
        CHECK_EQ(symbol.decemberVa, symbol.novemberVa);
        CHECK_EQ(symbol.decemberValue - symbol.novemberValue,
                 kShiftedLayoutEntryDelta);
    }
    CHECK_EQ(codePointers, 58u);
}

TEST(native_catalog_prologue_slots_are_curated_only_where_evidence_exists) {
    const std::set<std::string> expectedCurated{
        "manualRecoveryEvaluator", "commandMatch", "fighterRecordInput",
        "throwTechEvaluator", "guardContactResolver", "hitContactResolver",
        "naturalActionTerminal", "editedActorTransition",
        "attackContactResolver", "damageCriticalScaler",
        "guardActionRequest", "faultlessButtonDetector",
        "reduceMashDetector",
    };
    std::set<std::string> observedCurated;
    for (const auto& symbol : kNativeSymbols) {
        const bool novemberCurated = symbol.novemberPrologue.status
            == PrologueSignatureStatus::Curated;
        const bool decemberCurated = symbol.decemberPrologue.status
            == PrologueSignatureStatus::Curated;
        CHECK(novemberCurated == decemberCurated);
        if (!novemberCurated) {
            CHECK(symbol.novemberPrologue.bytes == nullptr);
            CHECK(symbol.decemberPrologue.bytes == nullptr);
            CHECK_EQ(symbol.novemberPrologue.size, 0u);
            CHECK_EQ(symbol.decemberPrologue.size, 0u);
            continue;
        }
        observedCurated.insert(symbol.key);
        CHECK(symbol.kind == NativeSymbolKind::FunctionEntry
              || symbol.kind == NativeSymbolKind::InteriorCode);
        CHECK(symbol.novemberPrologue.bytes != nullptr);
        CHECK(symbol.decemberPrologue.bytes != nullptr);
        CHECK_EQ(symbol.novemberPrologue.size, kNativeEntryWindowBytesCompared);
        CHECK_EQ(symbol.decemberPrologue.size, kNativeEntryWindowBytesCompared);
        bool anyNonZero = false;
        for (std::size_t i = 0; i < symbol.novemberPrologue.size; ++i)
            anyNonZero = anyNonZero || symbol.novemberPrologue.bytes[i] != 0;
        CHECK(anyNonZero);
    }
    CHECK(observedCurated == expectedCurated);

    for (const auto& symbol : kNativeSymbols) {
        if (symbol.novemberPrologue.status != PrologueSignatureStatus::Curated)
            continue;
        if (symbol.id == NativeSymbolId::EditedActorTransition) {
            CHECK(symbol.functionId == FunctionId::Unknown);
            CHECK_EQ(resolveNativeSymbolVa(BuildId::Se2006, symbol.id),
                     symbol.novemberVa);
            CHECK_EQ(resolveNativeSymbolVa(BuildId::Game2000, symbol.id),
                     symbol.decemberVa);
            continue;
        }
        CHECK(symbol.functionId != FunctionId::Unknown);
        CHECK_EQ(symbol.novemberVa,
                 resolveFunctionVa(BuildId::Se2006, symbol.functionId));
    }
}

TEST(native_catalog_carries_the_recovery_evaluator_rows) {
    struct Row {
        NativeSymbolId id;
        GameAddr november;
    };
    const Row rows[] = {
        { NativeSymbolId::ManualRecoveryEvaluator, 0x47CAE0u },
        { NativeSymbolId::CommandMatch, 0x47B8F0u },
        { NativeSymbolId::FighterRecordInput, 0x47BF70u },
    };
    for (const Row& row : rows) {
        const NativeSymbol* symbol = findNativeSymbol(row.id);
        CHECK(symbol != nullptr);
        if (!symbol) continue;
        CHECK_EQ(symbol->novemberVa, row.november);
        CHECK(symbol->shiftClass == NativeShiftClass::ShiftedEntry);
        CHECK_EQ(symbol->decemberVa, row.november + kShiftedLayoutEntryDelta);
        CHECK(symbol->novemberPrologue.status
              == PrologueSignatureStatus::Curated);
    }

    for (NativeSymbolId id : { NativeSymbolId::ThrowEscapeEvaluator,
                               NativeSymbolId::ThrowContactTest }) {
        const NativeSymbol* symbol = findNativeSymbol(id);
        CHECK(symbol != nullptr);
        if (symbol) {
            CHECK(symbol->novemberPrologue.status
                  == PrologueSignatureStatus::NotCurated);
        }
    }

    const NativeSymbol* record = findNativeSymbol(NativeSymbolId::CombatRecordBase);
    CHECK(record != nullptr);
    if (record) {
        CHECK_EQ(record->novemberVa, 0x6265C0u);
        CHECK_EQ(record->decemberVa, record->novemberVa);
        CHECK(record->kind == NativeSymbolKind::DataGlobal);
        CHECK(record->shiftClass == NativeShiftClass::DataUnchanged);
        CHECK(record->novemberPrologue.status
              == PrologueSignatureStatus::NotCurated);
        CHECK_EQ(record->novemberVa + 28u, 0x6265DCu);
        CHECK_EQ(record->novemberVa + 8u, addr::kGuardGaugeArray);
    }
}

TEST(native_catalog_marks_verify_only_rows) {
    std::size_t verifyOnly = 0;
    for (const auto& symbol : kNativeSymbols) {
        if (symbol.runtimeUse != NativeRuntimeUse::SeamResolvedVerifyOnly)
            continue;
        ++verifyOnly;
        CHECK(symbol.kind == NativeSymbolKind::FunctionEntry);
        CHECK(symbol.novemberPrologue.status
              == PrologueSignatureStatus::Curated);
    }
    CHECK_EQ(verifyOnly, 3u);
}

TEST(native_catalog_records_entry_window_divergence) {
    std::size_t divergent = 0;
    for (const auto& symbol : kNativeSymbols) {
        const bool isCode = symbol.kind == NativeSymbolKind::FunctionEntry
            || symbol.kind == NativeSymbolKind::InteriorCode;
        if (!isCode) {
            CHECK(symbol.entryWindows == EntryWindowComparison::NotApplicable);
            continue;
        }
        CHECK(symbol.entryWindows != EntryWindowComparison::NotApplicable);
        if (symbol.entryWindows == EntryWindowComparison::Divergent)
            ++divergent;
    }
    CHECK(divergent > 0);

    const NativeSymbol* fcommand = findNativeSymbol(NativeSymbolId::FCommandParse);
    CHECK(fcommand != nullptr);
    if (fcommand) {
        CHECK(fcommand->shiftClass == NativeShiftClass::IdentityLayout);
        CHECK_EQ(fcommand->novemberVa, fcommand->decemberVa);
        CHECK(fcommand->entryWindows == EntryWindowComparison::Divergent);
    }
}

TEST(native_catalog_qualifies_december_without_merging_its_addresses) {
    const BuildProfile* december =
        findBuildByRuntimeProfileId(RuntimeProfileId::Qoh99DecemberCandidate);
    CHECK(december != nullptr);
    if (december) {
        CHECK(december->runtimeQualification
              == RuntimeQualification::Qualified);
        CHECK(runtimeProfileAllowsHookInstall(*december));
    }
    CHECK(buildIsRuntimeQualified(kSupportedRuntimeBuild));

    CHECK(resolveNativeSymbolVa(BuildId::Game2000,
                                NativeSymbolId::ReadLogicalInput) == 0x484BB0u);
    CHECK(resolveNativeSymbolVa(BuildId::Se2006,
                                NativeSymbolId::ReadLogicalInput) == 0x484B90u);
}

TEST(native_catalog_table_word_matches_build_content_preconditions) {
    const NativeSymbol* revision =
        findNativeSymbol(NativeSymbolId::DataTableWord4A71E0);
    CHECK(revision != nullptr);
    if (!revision) return;
    CHECK(revision->kind == NativeSymbolKind::DataTableWord);
    CHECK_EQ(revision->novemberVa, revision->decemberVa);
    CHECK(revision->novemberValue != revision->decemberValue);

    const BuildProfile* november =
        findBuildByRuntimeProfileId(RuntimeProfileId::Qoh99Se1126);
    const BuildProfile* december =
        findBuildByRuntimeProfileId(RuntimeProfileId::Qoh99DecemberCandidate);
    CHECK(november != nullptr);
    CHECK(december != nullptr);
    if (!november || !december) return;
    CHECK_EQ(november->contentPreconditions.chrHeaderRevisionVa,
             revision->novemberVa);
    CHECK_EQ(december->contentPreconditions.chrHeaderRevisionVa,
             revision->decemberVa);
    CHECK_EQ(november->contentPreconditions.chrHeaderRevision,
             revision->novemberValue);
    CHECK_EQ(december->contentPreconditions.chrHeaderRevision,
             revision->decemberValue);
}
