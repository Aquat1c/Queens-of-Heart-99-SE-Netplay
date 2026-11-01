// Strings crossing this boundary are UTF-8, not ACP; re-widen before opening a file.
// Layout: <state_root>/logs/session_YYYYMMDD_HHMMSS_mmm_pPID/
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <string>

#include "platform/WidePath.h"

namespace qpath {

inline std::string exeDir() { return qplat::narrow(qplat::exeDirW()); }

inline std::string stateRoot() { return qplat::narrow(qplat::stateRootW()); }
inline bool stateRootUsedFallback() { return qplat::stateRootUsedFallback(); }
inline std::string stateRootSource() {
    return qplat::narrow(qplat::stateRootSourceName());
}

inline std::string logsRoot() { return qplat::narrow(qplat::logsRootW()); }
inline std::string replayRoot() { return qplat::narrow(qplat::replayRootW()); }

inline bool ensureDir(const std::string& dir) {
    return qplat::ensureDirW(qplat::widen(dir));
}

inline std::string makeSessionDir() {
    return qplat::narrow(qplat::makeSessionDirW());
}

}
