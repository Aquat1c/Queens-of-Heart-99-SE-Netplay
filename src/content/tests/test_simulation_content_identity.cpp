#include "../../netplay/tests/TestHarness.h"

#include "../../client/BuildIdentity.h"
#include "../../compat/CompatibilityIdentity.h"
#include "../SimulationContentIdentity.h"
#include "../SimulationContentCatalog.generated.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace qoh99::content;

std::atomic<unsigned long> g_fixtureCounter{0u};

class DisposableFixture final {
public:
    DisposableFixture() {
        const unsigned long serial = ++g_fixtureCounter;
        path_ = std::filesystem::temp_directory_path()
            / ("qoh99_sim_content_" + std::to_string(::GetCurrentProcessId())
               + "_" + std::to_string(serial));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
    }

    ~DisposableFixture() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

    void write(const std::string& relative, const std::string& bytes) const {
        const std::filesystem::path destination = path_ / std::filesystem::u8path(relative);
        std::error_code error;
        std::filesystem::create_directories(destination.parent_path(), error);
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

private:
    std::filesystem::path path_;
};

std::vector<ContentRule> fixtureRules() {
    return {
        {"fighters/a.chr", ContentClassification::SimulationRequired, true, true},
        {"ai/com_a.fob", ContentClassification::SimulationRequired, true, false},
        {"system/system.bin", ContentClassification::SimulationRequired, true, true},
        {"system/winmes/mik_mes7.img", ContentClassification::NativeValidationRequired, false, true},
        {"system/winmes/tir_mes1.img", ContentClassification::NativeValidationRequired, false, true},
    };
}

void populateFixture(const DisposableFixture& fixture) {
    fixture.write("fighters/a.chr", "fighter-authored-frames");
    fixture.write("ai/com_a.fob", "ai-bytecode");
    fixture.write("system/system.bin", "chr-decryption-key-material");
    fixture.write("system/winmes/mik_mes7.img", "native-validation-image-a");
    fixture.write("system/winmes/tir_mes1.img", "native-validation-image-b");
}

ManifestResult buildFixture(const DisposableFixture& fixture) {
    return SimulationContentManifestBuilder::buildWithRules(
        fixture.path(), fixtureRules());
}

bool copyFixtureFile(const std::filesystem::path& source,
                     const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) return false;
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing,
                               error);
    return !error;
}

bool populateCopiedNovemberInstallation(const DisposableFixture& fixture) {
    const std::filesystem::path source =
        std::filesystem::u8path(QOH99_NOVEMBER_GAME_ROOT);
    if (!copyFixtureFile(source / "qoh99.exe", fixture.path() / "qoh99.exe"))
        return false;
    for (const ExpectedContentEntry& entry : generated::kExpectedContentEntries) {
        if (!copyFixtureFile(source / std::filesystem::u8path(entry.normalizedPath),
                             fixture.path()
                                 / std::filesystem::u8path(entry.normalizedPath))) {
            return false;
        }
    }
    return true;
}

bool restoreNovemberEntry(const DisposableFixture& fixture,
                          const char* const normalizedPath) {
    const std::filesystem::path source =
        std::filesystem::u8path(QOH99_NOVEMBER_GAME_ROOT)
        / std::filesystem::u8path(normalizedPath);
    return copyFixtureFile(source, fixture.path()
        / std::filesystem::u8path(normalizedPath));
}

ManifestEntry makeEntry(const char* path, const unsigned byte,
                        const bool sim, const bool native) {
    ManifestEntry entry{};
    entry.normalizedPath = path;
    entry.classification = sim ? ContentClassification::SimulationRequired
                               : ContentClassification::NativeValidationRequired;
    entry.requiredForSession = sim;
    entry.requiredForNativeValidation = native;
    entry.sizeBytes = 1u;
    entry.sha256.fill(static_cast<std::uint8_t>(byte));
    return entry;
}

}

QNP_TEST(ContentManifestIsIndependentOfDirectoryEnumerationOrder) {
    std::vector<ManifestEntry> first{
        makeEntry("z/com_z.fob", 3u, true, false),
        makeEntry("a/a.chr", 1u, true, true),
    };
    std::vector<ManifestEntry> second{
        first[1], first[0],
    };
    Digest firstRoot{};
    Digest secondRoot{};
    std::string error;
    QNP_CHECK(SimulationContentManifestBuilder::canonicalRoot(first, false, firstRoot, error));
    QNP_CHECK(SimulationContentManifestBuilder::canonicalRoot(second, false, secondRoot, error));
    QNP_CHECK(firstRoot == secondRoot);
}

