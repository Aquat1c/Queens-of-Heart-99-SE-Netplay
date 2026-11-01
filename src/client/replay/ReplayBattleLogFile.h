#pragma once

#include <string>

#include "ReplayBattleLog.h"

namespace qclient {
namespace replay {

std::wstring battleLogPathFor(const std::wstring& replayRoot);

bool appendBattleLogRow(const std::wstring& replayRoot,
                        const BattleLogRowV1& row);

bool readBattleLogFile(const std::wstring& replayRoot, std::string& text);

}
}
