#include "RelayServerCore.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace qnetplay::relay::server {
namespace {

constexpr size_t kSessionOffset = 12;
constexpr size_t kCookieLowOffset = 20;
constexpr size_t kCookieHighOffset = 28;
constexpr size_t kSequenceOffset = 36;

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readLe64(const uint8_t* p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(p[i]) << (i * 8);
    return value;
}

bool validPayload(
    const wire::MessageType type,
    const std::vector<uint8_t>& payload,
    PlayerDatagramInfo& info)
{
    switch (type) {
        case wire::MessageType::Hello:
        case wire::MessageType::HelloAck: {
            wire::HelloPayload decoded{};
            if (!wire::decodePayload(payload, decoded)) return false;
            info.carriesRole = true;
            info.role = decoded.role;
            return true;
        }
        case wire::MessageType::Ping:
        case wire::MessageType::Pong: {
            wire::PingPayload decoded{};
            return wire::decodePayload(payload, decoded);
        }
        case wire::MessageType::Inputs: {
            wire::InputsPayload decoded{};
            return wire::decodePayload(payload, decoded);
        }
        case wire::MessageType::Ready: {
            wire::ReadyPayload decoded{};
            return wire::decodePayload(payload, decoded);
        }
        case wire::MessageType::SyncHash: {
            wire::SyncHashPayload decoded{};
            return wire::decodePayload(payload, decoded);
        }
        case wire::MessageType::SyncHashAck: {
            wire::SyncHashAckPayload decoded{};
            return wire::decodePayload(payload, decoded);
        }
        case wire::MessageType::Disconnect: {
            wire::DisconnectPayload decoded{};
            return wire::decodePayload(payload, decoded);
        }
        case wire::MessageType::DisconnectAck: {
            wire::DisconnectAckPayload decoded{};
            return wire::decodePayload(payload, decoded);
        }
        case wire::MessageType::RouteProbe: {
            wire::RouteProbePayload decoded{};
            return wire::decodePayload(payload, decoded);
        }
        case wire::MessageType::RouteProbeAck: {
            wire::RouteProbeAckPayload decoded{};
            return wire::decodePayload(payload, decoded);
        }
        case wire::MessageType::SessionEvent: {
            wire::SessionEventPayload decoded{};
            return wire::decodePayload(payload, decoded);
        }
        case wire::MessageType::RematchChoice: {
            wire::RematchChoicePayload decoded{};
            return wire::decodePayload(payload, decoded);
        }
    }
    return false;
}

constexpr size_t kSpectatorSessionOffset = 16;
constexpr size_t kSpectatorEpochOffset = 24;
constexpr size_t kSpectatorCookieLowOffset = 32;
constexpr size_t kSpectatorCookieHighOffset = 40;
static_assert(kSpectatorCookieHighOffset + 8 + 8
                  == spectator::kSpectatorHeaderSize,
              "spectator header identity offsets must match the wire layout");

void extractSpectatorIdentityFields(
    const spectator::Datagram& datagram,
    SpectatorDatagramInfo& info)
{
    switch (datagram.header.type) {
        case spectator::MessageType::Hello: {
            spectator::HelloPayload decoded{};
            if (!spectator::decodePayload(datagram.payload, decoded)) return;
            info.viewerId = decoded.viewerId;
            info.carriesViewerId = decoded.viewerId != 0;
            info.generation = decoded.generation;
            info.carriesGeneration = true;
            return;
        }
        case spectator::MessageType::Ack: {
            spectator::AckPayload decoded{};
            if (!spectator::decodePayload(datagram.payload, decoded)) return;
            info.viewerId = decoded.viewerId;
            info.carriesViewerId = decoded.viewerId != 0;
            info.generation = decoded.generation;
            info.carriesGeneration = true;
            return;
        }
        case spectator::MessageType::TerminalAck: {
            spectator::TerminalAckPayload decoded{};
            if (!spectator::decodePayload(datagram.payload, decoded)) return;
            info.viewerId = decoded.viewerId;
            info.carriesViewerId = decoded.viewerId != 0;
            info.generation = decoded.generation;
            info.carriesGeneration = true;
            return;
        }
        case spectator::MessageType::InitAck: {
            spectator::InitAckPayload decoded{};
            if (!spectator::decodePayload(datagram.payload, decoded)) return;
            info.viewerId = decoded.viewerId;
            info.carriesViewerId = decoded.viewerId != 0;
            info.generation = decoded.generation;
            info.carriesGeneration = true;
            return;
        }
        case spectator::MessageType::GameStart: {
            spectator::GameStartPayload decoded{};
            if (!spectator::decodePayload(datagram.payload, decoded)) return;
            info.viewerId = decoded.viewerId;
            info.carriesViewerId = decoded.viewerId != 0;
            info.generation = decoded.generation;
            info.carriesGeneration = true;
            return;
        }
        case spectator::MessageType::Reject: {
            spectator::RejectPayload decoded{};
            if (!spectator::decodePayload(datagram.payload, decoded)) return;
            info.viewerId = decoded.viewerId;
            info.carriesViewerId = decoded.viewerId != 0;
            info.generation = decoded.generation;
            info.carriesGeneration = true;
            return;
        }
        case spectator::MessageType::Detach: {
            spectator::DetachPayload decoded{};
            if (!spectator::decodePayload(datagram.payload, decoded)) return;
            info.viewerId = decoded.viewerId;
            info.carriesViewerId = decoded.viewerId != 0;
            info.generation = decoded.generation;
            info.carriesGeneration = true;
            return;
        }
        case spectator::MessageType::Migrate: {
            spectator::MigratePayload decoded{};
            if (!spectator::decodePayload(datagram.payload, decoded)) return;
            info.viewerId = decoded.viewerId;
            info.carriesViewerId = decoded.viewerId != 0;
            info.generation = decoded.newGeneration;
            info.carriesGeneration = true;
            return;
        }
        default:
            return;
    }
}

}

