// Pipe name: \\.\pipe\qoh99np_<gamePid>; the client creates the server end before resuming the game process.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../compat/CompatibilityIdentity.h"
#include "../netplay/ConfigurationProvenance.h"
#include "input/InputLaunchConfig.h"

namespace qipc {

constexpr uint32_t kIpcMagic   = 0x51494243; // 'QIBC'
constexpr uint32_t kIpcVersion = 25;

constexpr uint16_t kReplayAutoStart = 1u;
constexpr uint16_t kReplayHoldFinal = 2u;
constexpr uint16_t kReplayShowInputs = 4u;
constexpr uint16_t kReplayShowTransport = 8u;
constexpr uint16_t kReplayShowHitboxes = 16u;
constexpr uint16_t kReplayDisplayFlagsMask = 28u;
constexpr uint16_t kReplayPresentationFlagsMask = 31u;
constexpr uint16_t kReplayPlaybackIpcSchemaVersion = 3u;
constexpr uint16_t kReplayPlaybackRequiredFeatureBits = 0x001Fu;
constexpr uint16_t kReplayPlaybackSupersededStateSchemaBit = 0x0020u;
constexpr uint16_t kReplayPlaybackOptionalFeatureBits =
    kReplayPlaybackSupersededStateSchemaBit;
constexpr uint8_t kReplayEndpointRecordedGameEnd = 1u;
constexpr uint8_t kReplayEndpointValidatedPrefix = 2u;
constexpr uint32_t kReplayMaxPrebufferFrames = 300000u;
constexpr uint32_t kReplayMaxEventsPerTick = 8u;
constexpr uint32_t kReplayPrebufferDeadlineMs = 120000u;
constexpr uint64_t kReplayMaxSourceBytes =
    512ull * 1024ull * 1024ull;
constexpr uint64_t kRuntimeCompatibilityId = 0x9F3A7C52D6E4B801ull;

enum RunModeWire : uint8_t {
    MODE_OFF      = 0,
    MODE_OBSERVE  = 1,
    MODE_SELFTEST = 2,
    MODE_NETPLAY  = 3,
    MODE_SPECTATOR= 4,
    MODE_REPLAY   = 5,
};

constexpr uint8_t kCompatFlagWine   = 0x01;
constexpr uint8_t kCompatFlagProton = 0x02;

enum SpectatorServiceWire : uint8_t {
    SPECTATOR_SERVICE_OFF = 0,
    SPECTATOR_SERVICE_INPROCESS = 1,
    SPECTATOR_SERVICE_SIDECAR = 2,
};

constexpr uint8_t kSpectatorTopologyRouteDirect = 1u;
constexpr uint8_t kSpectatorTopologyRoutePunched = 2u;
constexpr uint8_t kSpectatorTopologyRouteRelay = 3u;

constexpr size_t kSessionDirBytes = 512;

// VS-mode-select rows for sub_432EE0(row) direct-boot (see qoh99_game.h).
constexpr uint8_t kVsRow1vCpu = 0;   // "1P vs COM"
constexpr uint8_t kVsRow1v1   = 1;   // "1P vs 2P" (both human) - netplay target

#pragma pack(push, 1)
struct IpcConfig {
    uint32_t magic;
    uint32_t version;
    uint8_t  mode;
    uint8_t  isHost;
    uint8_t  localPlayer;
    uint8_t  delay;
    uint8_t  rollback;
    uint8_t  remoteDelay;
    uint8_t  remoteRollback;
    uint8_t  maxDelay;
    uint8_t  maxRollback;
    uint8_t  directBoot;
    uint8_t  vsRow;
    uint8_t  probeMode;
    uint32_t sessionId;
    uint64_t compatibilityId;
    uint64_t cookieLow;
    uint64_t cookieHigh;
    uint64_t spectatorCookieLow;
    uint64_t spectatorCookieHigh;
    uint8_t  serveSpectators;
    uint8_t  musicVolume;
    uint8_t  soundVolume;
    uint8_t  hideNativeScore;
    uint8_t  hideNativeTimer;
    uint8_t  disableExtraFunctions;
    // Peer UDP endpoint, with fields in network byte order as sockaddr uses them.
    uint32_t peerAddrBE;
    uint16_t peerPortBE;
    uint16_t hasSocket;
    uint16_t hasSpectatorSocket;
    char     sessionDir[kSessionDirBytes];
    uint16_t sessionDirBytes;
    char     localName[32];
    char     remoteName[32];
    uint32_t localNameCodePage;
    uint32_t remoteNameCodePage;
    // routeLabel mirrors qnet::RouteLabel: 0 none, 1 lan-direct, 2 public-direct, 3 public-direct possible NAT hairpin, 4 relay.
    uint8_t  routeLabel;
    uint32_t prelaunchRouteP50Us;
    uint32_t prelaunchRouteP95Us;
    uint32_t prelaunchRouteJitterUs;
    uint16_t prelaunchRouteLossPerMille;
    uint16_t prelaunchRouteSamples;
    WSAPROTOCOL_INFOW protocolInfo;
    WSAPROTOCOL_INFOW spectatorProtocolInfo;
    qinput::InputConfigEnvelope inputEnvelope;
    qinput::InputLaunchConfig inputConfig;
    uint8_t offlineExtraPresent[3];
    qinput::InputLaunchConfig offlineExtraConfigs[3];
    uint64_t offlineExtrasHash;
    uint8_t trainingEnabled;

