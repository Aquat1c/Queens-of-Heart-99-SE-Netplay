#include "TestHarness.h"

#include "client/replay/ReplayBattleLog.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace qclient::replay;

namespace {

BattleLogRowV1 populatedRow() {
    BattleLogRowV1 row;
    row.gameUuidHigh = 0x0123456789ABCDEFull;
    row.gameUuidLow = 0xFEDCBA9876543210ull;
    row.setUuidHigh = 0x1122334455667788ull;
    row.setUuidLow = 0x99AABBCCDDEEFF00ull;
    row.gameNo = 7u;
    row.startUnix = 1789905600;
    row.endUnix = 1789905813;
    row.setStartUnix = 1789903800;
    row.timezoneOffsetMinutes = 540;
    row.sourceKind = 2u;
    row.mySide = kBattleSideTwo;
    row.characterOne = 11u;
    row.characterTwo = 22u;
    row.paletteOne = 4u;
    row.paletteTwo = 9u;
    row.characterCatalogId = 0xA5A5A5A5A5A5A5A5ull;
    row.winningSideMask = 2u;
    row.roundsOne = 3;
    row.roundsTwo = 5;
    row.terminalReason = 7u;
    row.frames = 12345u;
    row.roundsToWin = 5u;
    row.stageId = 6u;
    row.bytes = 987654321ull;
    row.digest = 0xDEADBEEFCAFEF00Dull;
    row.nameOne = "Alice";
    row.nameTwo = "Bob";
    row.myName = "alice_2026";
    row.relativePath = "09-20_Alice-vs-Bob_1A2B3C4D\\netplay_G01_Akari.q99r";
    return row;
}

const char* firstDifference(const BattleLogRowV1& a,
                            const BattleLogRowV1& b) {
    if (a.gameUuidHigh != b.gameUuidHigh) return "gameUuidHigh";
    if (a.gameUuidLow != b.gameUuidLow) return "gameUuidLow";
    if (a.setUuidHigh != b.setUuidHigh) return "setUuidHigh";
    if (a.setUuidLow != b.setUuidLow) return "setUuidLow";
    if (a.gameNo != b.gameNo) return "gameNo";
    if (a.startUnix != b.startUnix) return "startUnix";
    if (a.endUnix != b.endUnix) return "endUnix";
    if (a.setStartUnix != b.setStartUnix) return "setStartUnix";
    if (a.timezoneOffsetMinutes != b.timezoneOffsetMinutes)
        return "timezoneOffsetMinutes";
    if (a.sourceKind != b.sourceKind) return "sourceKind";
    if (a.mySide != b.mySide) return "mySide";
    if (a.characterOne != b.characterOne) return "characterOne";
    if (a.characterTwo != b.characterTwo) return "characterTwo";
    if (a.paletteOne != b.paletteOne) return "paletteOne";
    if (a.paletteTwo != b.paletteTwo) return "paletteTwo";
    if (a.characterCatalogId != b.characterCatalogId)
        return "characterCatalogId";
    if (a.winningSideMask != b.winningSideMask) return "winningSideMask";
    if (a.roundsOne != b.roundsOne) return "roundsOne";
    if (a.roundsTwo != b.roundsTwo) return "roundsTwo";
    if (a.terminalReason != b.terminalReason) return "terminalReason";
    if (a.frames != b.frames) return "frames";
    if (a.roundsToWin != b.roundsToWin) return "roundsToWin";
    if (a.stageId != b.stageId) return "stageId";
    if (a.bytes != b.bytes) return "bytes";
    if (a.digest != b.digest) return "digest";
    if (a.nameOne != b.nameOne) return "nameOne";
    if (a.nameTwo != b.nameTwo) return "nameTwo";
    if (a.myName != b.myName) return "myName";
    if (a.relativePath != b.relativePath) return "relativePath";
    return nullptr;
}

bool sameRow(const BattleLogRowV1& a, const BattleLogRowV1& b) {
    return firstDifference(a, b) == nullptr;
}

void checkSameRow(const BattleLogRowV1& actual, const BattleLogRowV1& expected,
                  const char* label) {
    const char* field = firstDifference(actual, expected);
    if (field != nullptr) {
        std::printf("    row [%s] lost field: %s\n", label, field);
    }
    QNP_CHECK(field == nullptr);
}

std::string withoutLf(std::string text) {
    if (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

std::string encodedLine(const BattleLogRowV1& row) {
    return withoutLf(encodeBattleLogRowV1(row));
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::size_t count = 1u;
    for (const char c : line) {
        if (c == '\t') ++count;
    }
    std::vector<std::string> out;
    out.reserve(count);
    std::size_t begin = 0u;
    for (;;) {
        const std::size_t tab = line.find('\t', begin);
        if (tab == std::string::npos) {
            out.push_back(line.substr(begin));
            return out;
        }
        out.push_back(line.substr(begin, tab - begin));
        begin = tab + 1u;
    }
}

std::string joinTabs(const std::vector<std::string>& fields) {
    std::size_t size = 1u;
    for (const std::string& field : fields) size += field.size() + 1u;
    std::string out;
    out.reserve(size);
    for (std::size_t i = 0u; i < fields.size(); ++i) {
        if (i != 0u) out.push_back('\t');
        out.append(fields[i]);
    }
    out.push_back('\n');
    return out;
}

std::string headerFileLine() {
    std::string header = battleLogHeaderLineV1();
    if (header.empty() || header.back() != '\n') header.push_back('\n');
    return header;
}

std::string pseudoRandomBytes(std::size_t count, uint32_t seed,
                              bool printableOnly) {
    std::string out;
    out.reserve(count);
    uint32_t state = seed;
    for (std::size_t i = 0u; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        const uint32_t byte = (state >> 16) & 0xFFu;
        out.push_back(static_cast<char>(
            printableOnly ? (0x21u + (byte % 0x5Du)) : byte));
    }
    return out;
}

const char kJapanese[] = "\xE3\x81\x95\xE3\x81\x8F\xE3\x82\x89";
const char kEmoji[] = "\xF0\x9F\x8E\xAE";

}

QNP_TEST(replay_battle_log_row_round_trips_field_for_field) {
    const BattleLogRowV1 row = populatedRow();
    const std::string encoded = encodeBattleLogRowV1(row);

    QNP_REQUIRE(!encoded.empty());
    QNP_CHECK(encoded.back() == '\n');
    QNP_CHECK(encoded.find('\n') == encoded.size() - 1u);
    QNP_CHECK(encoded.find('\r') == std::string::npos);

    BattleLogRowV1 decoded;
    QNP_CHECK(decodeBattleLogRowV1(withoutLf(encoded), decoded));
    checkSameRow(decoded, row, "populated");

    const std::vector<BattleLogRowV1> fromFile = decodeBattleLogV1(encoded);
    QNP_REQUIRE(fromFile.size() == 1u);
    checkSameRow(fromFile[0], row, "populated via file");

    const BattleLogRowV1 fresh;
    BattleLogRowV1 decodedFresh;
    QNP_CHECK(decodeBattleLogRowV1(encodedLine(fresh), decodedFresh));
    checkSameRow(decodedFresh, fresh, "default");
    QNP_CHECK(decodedFresh.mySide == kBattleSideSpectated);
    QNP_CHECK(decodedFresh.roundsOne == kBattleRoundsUnknown);
    QNP_CHECK(decodedFresh.roundsTwo == kBattleRoundsUnknown);

    BattleLogRowV1 zero;
    zero.mySide = kBattleSideOne;
    zero.roundsOne = 0;
    zero.roundsTwo = 0;
    BattleLogRowV1 decodedZero;
    QNP_CHECK(decodeBattleLogRowV1(encodedLine(zero), decodedZero));
    checkSameRow(decodedZero, zero, "zero");
    QNP_CHECK(!sameRow(decodedZero, fresh));

    QNP_CHECK(splitTabs(encodedLine(zero)).size()
              == splitTabs(withoutLf(encoded)).size());
    QNP_CHECK(splitTabs(encodedLine(fresh)).size()
              == splitTabs(withoutLf(encoded)).size());
}

QNP_TEST(replay_battle_log_carries_negative_and_extreme_values) {
    const int8_t sides[] = { kBattleSideOne, kBattleSideTwo,
                             kBattleSideSpectated, kBattleSideOffline };
    for (const int8_t side : sides) {
        BattleLogRowV1 row = populatedRow();
        row.mySide = side;
        BattleLogRowV1 decoded;
        QNP_CHECK(decodeBattleLogRowV1(encodedLine(row), decoded));
        QNP_CHECK(decoded.mySide == side);
        checkSameRow(decoded, row, "side");
    }

    BattleLogRowV1 past = populatedRow();
    past.startUnix = -1;
    past.endUnix = -86400;
    past.setStartUnix = -2208988800LL;
    past.timezoneOffsetMinutes = -720;
    past.roundsOne = kBattleRoundsUnknown;
    past.roundsTwo = kBattleRoundsUnknown;
    BattleLogRowV1 decodedPast;
    QNP_CHECK(decodeBattleLogRowV1(encodedLine(past), decodedPast));
    checkSameRow(decodedPast, past, "negative clock");

    BattleLogRowV1 low = populatedRow();
    low.gameUuidHigh = 0u;
    low.gameUuidLow = 1u;
    low.startUnix = INT64_MIN;
    low.endUnix = INT64_MAX;
    low.setStartUnix = INT64_MIN + 1;
    low.timezoneOffsetMinutes = INT32_MIN;
    low.mySide = INT8_MIN;
    low.roundsOne = INT16_MIN;
    low.roundsTwo = INT16_MAX;
    BattleLogRowV1 decodedLow;
    QNP_CHECK(decodeBattleLogRowV1(encodedLine(low), decodedLow));
    checkSameRow(decodedLow, low, "minimums");

    BattleLogRowV1 high;
    high.gameUuidHigh = UINT64_MAX;
    high.gameUuidLow = UINT64_MAX;
    high.setUuidHigh = UINT64_MAX;
    high.setUuidLow = UINT64_MAX;
    high.characterCatalogId = UINT64_MAX;
    high.bytes = UINT64_MAX;
    high.digest = UINT64_MAX;
    high.gameNo = UINT32_MAX;
    high.frames = UINT32_MAX;
    high.roundsToWin = UINT32_MAX;
    high.stageId = UINT32_MAX;
    high.terminalReason = UINT16_MAX;
    high.sourceKind = 255u;
    high.characterOne = 254u;
    high.characterTwo = 253u;
    high.paletteOne = 252u;
    high.paletteTwo = 251u;
    high.winningSideMask = 3u;
    high.mySide = kBattleSideOffline;
    high.roundsOne = INT16_MAX;
    high.roundsTwo = INT16_MAX;
    high.nameOne = "A";
    high.nameTwo = "B";
    high.myName = "C";
    high.relativePath = "d.q99r";
    BattleLogRowV1 decodedHigh;
    QNP_CHECK(decodeBattleLogRowV1(encodedLine(high), decodedHigh));
    checkSameRow(decodedHigh, high, "maximums");
    QNP_CHECK(decodedHigh.gameUuidHigh == UINT64_MAX);
    QNP_CHECK(decodedHigh.frames == UINT32_MAX);

    const std::string text = encodeBattleLogRowV1(low)
        + encodeBattleLogRowV1(high) + encodeBattleLogRowV1(past);
    const std::vector<BattleLogRowV1> rows = decodeBattleLogV1(text);
    QNP_REQUIRE(rows.size() == 3u);
    checkSameRow(rows[0], low, "file minimums");
    checkSameRow(rows[1], high, "file maximums");
    checkSameRow(rows[2], past, "file negative clock");
}

QNP_TEST(replay_battle_log_ignores_a_newer_tail_and_refuses_a_foreign_line) {
    const BattleLogRowV1 row = populatedRow();
    const std::vector<std::string> fields = splitTabs(encodedLine(row));
    QNP_REQUIRE(fields.size() > 6u);

    std::vector<std::string> extended = fields;
    extended.push_back("777");
    extended.push_back("-3");
    extended.push_back("future-kind-of-thing");
    extended.push_back("");
    extended.push_back("18446744073709551615");
    BattleLogRowV1 decoded;
    QNP_CHECK(decodeBattleLogRowV1(withoutLf(joinTabs(extended)), decoded));
    checkSameRow(decoded, row, "five extra fields");
    QNP_CHECK(decoded.relativePath == row.relativePath);

    std::vector<std::string> foreign = fields;
    foreign[0] = "qoh99-not-a-battle";
    BattleLogRowV1 sink;
    QNP_CHECK(!decodeBattleLogRowV1(withoutLf(joinTabs(foreign)), sink));
    std::vector<std::string> unkinded = fields;
    unkinded[0] = "";
    QNP_CHECK(!decodeBattleLogRowV1(withoutLf(joinTabs(unkinded)), sink));

    std::vector<std::string> newer = fields;
    newer[1] = fields[1] + "99";
    QNP_CHECK(!decodeBattleLogRowV1(withoutLf(joinTabs(newer)), sink));
    std::vector<std::string> nonsense = fields;
    nonsense[1] = "not-a-version";
    QNP_CHECK(!decodeBattleLogRowV1(withoutLf(joinTabs(nonsense)), sink));
    std::vector<std::string> blank = fields;
    blank[1] = "";
    QNP_CHECK(!decodeBattleLogRowV1(withoutLf(joinTabs(blank)), sink));

    for (std::size_t drop = 1u; drop <= 4u; ++drop) {
        const std::vector<std::string> shortened(
            fields.begin(), fields.end() - static_cast<std::ptrdiff_t>(drop));
        QNP_CHECK(!decodeBattleLogRowV1(withoutLf(joinTabs(shortened)),
                                        sink));
    }
    QNP_CHECK(!decodeBattleLogRowV1("", sink));
    QNP_CHECK(!decodeBattleLogRowV1(fields[0], sink));
    QNP_CHECK(!decodeBattleLogRowV1(fields[0] + "\t" + fields[1], sink));

    const std::string text = encodeBattleLogRowV1(row)
        + joinTabs(newer) + joinTabs(foreign) + encodeBattleLogRowV1(row);
    const std::vector<BattleLogRowV1> rows = decodeBattleLogV1(text);
    QNP_REQUIRE(rows.size() == 2u);
    checkSameRow(rows[0], row, "before a foreign line");
    checkSameRow(rows[1], row, "after a foreign line");
}

QNP_TEST(replay_battle_log_drops_the_torn_last_line_and_nothing_else) {
    BattleLogRowV1 one = populatedRow();
    one.gameNo = 1u;
    BattleLogRowV1 two = populatedRow();
    two.gameNo = 2u;
    BattleLogRowV1 three = populatedRow();
    three.gameNo = 3u;

    const std::string head =
        encodeBattleLogRowV1(one) + encodeBattleLogRowV1(two);
    const std::string third = encodeBattleLogRowV1(three);
    QNP_REQUIRE(third.size() > 8u);

    const std::size_t cuts[] = { 1u, 3u, third.size() / 2u,
                                 third.size() - 2u };
    for (const std::size_t cut : cuts) {
        const std::vector<BattleLogRowV1> rows =
            decodeBattleLogV1(head + third.substr(0u, cut));
        QNP_REQUIRE(rows.size() == 2u);
        checkSameRow(rows[0], one, "before a torn tail");
        checkSameRow(rows[1], two, "before a torn tail");
    }

    const std::vector<BattleLogRowV1> unterminated =
        decodeBattleLogV1(head + withoutLf(third));
    QNP_REQUIRE(unterminated.size() == 2u);
    checkSameRow(unterminated[1], two, "unterminated tail");

    const std::vector<BattleLogRowV1> terminated =
        decodeBattleLogV1(head + third);
    QNP_REQUIRE(terminated.size() == 3u);
    checkSameRow(terminated[2], three, "terminated tail");

    QNP_CHECK(decodeBattleLogV1("").empty());
    QNP_CHECK(decodeBattleLogV1("\n").empty());
    QNP_CHECK(decodeBattleLogV1(withoutLf(encodeBattleLogRowV1(one))).empty());

    const std::vector<BattleLogRowV1> gapped = decodeBattleLogV1(
        encodeBattleLogRowV1(one) + "\n\n" + encodeBattleLogRowV1(two));
    QNP_REQUIRE(gapped.size() == 2u);
    checkSameRow(gapped[0], one, "before a blank line");
    checkSameRow(gapped[1], two, "after a blank line");
}

QNP_TEST(replay_battle_log_survives_a_file_that_is_not_a_log) {
    const BattleLogRowV1 row = populatedRow();
    const std::string good = encodeBattleLogRowV1(row);

    QNP_CHECK(decodeBattleLogV1(pseudoRandomBytes(4096u, 0x5EED1234u, false))
                  .empty());
    QNP_CHECK(decodeBattleLogV1(pseudoRandomBytes(64u, 0x00C0FFEEu, false))
                  .empty());

    const std::vector<BattleLogRowV1> mixed = decodeBattleLogV1(
        good + pseudoRandomBytes(2048u, 0x1234ABCDu, true) + "\n" + good);
    QNP_REQUIRE(mixed.size() == 2u);
    checkSameRow(mixed[0], row, "before noise");
    checkSameRow(mixed[1], row, "after noise");

    const std::vector<BattleLogRowV1> aroundNul =
        decodeBattleLogV1(good + std::string("junk\0junk\n", 10u) + good);
    QNP_CHECK(aroundNul.size() <= 2u);
    for (const BattleLogRowV1& seen : aroundNul) {
        checkSameRow(seen, row, "either side of a NUL");
    }

    QNP_CHECK(decodeBattleLogV1(std::string(1000u, '\n')).empty());
    QNP_CHECK(decodeBattleLogV1(std::string(4096u, '\t') + "\n").empty());
    QNP_CHECK(decodeBattleLogV1(std::string(1u << 18, 'A')).empty());
    QNP_CHECK(decodeBattleLogV1(std::string(1u << 18, 'A') + "\n").empty());
    QNP_CHECK(decodeBattleLogV1(std::string(1u << 14, '\t')).empty());

    const std::vector<std::string> fields = splitTabs(withoutLf(good));
    QNP_REQUIRE(fields.size() > 6u);

    std::vector<std::string> huge = fields;
    huge.back() = std::string(1u << 16, 'x');
    BattleLogRowV1 fromHuge;
    if (decodeBattleLogRowV1(withoutLf(joinTabs(huge)), fromHuge)) {
        QNP_CHECK(fromHuge.digest == row.digest);
    }

    const char* const hostileNumbers[] = { "99999999999999999999999999",
                                           "-1", "0x10", "1e9", " 5", "+7" };
    for (const char* const number : hostileNumbers) {
        std::vector<std::string> bent = fields;
        bent[2] = number;
        BattleLogRowV1 fromBent;
        if (decodeBattleLogRowV1(withoutLf(joinTabs(bent)), fromBent)) {
            QNP_CHECK(fromBent.relativePath == row.relativePath);
            QNP_CHECK(fromBent.digest == row.digest);
        }
    }
}

QNP_TEST(replay_battle_log_sanitizes_text_without_erasing_a_name) {
    const std::string hostile =
        std::string("Ali\tce\rBo\nb") + std::string("\x01\x1F", 2u) + "end";
    BattleLogRowV1 row = populatedRow();
    row.nameOne = hostile;
    const std::string encoded = encodeBattleLogRowV1(row);

    QNP_REQUIRE(!encoded.empty());
    QNP_CHECK(encoded.find('\r') == std::string::npos);
    QNP_CHECK(encoded.find('\n') == encoded.size() - 1u);
    QNP_CHECK(splitTabs(withoutLf(encoded)).size()
              == splitTabs(encodedLine(populatedRow())).size());

    BattleLogRowV1 decoded;
    QNP_CHECK(decodeBattleLogRowV1(withoutLf(encoded), decoded));
    QNP_CHECK(decoded.nameTwo == row.nameTwo);
    QNP_CHECK(decoded.myName == row.myName);
    QNP_CHECK(decoded.relativePath == row.relativePath);
    QNP_CHECK(decoded.digest == row.digest);
    QNP_CHECK(decoded.frames == row.frames);
    QNP_CHECK(decoded.nameOne == sanitizeBattleLogText(hostile));
    QNP_CHECK(decoded.nameOne.find('\t') == std::string::npos);
    for (const char raw : decoded.nameOne) {
        QNP_CHECK(static_cast<unsigned char>(raw) >= 0x20u);
    }

    QNP_CHECK(sanitizeBattleLogText("a\tb\rc\nd") == "a b c d");
    QNP_CHECK(sanitizeBattleLogText("a\t\tb") == "a  b");
    QNP_CHECK(sanitizeBattleLogText(std::string("x\0y", 3u)) == "x y");
    QNP_CHECK(sanitizeBattleLogText("Alice") == "Alice");
    QNP_CHECK(sanitizeBattleLogText("") == "");

    const std::string japanese(kJapanese);
    QNP_CHECK(sanitizeBattleLogText(japanese) == japanese);
    BattleLogRowV1 named = populatedRow();
    named.nameOne = japanese;
    named.nameTwo = japanese + " " + japanese;
    named.myName = japanese;
    BattleLogRowV1 decodedNamed;
    QNP_CHECK(decodeBattleLogRowV1(encodedLine(named), decodedNamed));
    checkSameRow(decodedNamed, named, "japanese");
    QNP_REQUIRE(decodedNamed.nameOne.size() == 9u);
    QNP_CHECK(decodedNamed.nameOne == japanese);

    QNP_CHECK(sanitizeBattleLogText(std::string(200u, 'x')).size() == 64u);
    QNP_CHECK(sanitizeBattleLogText(std::string(200u, 'x'), 8u).size() == 8u);

    const std::string many = japanese + japanese + japanese + japanese;
    const std::string cut = sanitizeBattleLogText(many, 8u);
    QNP_CHECK(cut.size() <= 8u);
    QNP_CHECK(cut == japanese.substr(0u, 6u));
    const std::string emoji(kEmoji);
    QNP_CHECK(sanitizeBattleLogText(emoji, 4u) == emoji);
    QNP_CHECK(sanitizeBattleLogText(emoji, 3u) == "");
    QNP_CHECK(sanitizeBattleLogText(emoji, 2u) == "");
    QNP_CHECK(sanitizeBattleLogText(emoji, 1u) == "");
    QNP_CHECK(sanitizeBattleLogText(emoji + emoji, 5u) == emoji);
    QNP_CHECK(sanitizeBattleLogText(japanese + emoji, 12u) == japanese);
}

QNP_TEST(replay_battle_log_duration_is_sixty_point_six_hertz) {
    QNP_CHECK(battleLogSecondsFromFrames(6060u) == 100u);
    QNP_CHECK(battleLogSecondsFromFrames(6060u) != 101u);
    QNP_CHECK(battleLogSecondsFromFrames(3636u) == 60u);
    QNP_CHECK(battleLogSecondsFromFrames(3600u) == 59u);

    QNP_CHECK(battleLogSecondsFromFrames(0u) == 0u);
    QNP_CHECK(battleLogSecondsFromFrames(1u) == 0u);
    QNP_CHECK(battleLogSecondsFromFrames(60u) == 0u);
    QNP_CHECK(battleLogSecondsFromFrames(61u) == 1u);
    QNP_CHECK(battleLogSecondsFromFrames(303u) == 5u);
    QNP_CHECK(battleLogSecondsFromFrames(6059u) == 99u);

    QNP_CHECK(battleLogSecondsFromFrames(UINT32_MAX) == 70874047u);
    QNP_CHECK(battleLogSecondsFromFrames(4294967295u) != 708740u);
    QNP_CHECK(battleLogSecondsFromFrames(43000000u) == 709570u);
}

QNP_TEST(replay_battle_log_header_line_is_not_a_row) {
    const std::string header = battleLogHeaderLineV1();
    QNP_REQUIRE(!header.empty());
    QNP_CHECK(header == std::string(battleLogHeaderLineV1()));
    QNP_CHECK(header.find('\r') == std::string::npos);

    BattleLogRowV1 sink;
    QNP_CHECK(!decodeBattleLogRowV1(withoutLf(header), sink));

    const BattleLogRowV1 row = populatedRow();
    const std::string file = headerFileLine()
        + encodeBattleLogRowV1(row) + encodeBattleLogRowV1(row);
    const std::vector<BattleLogRowV1> rows = decodeBattleLogV1(file);
    QNP_REQUIRE(rows.size() == 2u);
    checkSameRow(rows[0], row, "first row under the header");
    checkSameRow(rows[1], row, "second row under the header");
}
