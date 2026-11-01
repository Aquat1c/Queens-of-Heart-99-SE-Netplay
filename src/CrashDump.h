#pragma once
#include <string>

#include "CrashContext.h"

namespace qcrash {

void install(const std::string& dumpDir, const char* tag);

void installW(const std::wstring& dumpDir, const char* tag);

bool minidumpsAvailable();

void reportCaughtOwnedTick(const CapturedCrashContext& captured);

}
