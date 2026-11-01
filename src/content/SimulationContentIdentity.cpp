#include "SimulationContentIdentity.h"

#include "SimulationContentCatalog.generated.hpp"
#include "replay/ReplayIntegrity.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace qoh99::content {
namespace {

constexpr char kSimulationDomain[] = "qoh99-simulation-content-v2-chr";
constexpr char kValidationDomain[] = "qoh99-native-validation-content-v2-chr";

// This spelling is frozen: it must recompute values stored by older recordings byte for byte.
constexpr char kSimulationDomainV1[] = "qoh99-simulation-content-v1";
constexpr char kValidationDomainV1[] = "qoh99-native-validation-content-v1";

struct CachedFileDigest {
    std::uint64_t sizeBytes = 0u;
    std::filesystem::file_time_type lastWrite{};
    Digest digest{};
};

std::mutex g_fileHashCacheMutex;
std::unordered_map<std::string, CachedFileDigest> g_fileHashCache;

void appendLe64(std::vector<std::uint8_t>& out, const std::uint64_t value) {
    for (unsigned index = 0; index < 8u; ++index)
        out.push_back(static_cast<std::uint8_t>(value >> (index * 8u)));
}

bool isCharacterData(const std::string& path) {
    static constexpr char kChr[] = ".chr";
    constexpr std::size_t kLength = sizeof(kChr) - 1u;
    return path.size() > kLength
        && path.compare(path.size() - kLength, kLength, kChr) == 0;
}

bool hasFacet(const ManifestEntry& entry, const bool validationRoot,
              const bool legacy = false) {
    const bool required = validationRoot ? entry.requiredForNativeValidation
                                         : entry.requiredForSession;
    if (legacy) return required;
    return required && isCharacterData(entry.normalizedPath);
}

void addBounded(std::vector<std::string>& target, const std::string& value,
                const std::size_t maximum) {
    if (target.size() < maximum) target.push_back(value);
}

std::string cacheKeyForPath(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal().generic_u8string();
}

bool hashFile(const std::filesystem::path& path, Digest& digest,
              std::uint64_t& sizeBytes, std::string& error,
              const bool verifyActualBytes, bool& readActualBytes) {
    readActualBytes = false;
    std::error_code fsError;
    sizeBytes = std::filesystem::file_size(path, fsError);
    if (fsError) {
        error = "could not determine file size";
        return false;
    }
    const std::filesystem::file_time_type lastWrite =
        std::filesystem::last_write_time(path, fsError);
    const bool metadataAvailable = !fsError;
    const std::string cacheKey = metadataAvailable ? cacheKeyForPath(path)
                                                   : std::string();
    if (!verifyActualBytes && metadataAvailable) {
        std::lock_guard<std::mutex> lock(g_fileHashCacheMutex);
        const auto cached = g_fileHashCache.find(cacheKey);
        if (cached != g_fileHashCache.end()
            && cached->second.sizeBytes == sizeBytes
            && cached->second.lastWrite == lastWrite) {
            digest = cached->second.digest;
            return true;
        }
    }
    std::ifstream source(path, std::ios::binary);
    if (!source) {
        error = "could not open file for byte hashing";
        return false;
    }
    qreplay::Sha256 sha;
    std::array<std::uint8_t, 256u * 1024u> buffer{};
    while (source) {
        source.read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = source.gcount();
        if (read > 0)
            sha.update(buffer.data(), static_cast<std::size_t>(read));
    }
    if (!source.eof()) {
        error = "could not read complete file for byte hashing";
        return false;
    }
    sha.finish(digest);
    readActualBytes = true;
    if (metadataAvailable) {
        std::lock_guard<std::mutex> lock(g_fileHashCacheMutex);
        g_fileHashCache[cacheKey] = CachedFileDigest{ sizeBytes, lastWrite, digest };
    }
    return true;
}

bool sameBytes(const Digest& left, const Digest& right) {
    return left == right;
}

const ExpectedProfileManifest* expectedProfile(const RuntimeProfile profile) {
    for (const ExpectedProfileManifest& candidate :
         generated::kExpectedProfiles) {
        if (candidate.profile == profile) return &candidate;
    }
    return nullptr;
}

std::vector<ContentRule> generatedRules() {
    std::vector<ContentRule> rules;
    rules.reserve(std::size(generated::kExpectedContentEntries));
    for (const ExpectedContentEntry& expected :
         generated::kExpectedContentEntries) {
        ContentRule rule{};
        rule.normalizedPath = expected.normalizedPath;
        rule.classification = expected.classification;
        rule.requiredForSession = expected.requiredForSession;
        rule.requiredForNativeValidation = expected.requiredForNativeValidation;
        rules.push_back(std::move(rule));
    }
    return rules;
}

const ExpectedContentEntry* expectedEntryForPath(const std::string& path) {
    for (const ExpectedContentEntry& expected :
         generated::kExpectedContentEntries) {
        if (path == expected.normalizedPath) return &expected;
    }
    return nullptr;
}

bool isPotentiallyRelevantExtraResource(const std::string& normalizedPath) {
    const std::size_t dot = normalizedPath.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string extension = normalizedPath.substr(dot);
    return extension == ".chr" || extension == ".fob";
}

std::string jsonEscape(const std::string& text) {
    std::ostringstream result;
    for (const unsigned char ch : text) {
        switch (ch) {
            case '\\': result << "\\\\"; break;
            case '"': result << "\\\""; break;
            case '\n': result << "\\n"; break;
            case '\r': result << "\\r"; break;
            case '\t': result << "\\t"; break;
            default:
                if (ch < 0x20u) {
                    result << "\\u" << std::hex << std::setw(4)
                           << std::setfill('0') << static_cast<unsigned>(ch)
                           << std::dec << std::setfill(' ');
                } else {
                    result << static_cast<char>(ch);
                }
                break;
        }
    }
    return result.str();
}

void appendJsonPaths(std::ostringstream& out,
                     const std::vector<std::string>& paths) {
    out << '[';
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (index != 0u) out << ',';
        out << '"' << jsonEscape(paths[index]) << '"';
    }
    out << ']';
}

}

