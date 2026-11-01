#include "training/NativeRecoveryQualification.h"

#include <cstring>
#include <string>

#include "RuntimeProfileRegistry.h"

namespace qoh99::recovery {
namespace {

using generated_model::FunctionId;
using generated_model::NativeSymbol;
using generated_model::NativeSymbolId;

constexpr std::uint32_t kMaxPrologueBytes = 64;

bool profileSelected() {
    return runtime_profile::selectedProfile() != nullptr;
}

const NativeSymbol* resolveRequirement(const Requirement& requirement,
                                       const generated_model::BuildProfile& profile,
                                       std::uintptr_t& va) {
    va = 0;
    std::string error;
    if (!runtime_profile::resolveSelectedFunction(requirement.functionId, va,
                                                  error)) {
        return nullptr;
    }
    const NativeSymbol* symbol =
        generated_model::findNativeSymbol(requirement.symbolId);
    if (symbol == nullptr) return nullptr;
    if (symbol->functionId != requirement.functionId) return nullptr;
    if (generated_model::findNativeSymbolByFunctionId(requirement.functionId)
        != symbol) {
        return nullptr;
    }
    if (va == 0) return nullptr;
    if (static_cast<std::uint32_t>(va)
        != generated_model::resolveNativeSymbolVa(profile.id,
                                                   requirement.symbolId)) {
        return nullptr;
    }
    return symbol;
}

const generated_model::PrologueSignature* activePrologue(
    const NativeSymbol& symbol,
    const generated_model::BuildProfile& profile) {
    switch (profile.id) {
        case generated_model::BuildId::Game2000:
            return &symbol.decemberPrologue;
        case generated_model::BuildId::Se2006:
            return &symbol.novemberPrologue;
    }
    return nullptr;
}

void fail(Report& report, QualificationFailure failure, const char* label,
          std::uint32_t va) {
    report.failure = failure;
    report.failedLabel = label;
    report.failedVa = va;
}

}

const Requirement kRecoveryRequirements[kRecoveryRequirementCount] = {
    { "sub_47CAE0 manual recovery evaluator",
      FunctionId::ManualRecoveryEvaluator,
      NativeSymbolId::ManualRecoveryEvaluator },
    { "sub_47B8F0 command matcher", FunctionId::CommandMatch,
      NativeSymbolId::CommandMatch },
    { "sub_47BF70 direction ring recorder", FunctionId::FighterRecordInput,
      NativeSymbolId::FighterRecordInput },
};

const DataRequirement
    kRecoveryDataRequirements[kRecoveryDataRequirementCount] = {
        { "0x6265C0 per-slot combat record (+28 recovery lock)",
          NativeSymbolId::CombatRecordBase },
};

Report qualifyRecoveryEvaluator(const Requirement* requirements,
                                std::size_t requirementCount,
                                const DataRequirement* dataRequirements,
                                std::size_t dataRequirementCount,
                                ByteReader reader, void* context) {
    Report report;

    if (requirements == nullptr || requirementCount == 0
        || reader == nullptr) {
        fail(report, QualificationFailure::AddressUnresolved,
             "no recovery requirements were supplied", 0);
        return report;
    }
    if (dataRequirementCount != 0 && dataRequirements == nullptr) {
        fail(report, QualificationFailure::AddressUnresolved,
             "no recovery data requirements were supplied", 0);
        return report;
    }

    if (!profileSelected()) {
        fail(report, QualificationFailure::ProfileNotSelected,
             requirements[0].label, 0);
        return report;
    }
    const generated_model::BuildProfile* profile =
        runtime_profile::selectedProfile();

    for (std::size_t i = 0; i < requirementCount; ++i) {
        std::uintptr_t va = 0;
        if (resolveRequirement(requirements[i], *profile, va) == nullptr) {
            fail(report, QualificationFailure::AddressUnresolved,
                 requirements[i].label, static_cast<std::uint32_t>(va));
            return report;
        }
    }

    for (std::size_t i = 0; i < dataRequirementCount; ++i) {
        const NativeSymbol* symbol =
            generated_model::findNativeSymbol(dataRequirements[i].symbolId);
        // Data rows must be DataUnchanged: never apply the code entry shift to a data address.
        if (symbol == nullptr
            || symbol->shiftClass
                   != generated_model::NativeShiftClass::DataUnchanged
            || symbol->novemberVa != symbol->decemberVa
            || generated_model::resolveNativeSymbolVa(
                   profile->id, dataRequirements[i].symbolId)
                   != symbol->novemberVa) {
            fail(report, QualificationFailure::AddressUnresolved,
                 dataRequirements[i].label,
                 symbol != nullptr ? symbol->novemberVa : 0u);
            return report;
        }
    }

    report.addressesResolved = true;

    for (std::size_t i = 0; i < requirementCount; ++i) {
        std::uintptr_t va = 0;
        const NativeSymbol* symbol =
            resolveRequirement(requirements[i], *profile, va);
        if (symbol == nullptr) {
            fail(report, QualificationFailure::AddressUnresolved,
                 requirements[i].label, static_cast<std::uint32_t>(va));
            report.addressesResolved = false;
            return report;
        }
        const generated_model::PrologueSignature* expected =
            activePrologue(*symbol, *profile);
        if (expected == nullptr
            || expected->status
                != generated_model::PrologueSignatureStatus::Curated
            || expected->bytes == nullptr || expected->size == 0
            || expected->size > kMaxPrologueBytes) {
            fail(report, QualificationFailure::SignatureNotCurated,
                 requirements[i].label, static_cast<std::uint32_t>(va));
            return report;
        }
        std::uint8_t observed[kMaxPrologueBytes] = {};
        const std::uint32_t count =
            static_cast<std::uint32_t>(expected->size);
        if (!reader(context, static_cast<std::uint32_t>(va), observed, count)) {
            fail(report, QualificationFailure::ReadFailed,
                 requirements[i].label, static_cast<std::uint32_t>(va));
            return report;
        }
        if (std::memcmp(observed, expected->bytes, count) != 0) {
            fail(report, QualificationFailure::PrologueMismatch,
                 requirements[i].label, static_cast<std::uint32_t>(va));
            return report;
        }
        ++report.comparedPrologues;
    }

    report.prologuesMatched = report.comparedPrologues == requirementCount;
    if (!report.prologuesMatched) {
        fail(report, QualificationFailure::PrologueMismatch,
             requirements[0].label, 0);
        return report;
    }
    report.failure = QualificationFailure::None;
    report.failedLabel = nullptr;
    report.failedVa = 0;
    return report;
}

const char* qualificationFailureText(QualificationFailure failure) {
    switch (failure) {
        case QualificationFailure::None: return "none";
        case QualificationFailure::ProfileNotSelected:
            return "no qualified runtime profile is selected";
        case QualificationFailure::AddressUnresolved:
            return "address did not resolve to the selected build column";
        case QualificationFailure::SignatureNotCurated:
            return "catalog row carries no curated prologue";
        case QualificationFailure::ReadFailed:
            return "image bytes are not readable at the resolved address";
        case QualificationFailure::PrologueMismatch:
            return "image bytes are not the modelled function";
    }
    return "unknown";
}

}
