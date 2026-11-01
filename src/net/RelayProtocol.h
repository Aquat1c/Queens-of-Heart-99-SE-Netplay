#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qnet::relay {

// Little-endian bytes spell "QRL1".
constexpr uint32_t kMagic = 0x314C5251u;
constexpr uint16_t kVersion = 1u;
constexpr size_t kHeaderSize = 8u;

constexpr uint16_t kSpectatorVersion = 3u;
constexpr uint16_t kObservedVersion = 3u;
constexpr uint16_t kTopologyVersion = 4u;

constexpr uint8_t kTcpEnvelopeType = 0x70u;

constexpr size_t kHelloSize = 28u;
constexpr size_t kWelcomeSize = 80u;
constexpr size_t kRegisterSize = 48u;
constexpr size_t kReadySize = 52u;

struct Id128 {
    uint64_t low = 0;
    uint64_t high = 0;

    bool operator==(const Id128& other) const {
        return low == other.low && high == other.high;
    }
    bool operator!=(const Id128& other) const {
        return !(*this == other);
    }
};

using RoomId = Id128;
using RegistrationToken = Id128;

enum class RelayRole : uint8_t {
    Host = 1,
    Client = 2,
};

enum class LaneDomain : uint8_t {
    Player = 1,
    Spectator = 2,
    Topology = 3,
};

enum class MessageKind : uint8_t {
    Hello = 1,
    Welcome = 2,
    Register = 3,
    Ready = 4,
};

enum class WelcomeStatus : uint8_t {
    Accepted = 1,
    Waiting = 2,
    RoomNotFound = 3,
    RoomFull = 4,
    Rejected = 5,
    ServerFull = 6,
};

// ipv4 uses the same opaque/network representation as
// sockaddr_in::sin_addr.s_addr. port is host order.
struct Ipv4Endpoint {
    uint32_t ipv4 = 0;
    uint16_t port = 0;

    bool operator==(const Ipv4Endpoint& other) const {
        return ipv4 == other.ipv4 && port == other.port;
    }
    bool operator!=(const Ipv4Endpoint& other) const {
        return !(*this == other);
    }
};

struct Hello {
    RoomId room{};
    LaneDomain domain = LaneDomain::Player;
    RelayRole role = RelayRole::Host;
};

struct Welcome {
    RoomId room{};
    LaneDomain domain = LaneDomain::Player;
    RelayRole role = RelayRole::Host;
    WelcomeStatus status = WelcomeStatus::Rejected;

    RegistrationToken registrationToken{};

    RegistrationToken spectatorRegistrationToken{};
    RegistrationToken viewerAccessToken{};
};

struct Register {
    RoomId room{};
    LaneDomain domain = LaneDomain::Player;
    RelayRole role = RelayRole::Host;
    RegistrationToken token{};
};

struct Ready {
    RoomId room{};
    LaneDomain domain = LaneDomain::Player;
    RelayRole role = RelayRole::Host;
    RegistrationToken token{};
    Ipv4Endpoint peer{};
};

enum class DecodeError {
    None,
    TooShort,
    BadMagic,
    BadVersion,
    BadKind,
    BadLength,
    BadReserved,
    BadDomain,
    BadRole,
    BadStatus,
    InvalidRoom,
    InvalidToken,
    InvalidEndpoint,
    InvalidGrant,
    InvalidParty,
};

enum class ObservedMessageKind : uint8_t {
    Request = 0x30,
    Observed = 0x31,
};

constexpr size_t kObservedRequestSize = 48u;
constexpr size_t kObservedSize = 56u;

struct ObservedRequest {
    RoomId room{};
    LaneDomain domain = LaneDomain::Player;
    RelayRole role = RelayRole::Host;
    RegistrationToken token{};
};

struct ObservedEndpoint {
    RoomId room{};
    LaneDomain domain = LaneDomain::Player;
    RelayRole role = RelayRole::Host;
    RegistrationToken token{};
    Ipv4Endpoint observed{};
};

enum class SpectatorMessageKind : uint8_t {
    Hello = 0x11,
    Welcome = 0x12,
    Register = 0x13,
    Ready = 0x14,
    Detach = 0x15,
};

enum class SpectatorParty : uint8_t {
    Source = 1,
    Viewer = 2,
};

enum class SpectatorAdmission : uint8_t {
    None = 0,
    Grant = 1,
    PublicToken = 2,
};

