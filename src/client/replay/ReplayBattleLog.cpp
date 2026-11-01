#include "ReplayBattleLog.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace qclient {
namespace replay {
namespace {

// Field order below IS the format: append only, never renumber. Appending keeps
// version 1; bump only when an existing field changes meaning, units or encoding.
constexpr char kSeparator = '\t';
constexpr char kRecordKindGame = 'G';
constexpr uint64_t kSchemaVersion = 1u;

enum Field : std::size_t {
    kKind = 0u,
    kVersion,
    kGameUuidHigh,
    kGameUuidLow,
    kSetUuidHigh,
    kSetUuidLow,
    kGameNo,
    kStartUnix,
    kEndUnix,
    kSetStartUnix,
    kTimezoneOffsetMinutes,
    kSourceKind,
    kMySide,
    kCharacterOne,
    kCharacterTwo,
    kPaletteOne,
    kPaletteTwo,
    kCharacterCatalogId,
    kWinningSideMask,
    kRoundsOne,
    kRoundsTwo,
    kTerminalReason,
    kFrames,
    kRoundsToWin,
    kStageId,
    kBytes,
    kDigest,
    kNameOne,
    kNameTwo,
    kMyName,
    kRelativePath,
    kFieldCount
};

constexpr std::size_t kNameBytes = 64u;
constexpr std::size_t kPathBytes = 260u;

void appendUnsigned(std::string& out, uint64_t value) {
    char digits[20];
    std::size_t count = 0u;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);
    while (count != 0u) out.push_back(digits[--count]);
}

void appendSigned(std::string& out, int64_t value) {
    if (value >= 0) {
        appendUnsigned(out, static_cast<uint64_t>(value));
        return;
    }
    out.push_back('-');
    appendUnsigned(out, ~static_cast<uint64_t>(value) + 1u);
}

void appendHex64(std::string& out, uint64_t value) {
    static const char kDigits[] = "0123456789ABCDEF";
    for (int shift = 60; shift >= 0; shift -= 4) {
        out.push_back(kDigits[(value >> shift) & 0xFu]);
    }
}

bool continuationByte(unsigned char byte) {
    return (byte & 0xC0u) == 0x80u;
}

std::size_t sequenceLength(unsigned char lead) {
    if ((lead & 0x80u) == 0x00u) return 1u;
    if ((lead & 0xE0u) == 0xC0u) return 2u;
    if ((lead & 0xF0u) == 0xE0u) return 3u;
    if ((lead & 0xF8u) == 0xF0u) return 4u;
    return 0u;
}

void sanitizeInto(std::string& out, std::string_view text,
                  std::size_t maxBytes) {
    const std::size_t begin = out.size();
    const std::size_t limit = begin + maxBytes;
    out.reserve(begin + std::min(text.size(), maxBytes));

    bool truncated = false;
    for (std::size_t i = 0u; i < text.size(); ++i) {
        if (out.size() >= limit) {
            truncated = true;
            break;
        }
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x20u || byte == 0x7Fu) {
            out.push_back(' ');
            continue;
        }
        out.push_back(static_cast<char>(byte));
    }
    if (!truncated) return;

    std::size_t tail = out.size();
    while (tail > begin
           && continuationByte(static_cast<unsigned char>(out[tail - 1u]))
           && out.size() - tail < 3u) {
        --tail;
    }
    if (tail == begin) return;
    const std::size_t lead = tail - 1u;
    const std::size_t need =
        sequenceLength(static_cast<unsigned char>(out[lead]));
    if (need > 1u && out.size() - lead < need) out.resize(lead);
}