QNP_TEST(ContentManifestNormalizesPathCasing) {
    std::string first;
    std::string second;
    std::string error;
    QNP_CHECK(normalizeWindowsRelativePath("Fighters/A.CHR", first, error));
    QNP_CHECK(normalizeWindowsRelativePath("fighters/a.chr", second, error));
    QNP_CHECK(first == second && first == "fighters/a.chr");
}

QNP_TEST(ContentManifestNormalizesSlashDirection) {
    std::string first;
    std::string second;
    std::string error;
    QNP_CHECK(normalizeWindowsRelativePath("fighters\\a.chr", first, error));
    QNP_CHECK(normalizeWindowsRelativePath("fighters/a.chr", second, error));
    QNP_CHECK(first == second);
}

QNP_TEST(ContentManifestDoesNotDependOnAbsoluteInstallLocation) {
    DisposableFixture first;
    DisposableFixture second;
    populateFixture(first);
    populateFixture(second);
    const ManifestResult a = buildFixture(first);
    const ManifestResult b = buildFixture(second);
    QNP_CHECK(a.identity.complete());
    QNP_CHECK(b.identity.complete());
    QNP_CHECK(a.identity.simulationRootSha256 == b.identity.simulationRootSha256);
    QNP_CHECK(a.identity.validationRootSha256 == b.identity.validationRootSha256);
}

QNP_TEST(ContentManifestIgnoresTimestampOnlyChanges) {
    DisposableFixture fixture;
    populateFixture(fixture);
    const ManifestResult before = buildFixture(fixture);
    std::error_code error;
    const std::filesystem::path path = fixture.path() / "fighters/a.chr";
    std::filesystem::last_write_time(
        path, std::filesystem::file_time_type::clock::now() - std::chrono::hours(48), error);
    QNP_CHECK(!error);
    const ManifestResult after = buildFixture(fixture);
    QNP_CHECK(before.identity.simulationRootSha256 == after.identity.simulationRootSha256);
    QNP_CHECK(before.identity.validationRootSha256 == after.identity.validationRootSha256);
}

QNP_TEST(ContentManifestChangesForOneByteContentChange) {
    DisposableFixture fixture;
    populateFixture(fixture);
    const ManifestResult before = buildFixture(fixture);
    fixture.write("fighters/a.chr", "fighter-authored-frameS");
    const ManifestResult after = buildFixture(fixture);
    QNP_CHECK(before.identity.simulationRootSha256 != after.identity.simulationRootSha256);
}

QNP_TEST(ContentManifestInvalidatesMissingRequiredFile) {
    DisposableFixture fixture;
    populateFixture(fixture);
    std::error_code error;
    std::filesystem::remove(fixture.path() / "fighters/a.chr", error);
    const ManifestResult result = buildFixture(fixture);
    QNP_CHECK((result.identity.manifestFlags & ManifestFlagMissingRequired) != 0u);
    QNP_CHECK(!result.identity.complete());
    QNP_CHECK(!result.missingPaths.empty());
}

QNP_TEST(ContentManifestInvalidatesUnreviewedPotentiallyRelevantExtra) {
    DisposableFixture fixture;
    populateFixture(fixture);
    const ManifestResult before = buildFixture(fixture);
    fixture.write("fighters/unknown_extra.chr", "unattested-simulation-data");
    const ManifestResult after = buildFixture(fixture);
    QNP_CHECK((after.identity.manifestFlags
               & ManifestFlagUnreviewedPotentiallyRelevantExtra) != 0u);
    QNP_CHECK((after.identity.manifestFlags
               & ManifestFlagExtraSimulationResource) == 0u);
    QNP_CHECK(!after.identity.complete());
    QNP_CHECK(!after.admissionAllowed());
    QNP_CHECK(before.identity.simulationRootSha256 == after.identity.simulationRootSha256);
    QNP_CHECK(after.unreviewedPotentiallyRelevantPaths.size() == 1u);
    QNP_CHECK(after.unreviewedPotentiallyRelevantPaths[0]
              == "fighters/unknown_extra.chr");
}

QNP_TEST(ContentManifestIgnoresExtraPresentationOnlyFile) {
    DisposableFixture fixture;
    populateFixture(fixture);
    const ManifestResult before = buildFixture(fixture);
    fixture.write("presentation/portrait.img", "presentation-only");
    const ManifestResult after = buildFixture(fixture);
    QNP_CHECK(after.identity.complete());
    QNP_CHECK(before.identity.simulationRootSha256 == after.identity.simulationRootSha256);
}