enum class SpectatorRouteKind : uint8_t {
    Direct = 1,
    Punched = 2,
    Relay = 3,
};

enum class SpectatorStatus : uint8_t {
    Accepted = 1,
    Waiting = 2,
    RoomNotFound = 3,
    SourceUnavailable = 4,
    CapacityReached = 5,
    GrantRejected = 6,
    GrantExpired = 7,
    Superseded = 8,
    Unsupported = 9,
    RateLimited = 10,
};

enum class SpectatorDetachReason : uint16_t {
    ViewerRequested = 1,
    Expired = 2,
    SourceLost = 3,
    Superseded = 4,
    CapacityReclaimed = 5,
    ServiceStopping = 6,
    ProtocolViolation = 7,
    RateLimited = 8,
    EndpointMigrated = 9,
};

constexpr size_t kGrantKeyBytes = 32u;

constexpr size_t kSpectatorHelloSize = 112u;
constexpr size_t kSpectatorWelcomeSize = 112u;
constexpr size_t kSpectatorRegisterSize = 64u;
constexpr size_t kSpectatorReadySize = 72u;
constexpr size_t kSpectatorDetachSize = 64u;

using GrantKey = std::vector<uint8_t>;

struct SpectatorHello {
    RoomId room{};
    SpectatorParty party = SpectatorParty::Viewer;
    SpectatorRouteKind route = SpectatorRouteKind::Relay;
    SpectatorAdmission admission = SpectatorAdmission::Grant;
    RegistrationToken sourceToken{};
    uint64_t sessionId = 0;
    uint64_t grantId = 0;
    uint64_t viewerId = 0;
    uint64_t expiryUnix = 0;
    uint32_t sourceGeneration = 0;
    uint32_t resumeRecord = 0;
    uint64_t resumeDigest = 0;
    uint64_t macLow = 0;
    uint64_t macHigh = 0;
};

struct SpectatorWelcome {
    RoomId room{};
    SpectatorParty party = SpectatorParty::Viewer;
    SpectatorStatus status = SpectatorStatus::Unsupported;
    RegistrationToken registrationToken{};
    uint64_t viewerId = 0;
    uint64_t sessionId = 0;
    uint32_t sourceGeneration = 0;
    uint16_t viewerCount = 0;
    uint16_t viewerCapacity = 0;
    uint32_t retryAfterMs = 0;
    GrantKey grantKey{};
};

struct SpectatorRegister {
    RoomId room{};
    SpectatorParty party = SpectatorParty::Viewer;
    uint32_t sourceGeneration = 0;
    RegistrationToken token{};
    uint64_t viewerId = 0;
};

struct SpectatorReady {
    RoomId room{};
    SpectatorParty party = SpectatorParty::Viewer;
    uint32_t sourceGeneration = 0;
    RegistrationToken token{};
    uint64_t viewerId = 0;
    Ipv4Endpoint peer{};
};

struct SpectatorDetach {
    RoomId room{};
    SpectatorParty party = SpectatorParty::Viewer;
    SpectatorDetachReason reason = SpectatorDetachReason::ViewerRequested;
    uint32_t sourceGeneration = 0;
    RegistrationToken token{};
    uint64_t viewerId = 0;
    uint32_t retryAfterMs = 0;
};

// Spectator data frame: fixed 24-byte header followed by one complete QSP datagram.
constexpr uint8_t kSpectatorFrameKind = 0x16u;
constexpr size_t kSpectatorFrameHeaderSize = 24u;

struct SpectatorFrameHeader {
    SpectatorParty party = SpectatorParty::Viewer;
    uint64_t viewerId = 0;
    uint32_t sourceGeneration = 0;
};

bool encodeSpectatorFrameHeader(const SpectatorFrameHeader& header,
                                std::vector<uint8_t>& out);
DecodeError decodeSpectatorFrameHeader(const uint8_t* bytes,
                                       size_t length,
                                       SpectatorFrameHeader& out);
bool hasSpectatorFrameKind(const uint8_t* bytes, size_t length);

enum class TopologyMessageKind : uint8_t {
    Hello = 0x20,
    Welcome = 0x21,
    Register = 0x22,
    Ready = 0x23,
    Detach = 0x24,
};

enum class TopologyParty : uint8_t {
    Authority = 1,
    Node = 2,
};

enum class TopologyAdmission : uint8_t {
    None = 0,
    Grant = 1,
};

