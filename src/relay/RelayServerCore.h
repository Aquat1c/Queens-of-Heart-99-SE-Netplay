#pragma once

#include "../netplay/WireProtocol.h"
#include "../spectator/ResilientSpectatorProtocol.h"
#include "../spectator/SpectatorProtocol.h"

#include <cstddef>
#include <cstdint>

namespace qnetplay::relay::server {

enum class PlayerDatagramError {
    None,
    TooSmall,
    TooLarge,
    InvalidIdentity,
    InvalidFraming,
    InvalidPayload,
};

struct PlayerDatagramInfo {
    uint64_t sessionId = 0;
    wire::SessionCookie cookie{};
    wire::MessageType type = wire::MessageType::Hello;
    uint32_t sequence = 0;
    bool carriesRole = false;
    wire::PeerRole role = wire::PeerRole::Host;
};

PlayerDatagramError inspectPlayerDatagram(
    const uint8_t* bytes,
    size_t length,
    PlayerDatagramInfo& out);

enum class SpectatorDatagramError {
    None,
    TooSmall,
    TooLarge,
    InvalidIdentity,
    InvalidFraming,
    InvalidPayload,
    WrongOrigin,
};

struct SpectatorDatagramInfo {
    spectator::SessionIdentity identity{};
    spectator::MessageType type = spectator::MessageType::Hello;
    spectator::Direction direction = spectator::Direction::ViewerToSource;
    spectator::PeerRole senderRole = spectator::PeerRole::Viewer;
    uint32_t sequence = 0;
    uint64_t viewerId = 0;
    bool carriesViewerId = false;
    uint32_t generation = 0;
    bool carriesGeneration = false;
};

SpectatorDatagramError inspectSpectatorDatagram(
    const uint8_t* bytes,
    size_t length,
    spectator::PeerRole expectedSender,
    SpectatorDatagramInfo& out);

enum class TopologyDatagramError {
    None,
    TooSmall,
    TooLarge,
    InvalidIdentity,
    InvalidFraming,
    InvalidPayload,
};

struct TopologyDatagramInfo {
    uint64_t authorityGeneration = 0;
    uint64_t nodeId = 0;
    uint64_t sequence = 0;
    spectator::resilient::wire::ControlMessageType innerType =
        spectator::resilient::wire::ControlMessageType::NodeAdvertisement;
    uint16_t innerLength = 0;
};

TopologyDatagramError inspectTopologyControlDatagram(
    const uint8_t* bytes,
    size_t length,
    TopologyDatagramInfo& out);

bool constantTimeEqual(const void* left, const void* right, size_t length);

class TokenBucket {
public:
    TokenBucket() = default;
    TokenBucket(uint64_t ratePerSecond, uint64_t capacity, uint64_t nowMs) {
        reset(ratePerSecond, capacity, nowMs);
    }

    void reset(uint64_t ratePerSecond, uint64_t capacity, uint64_t nowMs);
    bool consume(uint64_t amount, uint64_t nowMs);

    uint64_t available(uint64_t nowMs);
    uint64_t capacity() const { return capacity_; }

private:
    void refill(uint64_t nowMs);

    uint64_t ratePerSecond_ = 0;
    uint64_t capacity_ = 0;
    uint64_t tokens_ = 0;
    uint64_t refillRemainder_ = 0;
    uint64_t lastRefillMs_ = 0;
};

}
