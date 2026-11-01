#pragma once

#include <cstddef>
#include <cstdint>

#include "qoh99/generated/FunctionCatalog.hpp"
#include "qoh99/generated/NativeSymbolCatalog.hpp"

namespace qoh99::recovery {

enum class QualificationFailure : std::uint32_t {
    None = 0,
    ProfileNotSelected = 1,
    AddressUnresolved = 2,
    SignatureNotCurated = 3,
    ReadFailed = 4,
    PrologueMismatch = 5,
};

struct Requirement {
    const char* label;
    generated_model::FunctionId functionId;
    generated_model::NativeSymbolId symbolId;
};

struct DataRequirement {
    const char* label;
    generated_model::NativeSymbolId symbolId;
};

using ByteReader = bool (*)(void* context, std::uint32_t va,
                            std::uint8_t* out, std::uint32_t count);

struct Report {
    bool addressesResolved = false;
    bool prologuesMatched = false;
    QualificationFailure failure = QualificationFailure::ProfileNotSelected;
    const char* failedLabel = nullptr;
    std::uint32_t failedVa = 0;
    std::uint32_t comparedPrologues = 0;
};

constexpr std::size_t kRecoveryRequirementCount = 3;
extern const Requirement kRecoveryRequirements[kRecoveryRequirementCount];

constexpr std::size_t kRecoveryDataRequirementCount = 1;
extern const DataRequirement
    kRecoveryDataRequirements[kRecoveryDataRequirementCount];

Report qualifyRecoveryEvaluator(const Requirement* requirements,
                                std::size_t requirementCount,
                                const DataRequirement* dataRequirements,
                                std::size_t dataRequirementCount,
                                ByteReader reader, void* context);

inline Report qualifyRecoveryEvaluator(ByteReader reader, void* context) {
    return qualifyRecoveryEvaluator(
        kRecoveryRequirements, kRecoveryRequirementCount,
        kRecoveryDataRequirements, kRecoveryDataRequirementCount,
        reader, context);
}

const char* qualificationFailureText(QualificationFailure failure);

}