bool SimulationContentIdentity::complete() const {
    return schemaVersion == kSimulationContentSchemaVersion
        && !isZeroDigest(simulationRootSha256)
        && !isZeroDigest(validationRootSha256)
        && entryCount != 0u && requiredEntryCount != 0u
        && (manifestFlags & ManifestFlagComplete) != 0u
        && (manifestFlags & (ManifestFlagMissingRequired
                             | ManifestFlagUnreadableRequired
                             | ManifestFlagDuplicateNormalizedPath
                             | ManifestFlagExtraSimulationResource
                             | ManifestFlagRootMismatch
                             | ManifestFlagNativeValidationMismatch
                             | ManifestFlagExtraScanIncomplete
                             | ManifestFlagUnreviewedPotentiallyRelevantExtra)) == 0u;
}

bool ManifestResult::admissionAllowed() const {
    return disposition == InstallationDisposition::KnownQualifiedManifest
        && identity.complete() && qualification == ProfileQualification::Qualified
        && bytesVerifiedForAdmission;
}

bool normalizeWindowsRelativePath(const std::string& raw,
                                  std::string& normalized,
                                  std::string& error) {
    normalized.clear();
    error.clear();
    if (raw.empty()) {
        error = "relative path is empty";
        return false;
    }
    std::string component;
    const auto commit = [&]() -> bool {
        if (component.empty() || component == "." || component == "..") {
            error = "relative path has an empty or traversal component";
            return false;
        }
        if (!normalized.empty()) normalized.push_back('/');
        for (const unsigned char ch : component) {
            if (ch == ':' || ch < 0x20u) {
                error = "relative path has an invalid character";
                return false;
            }
            normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
        component.clear();
        return true;
    };
    if (raw[0] == '/' || raw[0] == '\\'
        || (raw.size() >= 2u && raw[1] == ':')) {
        error = "absolute path is not permitted";
        return false;
    }
    for (const char ch : raw) {
        if (ch == '/' || ch == '\\') {
            if (!commit()) return false;
        } else {
            component.push_back(ch);
        }
    }
    return commit();
}

const char* profileKey(const RuntimeProfile profile) {
    switch (profile) {
        case RuntimeProfile::November1126: return "qoh99-se1126";
        case RuntimeProfile::DecemberCandidate: return "qoh99-december-candidate";
        default: return "unknown";
    }
}

const char* classificationName(const ContentClassification classification) {
    switch (classification) {
        case ContentClassification::SimulationRequired:
            return "SIMULATION_REQUIRED";
        case ContentClassification::NativeValidationRequired:
            return "NATIVE_VALIDATION_REQUIRED";
        case ContentClassification::PresentationOnly:
            return "PRESENTATION_ONLY";
        case ContentClassification::ToolOrDocumentation:
            return "TOOL_OR_DOCUMENTATION";
        case ContentClassification::CacheOrRuntimeOutput:
            return "CACHE_OR_RUNTIME_OUTPUT";
        case ContentClassification::UnknownRequiresReview:
            return "UNKNOWN_REQUIRES_REVIEW";
    }
    return "UNKNOWN_REQUIRES_REVIEW";
}

const char* dispositionName(const InstallationDisposition disposition) {
    switch (disposition) {
        case InstallationDisposition::KnownQualifiedManifest:
            return "known-qualified-manifest";
        case InstallationDisposition::KnownCandidateManifest:
            return "known-candidate-manifest";
        case InstallationDisposition::LocallyModifiedKnownBuild:
            return "locally-modified-known-build";
        case InstallationDisposition::IncompleteInstallation:
            return "incomplete-installation";
        case InstallationDisposition::UnknownInstallation:
            return "unknown-installation";
    }
    return "unknown-installation";
}

bool isZeroDigest(const Digest& value) {
    return std::all_of(value.begin(), value.end(),
                       [](const std::uint8_t byte) { return byte == 0u; });
}

std::string digestHex(const Digest& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string text;
    text.resize(value.size() * 2u);
    for (std::size_t index = 0; index < value.size(); ++index) {
        text[index * 2u] = kHex[value[index] >> 4u];
        text[index * 2u + 1u] = kHex[value[index] & 0x0Fu];
    }
    return text;
}

bool SimulationContentManifestBuilder::canonicalSerialize(
    const std::vector<ManifestEntry>& entries, const bool validationRoot,
    std::vector<std::uint8_t>& out, std::string& error, const bool legacy) {
    out.clear();
    error.clear();
    const char* domain = legacy
        ? (validationRoot ? kValidationDomainV1 : kSimulationDomainV1)
        : (validationRoot ? kValidationDomain : kSimulationDomain);
    out.insert(out.end(), domain, domain + std::char_traits<char>::length(domain));

    std::vector<ManifestEntry> ordered;
    ordered.reserve(entries.size());
    for (const ManifestEntry& original : entries) {
        if (!hasFacet(original, validationRoot, legacy)) continue;
        ManifestEntry entry = original;
        std::string normalized;
        if (!normalizeWindowsRelativePath(entry.normalizedPath, normalized, error))
            return false;
        entry.normalizedPath = std::move(normalized);
        ordered.push_back(std::move(entry));
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const ManifestEntry& left, const ManifestEntry& right) {
                  return left.normalizedPath < right.normalizedPath;
              });
    std::string previous;
    for (const ManifestEntry& entry : ordered) {
        if (entry.normalizedPath == previous) {
            error = "duplicate normalized path: " + entry.normalizedPath;
            return false;
        }
        previous = entry.normalizedPath;
        out.insert(out.end(), entry.normalizedPath.begin(),
                   entry.normalizedPath.end());
        out.push_back(0x1Fu);
        const char* classification = classificationName(entry.classification);
        out.insert(out.end(), classification,
                   classification + std::char_traits<char>::length(classification));
        out.push_back(0x1Fu);
        out.push_back(entry.requiredForSession ? static_cast<std::uint8_t>('1')
                                               : static_cast<std::uint8_t>('0'));
        out.push_back(0x1Fu);
        out.push_back(entry.requiredForNativeValidation
                          ? static_cast<std::uint8_t>('1')
                          : static_cast<std::uint8_t>('0'));
        out.push_back(0x1Fu);
        appendLe64(out, entry.sizeBytes);
        out.insert(out.end(), entry.sha256.begin(), entry.sha256.end());
        out.push_back(0x1Eu);
    }
    return true;
}