QNP_TEST(ContentManifestRejectsDuplicateNormalizedPath) {
    DisposableFixture fixture;
    populateFixture(fixture);
    std::vector<ContentRule> rules = fixtureRules();
    rules.push_back({"FIGHTERS\\A.CHR", ContentClassification::SimulationRequired, true, true});
    const ManifestResult result = SimulationContentManifestBuilder::buildWithRules(
        fixture.path(), rules);
    QNP_CHECK((result.identity.manifestFlags & ManifestFlagDuplicateNormalizedPath) != 0u);
    QNP_CHECK(!result.identity.complete());
}

QNP_TEST(ContentManifestRejectsUnreadableRequiredFile) {
    DisposableFixture fixture;
    populateFixture(fixture);
    const std::filesystem::path path = fixture.path() / "fighters/a.chr";
    HANDLE held = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QNP_REQUIRE(held != INVALID_HANDLE_VALUE);
    const ManifestResult result = buildFixture(fixture);
    ::CloseHandle(held);
    QNP_CHECK((result.identity.manifestFlags & ManifestFlagUnreadableRequired) != 0u);
    QNP_CHECK(!result.identity.complete());
}

QNP_TEST(ContentManifestRejectsWrongChrContent) {
    DisposableFixture fixture;
    populateFixture(fixture);
    const ManifestResult known = buildFixture(fixture);
    fixture.write("fighters/a.chr", "fighter-authored-frameX");
    const ManifestResult modified = buildFixture(fixture);
    QNP_CHECK(known.identity.complete());
    QNP_CHECK(modified.identity.complete());
    QNP_CHECK(known.identity.simulationRootSha256 != modified.identity.simulationRootSha256);
    QNP_CHECK(!modified.admissionAllowed());
}

QNP_TEST(AuthoritativeNovemberAndDecemberHaveDifferentSimulationRoots) {
    const ManifestResult november = SimulationContentManifestBuilder::build(
        std::filesystem::u8path(QOH99_NOVEMBER_GAME_ROOT),
        RuntimeProfile::November1126);
    const ManifestResult december = SimulationContentManifestBuilder::build(
        std::filesystem::u8path(QOH99_DECEMBER_GAME_ROOT),
        RuntimeProfile::DecemberCandidate);
    QNP_CHECK(november.disposition == InstallationDisposition::KnownQualifiedManifest);
    QNP_CHECK(november.admissionAllowed());
    QNP_CHECK(december.disposition == InstallationDisposition::KnownQualifiedManifest);
    QNP_CHECK(december.admissionAllowed());
    QNP_CHECK(november.identity.simulationRootSha256 != december.identity.simulationRootSha256);
}

QNP_TEST(CopiedNovemberInstallationRejectsModifiedMissingAndMixedContentBeforeAdmission) {
    DisposableFixture fixture;
    QNP_REQUIRE(populateCopiedNovemberInstallation(fixture));
    const std::string executable =
        (fixture.path() / "qoh99.exe").generic_u8string();

    const ManifestResult baseline = SimulationContentManifestBuilder::build(
        fixture.path(), RuntimeProfile::November1126);
    QNP_CHECK(baseline.disposition
              == InstallationDisposition::KnownQualifiedManifest);
    QNP_CHECK(baseline.admissionAllowed());
    QNP_CHECK(baseline.bytesVerifiedForAdmission);
    qclient::RuntimeInstallationIdentity admitted{};
    std::string error;
    QNP_CHECK(qclient::validateRuntimeInstallationForAdmission(
        executable, &admitted, error));
    QNP_CHECK(admitted.content.identity.simulationRootSha256
              == baseline.identity.simulationRootSha256);

    fixture.write("akari/akari.chr", "modified-character-resource");
    qclient::RuntimeInstallationIdentity modified{};
    QNP_CHECK(!qclient::validateRuntimeInstallationForAdmission(
        executable, &modified, error));
    QNP_CHECK(modified.executable.runtimeProfileId
              == admitted.executable.runtimeProfileId);
    QNP_CHECK(modified.content.disposition
              == InstallationDisposition::LocallyModifiedKnownBuild);
    QNP_CHECK((modified.content.identity.manifestFlags
               & ManifestFlagRootMismatch) != 0u);
    QNP_REQUIRE(restoreNovemberEntry(fixture, "akari/akari.chr"));

    std::error_code removeError;
    std::filesystem::remove(fixture.path() / "system/system.bin", removeError);
    QNP_REQUIRE(!removeError);
    qclient::RuntimeInstallationIdentity missing{};
    QNP_CHECK(!qclient::validateRuntimeInstallationForAdmission(
        executable, &missing, error));
    QNP_CHECK(missing.content.disposition
              == InstallationDisposition::IncompleteInstallation);
    QNP_CHECK((missing.content.identity.manifestFlags
               & ManifestFlagMissingRequired) != 0u);
    QNP_REQUIRE(restoreNovemberEntry(fixture, "system/system.bin"));

    const std::filesystem::path decemberCharacter =
        std::filesystem::u8path(QOH99_DECEMBER_GAME_ROOT)
        / "akari/akari.chr";
    QNP_REQUIRE(copyFixtureFile(decemberCharacter,
                                fixture.path() / "akari/akari.chr"));
    qclient::RuntimeInstallationIdentity mixed{};
    QNP_CHECK(!qclient::validateRuntimeInstallationForAdmission(
        executable, &mixed, error));
    QNP_CHECK(mixed.content.disposition
              == InstallationDisposition::LocallyModifiedKnownBuild);
    QNP_CHECK(mixed.content.identity.simulationRootSha256
              != baseline.identity.simulationRootSha256);
}

