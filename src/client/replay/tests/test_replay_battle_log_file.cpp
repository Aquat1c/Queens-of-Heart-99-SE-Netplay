#include "TestHarness.h"

#include "ReplayTestFixtures.h"
#include "client/replay/ReplayBattleLogFile.h"
#include "client/replay/ReplayBattleStats.h"

#include "platform/WidePath.h"

#include <string>
#include <vector>

using namespace qclient::replay::test;

using qclient::replay::BattleLogRowV1;
using qclient::replay::BattleStatsSummary;
using qclient::replay::appendBattleLogRow;
using qclient::replay::battleLogHeaderLineV1;
using qclient::replay::battleLogPathFor;
using qclient::replay::decodeBattleLogV1;
using qclient::replay::kBattleSideOne;
using qclient::replay::kBattleSideTwo;
using qclient::replay::readBattleLogFile;
using qclient::replay::summarizeBattleLog;

namespace {

BattleLogRowV1 gameRow(uint64_t gameId, uint64_t setId, uint32_t gameNo,
                       int8_t mySide, uint8_t winningSideMask) {
    BattleLogRowV1 row;
    row.gameUuidHigh = 0x5151515151515151ull;
    row.gameUuidLow = gameId;
    row.setUuidHigh = 0x9999999999999999ull;
    row.setUuidLow = setId;
    row.gameNo = gameNo;
    row.startUnix = 1789000000 + static_cast<int64_t>(gameNo) * 300;
    row.endUnix = row.startUnix + 120;
    row.setStartUnix = 1789000000;
    row.timezoneOffsetMinutes = 180;
    row.mySide = mySide;
    row.characterOne = 3u;
    row.characterTwo = 8u;
    row.winningSideMask = winningSideMask;
    row.roundsOne = 2;
    row.roundsTwo = 1;
    row.frames = 7272u;
    row.roundsToWin = 2u;
    row.nameOne = "Me";
    row.nameTwo = "Rival";
    row.myName = "Me";
    row.relativePath = "09-20_Me-vs-Rival_51515151\\netplay_G01.q99r";
    return row;
}

size_t countHeaders(const std::string& text) {
    const std::string header = battleLogHeaderLineV1();
    size_t found = 0u;
    size_t at = text.find(header);
    while (at != std::string::npos) {
        ++found;
        at = text.find(header, at + 1u);
    }
    return found;
}

}

QNP_TEST(replay_battle_log_file_missing_reads_as_empty) {
    TempDir dir(L"battlelog_missing");
    QNP_REQUIRE(dir.valid());

    std::string text = "not cleared yet";
    QNP_CHECK(readBattleLogFile(dir.path, text));
    QNP_CHECK(text.empty());
    QNP_CHECK(decodeBattleLogV1(text).empty());
    QNP_CHECK(!qplat::fileExistsW(battleLogPathFor(dir.path)));

    const BattleStatsSummary summary =
        summarizeBattleLog(decodeBattleLogV1(text));
    QNP_CHECK(summary.rows == 0u);
    QNP_CHECK(summary.countedGames == 0u);
    QNP_CHECK(summary.averageGamesPerSet == 0u);
    QNP_CHECK(summary.sets_.empty());

    QNP_CHECK(battleLogPathFor(std::wstring()).empty());
    QNP_CHECK(!appendBattleLogRow(std::wstring(),
                                  gameRow(1u, 1u, 1u, kBattleSideOne, 1u)));
}

