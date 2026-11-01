#include "ReplayBattleStats.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace qclient {
namespace replay {
namespace {

using UuidKey = std::pair<uint64_t, uint64_t>;

using SetKey = std::tuple<uint64_t, uint64_t, int>;

bool isLocalSide(int8_t mySide) {
    return mySide == kBattleSideOne || mySide == kBattleSideTwo;
}

uint32_t addSeconds(uint32_t total, uint32_t more) {
    const uint32_t sum = total + more;
    return sum < total ? 0xFFFFFFFFu : sum;
}

enum class Outcome { Win, Loss, Undecided };

Outcome outcomeFor(const BattleLogRowV1& row) {
    const unsigned mask = row.winningSideMask & 0x3u;
    if (mask == 0u || mask == 0x3u) return Outcome::Undecided;
    const unsigned mine = 1u << static_cast<unsigned>(row.mySide);
    return (mask & mine) != 0u ? Outcome::Win : Outcome::Loss;
}

void applyOutcome(BattleCountV2& count, Outcome outcome) {
    switch (outcome) {
        case Outcome::Win:  ++count.wins;   break;
        case Outcome::Loss: ++count.losses; break;
        default:            ++count.draws;  break;
    }
}

Outcome setOutcomeFrom(const BattleCountV2& games) {
    if (games.wins > games.losses) return Outcome::Win;
    if (games.losses > games.wins) return Outcome::Loss;
    return Outcome::Undecided;
}

bool earlierInSet(const BattleLogRowV1& a, const BattleLogRowV1& b) {
    if (a.gameNo != b.gameNo) return a.gameNo < b.gameNo;
    if (a.startUnix != b.startUnix) return a.startUnix < b.startUnix;
    if (a.gameUuidHigh != b.gameUuidHigh) {
        return a.gameUuidHigh < b.gameUuidHigh;
    }
    return a.gameUuidLow < b.gameUuidLow;
}

const std::string& opponentNameOf(const BattleLogRowV1& row) {
    return row.mySide == kBattleSideOne ? row.nameTwo : row.nameOne;
}

uint8_t myCharacterOf(const BattleLogRowV1& row) {
    return row.mySide == kBattleSideOne ? row.characterOne : row.characterTwo;
}

uint8_t theirCharacterOf(const BattleLogRowV1& row) {
    return row.mySide == kBattleSideOne ? row.characterTwo : row.characterOne;
}

struct OpponentAccum {
    BattleCountV2 games;
    BattleCountV2 sets;
    uint32_t seconds = 0u;
};

struct SetAccum {
    uint64_t uuidHigh = 0u;
    uint64_t uuidLow = 0u;
    uint32_t games = 0u;
    uint32_t seconds = 0u;
    BattleCountV2 result;
    bool hasLocal = false;
    int64_t setStart = 0;
    int64_t rowStart = 0;
    const BattleLogRowV1* first = nullptr;
    const BattleLogRowV1* firstLocal = nullptr;
    bool mineSwitched = false;
    bool theirsSwitched = false;
};

const BattleLogRowV1* preferredDuplicate(const BattleLogRowV1& kept,
                                         const BattleLogRowV1& candidate) {
    const bool keptLocal = isLocalSide(kept.mySide);
    const bool candidateLocal = isLocalSide(candidate.mySide);
    if (keptLocal != candidateLocal) return candidateLocal ? &candidate : &kept;
    if (kept.mySide != candidate.mySide) {
        return candidate.mySide < kept.mySide ? &candidate : &kept;
    }
    if (kept.startUnix != candidate.startUnix) {
        return candidate.startUnix < kept.startUnix ? &candidate : &kept;
    }
    if (kept.endUnix != candidate.endUnix) {
        return candidate.endUnix < kept.endUnix ? &candidate : &kept;
    }
    if (kept.nameOne != candidate.nameOne) {
        return candidate.nameOne < kept.nameOne ? &candidate : &kept;
    }
    if (kept.nameTwo != candidate.nameTwo) {
        return candidate.nameTwo < kept.nameTwo ? &candidate : &kept;
    }
    return &kept;
}

SetKey setKeyFor(const BattleLogRowV1& row, size_t position) {
    if (row.setUuidHigh != 0u || row.setUuidLow != 0u) {
        return SetKey(row.setUuidHigh, row.setUuidLow, 0);
    }
    if (row.gameUuidHigh != 0u || row.gameUuidLow != 0u) {
        return SetKey(row.gameUuidHigh, row.gameUuidLow, 1);
    }
    return SetKey(static_cast<uint64_t>(position), 0u, 2);
}

}

BattleStatsSummary summarizeBattleLog(
    const std::vector<BattleLogRowV1>& rows) {
    BattleStatsSummary out;
    out.rows = static_cast<uint32_t>(rows.size());

    std::vector<const BattleLogRowV1*> unique;
    unique.reserve(rows.size());
    std::map<UuidKey, size_t> byGame;
    for (const BattleLogRowV1& row : rows) {
        const UuidKey id(row.gameUuidHigh, row.gameUuidLow);
        if (id.first == 0u && id.second == 0u) {
            unique.push_back(&row);
            continue;
        }
        const auto found = byGame.find(id);
        if (found == byGame.end()) {
            byGame.emplace(id, unique.size());
            unique.push_back(&row);
            continue;
        }
        const BattleLogRowV1*& kept = unique[found->second];
        if (preferredDuplicate(*kept, row) == &row) kept = &row;
    }

    std::map<SetKey, SetAccum> sets;
    std::map<std::string, OpponentAccum> opponents;
    std::map<uint16_t, BattleMatchupStats> matchups;
    std::array<uint32_t, 256> playedGames{};
    std::array<BattleCountV2, 256> playedResults{};
    std::array<uint32_t, 256> facedGames{};

    for (size_t index = 0u; index < unique.size(); ++index) {
        const BattleLogRowV1& row = *unique[index];

        const uint32_t seconds = battleLogSecondsFromFrames(row.frames);
        out.totalSeconds = addSeconds(out.totalSeconds, seconds);

        if (row.startUnix > 0) {
            if (out.firstUnix == 0 || row.startUnix < out.firstUnix) {
                out.firstUnix = row.startUnix;
            }
            if (row.startUnix > out.lastUnix) out.lastUnix = row.startUnix;
        }
        if (row.endUnix > out.lastUnix) out.lastUnix = row.endUnix;

        if (row.mySide == kBattleSideSpectated) {
            ++out.spectatedGames;
        } else if (row.mySide == kBattleSideOffline) {
            ++out.offlineGames;
        }

        SetAccum& set = sets[setKeyFor(row, index)];
        set.uuidHigh = row.setUuidHigh;
        set.uuidLow = row.setUuidLow;
        ++set.games;
        set.seconds = addSeconds(set.seconds, seconds);
        if (row.setStartUnix > 0
            && (set.setStart == 0 || row.setStartUnix < set.setStart)) {
            set.setStart = row.setStartUnix;
        }
        if (row.startUnix > 0
            && (set.rowStart == 0 || row.startUnix < set.rowStart)) {
            set.rowStart = row.startUnix;
        }
        if (set.first == nullptr || earlierInSet(row, *set.first)) {
            set.first = &row;
        }

        if (!isLocalSide(row.mySide)) continue;

        set.hasLocal = true;
        if (set.firstLocal == nullptr || earlierInSet(row, *set.firstLocal)) {
            set.firstLocal = &row;
        }
        {
            const uint8_t mine = myCharacterOf(row);
            const uint8_t theirs = theirCharacterOf(row);
            const uint8_t firstMine = myCharacterOf(*set.firstLocal);
            const uint8_t firstTheirs = theirCharacterOf(*set.firstLocal);
            if (mine != kBattleCharacterUnknown
                && firstMine != kBattleCharacterUnknown
                && mine != firstMine) {
                set.mineSwitched = true;
            }
            if (theirs != kBattleCharacterUnknown
                && firstTheirs != kBattleCharacterUnknown
                && theirs != firstTheirs) {
                set.theirsSwitched = true;
            }
        }

        const Outcome outcome = outcomeFor(row);
        applyOutcome(set.result, outcome);
        applyOutcome(out.games, outcome);
        ++out.countedGames;

        OpponentAccum& opponent = opponents[opponentNameOf(row)];
        applyOutcome(opponent.games, outcome);
        opponent.seconds = addSeconds(opponent.seconds, seconds);

        const uint8_t mine = myCharacterOf(row);
        const uint8_t theirs = theirCharacterOf(row);
        if (mine != kBattleCharacterUnknown) {
            ++playedGames[mine];
            applyOutcome(playedResults[mine], outcome);
        }
        if (theirs != kBattleCharacterUnknown) ++facedGames[theirs];
        if (mine != kBattleCharacterUnknown
            && theirs != kBattleCharacterUnknown) {
            const uint16_t key = static_cast<uint16_t>(
                (static_cast<uint16_t>(mine) << 8) | theirs);
            BattleMatchupStats& matchup = matchups[key];
            matchup.mine = mine;
            matchup.theirs = theirs;
            ++matchup.games;
            applyOutcome(matchup.results, outcome);
        }
    }

    uint32_t countedSets = 0u;
    uint32_t gamesInCountedSets = 0u;
    uint32_t secondsInCountedSets = 0u;
    out.sets_.reserve(sets.size());
    for (const auto& entry : sets) {
        const SetAccum& accumulated = entry.second;
        BattleSetSummary summary;
        summary.setUuidHigh = accumulated.uuidHigh;
        summary.setUuidLow = accumulated.uuidLow;
        summary.startUnix = accumulated.setStart > 0 ? accumulated.setStart
                                                     : accumulated.rowStart;
        if (accumulated.first != nullptr) {
            summary.timezoneOffsetMinutes =
                accumulated.first->timezoneOffsetMinutes;
        }
        summary.games = accumulated.games;
        summary.seconds = accumulated.seconds;
        summary.result = accumulated.result;
        summary.hasLocalSide = accumulated.hasLocal;

        if (accumulated.firstLocal != nullptr) {
            summary.opponent = opponentNameOf(*accumulated.firstLocal);
            summary.myCharacter = myCharacterOf(*accumulated.firstLocal);
            summary.theirCharacter = theirCharacterOf(*accumulated.firstLocal);
            summary.iSwitchedCharacter = accumulated.mineSwitched;
            summary.theySwitchedCharacter = accumulated.theirsSwitched;
        }

        if (accumulated.hasLocal) {
            const Outcome outcome = setOutcomeFrom(accumulated.result);
            applyOutcome(out.sets, outcome);
            const auto found = opponents.find(summary.opponent);
            if (found != opponents.end()) {
                applyOutcome(found->second.sets, outcome);
            }
            ++countedSets;
            gamesInCountedSets += accumulated.games;
            if (accumulated.games > out.longestSetGames) {
                out.longestSetGames = accumulated.games;
            }
            if (accumulated.seconds > out.longestSetSeconds) {
                out.longestSetSeconds = accumulated.seconds;
            }
            secondsInCountedSets =
                addSeconds(secondsInCountedSets, accumulated.seconds);
        }
        out.sets_.push_back(std::move(summary));
    }

    if (countedSets > 0u) {
        out.averageGamesPerSet =
            (gamesInCountedSets + countedSets / 2u) / countedSets;
        out.averageSetSeconds =
            (secondsInCountedSets + countedSets / 2u) / countedSets;
    }
    out.countedSets = countedSets;

    std::sort(out.sets_.begin(), out.sets_.end(),
              [](const BattleSetSummary& a, const BattleSetSummary& b) {
                  if (a.startUnix != b.startUnix) {
                      return a.startUnix > b.startUnix;
                  }
                  if (a.setUuidHigh != b.setUuidHigh) {
                      return a.setUuidHigh > b.setUuidHigh;
                  }
                  return a.setUuidLow > b.setUuidLow;
              });

    out.opponents.reserve(opponents.size());
    for (const auto& entry : opponents) {
        BattleOpponentStats stats;
        stats.name = entry.first;
        stats.games = entry.second.games;
        stats.sets = entry.second.sets;
        stats.seconds = entry.second.seconds;
        out.opponents.push_back(std::move(stats));
    }
    std::sort(out.opponents.begin(), out.opponents.end(),
              [](const BattleOpponentStats& a, const BattleOpponentStats& b) {
                  const uint32_t left = a.games.total();
                  const uint32_t right = b.games.total();
                  if (left != right) return left > right;
                  if (a.seconds != b.seconds) return a.seconds > b.seconds;
                  return a.name < b.name;
              });

    size_t played = 0u;
    for (size_t id = 0u; id < playedGames.size(); ++id) {
        if (playedGames[id] != 0u) ++played;
    }
    out.characters.reserve(played);
    for (size_t id = 0u; id < playedGames.size(); ++id) {
        if (playedGames[id] == 0u) continue;
        BattleCharacterStats stats;
        stats.characterId = static_cast<uint8_t>(id);
        stats.games = playedGames[id];
        stats.results = playedResults[id];
        out.characters.push_back(stats);
    }
    std::sort(out.characters.begin(), out.characters.end(),
              [](const BattleCharacterStats& a,
                 const BattleCharacterStats& b) {
                  if (a.games != b.games) return a.games > b.games;
                  return a.characterId < b.characterId;
              });

    out.matchups.reserve(matchups.size());
    for (const auto& entry : matchups) {
        out.matchups.push_back(entry.second);
    }
    std::sort(out.matchups.begin(), out.matchups.end(),
              [](const BattleMatchupStats& a, const BattleMatchupStats& b) {
                  if (a.games != b.games) return a.games > b.games;
                  if (a.mine != b.mine) return a.mine < b.mine;
                  return a.theirs < b.theirs;
              });

    uint32_t bestPlayed = 0u;
    uint32_t bestFaced = 0u;
    for (size_t id = 0u; id < playedGames.size(); ++id) {
        if (playedGames[id] > bestPlayed) {
            bestPlayed = playedGames[id];
            out.mostUsedCharacter = static_cast<uint8_t>(id);
        }
        if (facedGames[id] > bestFaced) {
            bestFaced = facedGames[id];
            out.mostFacedCharacter = static_cast<uint8_t>(id);
        }
    }
    if (!out.matchups.empty()) {
        out.mostPlayedMatchup = static_cast<uint16_t>(
            (static_cast<uint16_t>(out.matchups.front().mine) << 8)
            | out.matchups.front().theirs);
    }

    for (const BattleSetSummary& set : out.sets_) {
        if (!set.hasLocalSide) continue;
        out.recentOpponent = set.opponent;
        out.recentUnix = set.startUnix;
        break;
    }

    return out;
}

std::vector<BattleLogRowV1> battleSetGames(
    const std::vector<BattleLogRowV1>& rows,
    const uint64_t setUuidHigh, const uint64_t setUuidLow,
    const uint64_t gameUuidHigh, const uint64_t gameUuidLow) {
    std::vector<BattleLogRowV1> out;
    const bool anonymousSet = setUuidHigh == 0u && setUuidLow == 0u;
    for (const BattleLogRowV1& row : rows) {
        if (row.setUuidHigh != setUuidHigh || row.setUuidLow != setUuidLow) {
            continue;
        }
        if (anonymousSet && (gameUuidHigh != 0u || gameUuidLow != 0u)
            && (row.gameUuidHigh != gameUuidHigh
                || row.gameUuidLow != gameUuidLow)) {
            continue;
        }
        out.push_back(row);
    }
    std::sort(out.begin(), out.end(),
              [](const BattleLogRowV1& a, const BattleLogRowV1& b) {
                  if (a.gameNo != b.gameNo) return a.gameNo < b.gameNo;
                  if (a.startUnix != b.startUnix) {
                      return a.startUnix < b.startUnix;
                  }
                  if (a.gameUuidHigh != b.gameUuidHigh) {
                      return a.gameUuidHigh < b.gameUuidHigh;
                  }
                  return a.gameUuidLow < b.gameUuidLow;
              });
    return out;
}

BattleStatsSummary summarizeAgainst(const std::vector<BattleLogRowV1>& rows,
                                    const std::string& opponentName) {
    std::vector<BattleLogRowV1> against;
    against.reserve(rows.size());
    for (const BattleLogRowV1& row : rows) {
        if (!isLocalSide(row.mySide)) continue;
        if (opponentNameOf(row) != opponentName) continue;
        against.push_back(row);
    }
    return summarizeBattleLog(against);
}

BattleGameVerdict battleGameVerdict(const BattleLogRowV1& row) {
    if (!isLocalSide(row.mySide)) return BattleGameVerdict::NoLocalSide;
    switch (outcomeFor(row)) {
        case Outcome::Win:  return BattleGameVerdict::Won;
        case Outcome::Loss: return BattleGameVerdict::Lost;
        default:            return BattleGameVerdict::Draw;
    }
}

const char* battleGameVerdictText(BattleGameVerdict verdict) {
    switch (verdict) {
        case BattleGameVerdict::Won:  return "Won";
        case BattleGameVerdict::Lost: return "Lost";
        case BattleGameVerdict::Draw: return "Draw";
        default:                      return "";
    }
}

const char* battleGameSidesText(const BattleLogRowV1& row) {
    const unsigned mask = row.winningSideMask & 0x3u;
    if (mask == 1u) return "1P won";
    if (mask == 2u) return "2P won";
    return "Draw";
}

std::string battleOpponentOf(const BattleLogRowV1& row) {
    return isLocalSide(row.mySide) ? opponentNameOf(row) : std::string();
}

int32_t battleWinRateTenths(const BattleCountV2& count) {
    const uint64_t decided =
        static_cast<uint64_t>(count.wins) + count.losses;
    if (decided == 0u) return -1;
    return static_cast<int32_t>(
        (static_cast<uint64_t>(count.wins) * 1000u + decided / 2u)
        / decided);
}

std::string battleCountText(const BattleCountV2& count) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%u-%u-%u",
                  static_cast<unsigned>(count.wins),
                  static_cast<unsigned>(count.losses),
                  static_cast<unsigned>(count.draws));
    return std::string(buffer);
}

std::string battleDurationText(uint32_t seconds) {
    char buffer[32];
    if (seconds >= 3600u) {
        std::snprintf(buffer, sizeof(buffer), "%uh %um",
                      static_cast<unsigned>(seconds / 3600u),
                      static_cast<unsigned>((seconds / 60u) % 60u));
    } else if (seconds >= 60u) {
        std::snprintf(buffer, sizeof(buffer), "%um %us",
                      static_cast<unsigned>(seconds / 60u),
                      static_cast<unsigned>(seconds % 60u));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%us",
                      static_cast<unsigned>(seconds));
    }
    return std::string(buffer);
}

}
}