PlayerDatagramError inspectPlayerDatagram(
    const uint8_t* const bytes,
    const size_t length,
    PlayerDatagramInfo& out)
{
    if (bytes == nullptr || length < wire::kHeaderSize)
        return PlayerDatagramError::TooSmall;
    if (length > wire::kMaxDatagramSize)
        return PlayerDatagramError::TooLarge;

    PlayerDatagramInfo decodedInfo{};
    decodedInfo.sessionId = readLe64(bytes + kSessionOffset);
    decodedInfo.cookie.low = readLe64(bytes + kCookieLowOffset);
    decodedInfo.cookie.high = readLe64(bytes + kCookieHighOffset);
    decodedInfo.sequence = readLe32(bytes + kSequenceOffset);
    if (decodedInfo.sessionId == 0
        || (decodedInfo.cookie.low == 0 && decodedInfo.cookie.high == 0)) {
        return PlayerDatagramError::InvalidIdentity;
    }

    wire::Datagram datagram{};
    if (wire::decodeDatagram(
            bytes,
            length,
            decodedInfo.sessionId,
            decodedInfo.cookie,
            datagram) != wire::DecodeError::None) {
        return PlayerDatagramError::InvalidFraming;
    }
    decodedInfo.type = datagram.header.type;
    if (!validPayload(datagram.header.type, datagram.payload, decodedInfo))
        return PlayerDatagramError::InvalidPayload;

    out = decodedInfo;
    return PlayerDatagramError::None;
}

SpectatorDatagramError inspectSpectatorDatagram(
    const uint8_t* const bytes,
    const size_t length,
    const spectator::PeerRole expectedSender,
    SpectatorDatagramInfo& out)
{
    if (bytes == nullptr || length < spectator::kSpectatorHeaderSize)
        return SpectatorDatagramError::TooSmall;
    if (length > spectator::kMaxSpectatorDatagramSize)
        return SpectatorDatagramError::TooLarge;

    SpectatorDatagramInfo decodedInfo{};
    decodedInfo.identity.sessionId = readLe64(bytes + kSpectatorSessionOffset);
    decodedInfo.identity.epoch = readLe64(bytes + kSpectatorEpochOffset);
    decodedInfo.identity.cookie.low =
        readLe64(bytes + kSpectatorCookieLowOffset);
    decodedInfo.identity.cookie.high =
        readLe64(bytes + kSpectatorCookieHighOffset);
    if (!spectator::isValidIdentity(decodedInfo.identity))
        return SpectatorDatagramError::InvalidIdentity;

    spectator::Datagram datagram{};
    const spectator::DecodeError error = spectator::decodeDatagram(
        bytes, length, decodedInfo.identity, datagram);
    if (error != spectator::DecodeError::None) {
        return error == spectator::DecodeError::BadPayload
            ? SpectatorDatagramError::InvalidPayload
            : SpectatorDatagramError::InvalidFraming;
    }
    if (datagram.header.senderRole != expectedSender)
        return SpectatorDatagramError::WrongOrigin;

    decodedInfo.type = datagram.header.type;
    decodedInfo.direction = datagram.header.direction;
    decodedInfo.senderRole = datagram.header.senderRole;
    decodedInfo.sequence = datagram.header.sequence;
    extractSpectatorIdentityFields(datagram, decodedInfo);
    out = decodedInfo;
    return SpectatorDatagramError::None;
}

