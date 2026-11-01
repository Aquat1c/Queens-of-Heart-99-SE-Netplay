#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ReplayBattleLog.h"

namespace qclient {
namespace replay {

struct BattleCountV2 {
    uint32_t wins = 0u;
    uint32_t losses = 0u;
    uint32_t draws = 0u;
    uint32_t total() const { return wins + losses + draws; }
};

struct BattleOpponentStats {
    std::string name;
    BattleCountV2 games;
    BattleCountV2 sets;
    uint32_t seconds = 0u;
};

struct BattleCharacterStats {
    uint8_t characterId = 0u;
    uint32_t games = 0u;
    BattleCountV2 results;
};

struct BattleMatchupStats {
    uint8_t mine = 0u;
    uint8_t theirs = 0u;
    uint32_t games = 0u;
    BattleCountV2 results;
};

struct BattleSetSummary {
    uint64_t setUuidHigh = 0u;
    uint64_t setUuidLow = 0u;
    int64_t startUnix = 0;
    int32_t timezoneOffsetMinutes = 0;
    uint32_t games = 0u;
    uint32_t seconds = 0u;
    BattleCountV2 result;
    std::string opponent;
    bool hasLocalSide = false;
    uint8_t myCharacter = 0xFFu;
    uint8_t theirCharacter = 0xFFu;
    bool iSwitchedCharacter = false;
    bool theySwitchedCharacter = false;
};

struct BattleStatsSummary {
    uint32_t rows = 0u;
    uint32_t countedGames = 0u;
    BattleCountV2 games;
    BattleCountV2 sets;
    uint32_t totalSeconds = 0u;
    uint32_t spectatedGames = 0u;
    uint32_t offlineGames = 0u;
    uint32_t averageGamesPerSet = 0u;
    uint32_t averageSetSeconds = 0u;
    uint32_t longestSetGames = 0u;
    uint32_t longestSetSeconds = 0u;
    int32_t longestSetByGamesIndex = -1;
    int32_t longestSetBySecondsIndex = -1;
    uint32_t countedSets = 0u;
    uint8_t mostUsedCharacter = 0xFFu;
    uint8_t mostFacedCharacter = 0xFFu;
    // The matchup played most, as mine << 8 | theirs, or 0xFFFF for none.
    uint16_t mostPlayedMatchup = 0xFFFFu;
    int64_t firstUnix = 0;
    int64_t lastUnix = 0;
    std::string recentOpponent;
    int64_t recentUnix = 0;

    std::vector<BattleSetSummary> sets_;
    std::vector<BattleOpponentStats> opponents;
    std::vector<BattleCharacterStats> characters;
    std::vector<BattleMatchupStats> matchups;
};

constexpr uint8_t kBattleCharacterUnknown = 0xFFu;

BattleStatsSummary summarizeBattleLog(const std::vector<BattleLogRowV1>& rows);

std::vector<BattleLogRowV1> battleSetGames(
    const std::vector<BattleLogRowV1>& rows,
    uint64_t setUuidHigh, uint64_t setUuidLow,
    uint64_t gameUuidHigh = 0u, uint64_t gameUuidLow = 0u);

BattleStatsSummary summarizeAgainst(const std::vector<BattleLogRowV1>& rows,
                                    const std::string& opponentName);

enum class BattleGameVerdict : uint8_t {
    NoLocalSide = 0,
    Won = 1,
    Lost = 2,
    Draw = 3,
};
BattleGameVerdict battleGameVerdict(const BattleLogRowV1& row);
const char* battleGameVerdictText(BattleGameVerdict verdict);
const char* battleGameSidesText(const BattleLogRowV1& row);
std::string battleOpponentOf(const BattleLogRowV1& row);
int32_t battleWinRateTenths(const BattleCountV2& count);

std::string battleCountText(const BattleCountV2& count);
std::string battleDurationText(uint32_t seconds);

}
}
