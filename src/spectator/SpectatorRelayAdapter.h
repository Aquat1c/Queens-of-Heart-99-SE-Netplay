#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "net/RelayProtocol.h"
#include "spectator/SpectatorProtocol.h"

namespace qnetplay::spectator {

constexpr uint64_t kRelayViewerRegistrationRetryMs = 500;
constexpr uint32_t kRelayViewerFastRegistrationAttempts = 8;
constexpr uint64_t kRelayViewerSlowRegistrationRetryMs = 2'000;

struct RelayViewerBinding {
    bool enabled = false;
    qnet::relay::RoomId room{};
    qnet::relay::RegistrationToken token{};
    uint64_t viewerId = 0;
    uint32_t sourceGeneration = 0;
    SessionIdentity identity{};
};

bool validRelayViewerBinding(const RelayViewerBinding& binding);

enum class RelayViewerIngress : uint8_t {
    PassThrough = 1,
    Data = 2,
    Ready = 3,
    Detached = 4,
    Rejected = 5,
};

class RelayViewerAdapter {
public:
    RelayViewerAdapter() = default;
    explicit RelayViewerAdapter(const RelayViewerBinding& binding) {
        configure(binding);
    }

    bool configure(const RelayViewerBinding& binding);
    void reset();

    bool enabled() const { return _binding.enabled; }
    bool ready() const { return _ready; }
    bool detached() const { return _detached; }
    uint32_t registrationAttempts() const { return _registrationAttempts; }
    const RelayViewerBinding& binding() const { return _binding; }

    bool encodeOutbound(const uint8_t* bytes,
                        size_t length,
                        std::vector<uint8_t>& out) const;

    RelayViewerIngress decodeInbound(bool fromRelay,
                                     const uint8_t* bytes,
                                     size_t length,
                                     std::vector<uint8_t>& out);

    bool registrationDue(uint64_t nowMs) const;
    bool encodeRegistration(uint64_t nowMs,
                            std::vector<uint8_t>& out,
                            bool force = false);
    bool encodeDetach(qnet::relay::SpectatorDetachReason reason,
                      std::vector<uint8_t>& out) const;

private:
    RelayViewerBinding _binding{};
    bool _ready = false;
    bool _detached = false;
    uint32_t _registrationAttempts = 0;
    uint64_t _lastRegistrationMs = 0;
};

constexpr uint64_t kRelaySourceRegistrationRefreshMs = 20'000;
constexpr uint32_t kRelayVirtualIpv4BE = 0xFEFFFFEFu;

struct RelaySourceBinding {
    bool enabled = false;
    qnet::relay::RoomId room{};
    qnet::relay::RegistrationToken token{};
    uint32_t sourceGeneration = 0;
    SessionIdentity identity{};
};

struct RelayVirtualEndpoint {
    uint32_t ipv4BE = 0;
    uint16_t port = 0;

    bool valid() const { return ipv4BE != 0 && port != 0; }
    bool operator==(const RelayVirtualEndpoint& other) const {
        return ipv4BE == other.ipv4BE && port == other.port;
    }
};

bool validRelaySourceBinding(const RelaySourceBinding& binding);

enum class RelaySourceIngress : uint8_t {
    PassThrough = 1,
    Data = 2,
    Ready = 3,
    Detached = 4,
    Rejected = 5,
};

class RelaySourceAdapter {
public:
    bool configure(const RelaySourceBinding& binding);
    void reset();

    bool enabled() const { return _binding.enabled; }
    const RelaySourceBinding& binding() const { return _binding; }
    size_t viewerCount() const { return _viewers.size(); }

    RelaySourceIngress decodeInbound(bool fromRelay,
                                     const uint8_t* bytes,
                                     size_t length,
                                     std::vector<uint8_t>& out,
                                     RelayVirtualEndpoint& from,
                                     uint64_t* detachedViewerId = nullptr);
    bool encodeOutbound(const RelayVirtualEndpoint& to,
                        const uint8_t* bytes,
                        size_t length,
                        std::vector<uint8_t>& out) const;

    bool encodeRegistration(uint64_t nowMs,
                            std::vector<uint8_t>& out,
                            bool force = false);
    bool encodeDetach(uint64_t viewerId,
                      qnet::relay::SpectatorDetachReason reason,
                      std::vector<uint8_t>& out) const;

    bool viewerForEndpoint(const RelayVirtualEndpoint& endpoint,
                           uint64_t& viewerId) const;

private:
    struct ViewerRoute {
        uint64_t viewerId = 0;
        RelayVirtualEndpoint endpoint{};
    };

    RelayVirtualEndpoint ensureViewer(uint64_t viewerId);
    void removeViewer(uint64_t viewerId);

    RelaySourceBinding _binding{};
    std::vector<ViewerRoute> _viewers;
    uint16_t _nextVirtualPort = 1;
    uint64_t _lastRegistrationMs = 0;
};

}
