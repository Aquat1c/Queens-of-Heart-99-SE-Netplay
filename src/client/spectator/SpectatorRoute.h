#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/RouteCandidates.h"
#include "compat/CompatibilityIdentity.h"
#include "spectator/SpectatorProtocol.h"
#include "spectator/SpectatorRelayAdapter.h"

namespace qnet {
struct RouteCandidates;
}

#if defined(__has_include)
#  if __has_include("spectator/SpectatorGrant.h")
#    include "spectator/SpectatorGrant.h"
#    define QCLIENT_SPECTATOR_GRANT_HEADER 1
#  endif
#endif
#ifndef QCLIENT_SPECTATOR_GRANT_HEADER
#define QCLIENT_SPECTATOR_GRANT_HEADER 0
namespace qnetplay::spectator {
struct SpectatorGrant {
    uint64_t grantId = 0;
    uint64_t viewerId = 0;
    uint64_t sessionId = 0;
    uint64_t expiryUnix = 0;
    uint32_t sourceGeneration = 0;
    uint32_t resumeRecord = 0;
    uint64_t resumeDigest = 0;
    uint64_t macLow = 0;
    uint64_t macHigh = 0;
    uint8_t route = 0;
};
}
#endif

namespace qclient::spectator {

bool parseSpectateKeyHex(const std::string& text, uint64_t& high, uint64_t& low);

enum class SpectatorRouteLabel : uint16_t {
    None = 0,
    LanDirect = 1,
    PublicDirect = 2,
    PublicDirectHairpin = 3,
    Punched = 4,
    Relay = 5,
};

const char* routeLabelName(SpectatorRouteLabel label);

struct RouteEndpoint {
    uint32_t ipv4BE = 0;
    uint16_t port = 0;

    bool valid() const { return ipv4BE != 0 && port != 0; }
    bool operator==(const RouteEndpoint& other) const {
        return ipv4BE == other.ipv4BE && port == other.port;
    }
    bool operator!=(const RouteEndpoint& other) const {
        return !(*this == other);
    }
};

enum class SpectateCandidateKind : uint8_t {
    None = 0,
    Host = 1,
    Lan = 2,
    Reflexive = 3,
    Relay = 4,
};

struct SpectateCandidate {
    SpectateCandidateKind kind = SpectateCandidateKind::None;
    RouteEndpoint endpoint{};
    uint8_t routeId = 0;
};

constexpr size_t kMaxInitialSpectateCandidates = qnet::kMaxRouteCandidates;
constexpr size_t kMaxControlSpectateCandidates = qnet::kMaxRouteCandidates;
constexpr size_t kMaxSpectateCandidates =
    kMaxInitialSpectateCandidates + kMaxControlSpectateCandidates + 3;

SpectatorRouteLabel labelForCandidate(const SpectateCandidate& candidate,
                                      uint32_t ownObservedPublicIpv4BE,
                                      bool confirmedByPunch);

bool decodeSpectateRouteCandidates(
    const qnet::RouteCandidates& wire,
    uint32_t expectedSessionId,
    qnet::CandidateEndpoint* out,
    size_t outCap,
    uint8_t& outCount,
    std::string& error);

enum class PreflightFailure : uint16_t {
    None = 0,
    Cancelled = 1,
    InvalidTarget = 2,
    WrongKey = 3,
    GrantInvalid = 4,
    GrantExpired = 5,
    StaleSession = 6,
    SourceUnavailable = 7,
    ArchiveUnavailable = 8,
    CapacityFull = 9,
    RateLimited = 10,
    Unreachable = 11,
    RelayUnavailable = 12,
    ManifestInvalid = 13,
    HostDeclined = 14,
    Timeout = 15,
    InternalError = 16,
    LiveRouteUnavailable = 17,
};

const char* preflightFailureText(PreflightFailure failure);

bool isDefinitivePreflightFailure(PreflightFailure failure);

struct SpectateTarget {
    std::string hostText;
    uint16_t hostPort = 0;
    uint16_t directUdpPort = 0;
    uint16_t viewerDataPort = 0;

    uint32_t sessionId = 0;
    uint64_t epoch = 0;
    uint64_t keyLow = 0;
    uint64_t keyHigh = 0;

    qnet::CandidateEndpoint
        initialCandidates[kMaxInitialSpectateCandidates]{};
    uint8_t initialCandidateCount = 0;

    uint64_t simulationContract = 0;
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity{};

    uint64_t viewerId = 0;
    uint32_t generation = 1;

    uint32_t resumeRecord = 0;
    uint64_t resumeDigest = 0;

    std::string relayHostText;
    uint16_t relayPort = 0;
    qnet::relay::RoomId relayRoom{};
    // This admission capability must never be placed in IPC or logged.
    qnet::relay::RegistrationToken relayViewerAccessToken{};
    bool allowRelayFallback = true;
    bool allowPunch = true;

    uint8_t requestedRoute = 0;

    uint32_t punchRelayIpv4BE = 0;
    uint16_t punchRelayPort = 0;

    uint32_t observedPublicIpv4BE = 0;

