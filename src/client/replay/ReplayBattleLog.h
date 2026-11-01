#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qclient {
namespace replay {

constexpr int8_t kBattleSideOne = 0;
constexpr int8_t kBattleSideTwo = 1;
constexpr int8_t kBattleSideSpectated = -1;
constexpr int8_t kBattleSideOffline = -2;

constexpr int16_t kBattleRoundsUnknown = -1;

struct BattleLogRowV1 {
    uint64_t gameUuidHigh = 0u;
    uint64_t gameUuidLow = 0u;
    uint64_t setUuidHigh = 0u;
    uint64_t setUuidLow = 0u;
    uint32_t gameNo = 0u;

    int64_t startUnix = 0;
    int64_t endUnix = 0;
    int64_t setStartUnix = 0;
    int32_t timezoneOffsetMinutes = 0;

    uint8_t sourceKind = 0u;
    int8_t mySide = kBattleSideSpectated;

    uint8_t characterOne = 0u;
    uint8_t characterTwo = 0u;
    uint8_t paletteOne = 0u;
    uint8_t paletteTwo = 0u;
    uint64_t characterCatalogId = 0u;

    uint8_t winningSideMask = 0u;   // bit0 side 0, bit1 side 1, 0 = neither
    int16_t roundsOne = kBattleRoundsUnknown;
    int16_t roundsTwo = kBattleRoundsUnknown;
    uint16_t terminalReason = 0u;

    uint32_t frames = 0u;
    uint32_t roundsToWin = 0u;
    uint32_t stageId = 0u;
    uint64_t bytes = 0u;
    uint64_t digest = 0u;

    std::string nameOne;
    std::string nameTwo;
    std::string myName;
    std::string relativePath;
};

const char* battleLogHeaderLineV1();

std::string encodeBattleLogRowV1(const BattleLogRowV1& row);

bool decodeBattleLogRowV1(const std::string& line, BattleLogRowV1& out);

std::vector<BattleLogRowV1> decodeBattleLogV1(const std::string& text);

std::string sanitizeBattleLogText(const std::string& text,
                                  std::size_t maxBytes = 64u);

uint32_t battleLogSecondsFromFrames(uint32_t frames);

}
}