QNP_TEST(DecemberInstallationIsAdmittedWithItsOwnIdentity) {
    const std::string executable = (std::filesystem::u8path(
        QOH99_DECEMBER_GAME_ROOT) / "QOH99.EXE").generic_u8string();
    qclient::RuntimeInstallationIdentity december{};
    std::string error;
    QNP_CHECK(qclient::validateRuntimeInstallationForAdmission(
        executable, &december, error));
    QNP_CHECK(error.empty());
    QNP_CHECK(december.content.disposition
              == InstallationDisposition::KnownQualifiedManifest);
    QNP_CHECK(december.content.qualification
              == ProfileQualification::Qualified);
    QNP_CHECK(december.content.admissionAllowed());
    QNP_CHECK(december.content.bytesVerifiedForAdmission);
    QNP_CHECK(december.content.profile == RuntimeProfile::DecemberCandidate);
}

QNP_TEST(DecemberExecutableInNovemberContentIsRefused) {
    DisposableFixture fixture;
    QNP_REQUIRE(populateCopiedNovemberInstallation(fixture));
    const std::filesystem::path decemberExecutable =
        std::filesystem::u8path(QOH99_DECEMBER_GAME_ROOT) / "QOH99.EXE";
    QNP_REQUIRE(copyFixtureFile(decemberExecutable,
                                fixture.path() / "qoh99.exe"));
    qclient::RuntimeInstallationIdentity mixed{};
    std::string error;
    QNP_CHECK(!qclient::validateRuntimeInstallationForAdmission(
        (fixture.path() / "qoh99.exe").generic_u8string(), &mixed, error));
    QNP_CHECK(!mixed.content.admissionAllowed());
    QNP_CHECK(mixed.executable.runtimeProfileId
              == qoh99::generated_model::RuntimeProfileId::
                     Qoh99DecemberCandidate);
    QNP_CHECK(mixed.content.disposition
              == InstallationDisposition::LocallyModifiedKnownBuild);
}

QNP_TEST(RealInstallationsAdmitSameBuildAndRefuseCrossBuildSessions) {
    const std::string novemberExe = (std::filesystem::u8path(
        QOH99_NOVEMBER_GAME_ROOT) / "qoh99.exe").generic_u8string();
    const std::string decemberExe = (std::filesystem::u8path(
        QOH99_DECEMBER_GAME_ROOT) / "QOH99.EXE").generic_u8string();

    qclient::RuntimeInstallationIdentity november{};
    qclient::RuntimeInstallationIdentity december{};
    std::string error;
    QNP_REQUIRE(qclient::validateRuntimeInstallationForAdmission(
        novemberExe, &november, error));
    QNP_REQUIRE(qclient::validateRuntimeInstallationForAdmission(
        decemberExe, &december, error));

    using qnetplay::compat::compatibilityMismatch;
    using qnetplay::compat::IdentityMismatch;

    QNP_CHECK(compatibilityMismatch(november.compatibility,
                                    november.compatibility)
              == IdentityMismatch::None);
    QNP_CHECK(compatibilityMismatch(december.compatibility,
                                    december.compatibility)
              == IdentityMismatch::None);

    QNP_CHECK(compatibilityMismatch(november.compatibility,
                                    december.compatibility)
              == IdentityMismatch::ProfileId);
    QNP_CHECK(compatibilityMismatch(december.compatibility,
                                    november.compatibility)
              == IdentityMismatch::ProfileId);

    QNP_CHECK(november.executable.sha256 != december.executable.sha256);
    QNP_CHECK(november.content.identity.simulationRootSha256
              != december.content.identity.simulationRootSha256);
}

