#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "RelayServiceVerb.h"

#include "RelayService.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace qrelay {
namespace {

std::atomic<RelayService*> g_activeService{ nullptr };

BOOL WINAPI consoleControlHandler(const DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            RelayService* const service =
                g_activeService.load(std::memory_order_acquire);
            if (service != nullptr) service->requestStop();
            return TRUE;
        }
        default:
            return FALSE;
    }
}

bool parseUnsigned(const char* const text,
                   const uint64_t maximum,
                   uint64_t& out) {
    if (text == nullptr || text[0] == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    if (value > maximum) return false;
    out = static_cast<uint64_t>(value);
    return true;
}

struct Parser {
    int argc = 0;
    char** argv = nullptr;
    int index = 1;
    std::string* error = nullptr;

    bool fail(const std::string& message) {
        *error = message;
        return false;
    }

    bool value(const std::string& argument,
               const char* const name,
               bool& consumed,
               const char*& out) {
        const std::string prefix = std::string("--") + name;
        if (argument == prefix) {
            if (consumed) return fail(std::string("duplicate ") + prefix);
            if (index + 1 >= argc || argv[index + 1] == nullptr
                || argv[index + 1][0] == '\0') {
                return fail(prefix + " requires a value");
            }
            out = argv[++index];
            consumed = true;
            return true;
        }
        if (argument.rfind(prefix + "=", 0) == 0) {
            if (consumed) return fail(std::string("duplicate ") + prefix);
            const std::string text = argument.substr(prefix.size() + 1u);
            if (text.empty()) return fail(prefix + " requires a value");
            out = argv[index] + prefix.size() + 1u;
            consumed = true;
            return true;
        }
        return false;
    }
};

}

void printRelayUsage() {
    std::printf(
        "qoh99 relay - rendezvous and raw UDP relay service\n"
        "\n"
        "Usage: <program> relay [options]\n"
        "\n"
        "Endpoint:\n"
        "  --bind IPV4                     numeric bind address (0.0.0.0)\n"
        "  --port N                        TCP+UDP port (%u)\n"
        "\n"
        "Player lane capacity (reserved; spectators never consume it):\n"
        "  --max-rooms N                   1..4096 (256)\n"
        "  --max-connections-per-ip N      1..1024 (8)\n"
        "  --max-rooms-per-ip N            1..4096 (16)\n"
        "\n"
        "Spectator lane (separately reserved, wire v%u):\n"
        "  --spectator | --no-spectator    enable the lane (enabled)\n"
        "  --max-spectator-rooms N         0..4096 (64)\n"
        "  --max-viewers N                 1..65535 (512)\n"
        "  --max-viewers-per-room N        1..1024 (32)\n"
        "  --max-viewers-per-ip N          1..1024 (4)\n"
        "  --max-grant-lifetime SECONDS    1..86400 (300)\n"
        "  --spectator-registration-timeout-ms N  100..120000 (15000)\n"
        "\n"
        "Topology lane (separately reserved, wire v%u):\n"
        "  --topology | --no-topology      enable the lane (enabled)\n"
        "  --max-topology-rooms N          0..4096 (64)\n"
        "  --max-topology-nodes N          1..65535 (512)\n"
        "  --max-topology-nodes-per-room N 1..1024 (32)\n"
        "  --max-topology-nodes-per-ip N   1..1024 (4)\n"
        "\n"
        "Budgets and fair scheduling:\n"
        "  --room-kib N                    per-room UDP KiB/s (512)\n"
        "  --viewer-kib N                  per-viewer UDP KiB/s (256)\n"
        "  --tcp-kib N                     per-connection KiB/s (512)\n"
        "  --room-packets-per-second N     (600)\n"
        "  --viewer-packets-per-second N   (400)\n"
        "  --registrations-per-second N    (4)\n"
        "  --connections-per-ip-per-second N (8)\n"
        "  --udp-packets-per-turn N        (512)\n"
        "  --room-packets-per-turn N       fair-share bound (64)\n"
        "  --room-kib-per-turn N           fair-share bound (96)\n"
        "  --accepts-per-turn N            (32)\n"
        "  --poll-interval-ms N            (100)\n"
        "\n"
        "Lifecycle and observability:\n"
        "  --drain-ms N                    bounded stop drain (3000)\n"
        "  --retry-after-ms N              advertised client retry (5000)\n"
        "  --metrics-ms N                  health line period, 0 off (30000)\n"
        "  --log PATH                      rotating log file\n"
        "  --log-max-kib N                 bounded total size (8192)\n"
        "  --log-files N                   2..64 rotations (4)\n"
        "  --quiet                         no console output\n"
        "  --verbose                       per-connection detail\n"
        "  --help                          this text\n"
        "\n"
        "Exit codes: 0 ok, 2 usage, 3 bind failure, 4 invalid configuration,\n"
        "5 fatal runtime error.\n",
        static_cast<unsigned>(kDefaultRelayPort),
        static_cast<unsigned>(qnet::relay::kSpectatorVersion),
        static_cast<unsigned>(qnet::relay::kTopologyVersion));
}

