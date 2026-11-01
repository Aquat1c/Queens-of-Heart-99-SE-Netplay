#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "input/InputProfile.h"

namespace qinput {

struct ModeControlConflict {
    bool fighter = false;
    size_t profileIndex = 0;
    uint32_t fightAction = 0;
    ModeControlAction modeAction = ModeControlAction::None;
};

void installDefaultModeControls(ControlsFile& file);

bool samePhysicalBinding(const ProfileBinding& a, const ProfileBinding& b);
bool validModeControlBinding(ModeControlAction action,
                             const ProfileBinding& binding,
                             std::string& error);

std::vector<ModeControlConflict> findModeControlConflicts(
    const ControlsFile& file,
    ModeControlAction destination,
    const ProfileBinding& binding);

bool moveModeControlBinding(ControlsFile& file,
                            ModeControlAction destination,
                            const ProfileBinding& binding,
                            std::string& error);

const char* modeControlRuntimeCapabilityReason(ModeControlAction action,
                                               bool runtimeModeControls,
                                               bool meterCharge);

}
