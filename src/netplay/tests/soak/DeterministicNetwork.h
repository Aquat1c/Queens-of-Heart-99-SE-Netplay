#pragma once

#include "../../WireProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <vector>

namespace qnetplay::soak {

enum class Direction : uint8_t {
    AtoB = 0,
    BtoA = 1,
};

struct ForcedDropBurst {
    wire::MessageType type = wire::MessageType::Inputs;
    uint64_t firstTypeOccurrence = 0;
    uint64_t count = 0;
};

struct LinkFaults {
    uint32_t baseDelayMs = 0;
    uint32_t jitterMs = 0;
    uint32_t lossBasisPoints = 0;
    uint32_t duplicateBasisPoints = 0;
    uint32_t reorderBasisPoints = 0;
    uint32_t reorderExtraMs = 0;

    bool blackoutEnabled = false;
    bool blackoutInputsOnly = true;
    uint64_t blackoutStartUs = 0;
    uint64_t blackoutEndUs = 0;

    std::vector<ForcedDropBurst> forcedDrops;
};

struct ScheduledPacket {
    Direction direction = Direction::AtoB;
    wire::MessageType type = wire::MessageType::Hello;
    uint32_t sequence = 0;
    uint64_t logicalOrdinal = 0;
    uint64_t eventOrdinal = 0;
    uint64_t sendTimeUs = 0;
    uint64_t deliveryTimeUs = 0;
    bool duplicate = false;
    std::vector<uint8_t> bytes;
};

struct PacketDecision {
    Direction direction = Direction::AtoB;
    wire::MessageType type = wire::MessageType::Hello;
    uint32_t sequence = 0;
    uint64_t logicalOrdinal = 0;
    uint64_t typeOccurrence = 0;
    uint64_t sendTimeUs = 0;
    uint64_t deliveryTimeUs = 0;
    bool dropped = false;
    bool duplicated = false;
    bool reordered = false;
    bool forcedDrop = false;
    bool blackoutDrop = false;
};

struct NetworkStats {
    uint64_t logicalPackets = 0;
    uint64_t scheduledPackets = 0;
    uint64_t deliveredPackets = 0;
    uint64_t droppedPackets = 0;
    uint64_t duplicatedPackets = 0;
    uint64_t reorderedPackets = 0;
    uint64_t forcedDropPackets = 0;
    uint64_t blackoutDropPackets = 0;
    size_t maxPendingPackets = 0;
};

class DeterministicNetwork {
public:
    DeterministicNetwork(uint64_t seed,
                         const LinkFaults& aToB,
                         const LinkFaults& bToA);

    void reset(uint64_t seed,
               const LinkFaults& aToB,
               const LinkFaults& bToA);

    void send(Direction direction,
              wire::MessageType type,
              uint32_t sequence,
              const std::vector<uint8_t>& bytes,
              uint64_t nowUs);

    size_t deliverDue(uint64_t nowUs, std::vector<ScheduledPacket>& out);

    bool empty() const { return _events.empty(); }
    size_t pendingPackets() const { return _events.size(); }
    void setDecisionRetention(bool enabled) {
        _retainDecisions = enabled;
        if (!enabled) {
            _decisions.clear();
            _decisions.shrink_to_fit();
        }
    }
    const NetworkStats& stats(Direction direction) const {
        return _links[static_cast<size_t>(direction)].stats;
    }
    uint64_t traceDigest() const { return _traceDigest; }
    const std::vector<PacketDecision>& decisions() const { return _decisions; }

private:
    struct EventCompare {
        bool operator()(const ScheduledPacket& left,
                        const ScheduledPacket& right) const {
            if (left.deliveryTimeUs != right.deliveryTimeUs) {
                return left.deliveryTimeUs > right.deliveryTimeUs;
            }
            return left.eventOrdinal > right.eventOrdinal;
        }
    };

    struct LinkState {
        LinkFaults faults{};
        uint64_t streamSeed = 0;
        uint64_t nextLogicalOrdinal = 0;
        std::array<uint64_t, 6> typeOccurrences{};
        NetworkStats stats{};
    };

    std::array<LinkState, 2> _links{};
    uint64_t _nextEventOrdinal = 0;
    uint64_t _traceDigest = 0;
    bool _retainDecisions = true;
    std::priority_queue<
        ScheduledPacket,
        std::vector<ScheduledPacket>,
        EventCompare> _events;
    std::vector<PacketDecision> _decisions;

    static uint64_t splitMix64(uint64_t value);
    static uint64_t randomLane(uint64_t streamSeed,
                               uint64_t ordinal,
                               uint64_t lane);
    static size_t typeIndex(wire::MessageType type);
    static void digestBytes(uint64_t& digest,
                            const void* bytes,
                            size_t size);
    void digestDecision(const PacketDecision& decision,
                        const std::vector<uint8_t>& bytes);
};

}