    uint8_t compatFlags;
    uint8_t spectatorServiceMode;
    uint8_t recordEnabled;
    uint8_t reservedV13a;
    uint32_t replayChannelGeneration;
    uint32_t recordChannelGeneration;
    uint64_t recordChannelNonce;
    uint64_t replayBrokerNonce;
    uint64_t replayManifestDigest;
    uint16_t replayIpcSchemaVersion;
    uint16_t replayFeatureBits;
    uint32_t replayMatchOrdinal;
    uint32_t replaySpeedPermille;
    uint64_t spectatorGrantId;
    uint64_t spectatorGrantExpiryUnix;
    uint64_t spectatorGrantMacLow;
    uint64_t spectatorGrantMacHigh;
    uint16_t spectatorMaxViewers;
    uint16_t reservedV13b;

    uint64_t spectatorViewerId;
    uint64_t spectatorGameId;
    uint32_t spectatorGeneration;
    uint32_t spectatorResumeRecord;
    uint64_t spectatorResumeDigest;

    // The relay registration token is a capability and must never be logged.
    uint8_t spectatorRelayActive;
    uint8_t reservedV15[3];
    uint64_t spectatorRelayRoomLow;
    uint64_t spectatorRelayRoomHigh;
    uint64_t spectatorRelayTokenLow;
    uint64_t spectatorRelayTokenHigh;
    uint32_t spectatorRelaySourceGeneration;

    uint8_t replayContentManifestRoot[32];

    uint32_t replayGameGeneration;
    uint32_t replayExpectedFrameCount;
    uint32_t replayExpectedEventCount;
    uint64_t replayGameUuidLow;
    uint64_t replayGameUuidHigh;
    uint64_t replayValidatedSourceByteCount;
    uint8_t replayValidatedSourceRangeDigest[32];
    uint8_t replayEndpointKind;
    uint8_t replayHeadless;
    uint16_t replayPresentationFlags;

    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity;

    qnetplay::config_provenance::ConfigurationProvenanceWire
        configurationProvenance;

    uint8_t spectatorTopologyEnabled;
    uint8_t spectatorTopologyRouteKind;
    // IPv4 is in network byte order; the port is in host order, matching SpectateTarget.
    uint16_t spectatorTopologyHostControlPort;
    uint32_t spectatorTopologyHostControlIpv4BE;
    uint64_t spectatorTopologyQspRouteGeneration;
    uint32_t spectatorTopologyGrantSourceGeneration;
    uint32_t spectatorSequenceBase;

    uint8_t spectatorClientPublisherEnabled;
    uint8_t spectatorWantsNewestCheckpoint;
    uint8_t reservedV22[2];
    uint32_t spectatorClientPublisherGeneration;
    uint64_t spectatorClientPublisherNonce;

