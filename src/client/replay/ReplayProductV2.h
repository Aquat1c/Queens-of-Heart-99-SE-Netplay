#pragma once

#include "ReplayCompatibilityV2.h"
#include "ReplayParserV2.h"

#include <string>

namespace qclient::replay {

bool makeReplayPreflightInputV2(
    const ParsedReplayV2& parsed,
    ReplayPreflightInputV2& out,
    std::string& detail);

}
