#pragma once

#include "DeterministicNetwork.h"
#include "../../RollbackSession.h"

#include <cstdint>
#include <string>

namespace qnetplay::soak {

struct ProductionStackProfile {
    std::string name;
    LinkFaults aToB{};
    LinkFaults bToA{};
    uint8_t delayA = 0;
    uint8_t delayB = 0;
    uint8_t rollbackA = 0;
    uint8_t rollbackB = 0;
};

struct ProductionStackFingerprint {
    uint64_t playerNetworkDigest = 0;
    uint64_t playerExecutionDigest = 0;
    uint64_t remoteInputDigestA = 0;
    uint64_t remoteInputDigestB = 0;
    uint64_t remoteHashDigestA = 0;
    uint64_t remoteHashDigestB = 0;
    uint64_t archiveDigest = 0;
    uint64_t finalStateHash = 0;
    uint64_t layoutBarrierDoneTick = 0;
    uint64_t firstGameplayTick = 0;
    uint64_t playerDoneTick = 0;
    uint32_t peerAckA = 0;
    uint32_t peerAckB = 0;
    uint32_t remoteAckA = 0;
    uint32_t remoteAckB = 0;
    uint32_t rttA = 0;
    uint32_t rttB = 0;
    RollbackStats rollbackA{};
    RollbackStats rollbackB{};
    NetworkStats aToB{};
    NetworkStats bToA{};
};

struct ProductionStackRun {
    bool ok = false;
    std::string error;
    ProductionStackFingerprint fingerprint{};
    uint64_t spectatorPackets = 0;
    uint64_t spectatorDropped = 0;
    uint64_t spectatorDuplicated = 0;
    uint64_t spectatorReordered = 0;
    uint64_t spectatorResumes = 0;
    uint64_t completedViewers = 0;
    bool capacityRejectObserved = false;
    uint64_t maximumLiveBacklog = 0;
    uint64_t backlogAtSeal = 0;
    bool convergedToLiveTarget = false;
    uint64_t convergenceSlots = 0;
};

ProductionStackRun runProductionStack(
    const ProductionStackProfile& profile,
    uint64_t scenarioSeed,
    uint32_t targetFrames,
    bool enableSpectators);

bool productionFingerprintsEqual(
    const ProductionStackFingerprint& left,
    const ProductionStackFingerprint& right,
    std::string& differingField);

}
