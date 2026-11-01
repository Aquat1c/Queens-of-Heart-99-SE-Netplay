#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace qoh99::content {

using Digest = std::array<std::uint8_t, 32>;

constexpr std::uint16_t kSimulationContentSchemaVersion = 2u;

constexpr std::uint16_t kSupersededSimulationContentSchemaVersion = 1u;
constexpr std::size_t kMaximumDiagnosticPaths = 16u;

enum class RuntimeProfile : std::uint16_t {
    Unknown = 0u,
    November1126 = 1u,
    DecemberCandidate = 2u,
};

enum class ProfileQualification : std::uint8_t {
    Unknown = 0u,
    Qualified = 1u,
    CandidateUnqualified = 2u,
};

enum class ContentClassification : std::uint8_t {
    SimulationRequired = 1u,
    NativeValidationRequired = 2u,
    PresentationOnly = 3u,
    ToolOrDocumentation = 4u,
    CacheOrRuntimeOutput = 5u,
    UnknownRequiresReview = 6u,
};

enum ManifestFlag : std::uint32_t {
    ManifestFlagNone = 0u,
    ManifestFlagComplete = 1u << 0u,
    ManifestFlagMissingRequired = 1u << 1u,
    ManifestFlagUnreadableRequired = 1u << 2u,
    ManifestFlagDuplicateNormalizedPath = 1u << 3u,
    ManifestFlagExtraSimulationResource = 1u << 4u,
    ManifestFlagUnknownProfile = 1u << 5u,
    ManifestFlagRootMismatch = 1u << 6u,
    ManifestFlagNativeValidationMismatch = 1u << 7u,
    ManifestFlagExtraScanIncomplete = 1u << 8u,
    ManifestFlagUnreviewedPotentiallyRelevantExtra = 1u << 9u,
};

struct ContentRule {
    std::string normalizedPath;
    ContentClassification classification =
        ContentClassification::UnknownRequiresReview;
    bool requiredForSession = false;
    bool requiredForNativeValidation = false;
};

struct ExpectedContentEntry {
    const char* normalizedPath = nullptr;
    ContentClassification classification =
        ContentClassification::UnknownRequiresReview;
    bool requiredForSession = false;
    bool requiredForNativeValidation = false;
    std::uint64_t novemberSize = 0u;
    Digest novemberSha256{};
    std::uint64_t decemberSize = 0u;
    Digest decemberSha256{};
};

struct ExpectedProfileManifest {
    RuntimeProfile profile = RuntimeProfile::Unknown;
    const char* profileKey = nullptr;
    ProfileQualification qualification = ProfileQualification::Unknown;
    Digest simulationRoot{};
    Digest validationRoot{};
    std::uint32_t entryCount = 0u;
    std::uint32_t requiredEntryCount = 0u;
};

struct ManifestEntry {
    std::string normalizedPath;
    ContentClassification classification =
        ContentClassification::UnknownRequiresReview;
    bool requiredForSession = false;
    bool requiredForNativeValidation = false;
    std::uint64_t sizeBytes = 0u;
    Digest sha256{};
};

struct SimulationContentIdentity {
    std::uint16_t schemaVersion = kSimulationContentSchemaVersion;
    Digest simulationRootSha256{};
    Digest validationRootSha256{};
    std::uint32_t entryCount = 0u;
    std::uint32_t requiredEntryCount = 0u;
    std::uint32_t manifestFlags = ManifestFlagNone;

    Digest legacySimulationRootSha256{};
    Digest legacyValidationRootSha256{};

    bool complete() const;
};

enum class InstallationDisposition : std::uint8_t {
    UnknownInstallation = 0u,
    IncompleteInstallation = 1u,
    LocallyModifiedKnownBuild = 2u,
    KnownQualifiedManifest = 3u,
    KnownCandidateManifest = 4u,
};

struct ManifestResult {
    RuntimeProfile profile = RuntimeProfile::Unknown;
    ProfileQualification qualification = ProfileQualification::Unknown;
    InstallationDisposition disposition =
        InstallationDisposition::UnknownInstallation;
    SimulationContentIdentity identity{};
    std::vector<ManifestEntry> entries;
    std::vector<std::string> mismatchedPaths;
    std::vector<std::string> missingPaths;
    std::vector<std::string> unexpectedSimulationPaths;
    std::vector<std::string> unreviewedPotentiallyRelevantPaths;
    std::string detail;
    bool bytesVerifiedForAdmission = false;

    bool admissionAllowed() const;
};

struct ManifestBuildOptions {
    bool verifyActualBytes = true;
    bool scanForExtraSimulationResources = true;
    std::size_t maximumDiagnosticPaths = kMaximumDiagnosticPaths;
};

bool normalizeWindowsRelativePath(const std::string& raw,
                                  std::string& normalized,
                                  std::string& error);

const char* profileKey(RuntimeProfile profile);
const char* classificationName(ContentClassification classification);
const char* dispositionName(InstallationDisposition disposition);
bool isZeroDigest(const Digest& value);
std::string digestHex(const Digest& value);

class SimulationContentManifestBuilder {
public:
    static ManifestResult build(const std::filesystem::path& gameDirectory,
                                RuntimeProfile profile,
                                const ManifestBuildOptions& options = {});

    static ManifestResult buildWithRules(
        const std::filesystem::path& gameDirectory,
        const std::vector<ContentRule>& rules,
        const ManifestBuildOptions& options = {});

    static bool canonicalSerialize(const std::vector<ManifestEntry>& entries,
                                   bool validationRoot,
                                   std::vector<std::uint8_t>& out,
                                   std::string& error,
                                   bool legacy = false);

    static bool canonicalRoot(const std::vector<ManifestEntry>& entries,
                              bool validationRoot,
                              Digest& root,
                              std::string& error,
                              bool legacy = false);
};

std::string formatManifestDiagnosticHuman(const ManifestResult& result);
std::string formatManifestDiagnosticJson(const ManifestResult& result);

}
