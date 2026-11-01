#include "DeterministicNetwork.h"

#include <algorithm>
#include <limits>

namespace qnetplay::soak {

namespace {

constexpr uint64_t kTraceSeed = 0xCBF29CE484222325ull;
constexpr uint64_t kTracePrime = 0x100000001B3ull;
constexpr uint64_t kDirectionSaltA = 0xA0761D6478BD642Full;
constexpr uint64_t kDirectionSaltB = 0xE7037ED1A0B428DBull;
constexpr uint64_t kOrdinalSalt = 0x9E3779B97F4A7C15ull;
constexpr uint64_t kLaneSalt = 0xD1B54A32D192ED03ull;

bool inForcedBurst(const LinkFaults& faults,
                   const wire::MessageType type,
                   const uint64_t occurrence) {
    for (const ForcedDropBurst& burst : faults.forcedDrops) {
        if (burst.type != type || burst.count == 0u) {
            continue;
        }
        const uint64_t end = burst.firstTypeOccurrence + burst.count;
        if (end >= burst.firstTypeOccurrence
            && occurrence >= burst.firstTypeOccurrence
            && occurrence < end) {
            return true;
        }
    }
    return false;
}

}

DeterministicNetwork::DeterministicNetwork(const uint64_t seed,
                                           const LinkFaults& aToB,
                                           const LinkFaults& bToA) {
    reset(seed, aToB, bToA);
}

void DeterministicNetwork::reset(const uint64_t seed,
                                 const LinkFaults& aToB,
                                 const LinkFaults& bToA) {
    _links = {};
    _links[0].faults = aToB;
    _links[1].faults = bToA;
    _links[0].streamSeed = splitMix64(seed ^ kDirectionSaltA);
    _links[1].streamSeed = splitMix64(seed ^ kDirectionSaltB);
    _nextEventOrdinal = 0u;
    _traceDigest = kTraceSeed;
    _events = {};
    _decisions.clear();
}

uint64_t DeterministicNetwork::splitMix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31u);
}

uint64_t DeterministicNetwork::randomLane(const uint64_t streamSeed,
                                          const uint64_t ordinal,
                                          const uint64_t lane) {
    return splitMix64(streamSeed
        ^ (ordinal * kOrdinalSalt)
        ^ (lane * kLaneSalt));
}

size_t DeterministicNetwork::typeIndex(const wire::MessageType type) {
    const size_t index = static_cast<size_t>(type);
    return index < 6u ? index : 0u;
}

void DeterministicNetwork::digestBytes(uint64_t& digest,
                                       const void* const bytes,
                                       const size_t size) {
    const auto* const data = static_cast<const uint8_t*>(bytes);
    for (size_t index = 0u; index < size; ++index) {
        digest ^= data[index];
        digest *= kTracePrime;
    }
}

void DeterministicNetwork::digestDecision(
    const PacketDecision& decision,
    const std::vector<uint8_t>& bytes) {
    const uint8_t direction = static_cast<uint8_t>(decision.direction);
    const uint8_t type = static_cast<uint8_t>(decision.type);
    const uint8_t flags = static_cast<uint8_t>(
        (decision.dropped ? 1u : 0u)
        | (decision.duplicated ? 2u : 0u)
        | (decision.reordered ? 4u : 0u)
        | (decision.forcedDrop ? 8u : 0u)
        | (decision.blackoutDrop ? 16u : 0u));
    digestBytes(_traceDigest, &direction, sizeof(direction));
    digestBytes(_traceDigest, &type, sizeof(type));
    digestBytes(_traceDigest, &flags, sizeof(flags));
    digestBytes(_traceDigest, &decision.sequence, sizeof(decision.sequence));
    digestBytes(_traceDigest,
                &decision.logicalOrdinal,
                sizeof(decision.logicalOrdinal));
    digestBytes(_traceDigest,
                &decision.typeOccurrence,
                sizeof(decision.typeOccurrence));
    digestBytes(_traceDigest, &decision.sendTimeUs,
                sizeof(decision.sendTimeUs));
    digestBytes(_traceDigest, &decision.deliveryTimeUs,
                sizeof(decision.deliveryTimeUs));
    if (!bytes.empty()) {
        digestBytes(_traceDigest, bytes.data(), bytes.size());
    }
}

