#pragma once

#include "../net/RelayProtocol.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace qrelay {

// Exit codes are an operator contract: never renumber.
enum class ExitCode : int {
    Ok = 0,
    Usage = 2,
    BindFailure = 3,
    ConfigInvalid = 4,
    FatalRuntime = 5,
};

const char* exitCodeText(ExitCode code);

constexpr uint16_t kDefaultRelayPort = 14764;

struct SpectatorGrantRequest {
    const uint8_t* key = nullptr;
    size_t keyLength = 0;
    uint64_t grantId = 0;
    uint64_t viewerId = 0;
    uint64_t sessionId = 0;
    uint64_t expiryUnix = 0;
    uint32_t sourceGeneration = 0;
    uint32_t resumeRecord = 0;
    uint64_t resumeDigest = 0;
    uint8_t route = 0;
    uint64_t macLow = 0;
    uint64_t macHigh = 0;
};

enum class GrantVerdict : uint8_t {
    Accepted = 0,
    BadMac = 1,
    Malformed = 2,
    Unsupported = 3,
};

class SpectatorGrantVerifier {
public:
    virtual ~SpectatorGrantVerifier() = default;
    virtual GrantVerdict verify(const SpectatorGrantRequest& request) const = 0;
};

const SpectatorGrantVerifier& defaultSpectatorGrantVerifier();
bool spectatorGrantVerifierAvailable();

struct TopologyGrantRequest {
    const uint8_t* key = nullptr;
    size_t keyLength = 0;
    uint64_t grantId = 0;
    uint64_t nodeId = 0;
    uint64_t sessionId = 0;
    uint64_t expiryUnix = 0;
    uint32_t authorityGeneration = 0;
    uint64_t macLow = 0;
    uint64_t macHigh = 0;
    uint64_t nowUnix = 0;
};

class TopologyGrantVerifier {
public:
    virtual ~TopologyGrantVerifier() = default;
    virtual GrantVerdict verify(const TopologyGrantRequest& request) const = 0;
};

const TopologyGrantVerifier& defaultTopologyGrantVerifier();
bool topologyGrantVerifierAvailable();

enum class LogLevel : uint8_t {
    Info = 0,
    Warn = 1,
    Error = 2,
};

using LogSink = void (*)(void* user, LogLevel level, const char* line);

struct RelayServiceConfig {
    std::string bindAddress = "0.0.0.0";
    uint16_t port = kDefaultRelayPort;

    uint32_t maximumRooms = 256;
    uint32_t maximumConnectionsPerIp = 8;
    uint32_t maximumRoomsPerIp = 16;

    bool spectatorLaneEnabled = true;
    uint32_t maximumSpectatorRooms = 64;
    uint32_t maximumViewersPerRoom = 32;
    uint32_t maximumViewers = 512;
    uint32_t maximumViewersPerIp = 4;
    uint64_t maximumGrantLifetimeSeconds = 300;

    bool topologyLaneEnabled = true;
    uint32_t maximumTopologyRooms = 64;
    uint32_t maximumTopologyNodesPerRoom = 32;
    uint32_t maximumTopologyNodes = 512;
    uint32_t maximumTopologyNodesPerIp = 4;

    uint64_t tcpBytesPerSecond = 512u * 1024u;
    uint64_t roomUdpBytesPerSecond = 512u * 1024u;
    uint64_t roomUdpPacketsPerSecond = 600;
    uint64_t viewerUdpBytesPerSecond = 256u * 1024u;
    uint64_t viewerUdpPacketsPerSecond = 400;
    uint64_t registrationsPerSecond = 4;
    uint64_t connectionsPerIpPerSecond = 8;
    uint32_t ipRateStateRetentionMs = 60'000;
    uint32_t maximumTrackedIpStates = 4096;

    uint32_t maximumUdpPacketsPerTurn = 512;
    uint32_t maximumUdpPacketsPerRoomPerTurn = 64;
    uint32_t maximumUdpBytesPerRoomPerTurn = 96u * 1024u;
    uint32_t maximumUnboundPacketsPerTurn = 64;
    uint32_t maximumAcceptsPerTurn = 32;
    uint32_t maximumTcpBytesPerConnectionPerTurn = 32u * 1024u;
    uint32_t pollIntervalMs = 100;

