#include "TestHarness.h"

#include "client/replay/ReplayBattleStats.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace qclient::replay;

namespace {

constexpr int64_t kNoon = 1789905600;
constexpr int64_t kHour = 3600;
constexpr int64_t kDay = 86400;

constexpr uint64_t kSetUuidHigh = 0x5E70000000000000ull;

constexpr int8_t kNobodyWon = -1;

uint32_t framesForSeconds(uint32_t seconds) {
    return seconds / 5u * 303u;
}

struct GameSpec {
    int8_t mySide;
    uint8_t characterOne;
    uint8_t characterTwo;
    int8_t winner;
    uint32_t seconds;
};

std::vector<BattleLogRowV1> setRows(uint64_t setLow, int64_t setStartUnix,
                                    const char* nameOne, const char* nameTwo,
                                    const std::vector<GameSpec>& games) {
    std::vector<BattleLogRowV1> rows;
    rows.reserve(games.size());
    int64_t clock = setStartUnix;
    for (std::size_t index = 0u; index < games.size(); ++index) {
        const GameSpec& spec = games[index];
        BattleLogRowV1 row{};
        row.gameUuidHigh = setLow;
        row.gameUuidLow = static_cast<uint64_t>(index) + 1u;
        row.setUuidHigh = kSetUuidHigh;
        row.setUuidLow = setLow;
        row.gameNo = static_cast<uint32_t>(index) + 1u;
        row.startUnix = clock;
        row.endUnix = clock + static_cast<int64_t>(spec.seconds);
        row.setStartUnix = setStartUnix;
        row.mySide = spec.mySide;
        row.characterOne = spec.characterOne;
        row.characterTwo = spec.characterTwo;
        uint8_t mask = 0u;
        if (spec.winner >= 0) {
            mask = static_cast<uint8_t>(1 << spec.winner);
        }
        row.winningSideMask = mask;
        row.frames = framesForSeconds(spec.seconds);
        row.roundsToWin = 2u;
        row.nameOne = nameOne;
        row.nameTwo = nameTwo;
        if (spec.mySide == kBattleSideOne) {
            row.myName = nameOne;
        } else if (spec.mySide == kBattleSideTwo) {
            row.myName = nameTwo;
        } else if (spec.mySide == kBattleSideOffline) {
            row.myName = nameOne;
        }
        rows.push_back(row);
        clock += static_cast<int64_t>(spec.seconds) + 30;
    }
    return rows;
}

void append(std::vector<BattleLogRowV1>& out,
            const std::vector<BattleLogRowV1>& more) {
    out.insert(out.end(), more.begin(), more.end());
}

bool countEquals(const BattleCountV2& count, uint32_t wins, uint32_t losses,
                 uint32_t draws) {
    return count.wins == wins && count.losses == losses
        && count.draws == draws;
}

const BattleOpponentStats* opponentNamed(const BattleStatsSummary& summary,
                                         const char* name) {
    for (const BattleOpponentStats& entry : summary.opponents) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

const BattleSetSummary* setWithLow(const BattleStatsSummary& summary,
                                   uint64_t setLow) {
    for (const BattleSetSummary& entry : summary.sets_) {
        if (entry.setUuidLow == setLow) return &entry;
    }
    return nullptr;
}

}

QNP_TEST(replay_battle_stats_browses_a_set_and_one_opponent) {
    std::vector<BattleLogRowV1> rows;
    append(rows, setRows(1u, kNoon, "Me", "Rival",
                         {{kBattleSideOne, 3u, 4u, 0, 60u},
                          {kBattleSideOne, 3u, 4u, 1, 90u},
                          {kBattleSideOne, 8u, 4u, 0, 45u}}));
    append(rows, setRows(2u, kNoon + kDay, "Me", "Other",
                         {{kBattleSideOne, 3u, 9u, 0, 30u}}));

    const BattleStatsSummary summary = summarizeBattleLog(rows);

    const BattleSetSummary* first = setWithLow(summary, 1u);
    QNP_REQUIRE(first != nullptr);
    QNP_CHECK(first->myCharacter == 3u);
    QNP_CHECK(first->theirCharacter == 4u);
    QNP_CHECK(first->iSwitchedCharacter);
    QNP_CHECK(!first->theySwitchedCharacter);
    const BattleSetSummary* second = setWithLow(summary, 2u);
    QNP_REQUIRE(second != nullptr);
    QNP_CHECK(!second->iSwitchedCharacter);
    QNP_CHECK(!second->theySwitchedCharacter);

    QNP_CHECK(summary.countedSets == 2u);
    QNP_CHECK(summary.averageGamesPerSet == 2u);
    QNP_CHECK(summary.longestSetGames == 3u);
    QNP_CHECK(summary.averageSetSeconds != 0u);
    QNP_REQUIRE(!summary.matchups.empty());
    QNP_CHECK(summary.mostPlayedMatchup
              == static_cast<uint16_t>(
                     (summary.matchups.front().mine << 8)
                     | summary.matchups.front().theirs));
    QNP_CHECK(summary.recentOpponent == "Other");

    const std::vector<BattleLogRowV1> games =
        battleSetGames(rows, first->setUuidHigh, first->setUuidLow);
    QNP_REQUIRE(games.size() == 3u);
    QNP_CHECK(games[0].gameNo < games[1].gameNo);
    QNP_CHECK(games[1].gameNo < games[2].gameNo);
    QNP_CHECK(games[2].characterOne == 8u);
    QNP_CHECK(battleSetGames(rows, 0x5151u, 0x5151u).empty());

    const BattleStatsSummary versusRival = summarizeAgainst(rows, "Rival");
    QNP_CHECK(versusRival.countedGames == 3u);
    QNP_CHECK(versusRival.games.wins == 2u);
    QNP_CHECK(versusRival.games.losses == 1u);
    QNP_CHECK(versusRival.recentOpponent == "Rival");
    QNP_CHECK(versusRival.sets.total() == 1u);
    for (const BattleOpponentStats& entry : versusRival.opponents)
        QNP_CHECK(entry.name == "Rival");

    const BattleStatsSummary versusNobody =
        summarizeAgainst(rows, "Somebody Else");
    QNP_CHECK(versusNobody.rows == 0u);
    QNP_CHECK(versusNobody.countedGames == 0u);
    QNP_CHECK(versusNobody.sets_.empty());
}

QNP_TEST(replay_battle_stats_counts_games_and_sets) {
    std::vector<BattleLogRowV1> rows;
    append(rows, setRows(1u, kNoon, "Me", "Rival",
                         {{kBattleSideOne, 3u, 4u, 0, 60u},
                          {kBattleSideOne, 3u, 4u, 0, 90u},
                          {kBattleSideOne, 3u, 4u, 1, 45u},
                          {kBattleSideOne, 3u, 4u, kNobodyWon, 105u}}));
    append(rows, setRows(2u, kNoon + kDay, "Other", "Me",
                         {{kBattleSideTwo, 8u, 3u, 1, 125u},
                          {kBattleSideTwo, 8u, 3u, 0, 60u}}));

    const BattleStatsSummary summary = summarizeBattleLog(rows);
    QNP_CHECK(summary.rows == 6u);
    QNP_CHECK(summary.countedGames == 6u);
    QNP_CHECK(countEquals(summary.games, 3u, 2u, 1u));
    QNP_CHECK(summary.countedGames == summary.games.total());

    QNP_CHECK(countEquals(summary.sets, 1u, 0u, 1u));

    QNP_CHECK(summary.totalSeconds == 485u);
    QNP_CHECK(battleDurationText(summary.totalSeconds) == "8m 5s");

    QNP_CHECK(summary.averageGamesPerSet == 3u);
    QNP_CHECK(summary.longestSetGames == 4u);
    QNP_CHECK(summary.longestSetSeconds == 300u);
    QNP_CHECK(summary.firstUnix == kNoon);
    QNP_CHECK(summary.lastUnix >= kNoon + kDay);

    QNP_REQUIRE(summary.sets_.size() == 2u);
    const BattleSetSummary* const first = setWithLow(summary, 1u);
    const BattleSetSummary* const second = setWithLow(summary, 2u);
    QNP_REQUIRE(first != nullptr);
    QNP_REQUIRE(second != nullptr);
    QNP_CHECK(first->setUuidHigh == kSetUuidHigh);
    QNP_CHECK(first->startUnix == kNoon);
    QNP_CHECK(first->games == 4u);
    QNP_CHECK(first->seconds == 300u);
    QNP_CHECK(countEquals(first->result, 2u, 1u, 1u));
    QNP_CHECK(first->hasLocalSide);
    QNP_CHECK(second->games == 2u);
    QNP_CHECK(second->seconds == 185u);
    QNP_CHECK(countEquals(second->result, 1u, 1u, 0u));

    QNP_CHECK(first->opponent == "Rival");
    QNP_CHECK(second->opponent == "Other");

    const BattleOpponentStats* const rival = opponentNamed(summary, "Rival");
    const BattleOpponentStats* const other = opponentNamed(summary, "Other");
    QNP_REQUIRE(rival != nullptr);
    QNP_REQUIRE(other != nullptr);
    QNP_CHECK(countEquals(rival->games, 2u, 1u, 1u));
    QNP_CHECK(countEquals(rival->sets, 1u, 0u, 0u));
    QNP_CHECK(rival->seconds == 300u);
    QNP_CHECK(countEquals(other->games, 1u, 1u, 0u));
    QNP_CHECK(countEquals(other->sets, 0u, 0u, 1u));
    QNP_CHECK(other->seconds == 185u);
}

QNP_TEST(replay_battle_stats_never_scores_spectated_or_offline_games) {
    std::vector<BattleLogRowV1> rows;
    append(rows, setRows(3u, kNoon, "Ann", "Ben",
                         {{kBattleSideSpectated, 5u, 6u, 0, 100u},
                          {kBattleSideSpectated, 5u, 6u, 1, 100u}}));
    append(rows, setRows(4u, kNoon + kHour, "Me", "",
                         {{kBattleSideOffline, 1u, 2u, 0, 50u},
                          {kBattleSideOffline, 1u, 2u, 1, 50u},
                          {kBattleSideOffline, 1u, 2u, 0, 50u}}));
    append(rows, setRows(5u, kNoon + 2 * kHour, "Me", "Rival",
                         {{kBattleSideOne, 3u, 4u, 0, 60u},
                          {kBattleSideOne, 3u, 4u, 1, 60u},
                          {kBattleSideOne, 3u, 4u, 0, 60u}}));

    const BattleStatsSummary summary = summarizeBattleLog(rows);
    QNP_CHECK(summary.rows == 8u);
    QNP_CHECK(summary.countedGames == 3u);
    QNP_CHECK(countEquals(summary.games, 2u, 1u, 0u));
    QNP_CHECK(summary.spectatedGames == 2u);
    QNP_CHECK(summary.offlineGames == 3u);

    QNP_CHECK(summary.totalSeconds == 530u);
    QNP_CHECK(battleDurationText(summary.totalSeconds) == "8m 50s");

    QNP_CHECK(countEquals(summary.sets, 1u, 0u, 0u));

    QNP_CHECK(summary.averageGamesPerSet == 3u);

    QNP_REQUIRE(summary.sets_.size() == 3u);
    const BattleSetSummary* const watched = setWithLow(summary, 3u);
    const BattleSetSummary* const offline = setWithLow(summary, 4u);
    const BattleSetSummary* const played = setWithLow(summary, 5u);
    QNP_REQUIRE(watched != nullptr);
    QNP_REQUIRE(offline != nullptr);
    QNP_REQUIRE(played != nullptr);
    QNP_CHECK(!watched->hasLocalSide);
    QNP_CHECK(watched->games == 2u);
    QNP_CHECK(watched->seconds == 200u);
    QNP_CHECK(countEquals(watched->result, 0u, 0u, 0u));
    QNP_CHECK(offline->games == 3u);
    QNP_CHECK(offline->seconds == 150u);
    QNP_CHECK(countEquals(offline->result, 0u, 0u, 0u));
    QNP_CHECK(played->hasLocalSide);
    QNP_CHECK(countEquals(played->result, 2u, 1u, 0u));

    uint32_t opponentWins = 0u;
    uint32_t opponentLosses = 0u;
    for (const BattleOpponentStats& entry : summary.opponents) {
        opponentWins += entry.games.wins;
        opponentLosses += entry.games.losses;
    }
    QNP_CHECK(opponentWins == summary.games.wins);
    QNP_CHECK(opponentLosses == summary.games.losses);

    QNP_CHECK(summary.mostUsedCharacter == 3u);
    QNP_CHECK(summary.mostFacedCharacter == 4u);
}

QNP_TEST(replay_battle_stats_attributes_by_side_not_by_name) {
    std::vector<BattleLogRowV1> rows;
    append(rows, setRows(6u, kNoon, "Q", "Q",
                         {{kBattleSideTwo, 5u, 9u, 0, 60u}}));
    append(rows, setRows(7u, kNoon + kHour, "Q", "Rival",
                         {{kBattleSideOne, 9u, 5u, 0, 60u}}));

    const BattleStatsSummary summary = summarizeBattleLog(rows);
    QNP_CHECK(summary.countedGames == 2u);
    QNP_CHECK(countEquals(summary.games, 1u, 1u, 0u));

    QNP_CHECK(summary.mostUsedCharacter == 9u);
    QNP_CHECK(summary.mostFacedCharacter == 5u);
    QNP_REQUIRE(summary.characters.size() == 1u);
    QNP_CHECK(summary.characters[0].characterId == 9u);
    QNP_CHECK(summary.characters[0].games == 2u);
    QNP_CHECK(countEquals(summary.characters[0].results, 1u, 1u, 0u));

    const BattleOpponentStats* const sameName = opponentNamed(summary, "Q");
    const BattleOpponentStats* const rival = opponentNamed(summary, "Rival");
    QNP_REQUIRE(sameName != nullptr);
    QNP_REQUIRE(rival != nullptr);
    QNP_CHECK(countEquals(sameName->games, 0u, 1u, 0u));
    QNP_CHECK(countEquals(rival->games, 1u, 0u, 0u));
    QNP_CHECK(summary.opponents.size() == 2u);
}

QNP_TEST(replay_battle_stats_reads_the_mask_not_the_round_tally) {
    std::vector<BattleLogRowV1> rows =
        setRows(8u, kNoon, "Me", "Rival",
                {{kBattleSideOne, 3u, 4u, kNobodyWon, 60u},
                 {kBattleSideOne, 3u, 4u, kNobodyWon, 60u},
                 {kBattleSideOne, 3u, 4u, 0, 60u}});
    QNP_REQUIRE(rows.size() == 3u);
    rows[1].roundsOne = 2;
    rows[1].roundsTwo = 1;
    QNP_CHECK(rows[2].roundsOne == kBattleRoundsUnknown);
    QNP_CHECK(rows[2].roundsTwo == kBattleRoundsUnknown);
    QNP_CHECK(rows[0].winningSideMask == 0u);
    QNP_CHECK(rows[2].winningSideMask == 1u);

    const BattleStatsSummary summary = summarizeBattleLog(rows);
    QNP_CHECK(summary.countedGames == 3u);
    QNP_CHECK(countEquals(summary.games, 1u, 0u, 2u));
    QNP_CHECK(countEquals(summary.sets, 1u, 0u, 0u));
    QNP_REQUIRE(summary.sets_.size() == 1u);
    QNP_CHECK(countEquals(summary.sets_[0].result, 1u, 0u, 2u));
    QNP_CHECK(summary.sets_[0].games == 3u);
    QNP_CHECK(summary.totalSeconds == 180u);
}

QNP_TEST(replay_battle_stats_names_the_characters_by_count) {
    std::vector<BattleLogRowV1> rows;
    append(rows, setRows(9u, kNoon, "Me", "Rival",
                         {{kBattleSideOne, 5u, 12u, 0, 60u},
                          {kBattleSideOne, 5u, 12u, 1, 60u},
                          {kBattleSideOne, 5u, 12u, 0, 60u}}));
    append(rows, setRows(10u, kNoon + kHour, "Me", "Rival",
                         {{kBattleSideOne, 2u, 12u, 0, 60u}}));

    const BattleStatsSummary summary = summarizeBattleLog(rows);
    QNP_CHECK(summary.mostUsedCharacter == 5u);
    QNP_CHECK(summary.mostFacedCharacter == 12u);
    QNP_REQUIRE(summary.characters.size() == 2u);
    QNP_CHECK(summary.characters[0].characterId == 5u);
    QNP_CHECK(summary.characters[0].games == 3u);
    QNP_CHECK(countEquals(summary.characters[0].results, 2u, 1u, 0u));
    QNP_CHECK(summary.characters[1].characterId == 2u);
    QNP_CHECK(summary.characters[1].games == 1u);

    QNP_REQUIRE(summary.matchups.size() == 2u);
    QNP_CHECK(summary.matchups[0].mine == 5u);
    QNP_CHECK(summary.matchups[0].theirs == 12u);
    QNP_CHECK(summary.matchups[0].games == 3u);
    QNP_CHECK(countEquals(summary.matchups[0].results, 2u, 1u, 0u));
    QNP_CHECK(summary.matchups[1].mine == 2u);
    QNP_CHECK(summary.matchups[1].theirs == 12u);

    std::vector<BattleLogRowV1> tiedRows;
    append(tiedRows, setRows(11u, kNoon + 2 * kHour, "Me", "Rival",
                             {{kBattleSideOne, 7u, 12u, 0, 60u},
                              {kBattleSideOne, 7u, 12u, 0, 60u}}));
    append(tiedRows, setRows(12u, kNoon, "Me", "Ann",
                             {{kBattleSideOne, 3u, 4u, 0, 60u},
                              {kBattleSideOne, 3u, 4u, 0, 60u}}));

    const BattleStatsSummary tied = summarizeBattleLog(tiedRows);
    QNP_CHECK(tied.mostUsedCharacter == 3u);
    QNP_CHECK(tied.mostFacedCharacter == 4u);
    QNP_REQUIRE(tied.matchups.size() == 2u);
    QNP_CHECK(tied.matchups[0].mine == 3u);
    QNP_CHECK(tied.matchups[0].theirs == 4u);
    QNP_CHECK(tied.matchups[0].games == 2u);
    QNP_CHECK(tied.matchups[1].games == 2u);
}

QNP_TEST(replay_battle_stats_orders_sets_and_tables) {
    const std::vector<BattleLogRowV1> newest =
        setRows(20u, kNoon + 2 * kDay, "Me", "Cara",
                {{kBattleSideOne, 11u, 4u, 0, 60u},
                 {kBattleSideOne, 11u, 4u, 0, 60u},
                 {kBattleSideOne, 11u, 4u, 1, 60u}});
    const std::vector<BattleLogRowV1> middle =
        setRows(21u, kNoon + kDay, "Me", "Bob",
                {{kBattleSideOne, 11u, 4u, 0, 60u}});
    const std::vector<BattleLogRowV1> oldest =
        setRows(22u, kNoon, "Me", "Alice",
                {{kBattleSideOne, 6u, 4u, 0, 60u},
                 {kBattleSideOne, 6u, 4u, 1, 60u},
                 {kBattleSideOne, 6u, 4u, 0, 60u}});

    std::vector<BattleLogRowV1> rows;
    rows.reserve(7u);
    rows.push_back(middle[0]);
    rows.push_back(newest[0]);
    rows.push_back(oldest[0]);
    rows.push_back(newest[1]);
    rows.push_back(oldest[1]);
    rows.push_back(newest[2]);
    rows.push_back(oldest[2]);

    const BattleStatsSummary summary = summarizeBattleLog(rows);
    QNP_CHECK(summary.countedGames == 7u);
    QNP_REQUIRE(summary.sets_.size() == 3u);
    QNP_CHECK(summary.sets_[0].setUuidLow == 20u);
    QNP_CHECK(summary.sets_[1].setUuidLow == 21u);
    QNP_CHECK(summary.sets_[2].setUuidLow == 22u);
    QNP_CHECK(summary.sets_[0].startUnix > summary.sets_[1].startUnix);
    QNP_CHECK(summary.sets_[1].startUnix > summary.sets_[2].startUnix);
    QNP_CHECK(summary.sets_[0].games == 3u);
    QNP_CHECK(summary.sets_[2].games == 3u);
    QNP_CHECK(summary.sets_[0].opponent == "Cara");
    QNP_CHECK(summary.sets_[2].opponent == "Alice");
    QNP_CHECK(summary.firstUnix == kNoon);

    QNP_REQUIRE(summary.opponents.size() == 3u);
    QNP_CHECK(summary.opponents[0].name == "Alice");
    QNP_CHECK(summary.opponents[1].name == "Cara");
    QNP_CHECK(summary.opponents[2].name == "Bob");
    QNP_CHECK(summary.opponents[0].games.total() == 3u);
    QNP_CHECK(summary.opponents[1].games.total() == 3u);
    QNP_CHECK(summary.opponents[2].games.total() == 1u);

    QNP_REQUIRE(summary.characters.size() == 2u);
    QNP_CHECK(summary.characters[0].characterId == 11u);
    QNP_CHECK(summary.characters[0].games == 4u);
    QNP_CHECK(summary.characters[1].characterId == 6u);
    QNP_CHECK(summary.characters[1].games == 3u);
    QNP_CHECK(summary.mostUsedCharacter == 11u);
}

QNP_TEST(replay_battle_stats_survives_an_empty_log) {
    const std::vector<BattleLogRowV1> none;
    const BattleStatsSummary empty = summarizeBattleLog(none);
    QNP_CHECK(empty.rows == 0u);
    QNP_CHECK(empty.countedGames == 0u);
    QNP_CHECK(countEquals(empty.games, 0u, 0u, 0u));
    QNP_CHECK(countEquals(empty.sets, 0u, 0u, 0u));
    QNP_CHECK(empty.totalSeconds == 0u);
    QNP_CHECK(empty.spectatedGames == 0u);
    QNP_CHECK(empty.offlineGames == 0u);
    QNP_CHECK(empty.averageGamesPerSet == 0u);
    QNP_CHECK(empty.longestSetGames == 0u);
    QNP_CHECK(empty.longestSetSeconds == 0u);
    QNP_CHECK(empty.mostUsedCharacter == kBattleCharacterUnknown);
    QNP_CHECK(empty.mostFacedCharacter == kBattleCharacterUnknown);
    QNP_CHECK(empty.firstUnix == 0);
    QNP_CHECK(empty.lastUnix == 0);
    QNP_CHECK(empty.sets_.empty());
    QNP_CHECK(empty.opponents.empty());
    QNP_CHECK(empty.characters.empty());
    QNP_CHECK(empty.matchups.empty());
    QNP_CHECK(battleCountText(empty.games) == "0-0-0");
    QNP_CHECK(battleDurationText(empty.totalSeconds) == "0s");

    const std::vector<BattleLogRowV1> watched =
        setRows(30u, kNoon, "Ann", "Ben",
                {{kBattleSideSpectated, 5u, 6u, 0, 100u},
                 {kBattleSideSpectated, 5u, 6u, 1, 100u}});
    const BattleStatsSummary seen = summarizeBattleLog(watched);
    QNP_CHECK(seen.rows == 2u);
    QNP_CHECK(seen.countedGames == 0u);
    QNP_CHECK(countEquals(seen.games, 0u, 0u, 0u));
    QNP_CHECK(countEquals(seen.sets, 0u, 0u, 0u));
    QNP_CHECK(seen.spectatedGames == 2u);
    QNP_CHECK(seen.offlineGames == 0u);
    QNP_CHECK(seen.totalSeconds == 200u);
    QNP_CHECK(seen.averageGamesPerSet == 0u);
    QNP_CHECK(seen.mostUsedCharacter == kBattleCharacterUnknown);
    QNP_CHECK(seen.mostFacedCharacter == kBattleCharacterUnknown);
    QNP_CHECK(seen.characters.empty());
    QNP_CHECK(seen.matchups.empty());
    QNP_REQUIRE(seen.sets_.size() == 1u);
    QNP_CHECK(!seen.sets_[0].hasLocalSide);
    QNP_CHECK(seen.sets_[0].games == 2u);
    QNP_CHECK(battleDurationText(seen.totalSeconds) == "3m 20s");
}

QNP_TEST(replay_battle_stats_formats_counts_and_durations) {
    BattleCountV2 count{};
    count.wins = 12u;
    count.losses = 3u;
    count.draws = 1u;
    QNP_CHECK(battleCountText(count) == "12-3-1");
    QNP_CHECK(count.total() == 16u);

    QNP_CHECK(battleCountText(BattleCountV2()) == "0-0-0");

    BattleCountV2 many{};
    many.wins = 1234u;
    QNP_CHECK(battleCountText(many) == "1234-0-0");

    QNP_CHECK(battleDurationText(0u) == "0s");
    QNP_CHECK(battleDurationText(12u) == "12s");
    QNP_CHECK(battleDurationText(59u) == "59s");
    QNP_CHECK(battleDurationText(75u) == "1m 15s");
    QNP_CHECK(battleDurationText(260u) == "4m 20s");

    QNP_CHECK(battleDurationText(3599u) == "59m 59s");
    const std::string hour = battleDurationText(3600u);
    QNP_CHECK(hour.compare(0u, 2u, "1h") == 0);
    QNP_CHECK(hour.find('s') == std::string::npos);
    QNP_CHECK(battleDurationText(3660u) == "1h 1m");
    QNP_CHECK(battleDurationText(5025u) == "1h 23m");
}

QNP_TEST(replay_battle_stats_names_one_game_like_the_totals_count_it) {
    const std::vector<BattleLogRowV1> rows = setRows(
        0x61u, kNoon, "alice", "bob",
        { { kBattleSideOne, 1u, 2u, 0, 60u },
          { kBattleSideTwo, 1u, 2u, 0, 60u },
          { kBattleSideOne, 1u, 2u, kNobodyWon, 60u },
          { kBattleSideSpectated, 1u, 2u, 1, 60u },
          { kBattleSideOffline, 1u, 2u, 0, 60u } });
    QNP_CHECK(battleGameVerdict(rows[0]) == BattleGameVerdict::Won);
    QNP_CHECK(battleGameVerdict(rows[1]) == BattleGameVerdict::Lost);
    QNP_CHECK(battleGameVerdict(rows[2]) == BattleGameVerdict::Draw);
    QNP_CHECK(battleGameVerdict(rows[3]) == BattleGameVerdict::NoLocalSide);
    QNP_CHECK(battleGameVerdict(rows[4]) == BattleGameVerdict::NoLocalSide);

    QNP_CHECK(std::string(battleGameVerdictText(BattleGameVerdict::Won))
              == "Won");
    QNP_CHECK(std::string(battleGameVerdictText(BattleGameVerdict::Lost))
              == "Lost");
    QNP_CHECK(std::string(battleGameVerdictText(BattleGameVerdict::Draw))
              == "Draw");
    QNP_CHECK(std::string(
                  battleGameVerdictText(BattleGameVerdict::NoLocalSide))
              .empty());
    QNP_CHECK(std::string(battleGameSidesText(rows[3])) == "2P won");
    QNP_CHECK(std::string(battleGameSidesText(rows[4])) == "1P won");
    QNP_CHECK(std::string(battleGameSidesText(rows[2])) == "Draw");

    QNP_CHECK(battleOpponentOf(rows[0]) == "bob");
    QNP_CHECK(battleOpponentOf(rows[1]) == "alice");
    QNP_CHECK(battleOpponentOf(rows[3]).empty());

    const BattleStatsSummary summary = summarizeBattleLog(rows);
    QNP_CHECK(summary.games.wins == 1u);
    QNP_CHECK(summary.games.losses == 1u);
    QNP_CHECK(summary.games.draws == 1u);
}

QNP_TEST(replay_battle_stats_win_rate_leaves_draws_and_nothing_out) {
    BattleCountV2 count;
    QNP_CHECK(battleWinRateTenths(count) == -1);
    count.draws = 3u;
    QNP_CHECK(battleWinRateTenths(count) == -1);
    count.wins = 4u;
    count.losses = 3u;
    QNP_CHECK(battleWinRateTenths(count) == 571);
    count.wins = 1u;
    count.losses = 2u;
    QNP_CHECK(battleWinRateTenths(count) == 333);
    count.wins = 2u;
    count.losses = 1u;
    QNP_CHECK(battleWinRateTenths(count) == 667);
}
