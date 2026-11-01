#pragma once

#include <string>

#include "input/InputLaunchConfig.h"
#include "input/InputProfile.h"

namespace qinput {

struct CompileResult {
    bool ok = false;
    std::string error;
    InputLaunchConfig config{};
    CompiledModeControlsV1 modeControls{};
};

CompileResult compileInputProfile(const InputProfile& profile,
                                  bool spectator,
                                  const ControlsFile* controls = nullptr);

}