QNP_TEST(replay_battle_log_file_appends_without_a_second_header) {
    TempDir dir(L"battlelog_append");
    QNP_REQUIRE(dir.valid());

    QNP_REQUIRE(appendBattleLogRow(
        dir.path, gameRow(0xA1ull, 7u, 1u, kBattleSideOne, 1u)));
    QNP_REQUIRE(appendBattleLogRow(
        dir.path, gameRow(0xA2ull, 7u, 2u, kBattleSideOne, 2u)));
    QNP_REQUIRE(appendBattleLogRow(
        dir.path, gameRow(0xA3ull, 7u, 3u, kBattleSideOne, 1u)));

    std::string text;
    QNP_REQUIRE(readBattleLogFile(dir.path, text));
    QNP_CHECK(countHeaders(text) == 1u);
    QNP_CHECK(text.compare(0u, std::string(battleLogHeaderLineV1()).size(),
                           battleLogHeaderLineV1()) == 0);

    const std::vector<BattleLogRowV1> rows = decodeBattleLogV1(text);
    QNP_REQUIRE(rows.size() == 3u);
    QNP_CHECK(rows[0].gameUuidLow == 0xA1ull);
    QNP_CHECK(rows[1].gameUuidLow == 0xA2ull);
    QNP_CHECK(rows[2].gameUuidLow == 0xA3ull);
    QNP_CHECK(rows[0].gameNo == 1u);
    QNP_CHECK(rows[2].gameNo == 3u);

    const BattleStatsSummary summary = summarizeBattleLog(rows);
    QNP_CHECK(summary.rows == 3u);
    QNP_CHECK(summary.countedGames == 3u);
    QNP_CHECK(summary.games.wins == 2u);
    QNP_CHECK(summary.games.losses == 1u);
    QNP_REQUIRE(summary.sets_.size() == 1u);
    QNP_CHECK(summary.sets_[0].games == 3u);
    QNP_CHECK(summary.sets.wins == 1u);

    QNP_CHECK(rows[0].relativePath
              == "09-20_Me-vs-Rival_51515151\\netplay_G01.q99r");
    QNP_CHECK(rows[0].nameTwo == "Rival");
}

QNP_TEST(replay_battle_log_file_torn_tail_keeps_every_whole_row) {
    TempDir dir(L"battlelog_torn");
    QNP_REQUIRE(dir.valid());

    QNP_REQUIRE(appendBattleLogRow(
        dir.path, gameRow(0xB1ull, 9u, 1u, kBattleSideTwo, 2u)));
    QNP_REQUIRE(appendBattleLogRow(
        dir.path, gameRow(0xB2ull, 9u, 2u, kBattleSideTwo, 2u)));

    std::string whole;
    QNP_REQUIRE(readBattleLogFile(dir.path, whole));
    QNP_REQUIRE(decodeBattleLogV1(whole).size() == 2u);

    const size_t lastLine = whole.find_last_of('\n', whole.size() - 2u);
    QNP_REQUIRE(lastLine != std::string::npos);
    const std::string torn =
        whole.substr(0u, lastLine + 1u + (whole.size() - lastLine) / 2u);
    const std::vector<uint8_t> bytes(torn.begin(), torn.end());
    QNP_REQUIRE(writeBytes(
        battleLogPathFor(dir.path), bytes));

    std::string readBack;
    QNP_REQUIRE(readBattleLogFile(dir.path, readBack));
    QNP_CHECK(readBack == torn);
    const std::vector<BattleLogRowV1> rows = decodeBattleLogV1(readBack);
    QNP_REQUIRE(rows.size() == 1u);
    QNP_CHECK(rows[0].gameUuidLow == 0xB1ull);

    QNP_REQUIRE(appendBattleLogRow(
        dir.path, gameRow(0xB3ull, 9u, 3u, kBattleSideTwo, 1u)));
    std::string repaired;
    QNP_REQUIRE(readBattleLogFile(dir.path, repaired));
    QNP_CHECK(countHeaders(repaired) == 1u);
    const std::vector<BattleLogRowV1> after = decodeBattleLogV1(repaired);
    QNP_REQUIRE(!after.empty());
    QNP_CHECK(after.front().gameUuidLow == 0xB1ull);
    QNP_CHECK(after.back().gameUuidLow == 0xB3ull);
}