bool parseUnsignedField(std::string_view field, uint64_t limit,
                        uint64_t& out) {
    if (field.empty()) return false;
    uint64_t value = 0u;
    for (const char raw : field) {
        if (raw < '0' || raw > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(raw - '0');
        if (digit > limit || value > (limit - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

bool parseSignedField(std::string_view field, int64_t minimum,
                      int64_t maximum, int64_t& out) {
    const bool negative = !field.empty() && field.front() == '-';
    if (negative) field.remove_prefix(1u);
    const uint64_t limit = negative
        ? ~static_cast<uint64_t>(minimum) + 1u
        : static_cast<uint64_t>(maximum);
    uint64_t magnitude = 0u;
    if (!parseUnsignedField(field, limit, magnitude)) return false;
    out = negative
        ? static_cast<int64_t>(~magnitude + 1u)
        : static_cast<int64_t>(magnitude);
    return true;
}

template <typename T>
bool takeUnsigned(std::string_view field, T& out) {
    uint64_t value = 0u;
    if (!parseUnsignedField(
            field, static_cast<uint64_t>(std::numeric_limits<T>::max()),
            value)) {
        return false;
    }
    out = static_cast<T>(value);
    return true;
}

template <typename T>
bool takeSigned(std::string_view field, T& out) {
    int64_t value = 0;
    if (!parseSignedField(field,
                          static_cast<int64_t>(std::numeric_limits<T>::min()),
                          static_cast<int64_t>(std::numeric_limits<T>::max()),
                          value)) {
        return false;
    }
    out = static_cast<T>(value);
    return true;
}

bool takeHex64(std::string_view field, uint64_t& out) {
    if (field.size() != 16u) return false;
    uint64_t value = 0u;
    for (const char raw : field) {
        uint64_t nibble = 0u;
        if (raw >= '0' && raw <= '9') {
            nibble = static_cast<uint64_t>(raw - '0');
        } else if (raw >= 'A' && raw <= 'F') {
            nibble = static_cast<uint64_t>(raw - 'A') + 10u;
        } else if (raw >= 'a' && raw <= 'f') {
            nibble = static_cast<uint64_t>(raw - 'a') + 10u;
        } else {
            return false;
        }
        value = (value << 4) | nibble;
    }
    out = value;
    return true;
}

std::size_t splitFields(std::string_view line, std::string_view* fields,
                        std::size_t capacity) {
    std::size_t count = 0u;
    std::size_t pos = 0u;
    while (count < capacity) {
        const std::size_t tab = line.find(kSeparator, pos);
        if (tab == std::string_view::npos) {
            fields[count++] = line.substr(pos);
            break;
        }
        fields[count++] = line.substr(pos, tab - pos);
        pos = tab + 1u;
    }
    return count;
}

bool decodeRowView(std::string_view line, BattleLogRowV1& out) {
    if (line.empty()) return false;

    std::string_view fields[kFieldCount];
    if (splitFields(line, fields, kFieldCount) != kFieldCount) return false;

    if (fields[kKind].size() != 1u || fields[kKind][0] != kRecordKindGame) {
        return false;
    }
    uint64_t version = 0u;
    if (!parseUnsignedField(fields[kVersion],
                            std::numeric_limits<uint32_t>::max(), version)
        || version != kSchemaVersion) {
        return false;
    }

    BattleLogRowV1 row;
    if (!takeHex64(fields[kGameUuidHigh], row.gameUuidHigh)
        || !takeHex64(fields[kGameUuidLow], row.gameUuidLow)
        || !takeHex64(fields[kSetUuidHigh], row.setUuidHigh)
        || !takeHex64(fields[kSetUuidLow], row.setUuidLow)
        || !takeUnsigned(fields[kGameNo], row.gameNo)
        || !takeSigned(fields[kStartUnix], row.startUnix)
        || !takeSigned(fields[kEndUnix], row.endUnix)
        || !takeSigned(fields[kSetStartUnix], row.setStartUnix)
        || !takeSigned(fields[kTimezoneOffsetMinutes],
                       row.timezoneOffsetMinutes)
        || !takeUnsigned(fields[kSourceKind], row.sourceKind)
        || !takeSigned(fields[kMySide], row.mySide)
        || !takeUnsigned(fields[kCharacterOne], row.characterOne)
        || !takeUnsigned(fields[kCharacterTwo], row.characterTwo)
        || !takeUnsigned(fields[kPaletteOne], row.paletteOne)
        || !takeUnsigned(fields[kPaletteTwo], row.paletteTwo)
        || !takeHex64(fields[kCharacterCatalogId], row.characterCatalogId)
        || !takeUnsigned(fields[kWinningSideMask], row.winningSideMask)
        || !takeSigned(fields[kRoundsOne], row.roundsOne)
        || !takeSigned(fields[kRoundsTwo], row.roundsTwo)
        || !takeUnsigned(fields[kTerminalReason], row.terminalReason)
        || !takeUnsigned(fields[kFrames], row.frames)
        || !takeUnsigned(fields[kRoundsToWin], row.roundsToWin)
        || !takeUnsigned(fields[kStageId], row.stageId)
        || !takeUnsigned(fields[kBytes], row.bytes)
        || !takeHex64(fields[kDigest], row.digest)) {
        return false;
    }

    sanitizeInto(row.nameOne, fields[kNameOne], kNameBytes);
    sanitizeInto(row.nameTwo, fields[kNameTwo], kNameBytes);
    sanitizeInto(row.myName, fields[kMyName], kNameBytes);
    sanitizeInto(row.relativePath, fields[kRelativePath], kPathBytes);

    out = std::move(row);
    return true;
}

void writeUnsigned(std::string& out, uint64_t value) {
    out.push_back(kSeparator);
    appendUnsigned(out, value);
}

void writeSigned(std::string& out, int64_t value) {
    out.push_back(kSeparator);
    appendSigned(out, value);
}

void writeHex(std::string& out, uint64_t value) {
    out.push_back(kSeparator);
    appendHex64(out, value);
}

void writeText(std::string& out, const std::string& text,
               std::size_t maxBytes) {
    out.push_back(kSeparator);
    sanitizeInto(out, text, maxBytes);
}

}

const char* battleLogHeaderLineV1() {
    return "#q99blog\t1\tQOH99 battle log: one line per finished game, "
           "UTF-8, TAB-separated\n";
}

std::string encodeBattleLogRowV1(const BattleLogRowV1& row) {
    std::string line;
    line.reserve(256u);

    line.push_back(kRecordKindGame);
    writeUnsigned(line, kSchemaVersion);

    writeHex(line, row.gameUuidHigh);
    writeHex(line, row.gameUuidLow);
    writeHex(line, row.setUuidHigh);
    writeHex(line, row.setUuidLow);
    writeUnsigned(line, row.gameNo);

    writeSigned(line, row.startUnix);
    writeSigned(line, row.endUnix);
    writeSigned(line, row.setStartUnix);
    writeSigned(line, row.timezoneOffsetMinutes);

    writeUnsigned(line, row.sourceKind);
    writeSigned(line, row.mySide);

    writeUnsigned(line, row.characterOne);
    writeUnsigned(line, row.characterTwo);
    writeUnsigned(line, row.paletteOne);
    writeUnsigned(line, row.paletteTwo);
    writeHex(line, row.characterCatalogId);

    writeUnsigned(line, row.winningSideMask);
    writeSigned(line, row.roundsOne);
    writeSigned(line, row.roundsTwo);
    writeUnsigned(line, row.terminalReason);

    writeUnsigned(line, row.frames);
    writeUnsigned(line, row.roundsToWin);
    writeUnsigned(line, row.stageId);
    writeUnsigned(line, row.bytes);
    writeHex(line, row.digest);

    writeText(line, row.nameOne, kNameBytes);
    writeText(line, row.nameTwo, kNameBytes);
    writeText(line, row.myName, kNameBytes);
    writeText(line, row.relativePath, kPathBytes);

    line.push_back('\n');
    return line;
}

bool decodeBattleLogRowV1(const std::string& line, BattleLogRowV1& out) {
    return decodeRowView(std::string_view(line), out);
}

std::vector<BattleLogRowV1> decodeBattleLogV1(const std::string& text) {
    std::vector<BattleLogRowV1> rows;

    const std::size_t newlines = static_cast<std::size_t>(
        std::count(text.begin(), text.end(), '\n'));
    rows.reserve(std::min<std::size_t>(newlines, 1024u));

    const std::string_view all(text);
    std::size_t pos = 0u;
    while (pos < all.size()) {
        const std::size_t lf = all.find('\n', pos);
        if (lf == std::string_view::npos) break;

        std::string_view line = all.substr(pos, lf - pos);
        pos = lf + 1u;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1u);

        BattleLogRowV1 row;
        if (decodeRowView(line, row)) rows.push_back(std::move(row));
    }
    return rows;
}

std::string sanitizeBattleLogText(const std::string& text,
                                  std::size_t maxBytes) {
    std::string out;
    sanitizeInto(out, std::string_view(text), maxBytes);
    return out;
}

uint32_t battleLogSecondsFromFrames(uint32_t frames) {
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(frames) * 100ull) / 6060ull);
}

}
}