    unsigned helloIntervalMs = 250;
    unsigned punchIntervalMs = 125;
    unsigned directRaceMs = 3000;

    qnetplay::spectator::SessionIdentity identity() const;
};

bool punchRelayEndpointForTarget(const SpectateTarget& target,
                                 RouteEndpoint& out);

struct SpectatePreflightResult {
    bool ok = false;
    std::string failureReason;
    uint16_t routeLabel = 0;
    qnetplay::spectator::SpectatorGrant grant{};
    uint32_t firstAvailableRecord = 0;
    uint64_t manifestDigest = 0;
    std::string statusText;
    std::string fallbackReason;

    PreflightFailure failure = PreflightFailure::None;
    bool inconclusive = false;
    bool sourceConfirmedLive = false;
    RouteEndpoint confirmedEndpoint{};
    SpectateCandidateKind confirmedKind = SpectateCandidateKind::None;
    bool waitingForStart = false;
    bool hasManifest = false;
    qnetplay::spectator::GameStartPayload manifest{};
    uint64_t gameId = 0;
    uint64_t viewerId = 0;
    uint32_t generation = 0;
    unsigned elapsedMs = 0;
    std::string routeSummary;
    // The adopting runtime must continue this viewer->source QSP sequence, because restarting it replays spent sequences. Zero means the preflight sent nothing on this socket.
    uint32_t nextSequence = 0;
    bool detachSent = false;
    bool reservationsReleased = true;
    bool processCreated = false;
};

enum class ControlOutcome : uint8_t {
    Ok = 1,
    NoMatch = 2,
    Refused = 3,
    CapacityFull = 4,
    Unreachable = 5,
    Stale = 6,
    Expired = 7,
    RateLimited = 8,
    Error = 9,
};

struct ControlAnswer {
    ControlOutcome outcome = ControlOutcome::Error;
    RouteEndpoint sourceDirect{};
    uint16_t sourceUdpPort = 0;
    qnet::CandidateEndpoint
        candidates[kMaxControlSpectateCandidates]{};
    uint8_t candidateCount = 0;
    bool sourceConfirmedLive = false;
    qnetplay::spectator::SessionIdentity identity{};
    uint64_t simulationContract = 0;
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity{};
    uint32_t sourceGeneration = 0;
    bool hasGrant = false;
    qnetplay::spectator::SpectatorGrant grant{};
    uint64_t viewerId = 0;
    uint16_t viewerCount = 0;
    uint16_t viewerCapacity = 0;
    uint32_t retryAfterMs = 0;
    uint32_t observedPublicIpv4BE = 0;
    qnet::relay::RoomId relayRoom{};
    qnet::relay::RegistrationToken relayViewerAccessToken{};
    std::string statusText;
    std::string error;
};

struct RelayReservation {
    bool ok = false;
    bool retryable = false;
    RouteEndpoint allocated{};
    uint64_t viewerId = 0;
    uint32_t retryAfterMs = 0;
    std::string error;
};

class RouteIo {
public:
    virtual ~RouteIo() = default;

    virtual uint64_t nowMs() = 0;
    virtual void waitMs(unsigned ms) = 0;

    virtual bool resolveHost(const std::string& text,
                             uint16_t port,
                             RouteEndpoint& out,
                             std::string& error) = 0;
    virtual size_t localIpv4BE(uint32_t* out, size_t cap) = 0;

    virtual bool openViewerSocket(uint16_t& boundPort, std::string& error) = 0;
    virtual void closeViewerSocket() = 0;
    virtual uintptr_t detachViewerSocket() { return ~uintptr_t(0); }
    virtual bool relayViewerBinding(
        qnetplay::spectator::RelayViewerBinding& out) const {
        out = {};
        return false;
    }
    virtual bool sendTo(const RouteEndpoint& to,
                        const uint8_t* bytes,
                        size_t length) = 0;
    virtual int recvFrom(uint8_t* buffer,
                         size_t capacity,
                         RouteEndpoint& from) = 0;

    virtual void controlExchange(const SpectateTarget& target,
                                 ControlAnswer& out) = 0;
    virtual bool controlRelease(const SpectateTarget& target,
                                uint64_t viewerId,
                                std::string& error) = 0;

    virtual void relayReserve(const SpectateTarget& target,
                              const qnetplay::spectator::SpectatorGrant& grant,
                              RelayReservation& out) = 0;
    virtual void relayRelease() = 0;
};

std::unique_ptr<RouteIo> makeSystemRouteIo();

SpectatePreflightResult preflightSpectate(
    const SpectateTarget& target,
    unsigned timeoutMs,
    const std::function<bool()>& cancelled);

SpectatePreflightResult preflightSpectateWith(
    const SpectateTarget& target,
    unsigned timeoutMs,
    const std::function<bool()>& cancelled,
    RouteIo& io);

SpectateTarget migratedTarget(const SpectateTarget& previous,
                              uint32_t receivedNextRecord,
                              uint64_t receivedDigest);

std::string redactedTag(uint64_t secret);

}