bool SimulationContentManifestBuilder::canonicalRoot(
    const std::vector<ManifestEntry>& entries, const bool validationRoot,
    Digest& root, std::string& error, const bool legacy) {
    std::vector<std::uint8_t> encoded;
    if (!canonicalSerialize(entries, validationRoot, encoded, error, legacy)) {
        root = Digest{};
        return false;
    }
    root = qreplay::sha256(encoded.data(), encoded.size());
    return true;
}

ManifestResult SimulationContentManifestBuilder::build(
    const std::filesystem::path& gameDirectory, const RuntimeProfile profile,
    const ManifestBuildOptions& options) {
    ManifestResult result = buildWithRules(gameDirectory, generatedRules(), options);
    result.profile = profile;
    const ExpectedProfileManifest* expected = expectedProfile(profile);
    if (!expected) {
        result.qualification = ProfileQualification::Unknown;
        result.identity.manifestFlags |= ManifestFlagUnknownProfile;
        result.disposition = result.identity.complete()
            ? InstallationDisposition::UnknownInstallation
            : InstallationDisposition::IncompleteInstallation;
        if (result.detail.empty()) result.detail = "runtime profile has no generated content catalogue";
        return result;
    }

    result.qualification = expected->qualification;
    if ((result.identity.manifestFlags & (ManifestFlagMissingRequired
                                          | ManifestFlagUnreadableRequired
                                          | ManifestFlagDuplicateNormalizedPath
                                          | ManifestFlagExtraScanIncomplete)) != 0u) {
        result.disposition = InstallationDisposition::IncompleteInstallation;
        return result;
    }
    if ((result.identity.manifestFlags
         & (ManifestFlagExtraSimulationResource
            | ManifestFlagUnreviewedPotentiallyRelevantExtra)) != 0u) {
        result.disposition = InstallationDisposition::LocallyModifiedKnownBuild;
        if (result.detail.empty()) {
            result.detail = "extra simulation resource or unreviewed potentially relevant resource is present";
        }
        return result;
    }

    for (const ManifestEntry& entry : result.entries) {
        const ExpectedContentEntry* expectedEntry =
            expectedEntryForPath(entry.normalizedPath);
        if (!expectedEntry) continue;
        const bool november = profile == RuntimeProfile::November1126;
        const std::uint64_t expectedSize = november
            ? expectedEntry->novemberSize : expectedEntry->decemberSize;
        const Digest& expectedHash = november
            ? expectedEntry->novemberSha256 : expectedEntry->decemberSha256;
        if (entry.sizeBytes != expectedSize
            || !sameBytes(entry.sha256, expectedHash)) {
            addBounded(result.mismatchedPaths, entry.normalizedPath,
                       options.maximumDiagnosticPaths);
        }
    }
    const bool rootsMatch = result.identity.simulationRootSha256
            == expected->simulationRoot
        && result.identity.validationRootSha256 == expected->validationRoot
        && result.identity.entryCount == expected->entryCount
        && result.identity.requiredEntryCount == expected->requiredEntryCount;
    if (!rootsMatch || !result.mismatchedPaths.empty()) {
        result.identity.manifestFlags |= ManifestFlagRootMismatch;
        result.disposition = InstallationDisposition::LocallyModifiedKnownBuild;
        if (result.detail.empty())
            result.detail = "required simulation-content bytes do not match the selected profile";
        return result;
    }

    result.identity.manifestFlags |= ManifestFlagComplete;
    result.disposition = expected->qualification == ProfileQualification::Qualified
        ? InstallationDisposition::KnownQualifiedManifest
        : InstallationDisposition::KnownCandidateManifest;
    if (!options.verifyActualBytes) {
        result.detail = "matches the profile content manifest from a metadata-cache diagnostic; "
            "fresh byte verification is required before admission";
    } else {
        result.detail = result.disposition == InstallationDisposition::KnownQualifiedManifest
            ? "matches the qualified profile content manifest"
            : "matches the candidate content manifest; candidate admission remains disabled";
    }
    return result;
}

