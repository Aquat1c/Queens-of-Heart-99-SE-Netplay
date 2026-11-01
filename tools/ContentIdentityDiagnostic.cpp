#include "client/BuildIdentity.h"
#include "content/SimulationContentIdentity.h"
#include "content/SimulationContentCatalog.generated.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

qoh99::content::RuntimeProfile contentProfileFor(
    const qoh99::runtime_profile::RuntimeProfileId profile) {
    using qoh99::runtime_profile::RuntimeProfileId;
    switch (profile) {
    case RuntimeProfileId::Qoh99Se1126:
        return qoh99::content::RuntimeProfile::November1126;
    case RuntimeProfileId::Qoh99DecemberCandidate:
        return qoh99::content::RuntimeProfile::DecemberCandidate;
    default:
        return qoh99::content::RuntimeProfile::Unknown;
    }
}

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch >= 0x20u) out.push_back(static_cast<char>(ch));
            break;
        }
    }
    return out;
}

const char* qualificationName(
    const qoh99::content::ProfileQualification qualification) {
    using qoh99::content::ProfileQualification;
    switch (qualification) {
    case ProfileQualification::Qualified:
        return "Qualified";
    case ProfileQualification::CandidateUnqualified:
        return "CandidateUnqualified";
    case ProfileQualification::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

constexpr std::array<qoh99::content::ContentClassification, 6u>
    kClassificationOrder = {
        qoh99::content::ContentClassification::SimulationRequired,
        qoh99::content::ContentClassification::NativeValidationRequired,
        qoh99::content::ContentClassification::PresentationOnly,
        qoh99::content::ContentClassification::ToolOrDocumentation,
        qoh99::content::ContentClassification::CacheOrRuntimeOutput,
        qoh99::content::ContentClassification::UnknownRequiresReview,
    };

using ClassificationCounts = std::array<std::uint32_t, kClassificationOrder.size()>;

std::size_t classificationIndex(
    const qoh99::content::ContentClassification classification) {
    for (std::size_t index = 0u; index < kClassificationOrder.size(); ++index) {
        if (classification == kClassificationOrder[index]) return index;
    }
    return kClassificationOrder.size();
}

qoh99::content::ContentClassification classificationForPath(
    const std::string& path) {
    using qoh99::content::ContentClassification;
    for (const qoh99::content::ExpectedContentEntry& expected :
         qoh99::content::generated::kExpectedContentEntries) {
        if (path == expected.normalizedPath) return expected.classification;
    }
    if (path.rfind("logs/", 0u) == 0u || path.rfind("replays/", 0u) == 0u
        || path == "system/qoherror.txt") {
        return ContentClassification::CacheOrRuntimeOutput;
    }
    if (path.rfind("docs/", 0u) == 0u
        || path.rfind("licenses/", 0u) == 0u
        || path.rfind("manual/", 0u) == 0u
        || path.rfind("system/htm/doc/", 0u) == 0u) {
        return ContentClassification::ToolOrDocumentation;
    }
    static constexpr const char* kExplicitToolPaths[] = {
        "buttonbinds_windows_1.2.0.exe", "config.exe", "ddraw.dll",
        "qoh99.exe",
        "qoh99_netplay.dll",
        "qoh99caster.exe", "sdl3.dll",
    };
    for (const char* toolPath : kExplicitToolPaths) {
        if (path == toolPath) return ContentClassification::ToolOrDocumentation;
    }
    return ContentClassification::UnknownRequiresReview;
}

bool installationClassificationCounts(
    const std::filesystem::path& root, ClassificationCounts& counts,
    std::string& error) {
    counts = {};
    error.clear();
    std::error_code fsError;
    std::filesystem::recursive_directory_iterator iterator(root, fsError);
    const std::filesystem::recursive_directory_iterator end;
    while (!fsError && iterator != end) {
        const std::filesystem::directory_entry current = *iterator;
        iterator.increment(fsError);
        if (fsError) break;
        std::error_code typeError;
        const bool regular = current.is_regular_file(typeError);
        if (typeError) {
            fsError = typeError;
            break;
        }
        if (!regular) continue;
        std::string normalized;
        std::string pathError;
        if (!qoh99::content::normalizeWindowsRelativePath(
                current.path().lexically_relative(root).generic_u8string(),
                normalized, pathError)) {
            error = "could not normalize an installation path for classification";
            return false;
        }
        const std::size_t index = classificationIndex(
            classificationForPath(normalized));
        if (index >= counts.size()) {
            error = "classification policy returned an unknown category";
            return false;
        }
        ++counts[index];
    }
    if (fsError) {
        error = "could not enumerate the complete installation for classification";
        return false;
    }
    return true;
}

ClassificationCounts checkedManifestClassificationCounts(
    const qoh99::content::ManifestResult& manifest) {
    ClassificationCounts counts{};
    for (const qoh99::content::ManifestEntry& entry : manifest.entries) {
        for (std::size_t index = 0u; index < kClassificationOrder.size(); ++index) {
            if (entry.classification == kClassificationOrder[index]) {
                ++counts[index];
                break;
            }
        }
    }
    return counts;
}

void printClassificationCountsHuman(const ClassificationCounts& counts) {
    std::fputs(" classificationCounts=", stdout);
    for (std::size_t index = 0u; index < kClassificationOrder.size(); ++index) {
        if (index != 0u) std::fputc(',', stdout);
        std::printf("%s:%u",
                    qoh99::content::classificationName(kClassificationOrder[index]),
                    counts[index]);
    }
}

void printClassificationCountsJson(const ClassificationCounts& counts) {
    std::fputs("{", stdout);
    for (std::size_t index = 0u; index < kClassificationOrder.size(); ++index) {
        if (index != 0u) std::fputc(',', stdout);
        std::printf("\"%s\":%u",
                    qoh99::content::classificationName(kClassificationOrder[index]),
                    counts[index]);
    }
    std::fputs("}", stdout);
}

void usage() {
    std::fputs("usage: qoh99_content_identity_diagnostic [--json] <qoh99.exe>\n",
               stderr);
}

}

int main(int argc, char** argv) {
    bool json = false;
    const char* executable = nullptr;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--json") {
            json = true;
        } else if (executable == nullptr) {
            executable = argv[index];
        } else {
            usage();
            return 64;
        }
    }
    if (executable == nullptr) {
        usage();
        return 64;
    }

    qclient::ExecutableIdentity runtime{};
    std::string error;
    if (!qclient::detectKnownRuntimeExecutable(executable, &runtime, error)) {
        const ClassificationCounts counts{};
        if (json) {
            std::printf("{\"detected\":false,\"qualification\":\"Unknown\","
                        "\"classificationScope\":\"none\","
                        "\"classificationCounts\":");
            printClassificationCountsJson(counts);
            std::printf(",\"admissionPermitted\":false,\"reason\":\"%s\"}\n",
                        jsonEscape(error).c_str());
        } else {
            std::fputs("detected=false qualification=Unknown classificationScope=none", stdout);
            printClassificationCountsHuman(counts);
            std::printf(" admission=denied reason=%s\n", error.c_str());
        }
        return 2;
    }

    const qoh99::content::RuntimeProfile profile =
        contentProfileFor(runtime.runtimeProfileId);
    const std::filesystem::path gameDirectory =
        std::filesystem::u8path(executable).parent_path();
    const qoh99::content::ManifestResult manifest =
        qoh99::content::SimulationContentManifestBuilder::build(
            gameDirectory, profile);
    ClassificationCounts counts{};
    std::string classificationError;
    const bool fullClassification = installationClassificationCounts(
        gameDirectory, counts, classificationError);
    if (!fullClassification) counts = checkedManifestClassificationCounts(manifest);
    const char* const classificationScope = fullClassification
        ? "full-installation-inventory"
        : "checked-manifest-entries-incomplete-inventory";
    if (json) {
        std::printf("{\"detected\":true,\"executableSha256\":\"%s\","
                    "\"executableSize\":%llu,\"forensicPeTimestamp\":%u,"
                    "\"qualification\":\"%s\","
                    "\"classificationScope\":\"%s\","
                    "\"classificationCounts\":",
                    runtime.sha256.c_str(),
                    static_cast<unsigned long long>(runtime.fileSize),
                    runtime.coffTimestamp,
                    qualificationName(manifest.qualification),
                    classificationScope);
        printClassificationCountsJson(counts);
        std::printf(",\"classificationError\":\"%s\",\"manifest\":%s}\n",
                    jsonEscape(classificationError).c_str(),
                    qoh99::content::formatManifestDiagnosticJson(manifest).c_str());
    } else {
        std::printf("detected=true executableSha256=%s executableSize=%llu "
                    "forensicPeTimestamp=%u qualification=%s "
                    "classificationScope=%s",
                    runtime.sha256.c_str(),
                    static_cast<unsigned long long>(runtime.fileSize),
                    runtime.coffTimestamp,
                    qualificationName(manifest.qualification),
                    classificationScope);
        printClassificationCountsHuman(counts);
        std::printf(" classificationError=%s %s\n",
                    classificationError.empty() ? "none"
                                                : classificationError.c_str(),
                    qoh99::content::formatManifestDiagnosticHuman(manifest).c_str());
    }
    return manifest.admissionAllowed() ? 0 : 2;
}
