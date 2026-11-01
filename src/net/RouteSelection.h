#pragma once

#include <cstddef>
#include <cstdint>

namespace qnet {

enum class RouteLabel : uint8_t {
    None,
    LanDirect,
    PublicDirect,
    PublicDirectPossibleHairpin,
    Relay,
};

struct RouteCandidate {
    RouteLabel label = RouteLabel::None;
    bool validated = false;
    bool sameObservedPublicAddr = false;
    int p95Micros = 0;
    int lossPerMille = 0;
};

constexpr RouteLabel resolveLabel(const RouteCandidate& c) {
    return (c.label == RouteLabel::PublicDirect && c.sameObservedPublicAddr)
        ? RouteLabel::PublicDirectPossibleHairpin
        : c.label;
}

constexpr bool isDirect(RouteLabel l) {
    return l == RouteLabel::LanDirect
        || l == RouteLabel::PublicDirect
        || l == RouteLabel::PublicDirectPossibleHairpin;
}

constexpr bool isLan(RouteLabel l) { return l == RouteLabel::LanDirect; }

struct RouteSelection {
    int index = -1;
    RouteLabel label = RouteLabel::None;
};

struct RouteSelectionConfig {
    int tieEpsilonMicros = 3000;
};

inline RouteSelection selectRoute(const RouteCandidate* candidates, size_t count,
                                  bool directDeadlineExpired,
                                  const RouteSelectionConfig& cfg) {
    int bestDirect = -1;
    int relayIdx = -1;
    for (size_t i = 0; i < count; ++i) {
        const RouteLabel l = resolveLabel(candidates[i]);
        if (l == RouteLabel::Relay) {
            if (candidates[i].validated) relayIdx = static_cast<int>(i);
            continue;
        }
        if (!isDirect(l) || !candidates[i].validated) continue;
        if (bestDirect < 0) {
            bestDirect = static_cast<int>(i);
            continue;
        }
        const RouteCandidate& best = candidates[bestDirect];
        const RouteCandidate& cur = candidates[i];
        const int delta = cur.p95Micros - best.p95Micros;
        if (delta < -cfg.tieEpsilonMicros) {
            bestDirect = static_cast<int>(i);
        } else if (delta <= cfg.tieEpsilonMicros) {
            const bool curLan = isLan(resolveLabel(cur));
            const bool bestLan = isLan(resolveLabel(best));
            if (curLan && !bestLan) {
                bestDirect = static_cast<int>(i);
            } else if (curLan == bestLan && cur.lossPerMille < best.lossPerMille) {
                bestDirect = static_cast<int>(i);
            }
        }
    }
    if (bestDirect >= 0)
        return RouteSelection{bestDirect, resolveLabel(candidates[bestDirect])};
    if (directDeadlineExpired && relayIdx >= 0)
        return RouteSelection{relayIdx, RouteLabel::Relay};
    return RouteSelection{-1, RouteLabel::None};
}

constexpr bool routeCommitMatches(uint32_t localSessionId, uint8_t localKind,
                                  uint32_t peerSessionId, uint8_t peerKind) {
    return localSessionId == peerSessionId
        && localKind == peerKind
        && (localKind == 1u || localKind == 2u);
}

}