ManifestResult SimulationContentManifestBuilder::buildWithRules(
    const std::filesystem::path& gameDirectory,
    const std::vector<ContentRule>& rules,
    const ManifestBuildOptions& options) {
    ManifestResult result{};
    result.identity.schemaVersion = kSimulationContentSchemaVersion;
    std::set<std::string> knownPaths;
    std::vector<ContentRule> normalizedRules;
    normalizedRules.reserve(rules.size());
    for (const ContentRule& rawRule : rules) {
        ContentRule rule = rawRule;
        std::string pathError;
        const std::string rawPath = rule.normalizedPath;
        std::string canonicalPath;
        if (!normalizeWindowsRelativePath(rawPath, canonicalPath, pathError))
        {
            result.identity.manifestFlags |= ManifestFlagDuplicateNormalizedPath;
            result.detail = pathError;
            return result;
        }
        rule.normalizedPath = std::move(canonicalPath);
        if (!knownPaths.insert(rule.normalizedPath).second) {
            result.identity.manifestFlags |= ManifestFlagDuplicateNormalizedPath;
            result.detail = "duplicate normalized required path: "
                + rule.normalizedPath;
            return result;
        }
        normalizedRules.push_back(std::move(rule));
    }
    std::error_code fsError;
    if (!std::filesystem::is_directory(gameDirectory, fsError) || fsError) {
        result.identity.manifestFlags |= ManifestFlagMissingRequired;
        result.detail = "game directory is unavailable";
        return result;
    }

    for (const ContentRule& rule : normalizedRules) {
        const std::filesystem::path fullPath = gameDirectory
            / std::filesystem::u8path(rule.normalizedPath);
        std::error_code existsError;
        if (!std::filesystem::is_regular_file(fullPath, existsError)
            || existsError) {
            if (rule.requiredForSession || rule.requiredForNativeValidation) {
                result.identity.manifestFlags |= ManifestFlagMissingRequired;
                addBounded(result.missingPaths, rule.normalizedPath,
                           options.maximumDiagnosticPaths);
            }
            continue;
        }
        ManifestEntry entry{};
        entry.normalizedPath = rule.normalizedPath;
        entry.classification = rule.classification;
        entry.requiredForSession = rule.requiredForSession;
        entry.requiredForNativeValidation = rule.requiredForNativeValidation;
        std::string readError;
        bool readActualBytes = false;
        if (!hashFile(fullPath, entry.sha256, entry.sizeBytes, readError,
                      options.verifyActualBytes, readActualBytes)) {
            result.identity.manifestFlags |= ManifestFlagUnreadableRequired;
            addBounded(result.missingPaths, rule.normalizedPath,
                       options.maximumDiagnosticPaths);
            if (result.detail.empty())
                result.detail = "unreadable required file: " + rule.normalizedPath;
            continue;
        }
        if (!readActualBytes) result.bytesVerifiedForAdmission = false;
        result.entries.push_back(std::move(entry));
    }

    if (options.scanForExtraSimulationResources) {
        std::filesystem::recursive_directory_iterator iterator(gameDirectory,
                                                                 fsError);
        const std::filesystem::recursive_directory_iterator end;
        while (!fsError && iterator != end) {
            const std::filesystem::directory_entry current = *iterator;
            iterator.increment(fsError);
            if (fsError) break;
            std::error_code typeError;
            const bool regularFile = current.is_regular_file(typeError);
            if (typeError) {
                fsError = typeError;
                break;
            }
            if (!regularFile) continue;
            std::string raw = current.path().lexically_relative(gameDirectory)
                .generic_u8string();
            std::string path;
            std::string pathError;
            if (!normalizeWindowsRelativePath(raw, path, pathError)) {
                result.identity.manifestFlags |= ManifestFlagExtraScanIncomplete;
                if (result.detail.empty())
                    result.detail = "could not normalize an installation path during extra-resource scan";
                break;
            }
            if (knownPaths.count(path) != 0u
                || !isPotentiallyRelevantExtraResource(path)) continue;
            result.identity.manifestFlags
                |= ManifestFlagUnreviewedPotentiallyRelevantExtra;
            addBounded(result.unreviewedPotentiallyRelevantPaths, path,
                       options.maximumDiagnosticPaths);
        }
        if (fsError) {
            result.identity.manifestFlags |= ManifestFlagExtraScanIncomplete;
            if (result.detail.empty())
                result.detail = "could not enumerate installation for extra simulation resources";
        }
    }

    std::string rootError;
    if (!canonicalRoot(result.entries, false, result.identity.simulationRootSha256,
                       rootError)
        || !canonicalRoot(result.entries, true, result.identity.validationRootSha256,
                          rootError)) {
        result.identity.manifestFlags |= ManifestFlagDuplicateNormalizedPath;
        result.detail = rootError;
        return result;
    }
    std::string legacyError;
    if (!canonicalRoot(result.entries, false,
                       result.identity.legacySimulationRootSha256, legacyError,
                       true)
        || !canonicalRoot(result.entries, true,
                          result.identity.legacyValidationRootSha256,
                          legacyError, true)) {
        result.identity.legacySimulationRootSha256 = Digest{};
        result.identity.legacyValidationRootSha256 = Digest{};
    }
    result.identity.entryCount = static_cast<std::uint32_t>(
        std::count_if(result.entries.begin(), result.entries.end(),
                      [](const ManifestEntry& entry) {
                          return entry.requiredForSession;
                      }));
    result.identity.requiredEntryCount = static_cast<std::uint32_t>(
        std::count_if(normalizedRules.begin(), normalizedRules.end(),
                      [](const ContentRule& rule) {
                          return rule.requiredForSession;
                      }));
    const std::uint32_t incompleteFlags = ManifestFlagMissingRequired
        | ManifestFlagUnreadableRequired | ManifestFlagDuplicateNormalizedPath
        | ManifestFlagExtraScanIncomplete;
    const std::uint32_t unsafeExtraFlags = ManifestFlagExtraSimulationResource
        | ManifestFlagUnreviewedPotentiallyRelevantExtra;
    if ((result.identity.manifestFlags & incompleteFlags) == 0u
        && (result.identity.manifestFlags & unsafeExtraFlags) == 0u) {
        result.identity.manifestFlags |= ManifestFlagComplete;
        result.disposition = InstallationDisposition::UnknownInstallation;
        if (result.detail.empty()) result.detail = "content manifest is complete but not associated with a qualified profile";
    } else {
        result.disposition = (result.identity.manifestFlags & incompleteFlags) != 0u
            ? InstallationDisposition::IncompleteInstallation
            : InstallationDisposition::LocallyModifiedKnownBuild;
        if (result.detail.empty()) {
            result.detail = result.disposition
                == InstallationDisposition::IncompleteInstallation
                ? "required simulation content is missing, unreadable, duplicated, or scan coverage is incomplete"
                : "extra simulation resource or unreviewed potentially relevant resource is present";
        }
    }
    result.bytesVerifiedForAdmission = options.verifyActualBytes
        && (result.identity.manifestFlags & (ManifestFlagMissingRequired
                                             | ManifestFlagUnreadableRequired
                                             | ManifestFlagDuplicateNormalizedPath
                                             | ManifestFlagExtraScanIncomplete)) == 0u;
    return result;
}