QNP_TEST(ExecutableIdentityAloneCannotSatisfyContentIdentity) {
    SimulationContentIdentity noContent{};
    noContent.entryCount = 68u;
    noContent.requiredEntryCount = 68u;
    noContent.manifestFlags = ManifestFlagComplete;
    QNP_CHECK(!noContent.complete());
    ManifestResult executableOnly{};
    executableOnly.profile = RuntimeProfile::November1126;
    executableOnly.qualification = ProfileQualification::Qualified;
    executableOnly.identity = noContent;
    QNP_CHECK(!executableOnly.admissionAllowed());
}

QNP_TEST(ContentManifestSerializationIsByteForByteDeterministic) {
    std::vector<ManifestEntry> entries{
        makeEntry("B\\COM_B.FOB", 2u, true, false),
        makeEntry("a/a.chr", 1u, true, true),
    };
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    std::string error;
    QNP_CHECK(SimulationContentManifestBuilder::canonicalSerialize(entries, false, first, error));
    std::reverse(entries.begin(), entries.end());
    QNP_CHECK(SimulationContentManifestBuilder::canonicalSerialize(entries, false, second, error));
    QNP_CHECK(first == second);
}

QNP_TEST(ContentManifestCacheOptimizationNeverAcceptsStaleBytes) {
    DisposableFixture fixture;
    populateFixture(fixture);
    const std::filesystem::path chr = fixture.path() / "fighters/a.chr";
    const auto originalTime = std::filesystem::last_write_time(chr);
    ManifestBuildOptions diagnosticOptions{};
    diagnosticOptions.verifyActualBytes = false;
    const ManifestResult before = SimulationContentManifestBuilder::buildWithRules(
        fixture.path(), fixtureRules(), diagnosticOptions);
    QNP_CHECK(!before.bytesVerifiedForAdmission);
    fixture.write("fighters/a.chr", "fighter-authored-frameZ");
    std::error_code error;
    std::filesystem::last_write_time(chr, originalTime, error);
    QNP_CHECK(!error);
    const ManifestResult cached = SimulationContentManifestBuilder::buildWithRules(
        fixture.path(), fixtureRules(), diagnosticOptions);
    QNP_CHECK(!cached.bytesVerifiedForAdmission);
    QNP_CHECK(before.identity.simulationRootSha256
              == cached.identity.simulationRootSha256);
    const ManifestResult strict = buildFixture(fixture);
    QNP_CHECK(strict.bytesVerifiedForAdmission);
    QNP_CHECK(before.identity.simulationRootSha256
              != strict.identity.simulationRootSha256);
}

QNP_TEST(ContentManifestDiagnosticsExposeNoAbsolutePaths) {
    DisposableFixture fixture;
    populateFixture(fixture);
    fixture.write("fighters/unknown_extra.chr", "unexpected");
    const ManifestResult result = buildFixture(fixture);
    const std::string json = formatManifestDiagnosticJson(result);
    QNP_CHECK(json.find(fixture.path().string()) == std::string::npos);
    QNP_CHECK(json.find("fighters/unknown_extra.chr") != std::string::npos);
    QNP_CHECK(json.find("unreviewedPotentiallyRelevantPaths")
              != std::string::npos);
}

QNP_TEST(ContentManifestNeverWritesAuthoritativeGameRoots) {
    const std::filesystem::path november = std::filesystem::u8path(QOH99_NOVEMBER_GAME_ROOT);
    const std::filesystem::path december = std::filesystem::u8path(QOH99_DECEMBER_GAME_ROOT);
    std::error_code error;
    const auto novemberTime = std::filesystem::last_write_time(november / "System" / "Obj1.chr", error);
    QNP_REQUIRE(!error);
    const auto decemberTime = std::filesystem::last_write_time(december / "SYSTEM" / "OBJ1.CHR", error);
    QNP_REQUIRE(!error);
    (void)SimulationContentManifestBuilder::build(november, RuntimeProfile::November1126);
    (void)SimulationContentManifestBuilder::build(december, RuntimeProfile::DecemberCandidate);
    QNP_CHECK(std::filesystem::last_write_time(november / "System" / "Obj1.chr", error) == novemberTime);
    QNP_CHECK(!error);
    QNP_CHECK(std::filesystem::last_write_time(december / "SYSTEM" / "OBJ1.CHR", error) == decemberTime);
    QNP_CHECK(!error);
}