enum class TopologyStatus : uint8_t {
    Accepted = 1,
    Waiting = 2,
    RoomNotFound = 3,
    AuthorityUnavailable = 4,
    CapacityReached = 5,
    GrantRejected = 6,
    GrantExpired = 7,
    Superseded = 8,
    Unsupported = 9,
    RateLimited = 10,
};

enum class TopologyDetachReason : uint16_t {
    NodeRequested = 1,
    Expired = 2,
    AuthorityLost = 3,
    Superseded = 4,
    CapacityReclaimed = 5,
    ServiceStopping = 6,
    ProtocolViolation = 7,
    RateLimited = 8,
    EndpointMigrated = 9,
};

constexpr size_t kTopologyHelloSize = 100u;
constexpr size_t kTopologyWelcomeSize = 104u;
constexpr size_t kTopologyRegisterSize = 56u;
constexpr size_t kTopologyReadySize = 64u;
constexpr size_t kTopologyDetachSize = 64u;

struct TopologyHello {
    RoomId room{};
    TopologyParty party = TopologyParty::Node;
    TopologyAdmission admission = TopologyAdmission::Grant;
    RegistrationToken authorityToken{};
    uint64_t sessionId = 0;
    uint64_t grantId = 0;
    uint64_t nodeId = 0;
    uint64_t expiryUnix = 0;
    uint32_t authorityGeneration = 0;
    uint64_t macLow = 0;
    uint64_t macHigh = 0;
};

struct TopologyWelcome {
    RoomId room{};
    TopologyParty party = TopologyParty::Node;
    TopologyStatus status = TopologyStatus::Unsupported;
    RegistrationToken registrationToken{};
    uint64_t nodeId = 0;
    uint64_t sessionId = 0;
    uint32_t authorityGeneration = 0;
    uint16_t nodeCount = 0;
    uint16_t nodeCapacity = 0;
    uint32_t retryAfterMs = 0;
    GrantKey grantKey{};
};

struct TopologyRegister {
    RoomId room{};
    TopologyParty party = TopologyParty::Node;
    uint32_t authorityGeneration = 0;
    RegistrationToken token{};
    uint64_t nodeId = 0;
};

struct TopologyReady {
    RoomId room{};
    TopologyParty party = TopologyParty::Node;
    uint32_t authorityGeneration = 0;
    RegistrationToken token{};
    uint64_t nodeId = 0;
    Ipv4Endpoint peer{};
};

struct TopologyDetach {
    RoomId room{};
    TopologyParty party = TopologyParty::Node;
    TopologyDetachReason reason = TopologyDetachReason::NodeRequested;
    uint32_t authorityGeneration = 0;
    RegistrationToken token{};
    uint64_t nodeId = 0;
    uint32_t retryAfterMs = 0;
};

// Topology data frame: fixed 24-byte header followed by one complete QTA control envelope.
constexpr uint8_t kTopologyFrameKind = 0x25u;
constexpr size_t kTopologyFrameHeaderSize = 24u;

struct TopologyFrameHeader {
    TopologyParty party = TopologyParty::Node;
    uint64_t nodeId = 0;
    uint32_t authorityGeneration = 0;
};

bool encodeTopologyFrameHeader(const TopologyFrameHeader& header,
                               std::vector<uint8_t>& out);
DecodeError decodeTopologyFrameHeader(const uint8_t* bytes,
                                      size_t length,
                                      TopologyFrameHeader& out);
bool hasTopologyFrameKind(const uint8_t* bytes, size_t length);

bool isValidTopologyParty(TopologyParty party);
bool isValidTopologyAdmission(TopologyAdmission admission);
bool isValidTopologyStatus(TopologyStatus status);
bool isValidTopologyDetachReason(TopologyDetachReason reason);
size_t topologyEncodedSize(TopologyMessageKind kind);

DecodeError inspectTopologyMessageKind(const uint8_t* bytes,
                                       size_t length,
                                       TopologyMessageKind& kind);

bool encodeTopologyHello(const TopologyHello& message,
                         std::vector<uint8_t>& out);
DecodeError decodeTopologyHello(const uint8_t* bytes,
                                size_t length,
                                TopologyHello& out);

bool encodeTopologyWelcome(const TopologyWelcome& message,
                           std::vector<uint8_t>& out);
DecodeError decodeTopologyWelcome(const uint8_t* bytes,
                                  size_t length,
                                  TopologyWelcome& out);

bool encodeTopologyRegister(const TopologyRegister& message,
                            std::vector<uint8_t>& out);
