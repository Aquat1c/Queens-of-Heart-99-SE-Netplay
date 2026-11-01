#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "net/Protocol.h"
#include "net/RelayProtocol.h"
#include "spectator/TopologyGrant.h"

namespace qnetplay::spectator {

constexpr uint64_t kTopologyRelayRegistrationRetryMs = 500;
constexpr uint32_t kTopologyRelayFastRegistrationAttempts = 8;
constexpr uint64_t kTopologyRelaySlowRegistrationRetryMs = 2'000;
constexpr uint64_t kTopologyRelayAuthorityRefreshMs = 20'000;

struct TopologyRelayBinding {
    bool enabled = false;
    qnet::relay::RoomId room{};
    qnet::relay::RegistrationToken token{};
    uint64_t nodeId = 0;
    uint32_t authorityGeneration = 0;
};

bool validTopologyRelayBinding(const TopologyRelayBinding& binding);

enum class TopologyRelayIngress : uint8_t {
    PassThrough = 1,
    Data = 2,
    Ready = 3,
    Detached = 4,
    Rejected = 5,
};

class TopologyRelayNodeAdapter {
public:
    TopologyRelayNodeAdapter() = default;
    explicit TopologyRelayNodeAdapter(const TopologyRelayBinding& binding) {
        configure(binding);
    }

    bool configure(const TopologyRelayBinding& binding);
    void reset();

    bool enabled() const { return _binding.enabled; }
    bool ready() const { return _ready; }
    bool detached() const { return _detached; }
    uint32_t registrationAttempts() const { return _registrationAttempts; }
    const TopologyRelayBinding& binding() const { return _binding; }

    bool encodeOutbound(const uint8_t* bytes,
                        size_t length,
                        std::vector<uint8_t>& out) const;

    TopologyRelayIngress decodeInbound(bool fromRelay,
                                       const uint8_t* bytes,
                                       size_t length,
                                       std::vector<uint8_t>& out);

    bool registrationDue(uint64_t nowMs) const;
    bool encodeRegistration(uint64_t nowMs,
                            std::vector<uint8_t>& out,
                            bool force = false);
    bool encodeDetach(qnet::relay::TopologyDetachReason reason,
                      std::vector<uint8_t>& out) const;

private:
    TopologyRelayBinding _binding{};
    bool _ready = false;
    bool _detached = false;
    uint32_t _registrationAttempts = 0;
    uint64_t _lastRegistrationMs = 0;
};

struct TopologyRelayAuthorityBinding {
    bool enabled = false;
    qnet::relay::RoomId room{};
    qnet::relay::RegistrationToken token{};
    uint32_t authorityGeneration = 0;
};

bool validTopologyRelayAuthorityBinding(
    const TopologyRelayAuthorityBinding& binding);

class TopologyRelayAuthorityAdapter {
public:
    TopologyRelayAuthorityAdapter() = default;

    bool configure(const TopologyRelayAuthorityBinding& binding);
    void reset();

    bool enabled() const { return _binding.enabled; }
    const TopologyRelayAuthorityBinding& binding() const { return _binding; }

    bool encodeOutbound(uint64_t nodeId,
                        const uint8_t* bytes,
                        size_t length,
                        std::vector<uint8_t>& out) const;

    TopologyRelayIngress decodeInbound(bool fromRelay,
                                       const uint8_t* bytes,
                                       size_t length,
                                       std::vector<uint8_t>& out,
                                       uint64_t& nodeId);

    bool registrationDue(uint64_t nowMs) const;
    bool encodeRegistration(uint64_t nowMs,
                            std::vector<uint8_t>& out,
                            bool force = false);
    bool encodeDetach(uint64_t nodeId,
                      qnet::relay::TopologyDetachReason reason,
                      std::vector<uint8_t>& out) const;

private:
    TopologyRelayAuthorityBinding _binding{};
    uint32_t _registrationAttempts = 0;
    uint64_t _lastRegistrationMs = 0;
};

struct RelayQsrOffer {
    bool present = false;
    uint32_t relayIpv4BE = 0;
    uint16_t relayPort = 0;
    qnet::relay::RoomId room{};
    TopologyGrant grant{};
};

bool relayQsrOfferAbsent(const qnet::SpectateTopologyInfoV2& info);

bool relayQsrOfferFromInfo(const qnet::SpectateTopologyInfoV2& info,
                           RelayQsrOffer& out);

bool writeRelayQsrOffer(const RelayQsrOffer& offer,
                        qnet::SpectateTopologyInfoV2& out);

bool relayBindingFromOffer(const RelayQsrOffer& offer,
                           const qnet::relay::RegistrationToken& token,
                           TopologyRelayBinding& out);

}
