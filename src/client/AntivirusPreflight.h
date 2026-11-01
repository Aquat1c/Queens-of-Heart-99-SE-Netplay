#pragma once

#include <string>
#include <vector>

namespace qclient {

enum class AntivirusFileState : uint8_t {
    Present,
    Missing,
    Unreadable,
};

struct AntivirusFileReport {
    std::string path;
    std::string leaf;
    AntivirusFileState state = AntivirusFileState::Present;
};

struct AntivirusPreflightReport {
    std::vector<AntivirusFileReport> files;
    bool degraded = false;
    bool lastRunEndedAbruptly = false;
};

AntivirusPreflightReport preflightAntivirus();

bool antivirusPreflightShouldSkip(int argc, char** argv);

std::vector<std::string> antivirusPreflightMessages(
    const AntivirusPreflightReport& report);

void markRunStarted();
void markRunEndedCleanly();

std::string antivirusExclusionPath();

bool launchDefenderExclusionHelper(std::string& error);

}