DecodeError decodeTopologyRegister(const uint8_t* bytes,
                                   size_t length,
                                   TopologyRegister& out);

bool encodeTopologyReady(const TopologyReady& message,
                         std::vector<uint8_t>& out);
DecodeError decodeTopologyReady(const uint8_t* bytes,
                                size_t length,
                                TopologyReady& out);

bool encodeTopologyDetach(const TopologyDetach& message,
                          std::vector<uint8_t>& out);
DecodeError decodeTopologyDetach(const uint8_t* bytes,
                                 size_t length,
                                 TopologyDetach& out);

bool isValidId(const Id128& id);
bool isValidRole(RelayRole role);
bool isValidDomain(LaneDomain domain);
bool isValidStatus(WelcomeStatus status);
bool isValidEndpoint(const Ipv4Endpoint& endpoint);

size_t encodedSize(MessageKind kind);
bool hasRelayMagic(const uint8_t* bytes, size_t length);

DecodeError inspectMessageKind(const uint8_t* bytes,
                               size_t length,
                               MessageKind& kind);

bool parseRoomId(const std::string& text, RoomId& out);
std::string formatRoomId(const RoomId& room);

bool encodeHello(const Hello& message, std::vector<uint8_t>& out);
DecodeError decodeHello(const uint8_t* bytes,
                        size_t length,
                        Hello& out);

bool encodeWelcome(const Welcome& message, std::vector<uint8_t>& out);
DecodeError decodeWelcome(const uint8_t* bytes,
                          size_t length,
                          Welcome& out);

bool encodeRegister(const Register& message, std::vector<uint8_t>& out);
DecodeError decodeRegister(const uint8_t* bytes,
                           size_t length,
                           Register& out);

bool encodeReady(const Ready& message, std::vector<uint8_t>& out);
DecodeError decodeReady(const uint8_t* bytes,
                        size_t length,
                        Ready& out);

size_t observedEncodedSize(ObservedMessageKind kind);
DecodeError inspectObservedMessageKind(const uint8_t* bytes,
                                       size_t length,
                                       ObservedMessageKind& kind);

bool encodeObservedRequest(const ObservedRequest& message,
                           std::vector<uint8_t>& out);
DecodeError decodeObservedRequest(const uint8_t* bytes,
                                  size_t length,
                                  ObservedRequest& out);

bool encodeObservedEndpoint(const ObservedEndpoint& message,
                            std::vector<uint8_t>& out);
DecodeError decodeObservedEndpoint(const uint8_t* bytes,
                                   size_t length,
                                   ObservedEndpoint& out);

bool isValidParty(SpectatorParty party);
bool isValidRouteKind(SpectatorRouteKind route);
bool isValidSpectatorAdmission(SpectatorAdmission admission);
bool isValidSpectatorStatus(SpectatorStatus status);
bool isValidDetachReason(SpectatorDetachReason reason);
size_t spectatorEncodedSize(SpectatorMessageKind kind);

bool peekRelayVersion(const uint8_t* bytes, size_t length, uint16_t& out);

DecodeError inspectSpectatorMessageKind(const uint8_t* bytes,
                                        size_t length,
                                        SpectatorMessageKind& kind);

bool encodeSpectatorHello(const SpectatorHello& message,
                          std::vector<uint8_t>& out);
DecodeError decodeSpectatorHello(const uint8_t* bytes,
                                 size_t length,
                                 SpectatorHello& out);

bool encodeSpectatorWelcome(const SpectatorWelcome& message,
                            std::vector<uint8_t>& out);
DecodeError decodeSpectatorWelcome(const uint8_t* bytes,
                                   size_t length,
                                   SpectatorWelcome& out);

bool encodeSpectatorRegister(const SpectatorRegister& message,
                             std::vector<uint8_t>& out);
DecodeError decodeSpectatorRegister(const uint8_t* bytes,
                                    size_t length,
                                    SpectatorRegister& out);

bool encodeSpectatorReady(const SpectatorReady& message,
                          std::vector<uint8_t>& out);
DecodeError decodeSpectatorReady(const uint8_t* bytes,
                                 size_t length,
                                 SpectatorReady& out);

bool encodeSpectatorDetach(const SpectatorDetach& message,
                           std::vector<uint8_t>& out);
DecodeError decodeSpectatorDetach(const uint8_t* bytes,
                                  size_t length,
                                  SpectatorDetach& out);

}