std::string formatManifestDiagnosticHuman(const ManifestResult& result) {
    std::ostringstream out;
    out << "profile=" << profileKey(result.profile)
        << " qualification="
        << (result.qualification == ProfileQualification::Qualified ? "Qualified"
            : result.qualification == ProfileQualification::CandidateUnqualified
                ? "CandidateUnqualified" : "Unknown")
        << " disposition=" << dispositionName(result.disposition)
        << " simulationRoot=" << digestHex(result.identity.simulationRootSha256)
        << " validationRoot=" << digestHex(result.identity.validationRootSha256)
        << " entries=" << result.identity.entryCount
        << " required=" << result.identity.requiredEntryCount
        << " flags=0x" << std::hex << result.identity.manifestFlags << std::dec
        << " admission=" << (result.admissionAllowed() ? "permitted" : "denied")
        << " reason=" << result.detail;
    const auto paths = [&out](const char* label,
                              const std::vector<std::string>& values) {
        if (values.empty()) return;
        out << " " << label << "=";
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0u) out << ',';
            out << values[index];
        }
    };
    paths("mismatched", result.mismatchedPaths);
    paths("missing", result.missingPaths);
    paths("unexpected", result.unexpectedSimulationPaths);
    paths("unreviewedPotentiallyRelevant",
          result.unreviewedPotentiallyRelevantPaths);
    return out.str();
}

