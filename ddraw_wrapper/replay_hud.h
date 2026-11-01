#pragma once

#include "input/KeyboardScanCodec.h"
#include <cstdio>

inline void FormatReplayResumeHint(char* out, size_t capacity,
                                   uint8_t pauseKey, uint8_t menuKey) {
    if (pauseKey != 0u)
        std::snprintf(out, capacity, "Press %s to resume", qinput::dikDisplayLabel(pauseKey));
    else if (menuKey != 0u)
        std::snprintf(out, capacity, "Press %s > Keys > Resume", qinput::dikDisplayLabel(menuKey));
    else
        std::snprintf(out, capacity, "Replay pause unbound - configure in launcher");
}
