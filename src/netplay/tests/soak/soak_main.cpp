#include "SoakRunner.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

namespace {

void usage(const char* executable) {
    std::printf(
        "Usage: %s [--quick|--full|--nightly] [options]\n"
        "\n"
        "Options:\n"
        "  --frames N          Override frames per profile/seed.\n"
        "  --seed N            Run one seed (decimal or 0x-prefixed); repeatable.\n"
        "  --profile NAME      Run one profile; repeatable.\n"
        "  --no-spectators     Skip the paired spectator-invariance rerun.\n"
        "  --verbose           Print both baseline and spectator fingerprints.\n"
        "  --list-profiles     Print profile names and exit.\n"
        "  --help              Print this text.\n",
        executable);
}

bool parseU64(const char* text, uint64_t& value) {
    if (text == nullptr || *text == '\0' || *text == '-') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

}

int main(int argc, char** argv) {
    qnetplay::soak::SoakOptions options{};
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (std::strcmp(argument, "--quick") == 0) {
            options.mode = qnetplay::soak::SoakMode::Quick;
        } else if (std::strcmp(argument, "--full") == 0) {
            options.mode = qnetplay::soak::SoakMode::Full;
        } else if (std::strcmp(argument, "--nightly") == 0) {
            options.mode = qnetplay::soak::SoakMode::Nightly;
        } else if (std::strcmp(argument, "--no-spectators") == 0) {
            options.verifySpectatorInvariance = false;
        } else if (std::strcmp(argument, "--verbose") == 0) {
            options.verbose = true;
        } else if (std::strcmp(argument, "--help") == 0
                   || std::strcmp(argument, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (std::strcmp(argument, "--list-profiles") == 0) {
            for (const std::string& name :
                 qnetplay::soak::soakProfileNames()) {
                std::printf("%s\n", name.c_str());
            }
            return 0;
        } else if (std::strcmp(argument, "--frames") == 0
                   || std::strcmp(argument, "--seed") == 0
                   || std::strcmp(argument, "--profile") == 0) {
            if (++index >= argc) {
                std::fprintf(stderr, "%s requires a value\n", argument);
                return 2;
            }
            if (std::strcmp(argument, "--profile") == 0) {
                options.profileFilter.emplace_back(argv[index]);
                continue;
            }
            uint64_t value = 0;
            if (!parseU64(argv[index], value)) {
                std::fprintf(stderr, "invalid value for %s: %s\n",
                             argument, argv[index]);
                return 2;
            }
            if (std::strcmp(argument, "--frames") == 0) {
                if (value == 0u || value > 0xFFFFFFFFull) {
                    std::fprintf(stderr,
                                 "--frames must be in [1, 4294967295]\n");
                    return 2;
                }
                options.frameOverride = static_cast<uint32_t>(value);
            } else {
                options.seedOverride.push_back(value);
            }
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argument);
            usage(argv[0]);
            return 2;
        }
    }

    try {
        qnetplay::soak::SoakSummary summary{};
        return qnetplay::soak::runSoak(options, &summary);
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "soak runner exception: %s\n", exception.what());
    } catch (...) {
        std::fprintf(stderr, "soak runner exception: unknown\n");
    }
    return 1;
}