QNP_TEST(OnlyCharacterDataDecidesTheComparedContentRoots) {
    DisposableFixture baseline;
    populateFixture(baseline);
    const ManifestResult original = buildFixture(baseline);
    QNP_REQUIRE(original.identity.complete());

    for (const char* const path : {"ai/com_a.fob", "system/system.bin",
                                   "system/winmes/mik_mes7.img",
                                   "system/winmes/tir_mes1.img"}) {
        DisposableFixture altered;
        populateFixture(altered);
        altered.write(path, "different-bytes-entirely");
        const ManifestResult result = buildFixture(altered);
        QNP_CHECK(result.identity.simulationRootSha256
                  == original.identity.simulationRootSha256);
        QNP_CHECK(result.identity.validationRootSha256
                  == original.identity.validationRootSha256);
        QNP_CHECK(result.identity.legacySimulationRootSha256
                  != original.identity.legacySimulationRootSha256
                  || result.identity.legacyValidationRootSha256
                      != original.identity.legacyValidationRootSha256);
    }

    DisposableFixture chrChanged;
    populateFixture(chrChanged);
    chrChanged.write("fighters/a.chr", "different-hitboxes-and-frame-data");
    const ManifestResult chr = buildFixture(chrChanged);
    QNP_CHECK(chr.identity.simulationRootSha256
              != original.identity.simulationRootSha256);
    QNP_CHECK(chr.identity.validationRootSha256
              != original.identity.validationRootSha256);
}

QNP_TEST(LegacyContentRootsReproduceThePreNarrowingRecipeExactly) {
    DisposableFixture fixture;
    populateFixture(fixture);
    const ManifestResult result = buildFixture(fixture);
    QNP_REQUIRE(result.identity.complete());
    QNP_CHECK(!isZeroDigest(result.identity.legacySimulationRootSha256));
    QNP_CHECK(!isZeroDigest(result.identity.legacyValidationRootSha256));
    QNP_CHECK(result.identity.legacySimulationRootSha256
              != result.identity.simulationRootSha256);
    QNP_CHECK(result.identity.legacyValidationRootSha256
              != result.identity.validationRootSha256);

    std::vector<ManifestEntry> simulation;
    std::vector<ManifestEntry> validation;
    for (const ManifestEntry& entry : result.entries) {
        if (entry.requiredForSession) simulation.push_back(entry);
        if (entry.requiredForNativeValidation) validation.push_back(entry);
    }
    Digest expectedSimulation{};
    Digest expectedValidation{};
    std::string error;
    QNP_REQUIRE(SimulationContentManifestBuilder::canonicalRoot(
        simulation, false, expectedSimulation, error, true));
    QNP_REQUIRE(SimulationContentManifestBuilder::canonicalRoot(
        validation, true, expectedValidation, error, true));
    QNP_CHECK(result.identity.legacySimulationRootSha256 == expectedSimulation);
    QNP_CHECK(result.identity.legacyValidationRootSha256 == expectedValidation);
}

QNP_TEST(AuthoritativeInstallationsStillReproduceTheirShippedLegacyRoots) {
    const ManifestResult november = SimulationContentManifestBuilder::build(
        std::filesystem::u8path(QOH99_NOVEMBER_GAME_ROOT),
        RuntimeProfile::November1126);
    const ManifestResult december = SimulationContentManifestBuilder::build(
        std::filesystem::u8path(QOH99_DECEMBER_GAME_ROOT),
        RuntimeProfile::DecemberCandidate);
    QNP_REQUIRE(november.admissionAllowed());
    QNP_REQUIRE(december.admissionAllowed());
    QNP_CHECK(digestHex(november.identity.legacySimulationRootSha256)
              == "E974A8B34AF6C8C696D075E0A748914317CD60107D47D8384D0EDF79E9822EC6");
    QNP_CHECK(digestHex(december.identity.legacySimulationRootSha256)
              == "FEA0EEDA415D39776D26DAE2EDBDEC6A01F8337890F36811A2EF09CEC299C35C");
}