TopologyDatagramError inspectTopologyControlDatagram(
    const uint8_t* const bytes,
    const size_t length,
    TopologyDatagramInfo& out)
{
    namespace rwire = spectator::resilient::wire;
    constexpr size_t kPrefix = rwire::kTopologyQtaEnvelopePrefixBytes;
    constexpr size_t kGenerationOffset = 12;
    constexpr size_t kNodeOffset = 20;
    constexpr size_t kEnvelopeSequenceOffset = 28;
    constexpr size_t kMacOffset = 36;
    static_assert(kPrefix
                      == kMacOffset + rwire::kTopologyQtaEnvelopeMacBytes,
                  "QTA envelope prefix layout drifted");

    if (bytes == nullptr
        || length < kPrefix + rwire::kResilientControlHeaderBytes)
        return TopologyDatagramError::TooSmall;
    if (length > kPrefix + rwire::kMaxResilientControlDatagramBytes)
        return TopologyDatagramError::TooLarge;
    if (readLe32(bytes) != rwire::kTopologyQtaEnvelopeMagic
        || (static_cast<uint16_t>(bytes[4])
            | static_cast<uint16_t>(bytes[5]) << 8)
            != rwire::kTopologyQtaEnvelopeVersion
        || bytes[6] != rwire::kTopologyQtaEnvelopeControlType
        || bytes[7] != 0u || bytes[10] != 0u || bytes[11] != 0u) {
        return TopologyDatagramError::InvalidFraming;
    }
    const size_t innerLength = static_cast<size_t>(bytes[8])
        | static_cast<size_t>(bytes[9]) << 8;
    if (innerLength < rwire::kResilientControlHeaderBytes
        || innerLength > rwire::kMaxResilientControlDatagramBytes
        || kPrefix + innerLength != length) {
        return TopologyDatagramError::InvalidFraming;
    }

    const uint64_t authorityGeneration = readLe64(bytes + kGenerationOffset);
    const uint64_t nodeId = readLe64(bytes + kNodeOffset);
    const uint64_t sequence = readLe64(bytes + kEnvelopeSequenceOffset);
    uint8_t macBits = 0;
    for (size_t i = 0; i < rwire::kTopologyQtaEnvelopeMacBytes; ++i)
        macBits = static_cast<uint8_t>(macBits | bytes[kMacOffset + i]);
    if (authorityGeneration == 0 || nodeId == 0 || sequence == 0
        || macBits == 0) {
        return TopologyDatagramError::InvalidIdentity;
    }

    rwire::ControlDatagram inner{};
    if (rwire::decodeControlDatagram(bytes + kPrefix, innerLength, inner)
        != rwire::DecodeResult::Ok) {
        return TopologyDatagramError::InvalidPayload;
    }

    out.authorityGeneration = authorityGeneration;
    out.nodeId = nodeId;
    out.sequence = sequence;
    out.innerType = inner.type;
    out.innerLength = static_cast<uint16_t>(innerLength);
    return TopologyDatagramError::None;
}

bool constantTimeEqual(
    const void* const left,
    const void* const right,
    const size_t length)
{
    if ((left == nullptr || right == nullptr) && length != 0)
        return false;
    const auto* const a = static_cast<const uint8_t*>(left);
    const auto* const b = static_cast<const uint8_t*>(right);
    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i)
        difference = static_cast<uint8_t>(difference | (a[i] ^ b[i]));
    return difference == 0;
}

void TokenBucket::reset(
    const uint64_t ratePerSecond,
    const uint64_t capacity,
    const uint64_t nowMs)
{
    ratePerSecond_ = ratePerSecond;
    capacity_ = capacity;
    tokens_ = capacity;
    refillRemainder_ = 0;
    lastRefillMs_ = nowMs;
}

void TokenBucket::refill(const uint64_t nowMs) {
    if (nowMs <= lastRefillMs_) return;
    if (ratePerSecond_ == 0 || capacity_ == 0) {
        lastRefillMs_ = nowMs;
        return;
    }

    const uint64_t elapsedMs = nowMs - lastRefillMs_;
    lastRefillMs_ = nowMs;
    if (tokens_ >= capacity_) {
        tokens_ = capacity_;
        refillRemainder_ = 0;
        return;
    }

    const uint64_t missing = capacity_ - tokens_;
    const uint64_t fillMs = ratePerSecond_ > 0
        ? (missing * 1000u + ratePerSecond_ - 1u) / ratePerSecond_
        : (std::numeric_limits<uint64_t>::max)();
    if (elapsedMs >= fillMs) {
        tokens_ = capacity_;
        refillRemainder_ = 0;
        return;
    }

    const uint64_t numerator = elapsedMs * ratePerSecond_ + refillRemainder_;
    const uint64_t added = numerator / 1000u;
    refillRemainder_ = numerator % 1000u;
    tokens_ = (std::min)(capacity_, tokens_ + added);
}

bool TokenBucket::consume(const uint64_t amount, const uint64_t nowMs) {
    refill(nowMs);
    if (amount > tokens_) return false;
    tokens_ -= amount;
    return true;
}

uint64_t TokenBucket::available(const uint64_t nowMs) {
    refill(nowMs);
    return tokens_;
}

}