bool parseRelayOptions(const int argc,
                       char** const argv,
                       RelayServiceConfig& config,
                       bool& showHelp,
                       std::string& error) {
    showHelp = false;
    error.clear();
    RelayServiceConfig parsed{};

    Parser parser{};
    parser.argc = argc;
    parser.argv = argv;
    parser.error = &error;

    bool sawBind = false;
    bool sawPort = false;
    bool sawMaxRooms = false;
    bool sawConnectionsPerIp = false;
    bool sawRoomsPerIp = false;
    bool sawSpectator = false;
    bool sawSpectatorRooms = false;
    bool sawViewers = false;
    bool sawViewersPerRoom = false;
    bool sawViewersPerIp = false;
    bool sawGrantLifetime = false;
    bool sawSpectatorRegistrationTimeout = false;
    bool sawTopology = false;
    bool sawTopologyRooms = false;
    bool sawTopologyNodes = false;
    bool sawTopologyNodesPerRoom = false;
    bool sawTopologyNodesPerIp = false;
    bool sawRoomKib = false;
    bool sawViewerKib = false;
    bool sawTcpKib = false;
    bool sawRoomPackets = false;
    bool sawViewerPackets = false;
    bool sawRegistrations = false;
    bool sawConnectionRate = false;
    bool sawUdpPerTurn = false;
    bool sawRoomPerTurn = false;
    bool sawRoomKibPerTurn = false;
    bool sawAcceptsPerTurn = false;
    bool sawPollInterval = false;
    bool sawDrain = false;
    bool sawRetryAfter = false;
    bool sawMetrics = false;
    bool sawLog = false;
    bool sawLogMax = false;
    bool sawLogFiles = false;
    bool sawQuiet = false;
    bool sawVerbose = false;

    struct NumericOption {
        const char* name;
        bool* seen;
        uint64_t maximum;
        uint64_t* target64;
        uint32_t* target32;
        uint64_t scale;
    };

    uint64_t roomKib = parsed.roomUdpBytesPerSecond / 1024u;
    uint64_t viewerKib = parsed.viewerUdpBytesPerSecond / 1024u;
    uint64_t tcpKib = parsed.tcpBytesPerSecond / 1024u;
    uint64_t roomKibPerTurn = parsed.maximumUdpBytesPerRoomPerTurn / 1024u;
    uint64_t logMaxKib = parsed.logMaximumTotalBytes / 1024u;

    const NumericOption numeric[] = {
        { "port", &sawPort, 65535, nullptr, nullptr, 0 },
        { "max-rooms", &sawMaxRooms, 0xFFFFFFFFull,
          nullptr, &parsed.maximumRooms, 1 },
        { "max-connections-per-ip", &sawConnectionsPerIp, 0xFFFFFFFFull,
          nullptr, &parsed.maximumConnectionsPerIp, 1 },
        { "max-rooms-per-ip", &sawRoomsPerIp, 0xFFFFFFFFull,
          nullptr, &parsed.maximumRoomsPerIp, 1 },
        { "max-spectator-rooms", &sawSpectatorRooms, 0xFFFFFFFFull,
          nullptr, &parsed.maximumSpectatorRooms, 1 },
        { "max-viewers", &sawViewers, 0xFFFFFFFFull,
          nullptr, &parsed.maximumViewers, 1 },
        { "max-viewers-per-room", &sawViewersPerRoom, 0xFFFFFFFFull,
          nullptr, &parsed.maximumViewersPerRoom, 1 },
        { "max-viewers-per-ip", &sawViewersPerIp, 0xFFFFFFFFull,
          nullptr, &parsed.maximumViewersPerIp, 1 },
        { "max-grant-lifetime", &sawGrantLifetime, 0xFFFFFFFFull,
          &parsed.maximumGrantLifetimeSeconds, nullptr, 1 },
        { "spectator-registration-timeout-ms",
          &sawSpectatorRegistrationTimeout, 0xFFFFFFFFull,
          nullptr, &parsed.spectatorRegistrationTimeoutMs, 1 },
        { "max-topology-rooms", &sawTopologyRooms, 0xFFFFFFFFull,
          nullptr, &parsed.maximumTopologyRooms, 1 },
        { "max-topology-nodes", &sawTopologyNodes, 0xFFFFFFFFull,
          nullptr, &parsed.maximumTopologyNodes, 1 },
        { "max-topology-nodes-per-room", &sawTopologyNodesPerRoom,
          0xFFFFFFFFull, nullptr, &parsed.maximumTopologyNodesPerRoom, 1 },
        { "max-topology-nodes-per-ip", &sawTopologyNodesPerIp,
          0xFFFFFFFFull, nullptr, &parsed.maximumTopologyNodesPerIp, 1 },
        { "room-kib", &sawRoomKib, 0xFFFFFFull, &roomKib, nullptr, 1 },
        { "viewer-kib", &sawViewerKib, 0xFFFFFFull, &viewerKib, nullptr, 1 },
        { "tcp-kib", &sawTcpKib, 0xFFFFFFull, &tcpKib, nullptr, 1 },
        { "room-packets-per-second", &sawRoomPackets, 0xFFFFFFFFull,
          &parsed.roomUdpPacketsPerSecond, nullptr, 1 },
        { "viewer-packets-per-second", &sawViewerPackets, 0xFFFFFFFFull,
          &parsed.viewerUdpPacketsPerSecond, nullptr, 1 },
        { "registrations-per-second", &sawRegistrations, 0xFFFFFFFFull,
          &parsed.registrationsPerSecond, nullptr, 1 },
        { "connections-per-ip-per-second", &sawConnectionRate, 0xFFFFFFFFull,
          &parsed.connectionsPerIpPerSecond, nullptr, 1 },
        { "udp-packets-per-turn", &sawUdpPerTurn, 0xFFFFFFFFull,
          nullptr, &parsed.maximumUdpPacketsPerTurn, 1 },
        { "room-packets-per-turn", &sawRoomPerTurn, 0xFFFFFFFFull,
          nullptr, &parsed.maximumUdpPacketsPerRoomPerTurn, 1 },
        { "room-kib-per-turn", &sawRoomKibPerTurn, 0xFFFFFFull,
          &roomKibPerTurn, nullptr, 1 },
        { "accepts-per-turn", &sawAcceptsPerTurn, 0xFFFFFFFFull,
          nullptr, &parsed.maximumAcceptsPerTurn, 1 },
        { "poll-interval-ms", &sawPollInterval, 0xFFFFFFFFull,
          nullptr, &parsed.pollIntervalMs, 1 },
        { "drain-ms", &sawDrain, 0xFFFFFFFFull,
          nullptr, &parsed.drainTimeoutMs, 1 },
        { "retry-after-ms", &sawRetryAfter, 0xFFFFFFFFull,
          nullptr, &parsed.retryAfterMs, 1 },
        { "metrics-ms", &sawMetrics, 0xFFFFFFFFull,
          nullptr, &parsed.metricsIntervalMs, 1 },
        { "log-max-kib", &sawLogMax, 0xFFFFFFull, &logMaxKib, nullptr, 1 },
        { "log-files", &sawLogFiles, 0xFFFFFFFFull,
          nullptr, &parsed.logFiles, 1 },
    };

    for (parser.index = 1; parser.index < argc; ++parser.index) {
        const std::string argument =
            argv[parser.index] != nullptr ? argv[parser.index] : "";
        if (argument == "--help" || argument == "-h" || argument == "/?") {
            showHelp = true;
            return true;
        }
        if (argument == "--spectator" || argument == "--no-spectator") {
            if (sawSpectator) {
                error = "duplicate --spectator/--no-spectator";
                return false;
            }
            parsed.spectatorLaneEnabled = argument == "--spectator";
            sawSpectator = true;
            continue;
        }
        if (argument == "--topology" || argument == "--no-topology") {
            if (sawTopology) {
                error = "duplicate --topology/--no-topology";
                return false;
            }
            parsed.topologyLaneEnabled = argument == "--topology";
            sawTopology = true;
            continue;
        }
        if (argument == "--quiet") {
            if (sawQuiet) {
                error = "duplicate --quiet";
                return false;
            }
            parsed.logToConsole = false;
            sawQuiet = true;
            continue;
        }
        if (argument == "--verbose") {
            if (sawVerbose) {
                error = "duplicate --verbose";
                return false;
            }
            parsed.verbose = true;
            sawVerbose = true;
            continue;
        }

        const char* text = nullptr;
        if (parser.value(argument, "bind", sawBind, text)) {
            parsed.bindAddress = text;
            continue;
        }
        if (!error.empty()) return false;
        if (parser.value(argument, "log", sawLog, text)) {
            parsed.logPath = text;
            continue;
        }
        if (!error.empty()) return false;

        bool matched = false;
        for (const NumericOption& option : numeric) {
            text = nullptr;
            if (!parser.value(argument, option.name, *option.seen, text)) {
                if (!error.empty()) return false;
                continue;
            }
            uint64_t value = 0;
            if (!parseUnsigned(text, option.maximum, value)) {
                error = std::string("--") + option.name
                    + " requires a non-negative integer up to "
                    + std::to_string(option.maximum);
                return false;
            }
            if (option.target64 != nullptr) {
                *option.target64 = value;
            } else if (option.target32 != nullptr) {
                if (value > 0xFFFFFFFFull) {
                    error = std::string("--") + option.name + " is too large";
                    return false;
                }
                *option.target32 = static_cast<uint32_t>(value);
            } else {
                if (value == 0 || value > 65535) {
                    error = "--port must be between 1 and 65535";
                    return false;
                }
                parsed.port = static_cast<uint16_t>(value);
            }
            matched = true;
            break;
        }
        if (matched) continue;
        if (!error.empty()) return false;

        error = "unknown argument: " + argument;
        return false;
    }

    parsed.roomUdpBytesPerSecond = roomKib * 1024ull;
    parsed.viewerUdpBytesPerSecond = viewerKib * 1024ull;
    parsed.tcpBytesPerSecond = tcpKib * 1024ull;
    if (roomKibPerTurn * 1024ull > 0xFFFFFFFFull) {
        error = "--room-kib-per-turn is too large";
        return false;
    }
    parsed.maximumUdpBytesPerRoomPerTurn =
        static_cast<uint32_t>(roomKibPerTurn * 1024ull);
    parsed.logMaximumTotalBytes = logMaxKib * 1024ull;
    config = parsed;
    return true;
}

