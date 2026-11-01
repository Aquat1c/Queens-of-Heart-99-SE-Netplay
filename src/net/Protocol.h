// Fixed-layout POD messages, one whole message per UDP datagram; the header carries type + crc32 of the body.
#pragma once
#include <cstdint>
#include <cstring>

#include "../compat/CompatibilityIdentity.h"
#include "../netplay/ConfigurationProvenance.h"
#include "RouteCandidates.h"

namespace qnet {

constexpr uint32_t kMagic           = 0x4E393951; // 'Q99N'
constexpr uint32_t kProtocolVersion = 21;

// Every inputs packet re-sends the last 30 frames, so the unreliable input stream needs no retransmission.
constexpr size_t kNumInputs = 30;

enum MsgType : uint8_t {
    MSG_NONE            = 0,
    MSG_VERSION_CONFIG  = 1,
    MSG_INITIAL_CONFIG  = 2,
    MSG_NETPLAY_CONFIG  = 3,
    MSG_CONFIRM_CONFIG  = 4,
    MSG_ERROR_TEXT      = 5,
    MSG_PING_STATS      = 6,
    MSG_SETTINGS_PENDING= 7,
    MSG_ROUTE_COMMIT    = 8,
    MSG_RUNTIME_READY   = 9,
    MSG_RUNTIME_START   = 10,
    MSG_ROUTE_CANDIDATES= 11,
    MSG_CLIENT_INTENT   = 12,
    MSG_SPECTATE_INFO   = 13,
    MSG_SPECTATE_TOPOLOGY_INFO = 14,
    MSG_SPECTATE_TOPOLOGY_PROOF = 15,
    MSG_PING            = 16,
    MSG_PONG            = 17,
    MSG_PLAYER_INPUTS   = 18,
    MSG_SPECTATE_TOPOLOGY_PROOF_V2 = 19,
    MSG_SPECTATE_TOPOLOGY_INFO_V2 = 20,
    MSG_SPECTATE_TOPOLOGY_CHALLENGE_V2 = 21,
    MSG_CLIENT_ROOT_OFFER = 22,
    MSG_CLIENT_ROOT_CHALLENGE = 23,
    MSG_CLIENT_ROOT_CLAIM = 24,
    MSG_CLIENT_ROOT_ENROLLMENT = 25,
    MSG_TUNING_PROPOSAL = 26,
    MSG_REL_DATA        = 32,
    MSG_REL_ACK         = 33,
    MSG_KEEPALIVE       = 34,
    MSG_LINK_HELLO      = 35,
    MSG_LINK_HELLO_ACK  = 36,
    MSG_RNG_STATE       = 48,
    MSG_MENU_INDEX      = 49,
    MSG_SYNC_HASH       = 50,
    MSG_CHANGE_CONFIG   = 51,
};

#pragma pack(push, 1)

struct MsgHeader {
    uint32_t magic;
    uint8_t  type;
    uint8_t  flags;
    uint16_t bodyLen;
    uint32_t bodyCrc;
};

struct VersionConfig { uint32_t protocolVersion; };

constexpr uint32_t kRuntimeArtifactIdentitySchemaVersion = 1u;
struct RuntimeArtifactIdentityWire {
    uint32_t schemaVersion;
    uint8_t casterSha256[32];
    uint8_t netplayDllSha256[32];
    uint8_t ddrawSha256[32];
};

enum class RuntimeArtifactMismatch : uint8_t {
    None = 0,
    MissingOrInvalid,
    Caster,
    NetplayDll,
    Ddraw,
    Multiple,
};

enum ClientIntentKind : uint8_t {
    INTENT_JOIN = 0,
    INTENT_SPECTATE = 1,
    INTENT_SPECTATE_RELEASE = 2,
    INTENT_CLIENT_ROOT = 3,
    INTENT_STATUS = 4,
};
enum SpectateRequestRoute : uint8_t {
    SPECTATE_ROUTE_NONE = 0,
    SPECTATE_ROUTE_DIRECT = 1,
    SPECTATE_ROUTE_PUNCHED = 2,
    SPECTATE_ROUTE_RELAY = 3,
};

constexpr uint16_t kSpectateCapabilityRelayTopology = 1u << 0;
constexpr uint16_t kSpectateCapabilityReleasedTopologyV2 = 1u << 1;
constexpr uint16_t kSpectateCapabilityReleasedTopologyV2Mask =
    kSpectateCapabilityRelayTopology
    | kSpectateCapabilityReleasedTopologyV2;

struct ClientIntent {
    uint8_t  intent;
    uint8_t  spectatorRoute;
    uint16_t spectatorCapabilities;
    uint32_t sessionId;
    uint64_t keyLow;
    uint64_t keyHigh;
    uint64_t viewerId;
    uint32_t resumeRecord;
    // UDP port of the viewer's spectator socket, zero for non-spectate intents or when unbound. The remaining two bytes are reserved and must be zero.
    uint16_t spectatorDataPort;
    uint16_t reserved3;
    uint64_t resumeDigest;
};
enum SpectateState : uint8_t {
    SPECTATE_NO_MATCH = 0,
    SPECTATE_LIVE = 1,
};

struct InitialConfig {
    char     name[32];
    uint32_t nameCodePage;
    uint8_t  isHost;
    uint16_t dataPort;
    uint8_t  roundsToWin;
    uint8_t  reserved[3];
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity;
    RuntimeArtifactIdentityWire runtimeArtifacts;
};

struct PeerTuning {
    uint8_t delay;
    uint8_t rollback;
    uint8_t maxDelay;
    uint8_t maxRollback;
};

struct NetplayConfig {
    PeerTuning hostTuning;
    uint8_t  hostPlayer;
    uint8_t  pad[3];
    uint32_t sessionId;
    uint64_t compatibilityId;
    uint64_t playerCookieLow;
    uint64_t playerCookieHigh;
    uint64_t spectatorCookieLow;
    uint64_t spectatorCookieHigh;
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity;
};

enum RouteKind : uint8_t {
    ROUTE_DIRECT = 1,
    ROUTE_RELAY  = 2,
};

struct RouteCandidateWire {
    uint8_t  kind;                 // CandidateKind (1 private, 2 public, 3 relay)
    uint8_t  routeId;
    uint16_t portBE;
    uint32_t ipv4BE;
};

struct RouteCandidates {
    uint32_t sessionId;
    uint32_t observedPublicIpv4BE;
    uint8_t  count;
    uint8_t  reserved[3];
    RouteCandidateWire entries[kMaxRouteCandidates];
};

enum SpectateAdmissionState : uint8_t {
    SPECTATE_ADMISSION_NONE = 0,
    SPECTATE_ADMISSION_GRANTED = 1,
    SPECTATE_ADMISSION_REFUSED = 2,
    SPECTATE_ADMISSION_PENDING = 3,
    SPECTATE_ADMISSION_CAPACITY = 4,
    SPECTATE_ADMISSION_REQUIRED = 5,
    SPECTATE_ADMISSION_UNAVAILABLE = 6,
    SPECTATE_ADMISSION_RELEASED = 7,
};

struct SpectatorGrantWire {
    uint64_t grantId;
    uint64_t viewerId;
    uint64_t sessionId;
    uint64_t expiryUnix;
    uint32_t sourceGeneration;
    uint32_t resumeRecord;
    uint64_t resumeDigest;
    uint8_t  route;
    uint8_t  reserved[7];
    uint64_t macLow;
    uint64_t macHigh;
};

enum LivePresenceState : uint8_t {
    LIVE_PRESENCE_HOSTING = 1,
    LIVE_PRESENCE_CHARACTER_SELECT = 2,
    LIVE_PRESENCE_MATCH = 3,
    LIVE_PRESENCE_RETAINED = 4,
};

constexpr uint8_t kLivePresenceRosterValid = 1u << 0;
constexpr uint8_t kLivePresenceRoundScoreValid = 1u << 1;
constexpr uint8_t kLivePresenceSetScoreValid = 1u << 2;
constexpr uint16_t kLivePresenceSchemaVersion = 1u;

struct LivePresence {
    uint16_t schemaVersion;
    uint8_t  state;
    uint8_t  flags;
    uint32_t sessionId;
    uint32_t confirmedTimelineFrame;
    uint16_t viewerCount;
    uint16_t viewerCapacity;
    uint8_t  roundsToWin;
    uint8_t  activeFighterMask;   // bits 0..3; meaningful with RosterValid
    uint8_t  characterIds[4];     // native IDs; inactive slots are 0xFF
    uint8_t  paletteIndices[4];
    uint8_t  roundWins[2];
    uint16_t matchWins[2];
    char     playerNames[2][32];
};

struct SpectateInfo {
    uint8_t  state;
    uint16_t spectatorPort;
    uint32_t sessionId;
    uint64_t keyLow;
    uint64_t keyHigh;
    RouteCandidates sourceRoutes;
    uint8_t  admissionState;
    uint8_t  admissionPolicy;
    uint16_t viewerCount;
    uint16_t viewerCapacity;
    uint16_t reserved;
    uint64_t simulationContract;
    uint64_t relayRoomLow;
    uint64_t relayRoomHigh;
    uint64_t relayViewerAccessLow;
    uint64_t relayViewerAccessHigh;
    SpectatorGrantWire grant;
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity;
    LivePresence presence;
};

enum SpectateTopologyState : uint8_t {
    SPECTATE_TOPOLOGY_UNAVAILABLE = 0,
    SPECTATE_TOPOLOGY_GRANTED = 1,
};

struct SpectateTopologyInfo {
    uint8_t state;
    uint8_t rootQspRouteKind;
    uint8_t rootQspRouteProved;
    uint8_t rootQsrRouteKind;
    uint8_t rootQsrRouteProved;
    uint8_t reserved[3];
    uint32_t sessionId;
    uint64_t authorityGeneration;
    uint64_t rootNodeId;
    uint64_t childNodeId;
    uint64_t expiryMs;
    uint64_t bindingGeneration;
    uint32_t rootQspIpv4BE;
    uint16_t rootQspPort;
    uint16_t reservedQsp;
    uint64_t rootQspRouteGeneration;
    uint32_t rootQsrIpv4BE;
    uint16_t rootQsrPort;
    uint16_t reservedQsr;
    uint64_t rootQsrRouteGeneration;
    uint8_t topologyCredential[32];
};

struct SpectateTopologyProof {
    uint32_t viewerGeneration;
    uint32_t reserved;
    SpectatorGrantWire grant;
};

constexpr uint16_t kSpectateTopologyControlSchemaVersion = 4u;
constexpr uint16_t kSpectateTopologyControlFeatureExactQsp = 1u << 0;
constexpr uint16_t kSpectateTopologyControlFeatureQtaTranscript = 1u << 1;
constexpr uint16_t kSpectateTopologyControlFeatureLinkChallenge = 1u << 2;
constexpr uint16_t kSpectateTopologyControlFeatureHostAssignmentSignature =
    1u << 3;
constexpr uint16_t kSpectateTopologyControlRequiredFeatures =
    kSpectateTopologyControlFeatureExactQsp
    | kSpectateTopologyControlFeatureQtaTranscript
    | kSpectateTopologyControlFeatureLinkChallenge
    | kSpectateTopologyControlFeatureHostAssignmentSignature;
constexpr size_t kSpectateTopologyAuthorityPublicKeyBytes = 72u;

struct SpectateTopologyEndpointBindingWire {
    uint32_t localIpv4BE;
    uint16_t localPort;
    uint8_t advertisedRouteKind;
    uint32_t advertisedIpv4BE;
    uint16_t advertisedPort;
    uint64_t advertisedRouteGeneration;
    uint8_t advertisedRouteProved;
    uint8_t localProvenance;
    uint8_t advertisedProvenance;
    uint64_t bindingGeneration;
};

struct SpectateTopologyCursorWire {
    uint32_t nextRecord;
    uint32_t nextEvent;
    uint64_t digest;
};

struct SpectateTopologyQtaLimitsWire {
    uint64_t maxTopologyNodes;
    uint8_t maxActiveDepth;
    uint64_t maxChildrenPerRoot;
    uint64_t maxChildrenPerRelay;
    uint64_t maxStandbyParents;
    uint64_t standbyLeaseMs;
};

struct SpectateTopologyChallengeV2 {
    uint16_t schemaVersion;
    uint16_t requiredFeatures;
    uint64_t sessionId;
    uint64_t viewerId;
    uint32_t viewerGeneration;
    uint32_t reserved;
    uint8_t peerChallenge[16];
};

struct SpectateTopologyProofV2 {
    uint16_t schemaVersion;
    uint16_t requiredFeatures;
    uint64_t requestNonce;
    uint8_t peerChallenge[16];
    uint64_t viewerId;
    uint32_t viewerGeneration;
    uint8_t qspRoute;
    uint8_t reservedRoute[3];
    uint64_t qspRouteGeneration;
    uint32_t qspIpv4BE;
    uint16_t qspPort;              // host byte order
    uint16_t reservedEndpoint;
    uint64_t sessionEpoch;
    uint64_t discoveryGeneration;
    uint64_t streamRootIdentity;
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity;
    SpectatorGrantWire grant;
    uint8_t requestTag[16];
};

struct SpectateTopologyInfoV2 {
    uint16_t schemaVersion;
    uint16_t requiredFeatures;
    uint8_t state;
    uint8_t qspRoute;
    uint8_t qspProvenance;
    uint8_t rootQspRouteKind;
    uint8_t rootQspRouteProved;
    uint8_t rootQsrRouteKind;
    uint8_t rootQsrRouteProved;
    uint8_t reservedState;
    uint64_t requestNonce;
    uint64_t sessionId;
    uint64_t sessionEpoch;
    uint64_t viewerId;
    uint32_t viewerGeneration;
    uint32_t confirmedCursor;
    uint64_t streamRootIdentity;
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity;
    uint64_t authorityGeneration;
    uint64_t sourceGeneration;
    uint64_t rootNodeId;
    uint64_t childNodeId;
    uint64_t expiryMs;
    uint64_t bindingGeneration;
    uint32_t qspIpv4BE;
    uint16_t qspPort;
    uint16_t reservedQsp;
    uint64_t qspRouteGeneration;
    SpectateTopologyEndpointBindingWire rootQsp;
    SpectateTopologyEndpointBindingWire rootQsr;
    SpectateTopologyCursorWire qtaCursor;
    SpectateTopologyQtaLimitsWire qtaLimits;
    uint32_t relayIpv4BE;
    uint16_t relayPort;
    uint16_t reservedRelay;
    uint8_t relayRoomId[16];
    uint64_t relayGrantId;
    uint64_t relayGrantExpiryUnix;
    uint32_t relayGrantAuthorityGeneration;
    uint32_t reservedRelayGrant;
    uint64_t relayGrantMacLow;
    uint64_t relayGrantMacHigh;
    uint8_t authorityAssignmentPublicKey[
        kSpectateTopologyAuthorityPublicKeyBytes];
    uint8_t topologyCredential[32];
    uint8_t responseTag[16];
};

constexpr size_t kClientRootTokenBytes = 32u;
constexpr size_t kClientRootCredentialBytes = 32u;
constexpr size_t kClientRootGrantBlobMax = 1024u;

struct ClientRootOffer {
    uint16_t schemaVersion;
    uint8_t  remotePlayerRole;
    uint8_t  reserved;
    uint32_t sessionId;
    uint64_t sessionEpoch;
    uint64_t offerExpiryMs;
    uint64_t hostSourceGeneration;
    uint8_t  claimToken[kClientRootTokenBytes];
};

struct ClientRootRouteWire {
    uint8_t  kind;
    uint8_t  routeProved;
    uint16_t port;                     // host byte order
    uint32_t ipv4BE;
    uint64_t generation;
};

struct ClientRootClaim {
    uint16_t schemaVersion;
    uint8_t  remotePlayerRole;
    uint8_t  playerSlot;
    uint64_t requestNonce;
    uint8_t  peerChallenge[16];
    uint32_t sessionId;
    uint32_t reservedSession;
    uint64_t sessionEpoch;
    uint32_t processId;
    uint32_t publisherGeneration;
    uint64_t publisherNonce;
    uint8_t  runtimeStage;
    uint8_t  delay;
    uint8_t  rollback;
    uint8_t  remoteDelay;
    uint8_t  remoteRollback;
    uint8_t  reservedRuntime[3];
    qnetplay::config_provenance::ConfigurationProvenanceWire provenance;
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity;
    uint64_t streamRootIdentity;
    uint64_t gameStartFingerprint;
    uint64_t sourceGeneration;
    uint64_t qspRouteGeneration;
    uint64_t qsrBindingGeneration;
    ClientRootRouteWire dataRoute;
    ClientRootRouteWire qsrRoute;
    uint32_t cacheFirstCursor;
    uint32_t cacheNextCursor;
    uint32_t cursorNextRecord;
    uint32_t cursorNextEvent;
    uint64_t cursorDigest;
    uint32_t childCapacity;
    uint32_t reservedTail;
    uint64_t expiresMs;
    uint8_t  requestTag[16];
};

struct ClientRootEnrollment {
    uint16_t schemaVersion;
    uint8_t  granted;
    uint8_t  reserved;
    uint64_t requestNonce;
    uint64_t clientRootNodeId;
    uint64_t hostRootNodeId;
    uint64_t authorityGeneration;
    uint64_t expiresMs;
    SpectateTopologyEndpointBindingWire rootQsp;
    SpectateTopologyEndpointBindingWire rootQsr;
    uint64_t hostSourceGeneration;
    uint8_t  authorityPublicKey[kSpectateTopologyAuthorityPublicKeyBytes];
    uint8_t  topologyCredential[kClientRootCredentialBytes];
    uint16_t grantLength;
    uint16_t reservedGrant;
    uint8_t  grantBlob[kClientRootGrantBlobMax];
    uint8_t  responseTag[16];
};

struct ConfirmConfig {
    uint32_t sessionId;
    PeerTuning clientTuning;
    uint8_t routeKind;
    uint8_t routeLabel;
    uint8_t routeId;
    uint8_t reserved;
};

struct TuningProposal {
    uint32_t sessionId;
    PeerTuning clientTuning;
    uint8_t routeKind;
    uint8_t routeLabel;
    uint8_t routeId;
    uint8_t requiredFrames;
};

struct RouteCommit {
    uint32_t sessionId;
    uint8_t routeKind;
    uint8_t routeLabel;
    uint8_t routeId;
    uint8_t requiredFrames;
    PeerTuning hostTuning;
};

enum RuntimeControlRole : uint8_t {
    RUNTIME_ROLE_HOST   = 1,
    RUNTIME_ROLE_CLIENT = 2,
};

struct RuntimeSync {
    uint32_t sessionId;
    uint64_t compatibilityId;
    uint64_t cookieLow;
    uint64_t cookieHigh;
    uint8_t routeKind;
    uint8_t senderRole;
    uint8_t reserved[2];
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity;
};

struct ErrorText { char text[120]; };

struct Ping { uint32_t seq; uint64_t sendTimeUs; };

struct PingStats { uint64_t avgRttUs; uint32_t received, sent; };

struct SettingsPending { uint8_t delay; uint8_t rollback; uint16_t pingMs; };

// indexedFrame packs {index:12, frame:20} (index = state-transition counter, frame within it).
struct PlayerInputs {
    uint32_t indexedFrame;
    uint16_t inputs[kNumInputs];   // frames [frame-29 .. frame], newest last
};

struct RelData  { uint32_t seq; uint8_t innerType; };
struct RelAck   { uint32_t ackSeq; };

struct RngState {
    uint32_t index;
    uint32_t seedGameplay, seedCosmetic, selector;
};

struct MenuIndexMsg { uint32_t index; uint8_t menuIndex; };

struct SyncHashMsg {
    uint32_t indexedFrame;
    uint32_t rngGameplay, rngCosmetic, rngSelector;
    int32_t  hp[2];
    uint32_t stateCrc;
};

struct ChangeConfigMsg { uint32_t indexedFrame; uint8_t delay; uint8_t rollback; };

#pragma pack(pop)

static_assert(sizeof(RouteCommit) == 12,
              "route-commit control wire layout changed");
static_assert(sizeof(ConfirmConfig) == 12,
              "confirm-config control wire layout changed");
static_assert(sizeof(TuningProposal) == 12,
              "tuning-proposal control wire layout changed");
static_assert(sizeof(RouteCandidates) == 12 + kMaxRouteCandidates * 8,
              "route-candidates control wire layout changed");
static_assert(sizeof(ClientIntent) == 48,
              "client-intent control wire layout changed");
static_assert(sizeof(RuntimeArtifactIdentityWire) == 100,
              "runtime-artifact identity control wire layout changed");
static_assert(sizeof(InitialConfig) == 143
                                     + qnetplay::compat::kCompatibilityIdentityWireBytes,
              "initial-config control wire layout changed");
static_assert(sizeof(NetplayConfig) == 52
                                     + qnetplay::compat::kCompatibilityIdentityWireBytes,
              "netplay-config control wire layout changed");
static_assert(sizeof(SpectatorGrantWire) == 72,
              "spectator-grant control wire layout changed");
static_assert(sizeof(LivePresence) == 96,
              "live-presence control wire layout changed");
static_assert(sizeof(SpectateInfo) == 71 + sizeof(RouteCandidates)
                                     + sizeof(SpectatorGrantWire)
                                     + qnetplay::compat::kCompatibilityIdentityWireBytes
                                     + sizeof(LivePresence),
              "spectate-info control wire layout changed");
static_assert(sizeof(SpectateTopologyInfo) == 116,
              "spectate-topology-info control wire layout changed");
static_assert(sizeof(SpectateTopologyProof) == 80,
              "spectate-topology-proof control wire layout changed");
static_assert(sizeof(SpectateTopologyProofV2)
                  == 84 + qnetplay::compat::kCompatibilityIdentityWireBytes
                     + sizeof(SpectatorGrantWire) + 16,
              "spectate-topology-proof-v2 control wire layout changed");
static_assert(sizeof(SpectateTopologyChallengeV2) == 44,
              "spectate-topology-challenge-v2 control wire layout changed");
static_assert(sizeof(SpectateTopologyEndpointBindingWire) == 32,
              "spectate-topology endpoint binding v2 layout changed");
static_assert(sizeof(SpectateTopologyCursorWire) == 16,
              "spectate-topology cursor v2 layout changed");
static_assert(sizeof(SpectateTopologyQtaLimitsWire) == 41,
              "spectate-topology limits v2 layout changed");
static_assert(sizeof(SpectateTopologyInfoV2)
                  == 357 + qnetplay::compat::kCompatibilityIdentityWireBytes
                     + kSpectateTopologyAuthorityPublicKeyBytes,
              "spectate-topology-info-v2 control wire layout changed");
static_assert(sizeof(RuntimeSync) == 32
                                    + qnetplay::compat::kCompatibilityIdentityWireBytes,
              "runtime barrier control wire layout changed");

inline bool isValidControlCompatibilityIdentity(
    const qnetplay::compat::CompatibilityIdentityWire& identity) {
    return qnetplay::compat::isValidCompatibilityIdentity(identity)
        && identity.protocolVersion == kProtocolVersion;
}

inline bool isValidRuntimeArtifactIdentity(
    const RuntimeArtifactIdentityWire& identity) {
    if (identity.schemaVersion != kRuntimeArtifactIdentitySchemaVersion)
        return false;
    const auto nonzero = [](const uint8_t* digest) {
        for (size_t i = 0; i < 32u; ++i) {
            if (digest[i] != 0u) return true;
        }
        return false;
    };
    return nonzero(identity.casterSha256)
        && nonzero(identity.netplayDllSha256)
        && nonzero(identity.ddrawSha256);
}

inline RuntimeArtifactMismatch runtimeArtifactMismatch(
    const RuntimeArtifactIdentityWire& local,
    const RuntimeArtifactIdentityWire& remote) {
    if (!isValidRuntimeArtifactIdentity(local)
        || !isValidRuntimeArtifactIdentity(remote)) {
        return RuntimeArtifactMismatch::MissingOrInvalid;
    }
    const bool caster = std::memcmp(local.casterSha256, remote.casterSha256,
                                    sizeof(local.casterSha256)) != 0;
    const bool netplay = std::memcmp(local.netplayDllSha256,
                                     remote.netplayDllSha256,
                                     sizeof(local.netplayDllSha256)) != 0;
    const bool ddraw = std::memcmp(local.ddrawSha256, remote.ddrawSha256,
                                   sizeof(local.ddrawSha256)) != 0;
    const unsigned mismatches = (caster ? 1u : 0u)
        + (netplay ? 1u : 0u) + (ddraw ? 1u : 0u);
    if (mismatches == 0u) return RuntimeArtifactMismatch::None;
    if (mismatches > 1u) return RuntimeArtifactMismatch::Multiple;
    if (caster) return RuntimeArtifactMismatch::Caster;
    if (netplay) return RuntimeArtifactMismatch::NetplayDll;
    return RuntimeArtifactMismatch::Ddraw;
}

inline const char* runtimeArtifactMismatchReason(
    RuntimeArtifactMismatch mismatch) {
    switch (mismatch) {
        case RuntimeArtifactMismatch::None: return "none";
        case RuntimeArtifactMismatch::MissingOrInvalid:
            return "missing or invalid artifact identity";
        case RuntimeArtifactMismatch::Caster: return "caster executable";
        case RuntimeArtifactMismatch::NetplayDll: return "netplay DLL";
        case RuntimeArtifactMismatch::Ddraw: return "ddraw wrapper";
        case RuntimeArtifactMismatch::Multiple: return "multiple artifacts";
    }
    return "unknown artifact mismatch";
}

inline uint32_t indexedFrame(uint32_t index, uint32_t frame) { return (index << 20) | (frame & 0xFFFFF); }
inline uint32_t ifIndex(uint32_t v) { return v >> 20; }
inline uint32_t ifFrame(uint32_t v) { return v & 0xFFFFF; }

inline uint32_t crc32(const void* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

constexpr size_t kMaxMsgSize = 1400;

inline size_t packMsg(uint8_t type, const void* body, size_t bodyLen,
                      uint8_t* out, size_t outCap) {
    if (sizeof(MsgHeader) + bodyLen > outCap || bodyLen > 0xFFFF) return 0;
    MsgHeader h{};
    h.magic = kMagic; h.type = type; h.flags = 0;
    h.bodyLen = static_cast<uint16_t>(bodyLen);
    h.bodyCrc = bodyLen ? crc32(body, bodyLen) : 0;
    std::memcpy(out, &h, sizeof(h));
    if (bodyLen) std::memcpy(out + sizeof(h), body, bodyLen);
    return sizeof(h) + bodyLen;
}

inline bool unpackMsg(const uint8_t* buf, size_t len,
                      uint8_t& type, const uint8_t*& body, size_t& bodyLen) {
    if (len < sizeof(MsgHeader)) return false;
    MsgHeader h;
    std::memcpy(&h, buf, sizeof(h));
    if (h.magic != kMagic) return false;
    if (sizeof(MsgHeader) + h.bodyLen != len) return false;
    if (h.bodyLen && crc32(buf + sizeof(h), h.bodyLen) != h.bodyCrc) return false;
    type = h.type;
    body = buf + sizeof(h);
    bodyLen = h.bodyLen;
    return true;
}

}
