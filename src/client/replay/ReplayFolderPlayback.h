#pragma once

#include "ReplayCatalog.h"
#include "replay/ReplayIpcWire.h"
#include "replay/ReplayWinScreen.h"

#include <algorithm>
#include <map>

namespace qclient::replay {

inline std::vector<size_t> folderPlaybackOrder(
    const ReplaySnapshot& snapshot, const std::wstring& directory) {
    std::vector<size_t> order;
    if (!snapshot.complete) return order;
    for (size_t i = 0u; i < snapshot.entries.size(); ++i) {
        const ReplayEntry& entry = snapshot.entries[i];
        if (entry.relativeDir == directory && !entry.inTrash && !entry.isPart)
            order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const ReplayEntry& left = snapshot.entries[a];
        const ReplayEntry& right = snapshot.entries[b];
        if (left.recordedUnixSeconds() != right.recordedUnixSeconds())
            return left.recordedUnixSeconds() < right.recordedUnixSeconds();
        return left.path < right.path;
    });
    std::map<std::pair<uint64_t, uint64_t>, std::vector<size_t>> seriesPositions;
    for (size_t i = 0u; i < order.size(); ++i) {
        const ReplayEntry& entry = snapshot.entries[order[i]];
        const auto& display = entry.startV2.manifest.display;
        if (entry.isV2 && (display.flags & qreplay::DisplayMetadataSeriesV2)
            && (display.seriesUuidLow != 0u || display.seriesUuidHigh != 0u)) {
            seriesPositions[{display.seriesUuidLow, display.seriesUuidHigh}]
                .push_back(i);
        }
    }
    for (const auto& series : seriesPositions) {
        std::vector<size_t> games;
        for (size_t position : series.second) games.push_back(order[position]);
        std::stable_sort(games.begin(), games.end(), [&](size_t a, size_t b) {
            return snapshot.entries[a].startV2.manifest.display.gameOrdinalInSeries
                < snapshot.entries[b].startV2.manifest.display.gameOrdinalInSeries;
        });
        for (size_t i = 0u; i < games.size(); ++i)
            order[series.second[i]] = games[i];
    }
    return order;
}

inline bool folderPlaybackCompleted(
    int processExit, bool received,
    const qreplay::ipc::v3::PlaybackResultPayload& result) {
    return processExit == 0 && received && qreplay::replayCompletedGame(result);
}

}