    qinput::CompiledModeControlsV1 modeControls;
};

inline const char* validateIpcCompatibilityIdentity(const IpcConfig& cfg) {
    if (cfg.compatibilityId == 0u)
        return "IPC compatibility bundle marker is missing";
    if (!qnetplay::compat::isValidCompatibilityIdentity(
            cfg.compatibilityIdentity)) {
        return "IPC compatibility identity is incomplete or invalid";
    }
    return nullptr;
}

inline const char* validateIpcConfigurationProvenance(const IpcConfig& cfg) {
    return qnetplay::config_provenance::validateCommittedWire(
        cfg.configurationProvenance);
}

inline uint64_t hashOfflineExtras(const IpcConfig& cfg) {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const void* data, size_t length) {
        const unsigned char* bytes =
            static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < length; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    mix(cfg.offlineExtraPresent, sizeof(cfg.offlineExtraPresent));
    for (int i = 0; i < 3; ++i)
        mix(&cfg.offlineExtraConfigs[i],
            sizeof(cfg.offlineExtraConfigs[i]));
    return hash;
}

inline const char* validateTrainingLaunch(const IpcConfig& cfg) {
    if (cfg.trainingEnabled > 1)
        return "trainingEnabled is not boolean";
    if (cfg.trainingEnabled == 0)
        return nullptr;
    if (cfg.mode != MODE_OBSERVE)
        return "training requires offline Observe mode";
    if (cfg.isHost != 1 || cfg.localPlayer != 1)
        return "training requires local player-one authority";
    if (cfg.directBoot != 1 || cfg.vsRow != kVsRow1v1)
        return "training requires direct-boot row 1 (1P vs 2P)";
    if (cfg.probeMode != 0)
        return "training cannot share a probe driver";
    if (cfg.hasSocket || cfg.hasSpectatorSocket || cfg.serveSpectators)
        return "training cannot own network or spectator sockets";
    for (uint8_t present : cfg.offlineExtraPresent)
        if (present != 0)
            return "training accepts only the local P1 input profile";
    return nullptr;
}

inline const char* validateSessionDir(const IpcConfig& cfg) {
    if (cfg.sessionDirBytes >= kSessionDirBytes)
        return "sessionDir length exceeds the transfer field";
    if (cfg.sessionDir[cfg.sessionDirBytes] != '\0')
        return "sessionDir is not terminated at its declared length";
    for (uint16_t i = 0; i < cfg.sessionDirBytes; ++i)
        if (cfg.sessionDir[i] == '\0')
            return "sessionDir contains an embedded NUL";
    return nullptr;
}

inline const char* validateReplayLaunch(const IpcConfig& cfg) {
    if (cfg.mode != MODE_REPLAY) {
        bool sourceDigestPresent = false;
        for (uint8_t byte : cfg.replayValidatedSourceRangeDigest)
            sourceDigestPresent = sourceDigestPresent || byte != 0u;
        if (cfg.replayChannelGeneration != 0
            || cfg.replayBrokerNonce != 0
            || cfg.replayManifestDigest != 0
            || cfg.replayIpcSchemaVersion != 0
            || cfg.replayFeatureBits != 0
            || cfg.replayMatchOrdinal != 0
            || cfg.replaySpeedPermille != 0
            || cfg.replayGameGeneration != 0
            || cfg.replayExpectedFrameCount != 0
            || cfg.replayExpectedEventCount != 0
            || cfg.replayGameUuidLow != 0
            || cfg.replayGameUuidHigh != 0
            || cfg.replayValidatedSourceByteCount != 0
            || sourceDigestPresent
            || cfg.replayEndpointKind != 0
            || cfg.replayHeadless != 0
            || cfg.replayPresentationFlags != 0)
            return "replay broker fields require MODE_REPLAY";
        return nullptr;
    }
    if (cfg.isHost != 0 || cfg.localPlayer != 0)
        return "replay has no player identity";
    if (cfg.hasSocket || cfg.hasSpectatorSocket || cfg.serveSpectators)
        return "replay cannot own a socket or serve spectators";
    if (cfg.cookieLow || cfg.cookieHigh
        || cfg.spectatorCookieLow || cfg.spectatorCookieHigh)
        return "replay carries no session cookie";
    if (cfg.peerAddrBE != 0 || cfg.peerPortBE != 0)
        return "replay has no peer endpoint";
    if (cfg.trainingEnabled != 0)
        return "replay cannot also be a training launch";
    if (cfg.recordEnabled != 0)
        return "replay never records another replay";
    if (cfg.probeMode != 0)
        return "replay cannot share a probe driver";
    if (cfg.replayChannelGeneration == 0 || cfg.replayBrokerNonce == 0)
        return "replay requires a broker channel generation and nonce";
    if (cfg.replayManifestDigest != 0 || cfg.replayMatchOrdinal != 0)
        return "Q99R-v2 playback cannot carry legacy set/manifest fields";
    if (cfg.replayIpcSchemaVersion != kReplayPlaybackIpcSchemaVersion)
        return "replay requires the exact playback wire-v3 feature contract";
    if ((cfg.replayFeatureBits & kReplayPlaybackRequiredFeatureBits)
            != kReplayPlaybackRequiredFeatureBits
        || (cfg.replayFeatureBits
            & ~static_cast<uint16_t>(kReplayPlaybackRequiredFeatureBits
                                     | kReplayPlaybackOptionalFeatureBits))
               != 0u)
        return "replay requires the exact playback wire-v3 feature contract";
    if (cfg.replaySpeedPermille != 250u
        && cfg.replaySpeedPermille != 500u
        && cfg.replaySpeedPermille != 1000u
        && cfg.replaySpeedPermille != 2000u
        && cfg.replaySpeedPermille != 4000u
        && cfg.replaySpeedPermille != 8000u)
        return "replay presentation speed is unsupported";
    if (cfg.replayGameGeneration == 0
        || (cfg.replayGameUuidLow == 0
            && cfg.replayGameUuidHigh == 0))
        return "replay requires an exact game generation and UUID";
    if (cfg.replayExpectedFrameCount == 0
        || cfg.replayExpectedFrameCount > kReplayMaxPrebufferFrames)
        return "replay frame count exceeds the full-prebuffer contract";
    if (cfg.replayExpectedEventCount
        > cfg.replayExpectedFrameCount
            * (2u * kReplayMaxEventsPerTick))
        return "replay event count exceeds the bounded timeline contract";
    if (cfg.replayValidatedSourceByteCount == 0
        || cfg.replayValidatedSourceByteCount > kReplayMaxSourceBytes)
        return "replay validated source range is invalid";
    bool sourceDigestPresent = false;
    for (uint8_t byte : cfg.replayValidatedSourceRangeDigest)
        sourceDigestPresent = sourceDigestPresent || byte != 0u;
    if (!sourceDigestPresent)
        return "replay validated source range has no digest";
    if (cfg.replayEndpointKind != kReplayEndpointRecordedGameEnd
        && cfg.replayEndpointKind
            != kReplayEndpointValidatedPrefix)
        return "replay endpoint classification is invalid";
    if (cfg.replayHeadless > 1
        || (cfg.replayPresentationFlags & ~kReplayPresentationFlagsMask) != 0)
        return "replay v18 flags or reserved bytes are invalid";
    bool contentRootPresent = false;
    for (uint8_t byte : cfg.replayContentManifestRoot)
        contentRootPresent = contentRootPresent || byte != 0u;
    if (!contentRootPresent)
        return "replay requires a verified content manifest root";
    for (uint8_t present : cfg.offlineExtraPresent)
        if (present != 0)
            return "replay accepts no offline extra input profiles";
    return nullptr;
}

inline const char* validateRecorderLaunch(const IpcConfig& cfg) {
    if (cfg.recordEnabled > 1)
        return "recordEnabled is not boolean";
    if (cfg.recordEnabled == 0) {
        if (cfg.recordChannelGeneration != 0 || cfg.recordChannelNonce != 0)
            return "disabled recorder carries channel credentials";
        if (cfg.mode != MODE_REPLAY) {
            for (uint8_t byte : cfg.replayContentManifestRoot)
                if (byte != 0u)
                    return "non-replay launch without a recorder carries a "
                           "content manifest root";
        }
        return nullptr;
    }
    if (cfg.mode == MODE_REPLAY || cfg.mode == MODE_SELFTEST)
        return "this run mode cannot record";
    if (cfg.trainingEnabled != 0)
        return "training is never recorded";
    if (cfg.probeMode != 0)
        return "probe sessions are never recorded";
    if (cfg.recordChannelGeneration == 0)
        return "recorder requires a nonzero channel generation";
    if (cfg.recordChannelNonce == 0)
        return "recorder requires a nonzero channel nonce";
    bool contentRootPresent = false;
    for (uint8_t byte : cfg.replayContentManifestRoot)
        contentRootPresent = contentRootPresent || byte != 0u;
    if (!contentRootPresent)
        return "recorder requires a verified content manifest root";
    return nullptr;
}

inline const char* validateSpectatorClientPrivatePublisher(
    const IpcConfig& cfg);

inline const char* validateSpectatorService(const IpcConfig& cfg) {
    if (cfg.spectatorServiceMode > SPECTATOR_SERVICE_SIDECAR)
        return "unknown spectator service mode";
    if (const char* why = validateSpectatorClientPrivatePublisher(cfg))
        return why;
    if (cfg.spectatorServiceMode == SPECTATOR_SERVICE_OFF) {
        if (cfg.serveSpectators || cfg.hasSpectatorSocket)
            return "spectator service is off but a source was configured";
        return nullptr;
    }
    if (!cfg.serveSpectators)
        return "spectator service mode requires serveSpectators";
    if (cfg.spectatorClientPublisherEnabled == 1u)
        return nullptr;
    if (cfg.mode != MODE_NETPLAY || cfg.isHost != 1)
        return "only a netplay host may serve spectators";
    return nullptr;
}

inline const char* validateSpectatorSocketHandoff(const IpcConfig& cfg) {
    if (cfg.hasSpectatorSocket
        && (!cfg.serveSpectators || cfg.mode != MODE_NETPLAY
            || cfg.isHost != 1)) {
        return "spectator socket requires a serving netplay host";
    }
    if (!cfg.serveSpectators) return nullptr;
    if (cfg.spectatorCookieLow == 0 && cfg.spectatorCookieHigh == 0)
        return "spectator source requires a cookie";
    if (cfg.cookieLow == cfg.spectatorCookieLow
        && cfg.cookieHigh == cfg.spectatorCookieHigh) {
        return "spectator source cookie must differ from player cookie";
    }
    if (cfg.spectatorServiceMode == SPECTATOR_SERVICE_INPROCESS
        && !cfg.hasSpectatorSocket) {
        return "in-process spectator source requires a dedicated socket";
    }
    if (cfg.spectatorServiceMode == SPECTATOR_SERVICE_SIDECAR
        && cfg.hasSpectatorSocket) {
        return "sidecar spectator source must keep its socket in the caster";
    }
    return nullptr;
}

inline const char* validateSpectatorRelayViewer(const IpcConfig& cfg) {
    if (cfg.spectatorRelayActive > 1)
        return "spectatorRelayActive is not boolean";
    const bool hasRoom =
        cfg.spectatorRelayRoomLow != 0
        || cfg.spectatorRelayRoomHigh != 0;
    const bool hasToken =
        cfg.spectatorRelayTokenLow != 0
        || cfg.spectatorRelayTokenHigh != 0;
    const bool hasGeneration =
        cfg.spectatorRelaySourceGeneration != 0;
    const bool hasReserved =
        cfg.reservedV15[0] != 0 || cfg.reservedV15[1] != 0
        || cfg.reservedV15[2] != 0;
    if (cfg.spectatorRelayActive == 0) {
        if (hasRoom || hasToken || hasGeneration || hasReserved)
            return "disabled spectator relay carries binding state";
        return nullptr;
    }
    if (hasReserved)
        return "spectator relay reserved bytes are nonzero";
    if (cfg.mode != MODE_SPECTATOR || cfg.isHost != 0
        || cfg.localPlayer != 0) {
        return "spectator relay requires a receive-only spectator launch";
    }
    if (!cfg.hasSocket || !cfg.peerAddrBE || !cfg.peerPortBE)
        return "spectator relay requires the proved UDP socket and endpoint";
    if (!hasRoom || !hasToken || !hasGeneration
        || cfg.spectatorViewerId == 0
        || cfg.sessionId == 0
        || (cfg.spectatorCookieLow == 0
            && cfg.spectatorCookieHigh == 0)) {
        return "spectator relay binding is incomplete";
    }
    return nullptr;
}

inline const char* validateSpectatorTopologyClient(const IpcConfig& cfg) {
    if (cfg.spectatorTopologyEnabled > 1u)
        return "spectatorTopologyEnabled is not boolean";
    const bool hasFields =
        cfg.spectatorTopologyRouteKind != 0u
        || cfg.spectatorTopologyHostControlPort != 0u
        || cfg.spectatorTopologyHostControlIpv4BE != 0u
        || cfg.spectatorTopologyQspRouteGeneration != 0u
        || cfg.spectatorTopologyGrantSourceGeneration != 0u;
    if (cfg.spectatorTopologyEnabled == 0u) {
        if (hasFields)
            return "disabled spectator topology carries bootstrap state";
        return nullptr;
    }
    if (cfg.mode != MODE_SPECTATOR || cfg.isHost != 0u
        || cfg.localPlayer != 0u) {
        return "spectator topology requires a receive-only spectator launch";
    }
    if (!cfg.hasSocket || cfg.peerAddrBE == 0u || cfg.peerPortBE == 0u)
        return "spectator topology requires the proved QSP socket and endpoint";
    const bool relayRouteKind =
        cfg.spectatorTopologyRouteKind == kSpectatorTopologyRouteRelay;
    const bool directOrPunchedRouteKind =
        cfg.spectatorTopologyRouteKind == kSpectatorTopologyRouteDirect
        || cfg.spectatorTopologyRouteKind == kSpectatorTopologyRoutePunched;
    if (!relayRouteKind && !directOrPunchedRouteKind)
        return "spectator topology requires a direct, punched, or relay QSP route";
    if (relayRouteKind) {
        if (cfg.spectatorRelayActive != 1u) {
            return "relayed spectator topology requires the proved relay "
                   "binding";
        }
        if (const char* relayReason = validateSpectatorRelayViewer(cfg))
            return relayReason;
    } else if (cfg.spectatorRelayActive != 0u) {
        return "spectator topology does not support traffic-relay E0";
    }
    if (cfg.spectatorTopologyHostControlIpv4BE == 0u
        || cfg.spectatorTopologyHostControlPort == 0u) {
        return "spectator topology requires the host control endpoint";
    }
    if (cfg.spectatorTopologyQspRouteGeneration == 0u
        || cfg.spectatorGeneration == 0u
        || cfg.spectatorTopologyQspRouteGeneration
            != static_cast<uint64_t>(cfg.spectatorGeneration)) {
        return "spectator topology QSP generation does not match the handoff";
    }
    if (cfg.spectatorTopologyGrantSourceGeneration == 0u
        || cfg.spectatorGrantId == 0u
        || cfg.spectatorGrantExpiryUnix == 0u
        || (cfg.spectatorGrantMacLow == 0u
            && cfg.spectatorGrantMacHigh == 0u)
        || cfg.spectatorViewerId == 0u || cfg.sessionId == 0u) {
        return "spectator topology requires an exact per-viewer E0 grant";
    }
    return nullptr;
}

inline const char* validateSpectatorClientPrivatePublisher(
    const IpcConfig& cfg) {
    if (cfg.spectatorClientPublisherEnabled > 1u)
        return "spectatorClientPublisherEnabled is not boolean";
    const bool hasReserved = cfg.reservedV22[0] != 0u
        || cfg.reservedV22[1] != 0u;
    const bool hasChannel = cfg.spectatorClientPublisherGeneration != 0u
        || cfg.spectatorClientPublisherNonce != 0u;
    if (cfg.spectatorClientPublisherEnabled == 0u) {
        if (hasReserved || hasChannel)
            return "disabled client spectator publisher carries channel state";
        return nullptr;
    }
    if (hasReserved)
        return "client spectator publisher reserved bytes are nonzero";
    if (cfg.spectatorWantsNewestCheckpoint != 0u)
        return "client spectator publisher must not request a viewer checkpoint";
    if (cfg.mode != MODE_NETPLAY || cfg.isHost != 0u
        || cfg.localPlayer < 1u || cfg.localPlayer > 2u) {
        return "client spectator publisher requires a client netplay launch";
    }
    if (cfg.serveSpectators != 1u
        || cfg.spectatorServiceMode != SPECTATOR_SERVICE_SIDECAR) {
        return "client spectator publisher requires sidecar spectator serving";
    }
    if (cfg.hasSpectatorSocket != 0u)
        return "client spectator publisher must not receive a spectator socket";
    if (cfg.spectatorMaxViewers != 0u
        || cfg.reservedV13b != 0u
        || cfg.spectatorGrantId != 0u
        || cfg.spectatorGrantExpiryUnix != 0u
        || cfg.spectatorGrantMacLow != 0u
        || cfg.spectatorGrantMacHigh != 0u) {
        return "client spectator publisher must not carry public admission state";
    }
    if ((cfg.spectatorCookieLow == 0u && cfg.spectatorCookieHigh == 0u)
        || (cfg.spectatorCookieLow == cfg.cookieLow
            && cfg.spectatorCookieHigh == cfg.cookieHigh)) {
        return "client spectator publisher requires a distinct source cookie";
    }
    if (cfg.spectatorViewerId != 0u || cfg.spectatorGameId != 0u
        || cfg.spectatorGeneration != 0u
        || cfg.spectatorResumeRecord != 0u
        || cfg.spectatorResumeDigest != 0u) {
        return "client spectator publisher must not carry viewer handoff state";
    }
    if (cfg.spectatorRelayActive != 0u
        || cfg.reservedV15[0] != 0u || cfg.reservedV15[1] != 0u
        || cfg.reservedV15[2] != 0u || cfg.spectatorRelayRoomLow != 0u
        || cfg.spectatorRelayRoomHigh != 0u || cfg.spectatorRelayTokenLow != 0u
        || cfg.spectatorRelayTokenHigh != 0u
        || cfg.spectatorRelaySourceGeneration != 0u) {
        return "client spectator publisher must not carry relay state";
    }
    if (cfg.spectatorTopologyEnabled != 0u
        || cfg.spectatorTopologyRouteKind != 0u
        || cfg.spectatorTopologyHostControlPort != 0u
        || cfg.spectatorTopologyHostControlIpv4BE != 0u
        || cfg.spectatorTopologyQspRouteGeneration != 0u
        || cfg.spectatorTopologyGrantSourceGeneration != 0u) {
        return "client spectator publisher must not bootstrap topology";
    }
    if (cfg.hasSocket != 1u || cfg.peerAddrBE == 0u || cfg.peerPortBE == 0u
        || cfg.sessionId == 0u) {
        return "client spectator publisher requires the player netplay session";
    }
    if (cfg.compatibilityId != kRuntimeCompatibilityId
        || (cfg.cookieLow == 0u && cfg.cookieHigh == 0u)) {
        return "client spectator publisher requires the committed player identity";
    }
    if (cfg.spectatorClientPublisherGeneration == 0u
        || cfg.spectatorClientPublisherNonce == 0u) {
        return "client spectator publisher requires a channel generation and nonce";
    }
    return nullptr;
}
#pragma pack(pop)

constexpr uint32_t kSpectatorClientPublisherReceiptMagic = 0x52504351u;
// 'QCPR' in little-endian memory.
constexpr uint16_t kSpectatorClientPublisherReceiptSchemaVersion = 1u;

enum class SpectatorClientPublisherReceiptKind : uint16_t {
    RuntimeApplied = 1u,
};

#pragma pack(push, 1)
struct SpectatorClientPublisherRuntimeAppliedReceipt {
    uint32_t magic = kSpectatorClientPublisherReceiptMagic;
    uint16_t schemaVersion = kSpectatorClientPublisherReceiptSchemaVersion;
    uint16_t kind = static_cast<uint16_t>(
        SpectatorClientPublisherReceiptKind::RuntimeApplied);
    uint32_t byteSize = 0u;
    uint32_t processId = 0u;
    uint32_t sessionId = 0u;
    uint32_t channelGeneration = 0u;
    uint64_t channelNonce = 0u;
    uint64_t compatibilityId = 0u;
    uint8_t mode = MODE_OFF;
    uint8_t isHost = 1u;
    uint8_t localPlayer = 0u;
    uint8_t runtimeStage = static_cast<uint8_t>(
        qnetplay::config_provenance::Stage::Unknown);
    uint8_t delay = 0u;
    uint8_t rollback = 0u;
    uint8_t remoteDelay = 0u;
    uint8_t remoteRollback = 0u;
    qnetplay::config_provenance::ConfigurationProvenanceWire
        configurationProvenance{};
    uint8_t reserved[6]{};
};
#pragma pack(pop)

static_assert(sizeof(SpectatorClientPublisherRuntimeAppliedReceipt) == 64u,
              "client publisher RuntimeApplied receipt wire size changed");
constexpr size_t kSpectatorClientPublisherRuntimeAppliedReceiptBytes =
    sizeof(SpectatorClientPublisherRuntimeAppliedReceipt);

inline bool validSpectatorClientPublisherRuntimeAppliedReceiptShape(
    const SpectatorClientPublisherRuntimeAppliedReceipt& receipt) {
    if (receipt.magic != kSpectatorClientPublisherReceiptMagic
        || receipt.schemaVersion
            != kSpectatorClientPublisherReceiptSchemaVersion
        || receipt.kind != static_cast<uint16_t>(
            SpectatorClientPublisherReceiptKind::RuntimeApplied)
        || receipt.byteSize
            != kSpectatorClientPublisherRuntimeAppliedReceiptBytes) {
        return false;
    }
    if (receipt.processId == 0u || receipt.sessionId == 0u
        || receipt.channelGeneration == 0u || receipt.channelNonce == 0u
        || receipt.compatibilityId != kRuntimeCompatibilityId) {
        return false;
    }
    if (receipt.mode != MODE_NETPLAY || receipt.isHost != 0u
        || receipt.localPlayer < 1u || receipt.localPlayer > 2u
        || receipt.runtimeStage != static_cast<uint8_t>(
            qnetplay::config_provenance::Stage::RuntimeApplied)) {
        return false;
    }
    for (uint8_t byte : receipt.reserved)
        if (byte != 0u) return false;
    if (qnetplay::config_provenance::validateCommittedWire(
            receipt.configurationProvenance) != nullptr) {
        return false;
    }
    return true;
}

inline bool buildSpectatorClientPublisherRuntimeAppliedReceipt(
    const IpcConfig& cfg, uint32_t processId,
    SpectatorClientPublisherRuntimeAppliedReceipt& out) {
    out = SpectatorClientPublisherRuntimeAppliedReceipt{};
    if (cfg.spectatorClientPublisherEnabled != 1u || processId == 0u
        || validateSpectatorClientPrivatePublisher(cfg) != nullptr) {
        return false;
    }
    out.byteSize = static_cast<uint32_t>(
        kSpectatorClientPublisherRuntimeAppliedReceiptBytes);
    out.processId = processId;
    out.sessionId = cfg.sessionId;
    out.channelGeneration = cfg.spectatorClientPublisherGeneration;
    out.channelNonce = cfg.spectatorClientPublisherNonce;
    out.compatibilityId = cfg.compatibilityId;
    out.mode = cfg.mode;
    out.isHost = cfg.isHost;
    out.localPlayer = cfg.localPlayer;
    out.runtimeStage = static_cast<uint8_t>(
        qnetplay::config_provenance::Stage::RuntimeApplied);
    out.delay = cfg.delay;
    out.rollback = cfg.rollback;
    out.remoteDelay = cfg.remoteDelay;
    out.remoteRollback = cfg.remoteRollback;
    out.configurationProvenance = cfg.configurationProvenance;
    return validSpectatorClientPublisherRuntimeAppliedReceiptShape(out);
}

inline bool encodeSpectatorClientPublisherRuntimeAppliedReceipt(
    const SpectatorClientPublisherRuntimeAppliedReceipt& receipt,
    uint8_t* out, size_t capacity, size_t& written) {
    written = 0u;
    if (out == nullptr || capacity < sizeof(receipt)
        || !validSpectatorClientPublisherRuntimeAppliedReceiptShape(receipt)) {
        return false;
    }
    std::memcpy(out, &receipt, sizeof(receipt));
    written = sizeof(receipt);
    return true;
}

inline bool decodeSpectatorClientPublisherRuntimeAppliedReceipt(
    const uint8_t* bytes, size_t byteCount,
    SpectatorClientPublisherRuntimeAppliedReceipt& out) {
    out = SpectatorClientPublisherRuntimeAppliedReceipt{};
    if (bytes == nullptr || byteCount != sizeof(out)) return false;
    std::memcpy(&out, bytes, sizeof(out));
    if (!validSpectatorClientPublisherRuntimeAppliedReceiptShape(out)) {
        out = SpectatorClientPublisherRuntimeAppliedReceipt{};
        return false;
    }
    return true;
}

inline bool spectatorClientPublisherRuntimeAppliedReceiptMatches(
    const SpectatorClientPublisherRuntimeAppliedReceipt& receipt,
    const IpcConfig& cfg, uint32_t expectedProcessId) {
    if (!validSpectatorClientPublisherRuntimeAppliedReceiptShape(receipt)
        || expectedProcessId == 0u
        || cfg.spectatorClientPublisherEnabled != 1u
        || validateSpectatorClientPrivatePublisher(cfg) != nullptr) {
        return false;
    }
    return receipt.processId == expectedProcessId
        && receipt.sessionId == cfg.sessionId
        && receipt.channelGeneration == cfg.spectatorClientPublisherGeneration
        && receipt.channelNonce == cfg.spectatorClientPublisherNonce
        && receipt.compatibilityId == cfg.compatibilityId
        && receipt.mode == cfg.mode
        && receipt.isHost == cfg.isHost
        && receipt.localPlayer == cfg.localPlayer
        && receipt.delay == cfg.delay
        && receipt.rollback == cfg.rollback
        && receipt.remoteDelay == cfg.remoteDelay
        && receipt.remoteRollback == cfg.remoteRollback
        && std::memcmp(&receipt.configurationProvenance,
                       &cfg.configurationProvenance,
                       sizeof(receipt.configurationProvenance)) == 0;
}

inline void ipcReplayBrokerPipeName(wchar_t* out, size_t cap, uint32_t gamePid,
                                    uint32_t generation) {
    _snwprintf_s(out, cap, _TRUNCATE, L"\\\\.\\pipe\\qoh99np_replay_%u_%u",
                 gamePid, generation);
}

inline void ipcRecorderPipeName(wchar_t* out, size_t cap, uint32_t gamePid,
                                uint32_t generation) {
    _snwprintf_s(out, cap, _TRUNCATE, L"\\\\.\\pipe\\qoh99np_record_%u_%u",
                 gamePid, generation);
}

inline void ipcSpectatorSidecarPipeName(wchar_t* out, size_t cap,
                                        uint32_t gamePid) {
    _snwprintf_s(out, cap, _TRUNCATE, L"\\\\.\\pipe\\qoh99np_spectator_%u",
                 gamePid);
}

inline void ipcSpectatorClientPublisherPipeName(wchar_t* out, size_t cap,
                                                uint32_t gamePid,
                                                uint32_t generation) {
    _snwprintf_s(out, cap, _TRUNCATE,
                 L"\\\\.\\pipe\\qoh99np_spectator_clientpub_%u_%u",
                 gamePid, generation);
}

inline void ipcPipeName(wchar_t* out, size_t cap, uint32_t gamePid) {
    _snwprintf_s(out, cap, _TRUNCATE, L"\\\\.\\pipe\\qoh99np_%u", gamePid);
}

inline void ipcReadyEventName(wchar_t* out, size_t cap, uint32_t gamePid) {
    _snwprintf_s(out, cap, _TRUNCATE, L"Local\\qoh99np_ready_%u", gamePid);
}

inline void ipcFailedEventName(wchar_t* out, size_t cap, uint32_t gamePid) {
    _snwprintf_s(out, cap, _TRUNCATE, L"Local\\qoh99np_failed_%u", gamePid);
}

inline void ipcInputReadyEventName(wchar_t* out, size_t cap,
                                   uint32_t gamePid) {
    _snwprintf_s(out, cap, _TRUNCATE, L"Local\\qoh99np_inputready_%u",
                 gamePid);
}
inline void ipcInputFailedEventName(wchar_t* out, size_t cap,
                                    uint32_t gamePid) {
    _snwprintf_s(out, cap, _TRUNCATE, L"Local\\qoh99np_inputfailed_%u",
                 gamePid);
}

}
