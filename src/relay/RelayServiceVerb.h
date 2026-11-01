#pragma once

#include <string>

namespace qrelay {

struct RelayServiceConfig;

int runRelayVerb(int argc, char** argv);

void printRelayUsage();

bool parseRelayOptions(int argc,
                       char** argv,
                       RelayServiceConfig& config,
                       bool& showHelp,
                       std::string& error);

}
