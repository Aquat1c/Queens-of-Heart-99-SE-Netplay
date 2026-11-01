#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../../RuntimeProfileRegistry.h"
#include "../../client/BuildIdentity.h"
#include "../../netplay/tests/PinnedImageBytes.h"
#include "../../netplay/tests/RuntimeExecutableFixtures.h"
#include "../../training/NativeRecoveryQualification.h"
#include "../../training/TrainingRecovery.h"
#include "qoh99/generated/NativeSymbolCatalog.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const char* expression) {
    if (!condition) {
        std::printf("FAIL: %s\n", expression);
        ++g_failures;
    }
}

#define PROFILE_CHECK(expression) check((expression), #expression)

std::string currentExecutable() {
    char path[32768] = {};
    const DWORD length = GetModuleFileNameA(nullptr, path, sizeof(path));
    return length > 0 && length < sizeof(path) ? std::string(path, length)
                                                : std::string();
}

class TempWrongSizeExecutable {
public:
    explicit TempWrongSizeExecutable(const std::string& source) {
        char directory[MAX_PATH] = {};
        const DWORD directoryLength = GetTempPathA(sizeof(directory),
                                                    directory);
        if (directoryLength == 0 || directoryLength >= sizeof(directory))
            return;
        char generated[MAX_PATH] = {};
        if (GetTempFileNameA(directory, "qpr", 0, generated) == 0)
            return;
        path_ = generated;
        if (!DeleteFileA(path_.c_str())
            || !CopyFileA(source.c_str(), path_.c_str(), FALSE)) {
            return;
        }
        SetFileAttributesA(path_.c_str(), FILE_ATTRIBUTE_TEMPORARY);
        HANDLE file = CreateFileA(path_.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        const unsigned char padding = 0;
        DWORD written = 0;
        ok_ = WriteFile(file, &padding, sizeof(padding), &written, nullptr)
            && written == sizeof(padding);
        CloseHandle(file);
    }

    ~TempWrongSizeExecutable() {
        if (!path_.empty()) DeleteFileA(path_.c_str());
    }

    bool ok() const { return ok_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    bool ok_ = false;
};

struct ImageReaderContext {
    const qnetplay::test::PinnedImageBytes* image = nullptr;
    std::uint32_t corruptVa = 0;
    std::uint32_t calls = 0;
    bool failAll = false;
};

bool readFromPinnedImage(void* context, std::uint32_t va, std::uint8_t* out,
                         std::uint32_t count) {
    ImageReaderContext* reader = static_cast<ImageReaderContext*>(context);
    ++reader->calls;
    if (reader->failAll || reader->image == nullptr) return false;
    if (!reader->image->readVa(va, out, count)) return false;
    if (reader->corruptVa != 0 && reader->corruptVa == va)
        out[count / 2] = static_cast<std::uint8_t>(out[count / 2] ^ 0xFFu);
    return true;
}

}

namespace runtime = qoh99::runtime_profile;

int main() {
    using qoh99::generated_model::FunctionId;
    using qoh99::generated_model::RuntimeProfileId;
    using qoh99::generated_model::RuntimeQualification;
    using qoh99::runtime_profile::RuntimeProfileSelectionState;

    qnetplay::test::RuntimeExecutableFixtures fixtures{};
    std::string fixtureError;
    if (!qnetplay::test::resolveRuntimeExecutableFixtures(fixtures,
                                                           fixtureError)) {
        std::printf("FAIL: %s\n", fixtureError.c_str());
        return 1;
    }
    const std::string novemberExecutable =
        fixtures.novemberExecutable.generic_u8string();
    const std::string decemberExecutable =
        fixtures.decemberExecutable.generic_u8string();

    qclient::ExecutableIdentity november;
    std::string error;
    PROFILE_CHECK(qclient::detectKnownRuntimeExecutable(
        novemberExecutable, &november, error));
    PROFILE_CHECK(error.empty());
    PROFILE_CHECK(november.runtimeProfileId == RuntimeProfileId::Qoh99Se1126);
    PROFILE_CHECK(november.runtimeQualification == RuntimeQualification::Qualified);
    PROFILE_CHECK(!november.executableIdentityEstablishesSimulationContent);
    PROFILE_CHECK(qclient::validateSupportedRuntimeExecutable(
        novemberExecutable, &november, error));
    PROFILE_CHECK(error.empty());

    TempWrongSizeExecutable wrongSizeFixture(novemberExecutable);
    PROFILE_CHECK(wrongSizeFixture.ok());
    if (wrongSizeFixture.ok()) {
        qclient::ExecutableIdentity wrongSize;
        PROFILE_CHECK(qclient::inspectExecutable(wrongSizeFixture.path(),
                                                 wrongSize, error));
        PROFILE_CHECK(wrongSize.fileSize == november.fileSize + 1u);
        PROFILE_CHECK(wrongSize.coffTimestamp == november.coffTimestamp);
        PROFILE_CHECK(wrongSize.machine == november.machine);
        PROFILE_CHECK(wrongSize.sha256 != november.sha256);

        qclient::ExecutableIdentity detectedWrongSize;
        PROFILE_CHECK(!qclient::detectKnownRuntimeExecutable(
            wrongSizeFixture.path(), &detectedWrongSize, error));
        PROFILE_CHECK(detectedWrongSize.runtimeProfileId
                      == RuntimeProfileId::Unknown);

        runtime::clearSelectedProfile();
        PROFILE_CHECK(runtime::findProfileByExactExecutableEvidence(
            november.sha256.c_str(), wrongSize.fileSize,
            wrongSize.machine) == nullptr);
        PROFILE_CHECK(!runtime::selectProfileForExactExecutableEvidence(
            november.sha256.c_str(), wrongSize.fileSize, wrongSize.machine,
            error));
        PROFILE_CHECK(error.find("exact generated runtime profile")
                      != std::string::npos);
        PROFILE_CHECK(runtime::selectedProfileId()
                      == RuntimeProfileId::Unknown);
        PROFILE_CHECK(runtime::runtimeProfileSelectionRejected());
        runtime::clearSelectedProfile();
    }

    constexpr char kWrongHashWithNovemberMetadata[] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    PROFILE_CHECK(runtime::findProfileByCoffTimestamp(november.coffTimestamp)
                  != nullptr);
    PROFILE_CHECK(runtime::findProfileByExactExecutableEvidence(
        kWrongHashWithNovemberMetadata, november.fileSize,
        november.machine) == nullptr);
    runtime::clearSelectedProfile();
    PROFILE_CHECK(!runtime::selectProfileForExactExecutableEvidence(
        kWrongHashWithNovemberMetadata, november.fileSize, november.machine,
        error));
    PROFILE_CHECK(runtime::selectedProfileId() == RuntimeProfileId::Unknown);
    PROFILE_CHECK(runtime::runtimeProfileSelectionFinalized());
    PROFILE_CHECK(runtime::runtimeProfileSelectionRejected());
    PROFILE_CHECK(error.find("exact generated runtime profile")
                  != std::string::npos);
    PROFILE_CHECK(runtime::selectProfileForExactExecutableEvidence(
        november.sha256.c_str(), november.fileSize, november.machine, error));
    PROFILE_CHECK(runtime::selectedProfileId() == RuntimeProfileId::Qoh99Se1126);
    PROFILE_CHECK(runtime::runtimeProfileSelectionState()
                  == RuntimeProfileSelectionState::Qualified);
    runtime::clearSelectedProfile();
    PROFILE_CHECK(!runtime::runtimeProfileSelectionFinalized());

    qclient::ExecutableIdentity december;
    PROFILE_CHECK(qclient::detectKnownRuntimeExecutable(
        decemberExecutable, &december, error));
    PROFILE_CHECK(error.empty());
    PROFILE_CHECK(december.runtimeProfileId
                  == RuntimeProfileId::Qoh99DecemberCandidate);
    PROFILE_CHECK(december.runtimeQualification
                  == RuntimeQualification::Qualified);
    PROFILE_CHECK(!december.executableIdentityEstablishesSimulationContent);
    PROFILE_CHECK(qclient::validateSupportedRuntimeExecutable(
        decemberExecutable, &december, error));
    PROFILE_CHECK(error.empty());

    PROFILE_CHECK(qclient::validateSupportedRuntimeExecutable(
        novemberExecutable, &november, error));
    PROFILE_CHECK(error.empty());

    qoh99::runtime_profile::BuildProfile unqualified =
        *runtime::findProfile(RuntimeProfileId::Qoh99DecemberCandidate);
    unqualified.runtimeQualification = RuntimeQualification::CandidateUnqualified;
    PROFILE_CHECK(!qoh99::runtime_profile::profileAllowsHookInstall(unqualified));

    const std::string refusal =
        qoh99::runtime_profile::unsupportedRuntimeProfileReason(unqualified);
    PROFILE_CHECK(refusal.find("does not support yet") != std::string::npos);
    PROFILE_CHECK(refusal.find("will not start here") != std::string::npos);
    PROFILE_CHECK(refusal.find("nothing wrong with your copy")
                  != std::string::npos);
    PROFILE_CHECK(refusal.find("This is the December edition")
                  == std::string::npos);

    for (const char* jargon : {"qoh99-december-candidate", "yosie/yosie.chr",
                               "A4AD728F", "0x004A71E0", "sha256", "SHA-256",
                               "profile", "manifest", "hook", "injection",
                               "simulation-content"}) {
        PROFILE_CHECK(refusal.find(jargon) == std::string::npos);
    }

    const qoh99::runtime_profile::BuildProfile* decemberProfileRow =
        runtime::findProfile(RuntimeProfileId::Qoh99DecemberCandidate);
    PROFILE_CHECK(decemberProfileRow != nullptr);
    const std::string diagnostic = decemberProfileRow
        ? qoh99::runtime_profile::unsupportedRuntimeProfileDiagnostic(
              *decemberProfileRow)
        : std::string();
    PROFILE_CHECK(diagnostic.find("qoh99-december-candidate")
                  != std::string::npos);
    PROFILE_CHECK(diagnostic.find("yosie/yosie.chr") != std::string::npos);
    PROFILE_CHECK(
        diagnostic.find(
            "A4AD728F7E37856138B30DC8D9CD842DF142540E8EB5F8106743F26327E9F9E2")
        != std::string::npos);
    PROFILE_CHECK(diagnostic.find("004A71E0") != std::string::npos);
    PROFILE_CHECK(diagnostic.find("qualification=qualified")
                  != std::string::npos);
    PROFILE_CHECK(
        qoh99::runtime_profile::unsupportedRuntimeProfileDiagnostic(unqualified)
            .find("qualification=candidate") != std::string::npos);

    const auto* novemberBuild =
        qoh99::generated_model::findBuildByRuntimeProfileId(
            RuntimeProfileId::Qoh99Se1126);
    const auto* decemberBuild =
        qoh99::generated_model::findBuildByRuntimeProfileId(
            RuntimeProfileId::Qoh99DecemberCandidate);
    PROFILE_CHECK(novemberBuild != nullptr && decemberBuild != nullptr);
    if (novemberBuild && decemberBuild) {
        const auto& novemberContract = novemberBuild->contentPreconditions;
        const auto& decemberContract = decemberBuild->contentPreconditions;
        PROFILE_CHECK(novemberContract.chrHeaderRevisionVa == 0x004A71E0u);
        PROFILE_CHECK(decemberContract.chrHeaderRevisionVa == 0x004A71E0u);
        PROFILE_CHECK(novemberContract.chrHeaderRevision == 5u);
        PROFILE_CHECK(decemberContract.chrHeaderRevision == 6u);
        PROFILE_CHECK(std::string(novemberContract.simulationProbePath).empty());
        PROFILE_CHECK(
            std::string(novemberContract.simulationProbeSha256).empty());
        PROFILE_CHECK(std::string(decemberContract.simulationProbePath)
                      == "yosie/yosie.chr");
        PROFILE_CHECK(
            !std::string(decemberContract.simulationProbeSha256).empty());
        PROFILE_CHECK(novemberContract.chrHeaderRevisionVa
                      == decemberContract.chrHeaderRevisionVa);
        PROFILE_CHECK(novemberContract.chrHeaderRevision
                      != decemberContract.chrHeaderRevision);
    }

    qclient::ExecutableIdentity unknown;
    const std::string self = currentExecutable();
    PROFILE_CHECK(!self.empty());
    PROFILE_CHECK(!qclient::detectKnownRuntimeExecutable(self, &unknown, error));
    PROFILE_CHECK(unknown.runtimeProfileId == RuntimeProfileId::Unknown);
    PROFILE_CHECK(!error.empty());

    std::uintptr_t novemberRead = 0;
    std::uintptr_t decemberRead = 0;
    PROFILE_CHECK(runtime::resolveFunctionForProfile(
        RuntimeProfileId::Qoh99Se1126, FunctionId::ReadLogicalInput,
        novemberRead, error));
    PROFILE_CHECK(runtime::resolveFunctionForProfile(
        RuntimeProfileId::Qoh99DecemberCandidate,
        FunctionId::ReadLogicalInput, decemberRead, error));
    PROFILE_CHECK(novemberRead == 0x484B90u);
    PROFILE_CHECK(decemberRead == 0x484BB0u);
    PROFILE_CHECK(novemberRead != decemberRead);

    struct ProfileAddressExpectation {
        FunctionId id;
        std::uintptr_t november;
        std::uintptr_t december;
    };
    const ProfileAddressExpectation migratedHooks[] = {
        { FunctionId::PollAllInputs, 0x484A90u, 0x484AB0u },
        { FunctionId::NativeKeyboardCallback, 0x484B00u, 0x484B20u },
        { FunctionId::NativePresenceProbe, 0x484A20u, 0x484A40u },
        { FunctionId::ConfigSave, 0x485390u, 0x4853B0u },
        { FunctionId::SceneTitleMenu, 0x48CA00u, 0x48CA20u },
    };
    for (const ProfileAddressExpectation& expected : migratedHooks) {
        std::uintptr_t novemberAddress = 0;
        std::uintptr_t decemberAddress = 0;
        PROFILE_CHECK(runtime::resolveFunctionForProfile(
            RuntimeProfileId::Qoh99Se1126, expected.id,
            novemberAddress, error));
        PROFILE_CHECK(runtime::resolveFunctionForProfile(
            RuntimeProfileId::Qoh99DecemberCandidate, expected.id,
            decemberAddress, error));
        PROFILE_CHECK(novemberAddress == expected.november);
        PROFILE_CHECK(decemberAddress == expected.december);
    }

    runtime::clearSelectedProfile();
    std::uintptr_t selected = 0;
    PROFILE_CHECK(!runtime::resolveSelectedFunction(
        FunctionId::ReadLogicalInput, selected, error));

    PROFILE_CHECK(runtime::selectProfileForRuntime(
        RuntimeProfileId::Qoh99Se1126, error));
    PROFILE_CHECK(runtime::resolveSelectedFunction(
        FunctionId::ReadLogicalInput, selected, error));
    PROFILE_CHECK(selected == novemberRead);
    PROFILE_CHECK(selected != decemberRead);
    PROFILE_CHECK(std::string(runtime::selectedProfileKey()) == "qoh99-se1126");

    PROFILE_CHECK(runtime::selectProfileForRuntime(
        RuntimeProfileId::Qoh99DecemberCandidate, error));
    PROFILE_CHECK(runtime::resolveSelectedFunction(
        FunctionId::ReadLogicalInput, selected, error));
    PROFILE_CHECK(selected == decemberRead);
    PROFILE_CHECK(selected != novemberRead);
    PROFILE_CHECK(std::string(runtime::selectedProfileKey())
                  == "qoh99-december-candidate");

    PROFILE_CHECK(!runtime::resolveSelectedFunction(
        FunctionId::Unknown, selected, error));
    runtime::clearSelectedProfile();

    runtime::selectedProfileSlot().store(
        RuntimeProfileId::Unknown, std::memory_order_release);
    runtime::selectionStateSlot().store(
        RuntimeProfileSelectionState::Qualified, std::memory_order_release);
    selected = 0xDEADBEEFu;
    PROFILE_CHECK(!runtime::resolveSelectedFunction(
        FunctionId::ReadLogicalInput, selected, error));
    PROFILE_CHECK(selected == 0u);
    PROFILE_CHECK(runtime::selectedFunctionOrZero(
                      FunctionId::ReadLogicalInput) == 0u);
    PROFILE_CHECK(runtime::selectedProfile() == nullptr);
    PROFILE_CHECK(std::string(runtime::selectedProfileKey()) == "<none>");
    runtime::clearSelectedProfile();

    PROFILE_CHECK(qoh99::generated_model::runtimeQualifiedBuildCount() == 2u);
    PROFILE_CHECK(qoh99::generated_model::buildIsRuntimeQualified(
        qoh99::generated_model::BuildId::Se2006));
    PROFILE_CHECK(qoh99::generated_model::buildIsRuntimeQualified(
        qoh99::generated_model::BuildId::Game2000));
    PROFILE_CHECK(qoh99::generated_model::buildIsRuntimeQualified(
        qoh99::generated_model::kSupportedRuntimeBuild));

    for (const auto& function : qoh99::generated_model::kFunctions) {
        std::uintptr_t novemberVa = 0;
        std::uintptr_t decemberVa = 0;
        PROFILE_CHECK(runtime::resolveFunctionForProfile(
            RuntimeProfileId::Qoh99Se1126, function.id, novemberVa, error));
        PROFILE_CHECK(runtime::resolveFunctionForProfile(
            RuntimeProfileId::Qoh99DecemberCandidate, function.id, decemberVa,
            error));
        PROFILE_CHECK(novemberVa == function.se2006Va);
        PROFILE_CHECK(decemberVa == function.game2000Va);
        const auto* native =
            qoh99::generated_model::findNativeSymbolByFunctionId(function.id);
        PROFILE_CHECK(native != nullptr);
        if (native) {
            PROFILE_CHECK(native->novemberVa == function.se2006Va);
            PROFILE_CHECK(native->decemberVa == function.game2000Va);
        }
    }

    const auto* generatedNovember = qoh99::generated_model::findBuildByRuntimeProfileId(
        RuntimeProfileId::Qoh99Se1126);
    const auto* generatedDecember = qoh99::generated_model::findBuildByRuntimeProfileId(
        RuntimeProfileId::Qoh99DecemberCandidate);
    const auto* read = qoh99::generated_model::findFunction(
        FunctionId::ReadLogicalInput);
    PROFILE_CHECK(generatedNovember != nullptr);
    PROFILE_CHECK(generatedDecember != nullptr);
    PROFILE_CHECK(read != nullptr);
    PROFILE_CHECK(generatedNovember
                  && std::string(generatedNovember->runtimeProfileKey)
                         == "qoh99-se1126");
    PROFILE_CHECK(generatedDecember
                  && std::string(generatedDecember->runtimeProfileKey)
                         == "qoh99-december-candidate");
    PROFILE_CHECK(generatedNovember && read
                  && qoh99::generated_model::functionVaForBuild(
                         *read, generatedNovember->id) == novemberRead);
    PROFILE_CHECK(generatedDecember && read
                  && qoh99::generated_model::functionVaForBuild(
                         *read, generatedDecember->id) == decemberRead);

    {
        namespace recovery = qoh99::recovery;
        using recovery::QualificationFailure;

        qnetplay::test::PinnedImageBytes novemberImage;
        qnetplay::test::PinnedImageBytes decemberImage;
        std::string imageError;
        PROFILE_CHECK(novemberImage.load(novemberExecutable, imageError));
        PROFILE_CHECK(imageError.empty());
        PROFILE_CHECK(novemberImage.size() == november.fileSize);
        imageError.clear();
        PROFILE_CHECK(decemberImage.load(decemberExecutable, imageError));
        PROFILE_CHECK(imageError.empty());
        PROFILE_CHECK(decemberImage.size() == december.fileSize);

        PROFILE_CHECK(recovery::kRecoveryRequirementCount == 3u);
        const FunctionId expectedRecoveryFunctions[] = {
            FunctionId::ManualRecoveryEvaluator,
            FunctionId::CommandMatch,
            FunctionId::FighterRecordInput,
        };
        const qoh99::generated_model::NativeSymbolId
            expectedRecoverySymbols[] = {
                qoh99::generated_model::NativeSymbolId::
                    ManualRecoveryEvaluator,
                qoh99::generated_model::NativeSymbolId::CommandMatch,
                qoh99::generated_model::NativeSymbolId::FighterRecordInput,
            };
        for (std::size_t i = 0; i < recovery::kRecoveryRequirementCount; ++i) {
            const recovery::Requirement& requirement =
                recovery::kRecoveryRequirements[i];
            PROFILE_CHECK(requirement.functionId
                          == expectedRecoveryFunctions[i]);
            PROFILE_CHECK(requirement.symbolId
                          == expectedRecoverySymbols[i]);
            PROFILE_CHECK(requirement.functionId
                          != FunctionId::ThrowEscapeEvaluator);
            PROFILE_CHECK(requirement.functionId
                          != FunctionId::ThrowContactTest);
            const auto* symbol = qoh99::generated_model::findNativeSymbol(
                requirement.symbolId);
            PROFILE_CHECK(symbol != nullptr);
            if (symbol == nullptr) continue;
            PROFILE_CHECK(symbol->functionId == requirement.functionId);
            PROFILE_CHECK(symbol->novemberPrologue.status
                          == qoh99::generated_model::PrologueSignatureStatus::
                                 Curated);
            PROFILE_CHECK(symbol->decemberPrologue.status
                          == qoh99::generated_model::PrologueSignatureStatus::
                                 Curated);
            std::uint8_t observed[32] = {};
            const std::uint32_t count =
                static_cast<std::uint32_t>(symbol->novemberPrologue.size);
            PROFILE_CHECK(count > 0 && count <= sizeof(observed));
            PROFILE_CHECK(novemberImage.readVa(symbol->novemberVa, observed,
                                               count));
            PROFILE_CHECK(std::memcmp(observed, symbol->novemberPrologue.bytes,
                                      count) == 0);
            const std::uint32_t decemberCount =
                static_cast<std::uint32_t>(symbol->decemberPrologue.size);
            PROFILE_CHECK(decemberCount > 0
                          && decemberCount <= sizeof(observed));
            PROFILE_CHECK(decemberImage.readVa(
                symbol->decemberVa, observed, decemberCount));
            PROFILE_CHECK(std::memcmp(observed,
                                      symbol->decemberPrologue.bytes,
                                      decemberCount) == 0);
        }

        runtime::clearSelectedProfile();
        PROFILE_CHECK(runtime::selectProfileForRuntime(
            RuntimeProfileId::Qoh99Se1126, error));
        ImageReaderContext reader{&novemberImage, 0, 0, false};
        recovery::Report report =
            recovery::qualifyRecoveryEvaluator(&readFromPinnedImage, &reader);
        PROFILE_CHECK(report.addressesResolved);
        PROFILE_CHECK(report.prologuesMatched);
        PROFILE_CHECK(report.comparedPrologues
                      == recovery::kRecoveryRequirementCount);
        PROFILE_CHECK(report.failure == QualificationFailure::None);
        PROFILE_CHECK(report.failedLabel == nullptr);
        PROFILE_CHECK(reader.calls == recovery::kRecoveryRequirementCount);
        PROFILE_CHECK(qtraining::recoveryEvaluatorReasonFor(
                          true, report.addressesResolved,
                          report.prologuesMatched)
                      == qtraining::RecoveryEvaluatorReason::Qualified);

        runtime::clearSelectedProfile();
        PROFILE_CHECK(runtime::selectProfileForRuntime(
            RuntimeProfileId::Qoh99DecemberCandidate, error));
        ImageReaderContext decemberReader{&decemberImage, 0, 0, false};
        report = recovery::qualifyRecoveryEvaluator(&readFromPinnedImage,
                                                    &decemberReader);
        PROFILE_CHECK(report.addressesResolved);
        PROFILE_CHECK(report.prologuesMatched);
        PROFILE_CHECK(report.comparedPrologues
                      == recovery::kRecoveryRequirementCount);
        PROFILE_CHECK(report.failure == QualificationFailure::None);
        PROFILE_CHECK(report.failedLabel == nullptr);
        PROFILE_CHECK(decemberReader.calls
                      == recovery::kRecoveryRequirementCount);

        runtime::clearSelectedProfile();
        PROFILE_CHECK(runtime::selectProfileForRuntime(
            RuntimeProfileId::Qoh99Se1126, error));

        ImageReaderContext corrupt{&novemberImage, 0x47CAE0u, 0, false};
        report = recovery::qualifyRecoveryEvaluator(&readFromPinnedImage,
                                                    &corrupt);
        PROFILE_CHECK(report.addressesResolved);
        PROFILE_CHECK(!report.prologuesMatched);
        PROFILE_CHECK(report.failure == QualificationFailure::PrologueMismatch);
        PROFILE_CHECK(report.failedVa == 0x47CAE0u);
        PROFILE_CHECK(report.failedLabel != nullptr
                      && std::string(report.failedLabel).find("sub_47CAE0")
                             != std::string::npos);
        PROFILE_CHECK(report.comparedPrologues == 0u);
        PROFILE_CHECK(qtraining::recoveryEvaluatorReasonFor(
                          true, report.addressesResolved,
                          report.prologuesMatched)
                      == qtraining::RecoveryEvaluatorReason::PrologueMismatch);

        ImageReaderContext unreadable{&novemberImage, 0, 0, true};
        report = recovery::qualifyRecoveryEvaluator(&readFromPinnedImage,
                                                    &unreadable);
        PROFILE_CHECK(report.addressesResolved);
        PROFILE_CHECK(!report.prologuesMatched);
        PROFILE_CHECK(report.failure == QualificationFailure::ReadFailed);

        runtime::clearSelectedProfile();
        ImageReaderContext unselected{&novemberImage, 0, 0, false};
        report = recovery::qualifyRecoveryEvaluator(&readFromPinnedImage,
                                                    &unselected);
        PROFILE_CHECK(!report.addressesResolved);
        PROFILE_CHECK(!report.prologuesMatched);
        PROFILE_CHECK(report.failure
                      == QualificationFailure::ProfileNotSelected);
        PROFILE_CHECK(unselected.calls == 0u);

        ImageReaderContext unselectable{&novemberImage, 0, 0, false};
        runtime::clearSelectedProfile();
        PROFILE_CHECK(!runtime::selectProfileForRuntime(
            RuntimeProfileId::Unknown, error));
        PROFILE_CHECK(runtime::runtimeProfileSelectionRejected());
        report = recovery::qualifyRecoveryEvaluator(&readFromPinnedImage,
                                                    &unselectable);
        PROFILE_CHECK(!report.addressesResolved);
        PROFILE_CHECK(!report.prologuesMatched);
        PROFILE_CHECK(report.failure
                      == QualificationFailure::ProfileNotSelected);
        PROFILE_CHECK(unselectable.calls == 0u);

        runtime::clearSelectedProfile();
        PROFILE_CHECK(runtime::selectProfileForRuntime(
            RuntimeProfileId::Qoh99DecemberCandidate, error));
        runtime::clearSelectedProfile();

        runtime::clearSelectedProfile();
        PROFILE_CHECK(runtime::selectProfileForRuntime(
            RuntimeProfileId::Qoh99Se1126, error));
        const recovery::Requirement unknownFunction[] = {
            { "doctored: no seam entry", FunctionId::Unknown,
              qoh99::generated_model::NativeSymbolId::ManualRecoveryEvaluator },
        };
        {
            ImageReaderContext unresolved{&novemberImage, 0, 0, false};
            report = recovery::qualifyRecoveryEvaluator(
                unknownFunction, 1, recovery::kRecoveryDataRequirements,
                recovery::kRecoveryDataRequirementCount, &readFromPinnedImage,
                &unresolved);
            PROFILE_CHECK(!report.addressesResolved);
            PROFILE_CHECK(!report.prologuesMatched);
            PROFILE_CHECK(report.failure
                          == QualificationFailure::AddressUnresolved);
            PROFILE_CHECK(unresolved.calls == 0u);
        }

        {
            const recovery::Requirement crossed[] = {
                { "doctored: mismatched ids", FunctionId::CommandMatch,
                  qoh99::generated_model::NativeSymbolId::
                      ManualRecoveryEvaluator },
            };
            ImageReaderContext mismatched{&novemberImage, 0, 0, false};
            report = recovery::qualifyRecoveryEvaluator(
                crossed, 1, recovery::kRecoveryDataRequirements,
                recovery::kRecoveryDataRequirementCount, &readFromPinnedImage,
                &mismatched);
            PROFILE_CHECK(!report.addressesResolved);
            PROFILE_CHECK(report.failure
                          == QualificationFailure::AddressUnresolved);
            PROFILE_CHECK(mismatched.calls == 0u);
        }

        {
            const recovery::Requirement uncurated[] = {
                { "doctored: uncurated row", FunctionId::GameplayTick,
                  qoh99::generated_model::NativeSymbolId::GameplayTick },
            };
            ImageReaderContext neverRead{&novemberImage, 0, 0, false};
            report = recovery::qualifyRecoveryEvaluator(
                uncurated, 1, recovery::kRecoveryDataRequirements,
                recovery::kRecoveryDataRequirementCount, &readFromPinnedImage,
                &neverRead);
            PROFILE_CHECK(report.addressesResolved);
            PROFILE_CHECK(!report.prologuesMatched);
            PROFILE_CHECK(report.failure
                          == QualificationFailure::SignatureNotCurated);
            PROFILE_CHECK(neverRead.calls == 0u);
        }

        {
            const recovery::DataRequirement notData[] = {
                { "doctored: code row as data",
                  qoh99::generated_model::NativeSymbolId::CommandMatch },
            };
            ImageReaderContext badData{&novemberImage, 0, 0, false};
            report = recovery::qualifyRecoveryEvaluator(
                recovery::kRecoveryRequirements,
                recovery::kRecoveryRequirementCount, notData, 1,
                &readFromPinnedImage, &badData);
            PROFILE_CHECK(!report.addressesResolved);
            PROFILE_CHECK(report.failure
                          == QualificationFailure::AddressUnresolved);
        }

        {
            ImageReaderContext empty{&novemberImage, 0, 0, false};
            report = recovery::qualifyRecoveryEvaluator(
                recovery::kRecoveryRequirements, 0,
                recovery::kRecoveryDataRequirements,
                recovery::kRecoveryDataRequirementCount, &readFromPinnedImage,
                &empty);
            PROFILE_CHECK(!report.addressesResolved);
            PROFILE_CHECK(!report.prologuesMatched);
            PROFILE_CHECK(report.comparedPrologues == 0u);
            PROFILE_CHECK(empty.calls == 0u);
        }

        report = recovery::qualifyRecoveryEvaluator(&readFromPinnedImage,
                                                    &corrupt);
        qtraining::RecoverySettings settings{};
        settings.air = qtraining::ManualRecoveryMode::Earliest;
        settings.ground = qtraining::ManualRecoveryMode::Earliest;
        settings.wall = qtraining::ManualRecoveryMode::Earliest;
        settings.terminal = qtraining::ManualRecoveryMode::Earliest;
        settings.throwEscape = qtraining::ManualRecoveryMode::Earliest;
        settings.evaluatorQualified =
            report.addressesResolved && report.prologuesMatched;
        PROFILE_CHECK(!settings.evaluatorQualified);
        for (const qtraining::ManualRecoveryMode mode :
             { qtraining::ManualRecoveryMode::Earliest,
               qtraining::ManualRecoveryMode::FixedDelay,
               qtraining::ManualRecoveryMode::RandomDelay,
               qtraining::ManualRecoveryMode::Late,
               static_cast<qtraining::ManualRecoveryMode>(99) }) {
            PROFILE_CHECK(!qtraining::nativeRecoveryReleaseQualified(
                mode, settings.evaluatorQualified));
        }
        PROFILE_CHECK(qtraining::nativeRecoveryReleaseQualified(
            qtraining::ManualRecoveryMode::Off, settings.evaluatorQualified));

        runtime::clearSelectedProfile();
    }

    if (g_failures == 0) {
        std::puts("Runtime profile tests passed");
        return 0;
    }
    std::printf("Runtime profile tests failed: %d\n", g_failures);
    return 1;
}
