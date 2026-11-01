#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qnetplay::soak {

enum class SoakMode {
    Quick,
    Full,
    Nightly,
};

struct SoakOptions {
    SoakMode mode = SoakMode::Quick;
    uint32_t frameOverride = 0;
    std::vector<uint64_t> seedOverride;
    std::vector<std::string> profileFilter;
    bool verifySpectatorInvariance = true;
    bool verbose = false;
};

struct SoakSummary {
    uint64_t scenarioRuns = 0;
    uint64_t simulatedVisibleFrames = 0;
    uint64_t replayedFrames = 0;
    uint64_t rollbackCount = 0;
    uint64_t stallPolls = 0;
    uint64_t logicalPackets = 0;
    uint64_t droppedPackets = 0;
    uint64_t duplicatedPackets = 0;
    uint64_t reorderedPackets = 0;
    uint64_t productionRuns = 0;
    uint64_t productionPlayerPackets = 0;
    uint64_t productionSpectatorPackets = 0;
    uint64_t productionSpectatorDropped = 0;
};

int runSoak(const SoakOptions& options, SoakSummary* summary = nullptr);

const char* soakModeName(SoakMode mode);
uint32_t soakFramesForMode(SoakMode mode);
std::vector<uint64_t> soakSeedsForMode(SoakMode mode);
std::vector<std::string> soakProfileNames();

}