    uint32_t joinTimeoutMs = 10'000;
    uint32_t pairTimeoutMs = 60'000;
    uint32_t registrationTimeoutMs = 120'000;
    uint32_t spectatorRegistrationTimeoutMs = 15'000;
    uint32_t viewerIdleTimeoutMs = 120'000;
    uint64_t maximumRoomIdleMs = 12ull * 60ull * 60ull * 1000ull;

    uint32_t drainTimeoutMs = 3'000;
    uint32_t retryAfterMs = 5'000;
    uint32_t metricsIntervalMs = 30'000;

    std::string logPath;
    uint64_t logMaximumTotalBytes = 8ull * 1024ull * 1024ull;
    uint32_t logFiles = 4;
    bool logToConsole = true;
    bool verbose = false;
};

bool validateRelayServiceConfig(const RelayServiceConfig& config,
                                std::string& error);

struct RelayServiceMetrics {
    uint64_t uptimeMs = 0;
    uint64_t turns = 0;

    uint32_t rooms = 0;
    uint32_t pairedRooms = 0;
    uint32_t connections = 0;
    uint32_t pendingConnections = 0;
    uint32_t spectatorRooms = 0;
    uint32_t viewers = 0;
    uint32_t topologyRooms = 0;
    uint32_t topologyNodes = 0;
    uint32_t trackedIpStates = 0;

    uint64_t playerBytesIn = 0;
    uint64_t playerBytesOut = 0;
    uint64_t playerPacketsIn = 0;
    uint64_t playerPacketsOut = 0;
    uint64_t spectatorBytesIn = 0;
    uint64_t spectatorBytesOut = 0;
    uint64_t spectatorPacketsIn = 0;
    uint64_t spectatorPacketsOut = 0;
    uint64_t topologyBytesIn = 0;
    uint64_t topologyBytesOut = 0;
    uint64_t topologyPacketsIn = 0;
    uint64_t topologyPacketsOut = 0;
    uint64_t tcpBytesProxied = 0;

    uint64_t drops = 0;
    uint64_t roomBudgetDrops = 0;
    uint64_t rateLimitDrops = 0;
    uint64_t rejects = 0;
    uint64_t ipQuotaRejects = 0;
    uint64_t capacityRejects = 0;
    uint64_t grantRejects = 0;
    uint64_t grantExpiredRejects = 0;
    uint64_t grantReplayRejects = 0;
    uint64_t topologyGrantRejects = 0;
    uint64_t topologyGrantReplayRejects = 0;
    uint64_t crossLaneRejects = 0;

    uint64_t roomsOpened = 0;
    uint64_t roomsClosed = 0;
    uint64_t viewersAdmitted = 0;
    uint64_t viewersDetached = 0;
    uint64_t topologyNodesAdmitted = 0;
    uint64_t topologyNodesDetached = 0;

    uint64_t observedRequests = 0;
    uint64_t observedRejects = 0;

    uint64_t p95TurnMicros = 0;
    uint64_t maxTurnMicros = 0;
};

class RelayService {
public:
    RelayService();
    ~RelayService();
    RelayService(const RelayService&) = delete;
    RelayService& operator=(const RelayService&) = delete;

    bool configure(const RelayServiceConfig& config, std::string& error);
    const RelayServiceConfig& config() const;

    void setLogSink(LogSink sink, void* user);
    void setSpectatorGrantVerifier(const SpectatorGrantVerifier* verifier);
    void setTopologyGrantVerifier(const TopologyGrantVerifier* verifier);
    void setMonotonicClock(uint64_t (*clockMs)());
    void setWallClock(uint64_t (*clockUnixSeconds)());

    bool start(std::string& error);
    bool started() const;
    uint16_t boundPort() const;

    ExitCode run();

    bool pumpOnce(int pollTimeoutMs);

    void drain();

    void requestStop();
    bool stopRequested() const;

    RelayServiceMetrics snapshot() const;
    std::string healthLine() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}
