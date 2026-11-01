#pragma once

#include <cstdint>
#include <string>

#include "replay/ReplayTypes.h"

namespace qclient {
namespace replay {

std::string sanitizePathToken(const std::string& text,
                              std::size_t maxBytes = 24u);

void civilMonthDayFromUnix(int64_t unixSeconds, int32_t timezoneOffsetMinutes,
                           int& month, int& day);

/* Lower-case ASCII path components, never translated; must not change once a library exists. */
const char* sourceFolderNameFor(const qreplay::GameManifestV2& manifest);

std::string setFolderNameFor(const qreplay::GameManifestV2& manifest);

std::string gameNameHintFor(const qreplay::GameManifestV2& manifest,
                            const std::string& sourceHint);

}
}