void DeterministicNetwork::send(const Direction direction,
                                const wire::MessageType type,
                                const uint32_t sequence,
                                const std::vector<uint8_t>& bytes,
                                const uint64_t nowUs) {
    LinkState& link = _links[static_cast<size_t>(direction)];
    const uint64_t ordinal = link.nextLogicalOrdinal++;
    const size_t typeSlot = typeIndex(type);
    const uint64_t typeOccurrence = link.typeOccurrences[typeSlot]++;
    ++link.stats.logicalPackets;

    const bool forcedDrop =
        inForcedBurst(link.faults, type, typeOccurrence);
    const bool blackoutApplies = link.faults.blackoutEnabled
        && nowUs >= link.faults.blackoutStartUs
        && nowUs < link.faults.blackoutEndUs
        && (!link.faults.blackoutInputsOnly
            || type == wire::MessageType::Inputs);
    const bool randomDrop = link.faults.lossBasisPoints != 0u
        && randomLane(link.streamSeed, ordinal, 0u) % 10000u
            < std::min<uint32_t>(link.faults.lossBasisPoints, 10000u);
    const bool dropped = forcedDrop || blackoutApplies || randomDrop;

    const uint64_t minimumDelayMs =
        link.faults.baseDelayMs > link.faults.jitterMs
        ? link.faults.baseDelayMs - link.faults.jitterMs
        : 0u;
    const uint64_t maximumDelayMs =
        static_cast<uint64_t>(link.faults.baseDelayMs)
        + link.faults.jitterMs;
    const uint64_t delayRange = maximumDelayMs - minimumDelayMs;
    uint64_t delayMs = minimumDelayMs;
    if (delayRange != 0u) {
        delayMs += randomLane(link.streamSeed, ordinal, 1u)
            % (delayRange + 1u);
    }

    const bool reordered = !dropped
        && link.faults.reorderBasisPoints != 0u
        && randomLane(link.streamSeed, ordinal, 2u) % 10000u
            < std::min<uint32_t>(
                link.faults.reorderBasisPoints, 10000u);
    if (reordered) {
        delayMs += link.faults.reorderExtraMs;
        ++link.stats.reorderedPackets;
    }

    const bool duplicated = !dropped
        && link.faults.duplicateBasisPoints != 0u
        && randomLane(link.streamSeed, ordinal, 3u) % 10000u
            < std::min<uint32_t>(
                link.faults.duplicateBasisPoints, 10000u);

    PacketDecision decision{};
    decision.direction = direction;
    decision.type = type;
    decision.sequence = sequence;
    decision.logicalOrdinal = ordinal;
    decision.typeOccurrence = typeOccurrence;
    decision.sendTimeUs = nowUs;
    decision.deliveryTimeUs = nowUs + delayMs * 1000u;
    decision.dropped = dropped;
    decision.duplicated = duplicated;
    decision.reordered = reordered;
    decision.forcedDrop = forcedDrop;
    decision.blackoutDrop = blackoutApplies;
    digestDecision(decision, bytes);
    if (_retainDecisions) {
        _decisions.push_back(decision);
    }

    if (dropped) {
        ++link.stats.droppedPackets;
        if (forcedDrop) {
            ++link.stats.forcedDropPackets;
        }
        if (blackoutApplies) {
            ++link.stats.blackoutDropPackets;
        }
        return;
    }

    ScheduledPacket packet{};
    packet.direction = direction;
    packet.type = type;
    packet.sequence = sequence;
    packet.logicalOrdinal = ordinal;
    packet.eventOrdinal = _nextEventOrdinal++;
    packet.sendTimeUs = nowUs;
    packet.deliveryTimeUs = decision.deliveryTimeUs;
    packet.bytes = bytes;
    _events.push(packet);
    ++link.stats.scheduledPackets;

    if (duplicated) {
        ScheduledPacket copy = packet;
        copy.duplicate = true;
        copy.eventOrdinal = _nextEventOrdinal++;
        copy.deliveryTimeUs +=
            1u + randomLane(link.streamSeed, ordinal, 4u) % 5000u;
        _events.push(std::move(copy));
        ++link.stats.scheduledPackets;
        ++link.stats.duplicatedPackets;
    }
    link.stats.maxPendingPackets =
        std::max(link.stats.maxPendingPackets, _events.size());
}

size_t DeterministicNetwork::deliverDue(
    const uint64_t nowUs,
    std::vector<ScheduledPacket>& out) {
    const size_t before = out.size();
    while (!_events.empty()
           && _events.top().deliveryTimeUs <= nowUs) {
        ScheduledPacket packet = _events.top();
        _events.pop();
        ++_links[static_cast<size_t>(packet.direction)]
            .stats.deliveredPackets;
        out.push_back(std::move(packet));
    }
    return out.size() - before;
}

}