std::string formatManifestDiagnosticJson(const ManifestResult& result) {
    std::ostringstream out;
    out << "{\"schemaVersion\":" << result.identity.schemaVersion
        << ",\"profileId\":\"" << profileKey(result.profile) << "\""
        << ",\"disposition\":\"" << dispositionName(result.disposition) << "\""
        << ",\"simulationRootSha256\":\"" << digestHex(result.identity.simulationRootSha256) << "\""
        << ",\"validationRootSha256\":\"" << digestHex(result.identity.validationRootSha256) << "\""
        << ",\"entryCount\":" << result.identity.entryCount
        << ",\"requiredEntryCount\":" << result.identity.requiredEntryCount
        << ",\"manifestFlags\":" << result.identity.manifestFlags
        << ",\"admissionPermitted\":" << (result.admissionAllowed() ? "true" : "false")
        << ",\"reason\":\"" << jsonEscape(result.detail) << "\""
        << ",\"mismatchedPaths\":";
    appendJsonPaths(out, result.mismatchedPaths);
    out << ",\"missingPaths\":";
    appendJsonPaths(out, result.missingPaths);
    out << ",\"unexpectedSimulationPaths\":";
    appendJsonPaths(out, result.unexpectedSimulationPaths);
    out << ",\"unreviewedPotentiallyRelevantPaths\":";
    appendJsonPaths(out, result.unreviewedPotentiallyRelevantPaths);
    out << '}';
    return out.str();
}

}
