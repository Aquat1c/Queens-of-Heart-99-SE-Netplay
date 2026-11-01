#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace qnetplay::test {

struct RuntimeExecutableFixtures {
    std::filesystem::path root;
    std::filesystem::path novemberExecutable;
    std::filesystem::path decemberExecutable;
};

inline bool resolveRuntimeExecutableFixtures(
    RuntimeExecutableFixtures& out, std::string& error) {
    out = {};
    error.clear();

    const char* const configuredRoot = std::getenv("QOH99_RE_EVIDENCE_ROOT");
    if (configuredRoot == nullptr || *configuredRoot == '\0') {
        error =
            "required executable fixtures are unavailable: set "
            "QOH99_RE_EVIDENCE_ROOT to a directory containing "
            "november/qoh99.exe and december/QOH99.EXE";
        return false;
    }

    out.root = std::filesystem::u8path(configuredRoot);
    out.novemberExecutable = out.root / "november" / "qoh99.exe";
    out.decemberExecutable = out.root / "december" / "QOH99.EXE";

    const auto requireFixture = [&error](const std::filesystem::path& path,
                                         const char* const label) {
        std::error_code filesystemError;
        if (std::filesystem::is_regular_file(path, filesystemError))
            return true;

        error = std::string("required ") + label
            + " executable fixture is missing or unreadable: "
            + path.generic_u8string()
            + "; QOH99_RE_EVIDENCE_ROOT must contain "
              "november/qoh99.exe and december/QOH99.EXE";
        return false;
    };

    return requireFixture(out.novemberExecutable, "November/1126")
        && requireFixture(out.decemberExecutable, "December candidate");
}

}
