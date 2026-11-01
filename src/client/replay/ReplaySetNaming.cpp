#include "ReplaySetNaming.h"

#include "replay/CharacterCatalog.h"
#include "replay/ReplayCodec.h"

#include <algorithm>
#include <cstdio>

namespace qclient {
namespace replay {
namespace {

bool hostileAscii(unsigned char byte) {
    if (byte < 0x20u || byte == 0x7Fu) return true;
    switch (byte) {
        case '<': case '>': case ':': case '"': case '/': case '\\':
        case '|': case '?': case '*':
            return true;
        default:
            return false;
    }
}

std::string trimmed(std::string text) {
    const auto strip = [](char c) {
        return c == '-' || c == ' ' || c == '.' || c == '_';
    };
    size_t begin = 0u;
    while (begin < text.size() && strip(text[begin])) ++begin;
    size_t end = text.size();
    while (end > begin && strip(text[end - 1u])) --end;
    return text.substr(begin, end - begin);
}

std::string characterToken(const qreplay::FighterIdentityV2& identity) {
    const std::string name =
        qreplay::CharacterCatalog::forSeBuild().displayName(
            identity.nativeCharacterId);
    const std::string token = sanitizePathToken(name, 16u);
    return token.empty() ? std::string("char") : token;
}

}

std::string sanitizePathToken(const std::string& text, std::size_t maxBytes) {
    std::string out;
    out.reserve(text.size());
    bool pendingSeparator = false;
    for (const char raw : text) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        const bool separator =
            byte == ' ' || byte == '\t' || hostileAscii(byte);
        if (separator) {
            pendingSeparator = !out.empty();
            continue;
        }
        if (pendingSeparator) {
            out.push_back('-');
            pendingSeparator = false;
        }
        out.push_back(raw);
    }
    out = trimmed(std::move(out));
    if (out.size() > maxBytes) {
        out.resize(maxBytes);
        while (!out.empty()
               && (static_cast<unsigned char>(out.back()) & 0xC0u) == 0x80u) {
            out.pop_back();
        }
        if (!out.empty()
            && (static_cast<unsigned char>(out.back()) & 0xC0u) == 0xC0u) {
            out.pop_back();
        }
        out = trimmed(std::move(out));
    }
    return out;
}

void civilMonthDayFromUnix(int64_t unixSeconds, int32_t timezoneOffsetMinutes,
                           int& month, int& day) {
    int64_t local = unixSeconds
        + static_cast<int64_t>(timezoneOffsetMinutes) * 60;
    int64_t days = local / 86400;
    if (local % 86400 < 0) --days;
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t dayOfEra = days - era * 146097;
    const int64_t yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096)
        / 365;
    const int64_t dayOfYear =
        dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const int64_t mp = (5 * dayOfYear + 2) / 153;
    day = static_cast<int>(dayOfYear - (153 * mp + 2) / 5 + 1);
    month = static_cast<int>(mp + (mp < 10 ? 3 : -9));
}

const char* sourceFolderNameFor(const qreplay::GameManifestV2& manifest) {
    switch (manifest.display.sourceKind) {
        case qreplay::ReplaySourceKindV2::NetplayPlayer:  return "netplay";
        case qreplay::ReplaySourceKindV2::OfflineVsPlayer: return "offline";
        case qreplay::ReplaySourceKindV2::SpectatorSave:  return "spectate";
        case qreplay::ReplaySourceKindV2::RecoveredPrefix: return "recovered";
    }
    return "other";
}

std::string setFolderNameFor(const qreplay::GameManifestV2& manifest) {
    int month = 1;
    int day = 1;
    civilMonthDayFromUnix(manifest.display.recordedUnixSeconds,
                          manifest.display.timezoneOffsetMinutes, month, day);

    const std::string one =
        sanitizePathToken(qreplay::replayTextV2(manifest.lanes[0].name));
    const std::string two =
        sanitizePathToken(qreplay::replayTextV2(manifest.lanes[1].name));

    std::string players;
    if (!one.empty() && !two.empty()) {
        players = one + "-vs-" + two;
    } else {
        players = characterToken(manifest.slots[0].identity) + "-vs-"
            + characterToken(manifest.slots[1].identity);
    }

    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "%08X",
                  static_cast<unsigned>(
                      manifest.display.seriesUuidHigh >> 32));

    char out[128];
    std::snprintf(out, sizeof(out), "%02d-%02d_%s_%s", month, day,
                  players.c_str(), suffix);
    return std::string(out);
}

std::string gameNameHintFor(const qreplay::GameManifestV2& manifest,
                            const std::string& sourceHint) {
    const uint32_t ordinal = manifest.display.gameOrdinalInSeries + 1u;
    const std::string source = sanitizePathToken(sourceHint, 16u);
    char out[128];
    std::snprintf(out, sizeof(out), "%s%sG%02u_%s-vs-%s",
                  source.c_str(), source.empty() ? "" : "_", ordinal,
                  characterToken(manifest.slots[0].identity).c_str(),
                  characterToken(manifest.slots[1].identity).c_str());
    return std::string(out);
}

}
}
