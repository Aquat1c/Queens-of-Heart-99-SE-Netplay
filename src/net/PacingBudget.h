#pragma once

#include <cstdint>

namespace qnet {

constexpr int kNetplayFpsNumerator = 6060;
constexpr int kNetplayFpsDenominator = 100;

constexpr int kCapabilityDelay = 15;
constexpr int kCapabilityRollback = 30;

constexpr int kDefaultSafetyMarginFrames = 2;

constexpr int safetyMarginFramesForOneWay(const int oneWayFrames) {
    const int scaled = oneWayFrames / 3;
    return scaled > kDefaultSafetyMarginFrames ? scaled
                                               : kDefaultSafetyMarginFrames;
}

struct RouteRttMs {
    uint32_t value = 0;
};

constexpr int oneWayFrames(RouteRttMs routeRtt) {
    const uint64_t num = static_cast<uint64_t>(routeRtt.value) * kNetplayFpsNumerator;
    const uint64_t den = 2000ull * static_cast<uint64_t>(kNetplayFpsDenominator);
    return static_cast<int>((num + den - 1u) / den);
}

constexpr int requiredCoverage(RouteRttMs routeRtt, int safetyMarginFrames) {
    return oneWayFrames(routeRtt) + safetyMarginFrames;
}

constexpr int requiredCoverageForRoute(RouteRttMs routeRtt) {
    const int oneWay = oneWayFrames(routeRtt);
    return oneWay + safetyMarginFramesForOneWay(oneWay);
}

struct DirectionalRequirement {
    int hostFrames = 0;
    int clientFrames = 0;
};

constexpr DirectionalRequirement symmetricRequirement(RouteRttMs routeRtt,
                                                      int safetyMarginFrames) {
    const int r = requiredCoverage(routeRtt, safetyMarginFrames);
    return DirectionalRequirement{r, r};
}

constexpr DirectionalRequirement symmetricRequirementForRoute(
    RouteRttMs routeRtt) {
    const int r = requiredCoverageForRoute(routeRtt);
    return DirectionalRequirement{r, r};
}

struct DirectionalBudget {
    int hostReceive = 0;
    int clientReceive = 0;
};

constexpr DirectionalBudget directionalBudget(int hostDelay, int hostMaxRollback,
                                              int clientDelay, int clientMaxRollback) {
    return DirectionalBudget{clientDelay + hostMaxRollback,
                             hostDelay + clientMaxRollback};
}

enum class Classification {
    FullSpeed,
    Marginal,
    Underbuffered,
};

constexpr Classification classify(int budgetFrames, int requiredFrames) {
    return budgetFrames > requiredFrames ? Classification::FullSpeed
         : budgetFrames == requiredFrames ? Classification::Marginal
                                          : Classification::Underbuffered;
}

struct PeerAuthorization {
    int maxRollback = 0;
    int maxDelay = 0;
};

enum class Preset {
    LowLatency,
    Smoother,
};

struct DirectionSolution {
    int senderDelay = 0;
    int receiverRollback = 0;
    constexpr int coverage() const { return senderDelay + receiverRollback; }
};

constexpr int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr int enforcedOwnRollback(int requiredOwnReceiveFrames,
                                  int peerCommittedDelay,
                                  int ownRollback,
                                  int ownAuthRollback) {
    const int fullSpeedTarget =
        (requiredOwnReceiveFrames < 0 ? 0 : requiredOwnReceiveFrames) + 1;
    const int neededRollback =
        fullSpeedTarget - (peerCommittedDelay < 0 ? 0 : peerCommittedDelay);
    const int floorRollback =
        clampInt(neededRollback, 0, ownAuthRollback < 0 ? 0 : ownAuthRollback);
    return ownRollback > floorRollback ? ownRollback : floorRollback;
}

constexpr int enforcedOwnDelay(int requiredPeerReceiveFrames,
                               int peerAuthRollback,
                               int ownDelay,
                               int ownAuthDelay) {
    const int fullSpeedTarget =
        (requiredPeerReceiveFrames < 0 ? 0 : requiredPeerReceiveFrames) + 1;
    const int neededDelay =
        fullSpeedTarget - (peerAuthRollback < 0 ? 0 : peerAuthRollback);
    const int floorDelay =
        clampInt(neededDelay, 0, ownAuthDelay < 0 ? 0 : ownAuthDelay);
    return ownDelay > floorDelay ? ownDelay : floorDelay;
}

constexpr DirectionSolution solveDirection(int required, int rollbackTarget,
                                           int authReceiverRollback, int authSenderDelay) {
    const int req = required < 0 ? 0 : required;
    const int rb = clampInt(rollbackTarget < 0 ? 0 : rollbackTarget, 0,
                            clampInt(authReceiverRollback, 0, req));
    const int d = clampInt(req - rb, 0, authSenderDelay < 0 ? 0 : authSenderDelay);
    return DirectionSolution{d, rb};
}

struct Allocation {
    int hostDelay = 0;
    int hostMaxRollback = 0;
    int clientDelay = 0;
    int clientMaxRollback = 0;
};

constexpr Allocation recommend(DirectionalRequirement req,
                               PeerAuthorization hostAuth,
                               PeerAuthorization clientAuth,
                               Preset preset) {
    const int hostReq = req.hostFrames + 1;
    const int clientReq = req.clientFrames + 1;
    const int hostTarget =
        preset == Preset::LowLatency ? hostReq : hostReq / 2;
    const int clientTarget =
        preset == Preset::LowLatency ? clientReq : clientReq / 2;
    const DirectionSolution host = solveDirection(
        hostReq, hostTarget, hostAuth.maxRollback, clientAuth.maxDelay);
    const DirectionSolution client = solveDirection(
        clientReq, clientTarget, clientAuth.maxRollback, hostAuth.maxDelay);
    return Allocation{client.senderDelay,
                      host.receiverRollback,
                      host.senderDelay,
                      client.receiverRollback};
}

}