int runRelayVerb(const int argc, char** const argv) {
    RelayServiceConfig config{};
    bool showHelp = false;
    std::string error;
    if (!parseRelayOptions(argc, argv, config, showHelp, error)) {
        std::fprintf(stderr, "relay: %s\n", error.c_str());
        printRelayUsage();
        return static_cast<int>(ExitCode::Usage);
    }
    if (showHelp) {
        printRelayUsage();
        return static_cast<int>(ExitCode::Ok);
    }

    RelayService service;
    if (!service.configure(config, error)) {
        std::fprintf(stderr, "relay: invalid configuration: %s\n",
                     error.c_str());
        return static_cast<int>(ExitCode::ConfigInvalid);
    }

    g_activeService.store(&service, std::memory_order_release);
    if (!SetConsoleCtrlHandler(&consoleControlHandler, TRUE)) {
        std::fprintf(stderr,
                     "relay: SetConsoleCtrlHandler failed: %lu\n",
                     GetLastError());
        g_activeService.store(nullptr, std::memory_order_release);
        return static_cast<int>(ExitCode::FatalRuntime);
    }

    int result = static_cast<int>(ExitCode::Ok);
    if (!service.start(error)) {
        std::fprintf(stderr, "relay: %s\n", error.c_str());
        result = static_cast<int>(ExitCode::BindFailure);
    } else {
        std::printf("Press Ctrl-C to stop cleanly.\n");
        std::fflush(stdout);
        result = static_cast<int>(service.run());
    }

    (void)SetConsoleCtrlHandler(&consoleControlHandler, FALSE);
    g_activeService.store(nullptr, std::memory_order_release);
    return result;
}

}
