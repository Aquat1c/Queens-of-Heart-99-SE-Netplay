#include "RelayProtocol.h"

#include <cstdio>

namespace qnet::relay {

namespace {

constexpr size_t kMagicOffset = 0u;
constexpr size_t kVersionOffset = 4u;
constexpr size_t kKindOffset = 6u;
constexpr size_t kHeaderReservedOffset = 7u;

constexpr size_t kRoomLowOffset = 8u;
constexpr size_t kRoomHighOffset = 16u;
constexpr size_t kDomainOffset = 24u;
constexpr size_t kRoleOffset = 25u;

void writeLe16(uint8_t* out, const uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8u);
}

void writeLe32(uint8_t* out, const uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        out[shift / 8u] = static_cast<uint8_t>(value >> shift);
}

void writeLe64(uint8_t* out, const uint64_t value) {
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        out[shift / 8u] = static_cast<uint8_t>(value >> shift);
}

uint16_t readLe16(const uint8_t* in) {
    return static_cast<uint16_t>(
        static_cast<uint16_t>(in[0])
        | (static_cast<uint16_t>(in[1]) << 8u));
}

uint32_t readLe32(const uint8_t* in) {
    uint32_t value = 0;
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        value |= static_cast<uint32_t>(in[shift / 8u]) << shift;
    return value;
}

uint64_t readLe64(const uint8_t* in) {
    uint64_t value = 0;
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        value |= static_cast<uint64_t>(in[shift / 8u]) << shift;
    return value;
}

bool allZero(const uint8_t* bytes, const size_t length) {
    uint8_t any = 0;
    for (size_t index = 0u; index < length; ++index)
        any |= bytes[index];
    return any == 0u;
}

bool isKnownKind(const MessageKind kind) {
    switch (kind) {
        case MessageKind::Hello:
        case MessageKind::Welcome:
        case MessageKind::Register:
        case MessageKind::Ready:
            return true;
    }
    return false;
}

void encodeHeader(std::vector<uint8_t>& bytes, const MessageKind kind) {
    writeLe32(bytes.data() + kMagicOffset, kMagic);
    writeLe16(bytes.data() + kVersionOffset, kVersion);
    bytes[kKindOffset] = static_cast<uint8_t>(kind);
    bytes[kHeaderReservedOffset] = 0u;
}

void encodeRoom(std::vector<uint8_t>& bytes, const RoomId& room) {
    writeLe64(bytes.data() + kRoomLowOffset, room.low);
    writeLe64(bytes.data() + kRoomHighOffset, room.high);
}

RoomId decodeRoom(const uint8_t* bytes) {
    return RoomId{
        readLe64(bytes + kRoomLowOffset),
        readLe64(bytes + kRoomHighOffset),
    };
}

bool isZero(const Id128& value) {
    return value.low == 0u && value.high == 0u;
}

bool validWelcomeTokens(const Welcome& message) {
    if (message.status != WelcomeStatus::Accepted) {
        return isZero(message.registrationToken)
            && isZero(message.spectatorRegistrationToken)
            && isZero(message.viewerAccessToken);
    }
    if (!isValidId(message.registrationToken))
        return false;
    if (message.role == RelayRole::Client) {
        return isZero(message.spectatorRegistrationToken)
            && isZero(message.viewerAccessToken);
    }
    return isValidId(message.spectatorRegistrationToken)
        && isValidId(message.viewerAccessToken)
        && message.registrationToken
            != message.spectatorRegistrationToken
        && message.registrationToken != message.viewerAccessToken
        && message.spectatorRegistrationToken
            != message.viewerAccessToken;
}

int hexNibble(const char character) {
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

}

bool isValidId(const Id128& id) {
    return id.low != 0u || id.high != 0u;
}

bool isValidRole(const RelayRole role) {
    return role == RelayRole::Host || role == RelayRole::Client;
}

bool isValidDomain(const LaneDomain domain) {
    return domain == LaneDomain::Player
        || domain == LaneDomain::Spectator;
}

bool isValidStatus(const WelcomeStatus status) {
    switch (status) {
        case WelcomeStatus::Accepted:
        case WelcomeStatus::Waiting:
        case WelcomeStatus::RoomNotFound:
        case WelcomeStatus::RoomFull:
        case WelcomeStatus::Rejected:
        case WelcomeStatus::ServerFull:
            return true;
    }
    return false;
}

bool isValidEndpoint(const Ipv4Endpoint& endpoint) {
    return endpoint.ipv4 != 0u && endpoint.port != 0u;
}

size_t encodedSize(const MessageKind kind) {
    switch (kind) {
        case MessageKind::Hello: return kHelloSize;
        case MessageKind::Welcome: return kWelcomeSize;
        case MessageKind::Register: return kRegisterSize;
        case MessageKind::Ready: return kReadySize;
    }
    return 0u;
}

bool hasRelayMagic(const uint8_t* const bytes, const size_t length) {
    return bytes != nullptr && length >= sizeof(uint32_t)
        && readLe32(bytes + kMagicOffset) == kMagic;
}

DecodeError inspectMessageKind(const uint8_t* const bytes,
                               const size_t length,
                               MessageKind& kind) {
    if (bytes == nullptr || length < kHeaderSize)
        return DecodeError::TooShort;
    if (readLe32(bytes + kMagicOffset) != kMagic)
        return DecodeError::BadMagic;
    if (readLe16(bytes + kVersionOffset) != kVersion)
        return DecodeError::BadVersion;
    const MessageKind decoded =
        static_cast<MessageKind>(bytes[kKindOffset]);
    if (!isKnownKind(decoded))
        return DecodeError::BadKind;
    if (length != encodedSize(decoded))
        return DecodeError::BadLength;
    if (bytes[kHeaderReservedOffset] != 0u)
        return DecodeError::BadReserved;
    kind = decoded;
    return DecodeError::None;
}

bool parseRoomId(const std::string& text, RoomId& out) {
    if (text.size() != 32u)
        return false;
    RoomId parsed{};
    for (size_t index = 0u; index < text.size(); ++index) {
        const int nibble = hexNibble(text[index]);
        if (nibble < 0)
            return false;
        uint64_t& half = index < 16u ? parsed.high : parsed.low;
        half = (half << 4u) | static_cast<uint64_t>(nibble);
    }
    if (!isValidId(parsed))
        return false;
    out = parsed;
    return true;
}

std::string formatRoomId(const RoomId& room) {
    char text[33] = {};
    std::snprintf(
        text, sizeof(text), "%016llX%016llX",
        static_cast<unsigned long long>(room.high),
        static_cast<unsigned long long>(room.low));
    return text;
}

bool encodeHello(const Hello& message, std::vector<uint8_t>& out) {
    if (!isValidId(message.room)
        || !isValidDomain(message.domain)
        || !isValidRole(message.role)) {
        return false;
    }
    std::vector<uint8_t> encoded(kHelloSize, 0u);
    encodeHeader(encoded, MessageKind::Hello);
    encodeRoom(encoded, message.room);
    encoded[kDomainOffset] = static_cast<uint8_t>(message.domain);
    encoded[kRoleOffset] = static_cast<uint8_t>(message.role);
    out.swap(encoded);
    return true;
}

DecodeError decodeHello(const uint8_t* const bytes,
                        const size_t length,
                        Hello& out) {
    MessageKind kind{};
    const DecodeError header = inspectMessageKind(bytes, length, kind);
    if (header != DecodeError::None)
        return header;
    if (kind != MessageKind::Hello)
        return DecodeError::BadKind;
    if (!allZero(bytes + 26u, 2u))
        return DecodeError::BadReserved;
    const LaneDomain domain =
        static_cast<LaneDomain>(bytes[kDomainOffset]);
    if (!isValidDomain(domain))
        return DecodeError::BadDomain;
    const RelayRole role =
        static_cast<RelayRole>(bytes[kRoleOffset]);
    if (!isValidRole(role))
        return DecodeError::BadRole;
    const RoomId room = decodeRoom(bytes);
    if (!isValidId(room))
        return DecodeError::InvalidRoom;
    Hello decoded{};
    decoded.room = room;
    decoded.domain = domain;
    decoded.role = role;
    out = decoded;
    return DecodeError::None;
}

bool encodeWelcome(const Welcome& message, std::vector<uint8_t>& out) {
    if (!isValidId(message.room)
        || !isValidDomain(message.domain)
        || !isValidRole(message.role)
        || !isValidStatus(message.status)
        || !validWelcomeTokens(message)) {
        return false;
    }
    std::vector<uint8_t> encoded(kWelcomeSize, 0u);
    encodeHeader(encoded, MessageKind::Welcome);
    encodeRoom(encoded, message.room);
    encoded[kDomainOffset] = static_cast<uint8_t>(message.domain);
    encoded[kRoleOffset] = static_cast<uint8_t>(message.role);
    encoded[26u] = static_cast<uint8_t>(message.status);
    writeLe64(encoded.data() + 28u, message.registrationToken.low);
    writeLe64(encoded.data() + 36u, message.registrationToken.high);
    writeLe64(
        encoded.data() + 44u,
        message.spectatorRegistrationToken.low);
    writeLe64(
        encoded.data() + 52u,
        message.spectatorRegistrationToken.high);
    writeLe64(encoded.data() + 60u, message.viewerAccessToken.low);
    writeLe64(encoded.data() + 68u, message.viewerAccessToken.high);
    out.swap(encoded);
    return true;
}

DecodeError decodeWelcome(const uint8_t* const bytes,
                          const size_t length,
                          Welcome& out) {
    MessageKind kind{};
    const DecodeError header = inspectMessageKind(bytes, length, kind);
    if (header != DecodeError::None)
        return header;
    if (kind != MessageKind::Welcome)
        return DecodeError::BadKind;
    if (bytes[27u] != 0u || !allZero(bytes + 76u, 4u))
        return DecodeError::BadReserved;
    const LaneDomain domain =
        static_cast<LaneDomain>(bytes[kDomainOffset]);
    if (!isValidDomain(domain))
        return DecodeError::BadDomain;
    const RelayRole role =
        static_cast<RelayRole>(bytes[kRoleOffset]);
    if (!isValidRole(role))
        return DecodeError::BadRole;
    const WelcomeStatus status =
        static_cast<WelcomeStatus>(bytes[26u]);
    if (!isValidStatus(status))
        return DecodeError::BadStatus;
    const RoomId room = decodeRoom(bytes);
    if (!isValidId(room))
        return DecodeError::InvalidRoom;

    Welcome decoded{};
    decoded.room = room;
    decoded.domain = domain;
    decoded.role = role;
    decoded.status = status;
    decoded.registrationToken = {
        readLe64(bytes + 28u),
        readLe64(bytes + 36u),
    };
    decoded.spectatorRegistrationToken = {
        readLe64(bytes + 44u),
        readLe64(bytes + 52u),
    };
    decoded.viewerAccessToken = {
        readLe64(bytes + 60u),
        readLe64(bytes + 68u),
    };
    if (!validWelcomeTokens(decoded))
        return DecodeError::InvalidToken;
    out = decoded;
    return DecodeError::None;
}

bool encodeRegister(const Register& message, std::vector<uint8_t>& out) {
    if (!isValidId(message.room)
        || !isValidDomain(message.domain)
        || !isValidRole(message.role)
        || !isValidId(message.token)) {
        return false;
    }
    std::vector<uint8_t> encoded(kRegisterSize, 0u);
    encodeHeader(encoded, MessageKind::Register);
    encodeRoom(encoded, message.room);
    encoded[kDomainOffset] = static_cast<uint8_t>(message.domain);
    encoded[kRoleOffset] = static_cast<uint8_t>(message.role);
    writeLe64(encoded.data() + 32u, message.token.low);
    writeLe64(encoded.data() + 40u, message.token.high);
    out.swap(encoded);
    return true;
}

DecodeError decodeRegister(const uint8_t* const bytes,
                           const size_t length,
                           Register& out) {
    MessageKind kind{};
    const DecodeError header = inspectMessageKind(bytes, length, kind);
    if (header != DecodeError::None)
        return header;
    if (kind != MessageKind::Register)
        return DecodeError::BadKind;
    if (!allZero(bytes + 26u, 6u))
        return DecodeError::BadReserved;
    const LaneDomain domain =
        static_cast<LaneDomain>(bytes[kDomainOffset]);
    if (!isValidDomain(domain))
        return DecodeError::BadDomain;
    const RelayRole role =
        static_cast<RelayRole>(bytes[kRoleOffset]);
    if (!isValidRole(role))
        return DecodeError::BadRole;
    const RoomId room = decodeRoom(bytes);
    if (!isValidId(room))
        return DecodeError::InvalidRoom;
    const RegistrationToken token{
        readLe64(bytes + 32u),
        readLe64(bytes + 40u),
    };
    if (!isValidId(token))
        return DecodeError::InvalidToken;
    Register decoded{};
    decoded.room = room;
    decoded.domain = domain;
    decoded.role = role;
    decoded.token = token;
    out = decoded;
    return DecodeError::None;
}

bool encodeReady(const Ready& message, std::vector<uint8_t>& out) {
    if (!isValidId(message.room)
        || !isValidDomain(message.domain)
        || !isValidRole(message.role)
        || !isValidId(message.token)
        || !isValidEndpoint(message.peer)) {
        return false;
    }
    std::vector<uint8_t> encoded(kReadySize, 0u);
    encodeHeader(encoded, MessageKind::Ready);
    encodeRoom(encoded, message.room);
    encoded[kDomainOffset] = static_cast<uint8_t>(message.domain);
    encoded[kRoleOffset] = static_cast<uint8_t>(message.role);
    writeLe64(encoded.data() + 28u, message.token.low);
    writeLe64(encoded.data() + 36u, message.token.high);
    writeLe32(encoded.data() + 44u, message.peer.ipv4);
    writeLe16(encoded.data() + 48u, message.peer.port);
    out.swap(encoded);
    return true;
}

DecodeError decodeReady(const uint8_t* const bytes,
                        const size_t length,
                        Ready& out) {
    MessageKind kind{};
    const DecodeError header = inspectMessageKind(bytes, length, kind);
    if (header != DecodeError::None)
        return header;
    if (kind != MessageKind::Ready)
        return DecodeError::BadKind;
    if (!allZero(bytes + 26u, 2u)
        || !allZero(bytes + 50u, 2u)) {
        return DecodeError::BadReserved;
    }
    const LaneDomain domain =
        static_cast<LaneDomain>(bytes[kDomainOffset]);
    if (!isValidDomain(domain))
        return DecodeError::BadDomain;
    const RelayRole role =
        static_cast<RelayRole>(bytes[kRoleOffset]);
    if (!isValidRole(role))
        return DecodeError::BadRole;
    const RoomId room = decodeRoom(bytes);
    if (!isValidId(room))
        return DecodeError::InvalidRoom;
    const RegistrationToken token{
        readLe64(bytes + 28u),
        readLe64(bytes + 36u),
    };
    if (!isValidId(token))
        return DecodeError::InvalidToken;
    const Ipv4Endpoint peer{
        readLe32(bytes + 44u),
        readLe16(bytes + 48u),
    };
    if (!isValidEndpoint(peer))
        return DecodeError::InvalidEndpoint;
    Ready decoded{};
    decoded.room = room;
    decoded.domain = domain;
    decoded.role = role;
    decoded.token = token;
    decoded.peer = peer;
    out = decoded;
    return DecodeError::None;
}

namespace {

bool isKnownObservedKind(const ObservedMessageKind kind) {
    switch (kind) {
        case ObservedMessageKind::Request:
        case ObservedMessageKind::Observed:
            return true;
    }
    return false;
}

void encodeObservedHeader(std::vector<uint8_t>& bytes,
                          const ObservedMessageKind kind,
                          const RoomId& room,
                          const LaneDomain domain,
                          const RelayRole role) {
    writeLe32(bytes.data() + kMagicOffset, kMagic);
    writeLe16(bytes.data() + kVersionOffset, kObservedVersion);
    bytes[kKindOffset] = static_cast<uint8_t>(kind);
    bytes[kHeaderReservedOffset] = 0u;
    writeLe64(bytes.data() + kRoomLowOffset, room.low);
    writeLe64(bytes.data() + kRoomHighOffset, room.high);
    bytes[kDomainOffset] = static_cast<uint8_t>(domain);
    bytes[kRoleOffset] = static_cast<uint8_t>(role);
}

DecodeError decodeObservedPrefix(const uint8_t* const bytes,
                                 const size_t length,
                                 const ObservedMessageKind expected,
                                 LaneDomain& domain,
                                 RelayRole& role,
                                 RoomId& room) {
    ObservedMessageKind kind{};
    const DecodeError header =
        inspectObservedMessageKind(bytes, length, kind);
    if (header != DecodeError::None)
        return header;
    if (kind != expected)
        return DecodeError::BadKind;
    const LaneDomain decodedDomain =
        static_cast<LaneDomain>(bytes[kDomainOffset]);
    if (!isValidDomain(decodedDomain))
        return DecodeError::BadDomain;
    const RelayRole decodedRole =
        static_cast<RelayRole>(bytes[kRoleOffset]);
    if (!isValidRole(decodedRole))
        return DecodeError::BadRole;
    const RoomId decodedRoom = decodeRoom(bytes);
    if (!isValidId(decodedRoom))
        return DecodeError::InvalidRoom;
    domain = decodedDomain;
    role = decodedRole;
    room = decodedRoom;
    return DecodeError::None;
}

}

size_t observedEncodedSize(const ObservedMessageKind kind) {
    switch (kind) {
        case ObservedMessageKind::Request: return kObservedRequestSize;
        case ObservedMessageKind::Observed: return kObservedSize;
    }
    return 0u;
}

DecodeError inspectObservedMessageKind(const uint8_t* const bytes,
                                       const size_t length,
                                       ObservedMessageKind& kind) {
    if (bytes == nullptr || length < kHeaderSize)
        return DecodeError::TooShort;
    if (readLe32(bytes + kMagicOffset) != kMagic)
        return DecodeError::BadMagic;
    if (readLe16(bytes + kVersionOffset) != kObservedVersion)
        return DecodeError::BadVersion;
    const ObservedMessageKind decoded =
        static_cast<ObservedMessageKind>(bytes[kKindOffset]);
    if (!isKnownObservedKind(decoded))
        return DecodeError::BadKind;
    if (length != observedEncodedSize(decoded))
        return DecodeError::BadLength;
    if (bytes[kHeaderReservedOffset] != 0u)
        return DecodeError::BadReserved;
    kind = decoded;
    return DecodeError::None;
}

bool encodeObservedRequest(const ObservedRequest& message,
                           std::vector<uint8_t>& out) {
    if (!isValidId(message.room)
        || !isValidDomain(message.domain)
        || !isValidRole(message.role)
        || !isValidId(message.token)) {
        return false;
    }
    std::vector<uint8_t> encoded(kObservedRequestSize, 0u);
    encodeObservedHeader(
        encoded, ObservedMessageKind::Request, message.room,
        message.domain, message.role);
    writeLe64(encoded.data() + 32u, message.token.low);
    writeLe64(encoded.data() + 40u, message.token.high);
    out.swap(encoded);
    return true;
}

DecodeError decodeObservedRequest(const uint8_t* const bytes,
                                  const size_t length,
                                  ObservedRequest& out) {
    LaneDomain domain{};
    RelayRole role{};
    RoomId room{};
    const DecodeError prefix = decodeObservedPrefix(
        bytes, length, ObservedMessageKind::Request, domain, role, room);
    if (prefix != DecodeError::None)
        return prefix;
    if (!allZero(bytes + 26u, 6u))
        return DecodeError::BadReserved;
    const RegistrationToken token{
        readLe64(bytes + 32u),
        readLe64(bytes + 40u),
    };
    if (!isValidId(token))
        return DecodeError::InvalidToken;
    ObservedRequest decoded{};
    decoded.room = room;
    decoded.domain = domain;
    decoded.role = role;
    decoded.token = token;
    out = decoded;
    return DecodeError::None;
}

bool encodeObservedEndpoint(const ObservedEndpoint& message,
                            std::vector<uint8_t>& out) {
    if (!isValidId(message.room)
        || !isValidDomain(message.domain)
        || !isValidRole(message.role)
        || !isValidId(message.token)
        || !isValidEndpoint(message.observed)) {
        return false;
    }
    std::vector<uint8_t> encoded(kObservedSize, 0u);
    encodeObservedHeader(
        encoded, ObservedMessageKind::Observed, message.room,
        message.domain, message.role);
    writeLe64(encoded.data() + 28u, message.token.low);
    writeLe64(encoded.data() + 36u, message.token.high);
    writeLe32(encoded.data() + 44u, message.observed.ipv4);
    writeLe16(encoded.data() + 48u, message.observed.port);
    out.swap(encoded);
    return true;
}

DecodeError decodeObservedEndpoint(const uint8_t* const bytes,
                                   const size_t length,
                                   ObservedEndpoint& out) {
    LaneDomain domain{};
    RelayRole role{};
    RoomId room{};
    const DecodeError prefix = decodeObservedPrefix(
        bytes, length, ObservedMessageKind::Observed, domain, role, room);
    if (prefix != DecodeError::None)
        return prefix;
    if (!allZero(bytes + 26u, 2u) || !allZero(bytes + 50u, 6u))
        return DecodeError::BadReserved;
    const RegistrationToken token{
        readLe64(bytes + 28u),
        readLe64(bytes + 36u),
    };
    if (!isValidId(token))
        return DecodeError::InvalidToken;
    const Ipv4Endpoint observed{
        readLe32(bytes + 44u),
        readLe16(bytes + 48u),
    };
    if (!isValidEndpoint(observed))
        return DecodeError::InvalidEndpoint;
    ObservedEndpoint decoded{};
    decoded.room = room;
    decoded.domain = domain;
    decoded.role = role;
    decoded.token = token;
    decoded.observed = observed;
    out = decoded;
    return DecodeError::None;
}

namespace {

constexpr size_t kPartyOffset = 25u;

bool isKnownSpectatorKind(const SpectatorMessageKind kind) {
    switch (kind) {
        case SpectatorMessageKind::Hello:
        case SpectatorMessageKind::Welcome:
        case SpectatorMessageKind::Register:
        case SpectatorMessageKind::Ready:
        case SpectatorMessageKind::Detach:
            return true;
    }
    return false;
}

void encodeSpectatorHeader(std::vector<uint8_t>& bytes,
                           const SpectatorMessageKind kind,
                           const RoomId& room,
                           const SpectatorParty party) {
    writeLe32(bytes.data() + kMagicOffset, kMagic);
    writeLe16(bytes.data() + kVersionOffset, kSpectatorVersion);
    bytes[kKindOffset] = static_cast<uint8_t>(kind);
    bytes[kHeaderReservedOffset] = 0u;
    writeLe64(bytes.data() + kRoomLowOffset, room.low);
    writeLe64(bytes.data() + kRoomHighOffset, room.high);
    bytes[kDomainOffset] = static_cast<uint8_t>(LaneDomain::Spectator);
    bytes[kPartyOffset] = static_cast<uint8_t>(party);
}

DecodeError decodeSpectatorPrefix(const uint8_t* const bytes,
                                  const size_t length,
                                  const SpectatorMessageKind expected,
                                  SpectatorParty& party,
                                  RoomId& room) {
    SpectatorMessageKind kind{};
    const DecodeError header =
        inspectSpectatorMessageKind(bytes, length, kind);
    if (header != DecodeError::None)
        return header;
    if (kind != expected)
        return DecodeError::BadKind;
    if (static_cast<LaneDomain>(bytes[kDomainOffset])
        != LaneDomain::Spectator) {
        return DecodeError::BadDomain;
    }
    const SpectatorParty decodedParty =
        static_cast<SpectatorParty>(bytes[kPartyOffset]);
    if (!isValidParty(decodedParty))
        return DecodeError::InvalidParty;
    const RoomId decodedRoom = decodeRoom(bytes);
    if (!isValidId(decodedRoom))
        return DecodeError::InvalidRoom;
    party = decodedParty;
    room = decodedRoom;
    return DecodeError::None;
}

}

bool isValidParty(const SpectatorParty party) {
    return party == SpectatorParty::Source
        || party == SpectatorParty::Viewer;
}

bool isValidRouteKind(const SpectatorRouteKind route) {
    switch (route) {
        case SpectatorRouteKind::Direct:
        case SpectatorRouteKind::Punched:
        case SpectatorRouteKind::Relay:
            return true;
    }
    return false;
}

bool isValidSpectatorAdmission(const SpectatorAdmission admission) {
    return admission == SpectatorAdmission::None
        || admission == SpectatorAdmission::Grant
        || admission == SpectatorAdmission::PublicToken;
}

bool isValidSpectatorStatus(const SpectatorStatus status) {
    switch (status) {
        case SpectatorStatus::Accepted:
        case SpectatorStatus::Waiting:
        case SpectatorStatus::RoomNotFound:
        case SpectatorStatus::SourceUnavailable:
        case SpectatorStatus::CapacityReached:
        case SpectatorStatus::GrantRejected:
        case SpectatorStatus::GrantExpired:
        case SpectatorStatus::Superseded:
        case SpectatorStatus::Unsupported:
        case SpectatorStatus::RateLimited:
            return true;
    }
    return false;
}

bool isValidDetachReason(const SpectatorDetachReason reason) {
    switch (reason) {
        case SpectatorDetachReason::ViewerRequested:
        case SpectatorDetachReason::Expired:
        case SpectatorDetachReason::SourceLost:
        case SpectatorDetachReason::Superseded:
        case SpectatorDetachReason::CapacityReclaimed:
        case SpectatorDetachReason::ServiceStopping:
        case SpectatorDetachReason::ProtocolViolation:
        case SpectatorDetachReason::RateLimited:
        case SpectatorDetachReason::EndpointMigrated:
            return true;
    }
    return false;
}

size_t spectatorEncodedSize(const SpectatorMessageKind kind) {
    switch (kind) {
        case SpectatorMessageKind::Hello: return kSpectatorHelloSize;
        case SpectatorMessageKind::Welcome: return kSpectatorWelcomeSize;
        case SpectatorMessageKind::Register: return kSpectatorRegisterSize;
        case SpectatorMessageKind::Ready: return kSpectatorReadySize;
        case SpectatorMessageKind::Detach: return kSpectatorDetachSize;
    }
    return 0u;
}

bool peekRelayVersion(const uint8_t* const bytes,
                      const size_t length,
                      uint16_t& out) {
    if (!hasRelayMagic(bytes, length) || length < kHeaderSize)
        return false;
    out = readLe16(bytes + kVersionOffset);
    return true;
}

DecodeError inspectSpectatorMessageKind(const uint8_t* const bytes,
                                        const size_t length,
                                        SpectatorMessageKind& kind) {
    if (bytes == nullptr || length < kHeaderSize)
        return DecodeError::TooShort;
    if (readLe32(bytes + kMagicOffset) != kMagic)
        return DecodeError::BadMagic;
    if (readLe16(bytes + kVersionOffset) != kSpectatorVersion)
        return DecodeError::BadVersion;
    const SpectatorMessageKind decoded =
        static_cast<SpectatorMessageKind>(bytes[kKindOffset]);
    if (!isKnownSpectatorKind(decoded))
        return DecodeError::BadKind;
    if (length != spectatorEncodedSize(decoded))
        return DecodeError::BadLength;
    if (bytes[kHeaderReservedOffset] != 0u)
        return DecodeError::BadReserved;
    kind = decoded;
    return DecodeError::None;
}

bool encodeSpectatorHello(const SpectatorHello& message,
                          std::vector<uint8_t>& out) {
    if (!isValidId(message.room) || !isValidParty(message.party))
        return false;
    if (message.sessionId == 0u)
        return false;
    const bool viewer = message.party == SpectatorParty::Viewer;
    if (viewer) {
        if (!isValidRouteKind(message.route)
            || message.admission == SpectatorAdmission::None) {
            return false;
        }
        if (message.admission == SpectatorAdmission::Grant) {
            if (isValidId(message.sourceToken)
                || message.grantId == 0u
                || message.viewerId == 0u
                || message.expiryUnix == 0u
                || message.sourceGeneration == 0u
                || (message.macLow == 0u && message.macHigh == 0u)) {
                return false;
            }
        } else if (message.admission
                   == SpectatorAdmission::PublicToken) {
            if (!isValidId(message.sourceToken)
                || message.route != SpectatorRouteKind::Relay
                || message.grantId != 0u
                || message.viewerId != 0u
                || message.expiryUnix != 0u
                || message.sourceGeneration != 0u
                || message.macLow != 0u
                || message.macHigh != 0u) {
                return false;
            }
        } else {
            return false;
        }
    } else {
        if (message.admission != SpectatorAdmission::None
            || !isValidId(message.sourceToken)
            || message.grantId != 0u
            || message.viewerId != 0u
            || message.expiryUnix != 0u
            || message.resumeRecord != 0u
            || message.resumeDigest != 0u
            || message.macLow != 0u
            || message.macHigh != 0u) {
            return false;
        }
    }

    std::vector<uint8_t> encoded(kSpectatorHelloSize, 0u);
    encodeSpectatorHeader(
        encoded, SpectatorMessageKind::Hello, message.room, message.party);
    encoded[26u] = viewer ? static_cast<uint8_t>(message.route) : 0u;
    encoded[27u] = static_cast<uint8_t>(message.admission);
    writeLe64(encoded.data() + 28u, message.sourceToken.low);
    writeLe64(encoded.data() + 36u, message.sourceToken.high);
    writeLe64(encoded.data() + 44u, message.sessionId);
    writeLe64(encoded.data() + 52u, message.grantId);
    writeLe64(encoded.data() + 60u, message.viewerId);
    writeLe64(encoded.data() + 68u, message.expiryUnix);
    writeLe32(encoded.data() + 76u, message.sourceGeneration);
    writeLe32(encoded.data() + 80u, message.resumeRecord);
    writeLe64(encoded.data() + 84u, message.resumeDigest);
    writeLe64(encoded.data() + 92u, message.macLow);
    writeLe64(encoded.data() + 100u, message.macHigh);
    out.swap(encoded);
    return true;
}

DecodeError decodeSpectatorHello(const uint8_t* const bytes,
                                 const size_t length,
                                 SpectatorHello& out) {
    SpectatorParty party{};
    RoomId room{};
    const DecodeError prefix = decodeSpectatorPrefix(
        bytes, length, SpectatorMessageKind::Hello, party, room);
    if (prefix != DecodeError::None)
        return prefix;
    if (!allZero(bytes + 108u, 4u))
        return DecodeError::BadReserved;

    SpectatorHello decoded{};
    decoded.room = room;
    decoded.party = party;
    decoded.admission =
        static_cast<SpectatorAdmission>(bytes[27u]);
    if (!isValidSpectatorAdmission(decoded.admission))
        return DecodeError::InvalidGrant;
    decoded.sourceToken = {
        readLe64(bytes + 28u),
        readLe64(bytes + 36u),
    };
    decoded.sessionId = readLe64(bytes + 44u);
    decoded.grantId = readLe64(bytes + 52u);
    decoded.viewerId = readLe64(bytes + 60u);
    decoded.expiryUnix = readLe64(bytes + 68u);
    decoded.sourceGeneration = readLe32(bytes + 76u);
    decoded.resumeRecord = readLe32(bytes + 80u);
    decoded.resumeDigest = readLe64(bytes + 84u);
    decoded.macLow = readLe64(bytes + 92u);
    decoded.macHigh = readLe64(bytes + 100u);
    if (decoded.sessionId == 0u)
        return DecodeError::InvalidGrant;

    if (party == SpectatorParty::Viewer) {
        const SpectatorRouteKind route =
            static_cast<SpectatorRouteKind>(bytes[26u]);
        if (!isValidRouteKind(route))
            return DecodeError::BadRole;
        decoded.route = route;
        if (decoded.admission == SpectatorAdmission::Grant) {
            if (isValidId(decoded.sourceToken))
                return DecodeError::InvalidToken;
            if (decoded.grantId == 0u || decoded.viewerId == 0u
                || decoded.expiryUnix == 0u
                || decoded.sourceGeneration == 0u
                || (decoded.macLow == 0u && decoded.macHigh == 0u)) {
                return DecodeError::InvalidGrant;
            }
        } else if (decoded.admission
                   == SpectatorAdmission::PublicToken) {
            if (!isValidId(decoded.sourceToken))
                return DecodeError::InvalidToken;
            if (decoded.route != SpectatorRouteKind::Relay
                || decoded.grantId != 0u || decoded.viewerId != 0u
                || decoded.expiryUnix != 0u
                || decoded.sourceGeneration != 0u
                || decoded.macLow != 0u || decoded.macHigh != 0u) {
                return DecodeError::InvalidGrant;
            }
        } else {
            return DecodeError::InvalidGrant;
        }
    } else {
        if (bytes[26u] != 0u)
            return DecodeError::BadReserved;
        if (decoded.admission != SpectatorAdmission::None)
            return DecodeError::InvalidGrant;
        if (!isValidId(decoded.sourceToken))
            return DecodeError::InvalidToken;
        if (decoded.grantId != 0u || decoded.viewerId != 0u
            || decoded.expiryUnix != 0u || decoded.resumeRecord != 0u
            || decoded.resumeDigest != 0u || decoded.macLow != 0u
            || decoded.macHigh != 0u) {
            return DecodeError::InvalidGrant;
        }
    }
    out = decoded;
    return DecodeError::None;
}

bool encodeSpectatorWelcome(const SpectatorWelcome& message,
                            std::vector<uint8_t>& out) {
    if (!isValidId(message.room) || !isValidParty(message.party)
        || !isValidSpectatorStatus(message.status)) {
        return false;
    }
    const bool accepted = message.status == SpectatorStatus::Accepted
        || message.status == SpectatorStatus::Waiting;
    if (accepted != isValidId(message.registrationToken))
        return false;
    const bool sourceKey = accepted
        && message.party == SpectatorParty::Source;
    if (sourceKey) {
        if (message.grantKey.size() != kGrantKeyBytes)
            return false;
    } else if (!message.grantKey.empty()) {
        return false;
    }
    if (message.party == SpectatorParty::Source && message.viewerId != 0u)
        return false;

    std::vector<uint8_t> encoded(kSpectatorWelcomeSize, 0u);
    encodeSpectatorHeader(
        encoded, SpectatorMessageKind::Welcome, message.room, message.party);
    encoded[26u] = static_cast<uint8_t>(message.status);
    writeLe64(encoded.data() + 28u, message.registrationToken.low);
    writeLe64(encoded.data() + 36u, message.registrationToken.high);
    writeLe64(encoded.data() + 44u, message.viewerId);
    writeLe32(encoded.data() + 52u, message.sourceGeneration);
    writeLe16(encoded.data() + 56u, message.viewerCount);
    writeLe16(encoded.data() + 58u, message.viewerCapacity);
    writeLe32(encoded.data() + 60u, message.retryAfterMs);
    if (sourceKey) {
        for (size_t i = 0u; i < kGrantKeyBytes; ++i)
            encoded[64u + i] = message.grantKey[i];
    }
    writeLe64(encoded.data() + 96u, message.sessionId);
    out.swap(encoded);
    return true;
}

DecodeError decodeSpectatorWelcome(const uint8_t* const bytes,
                                   const size_t length,
                                   SpectatorWelcome& out) {
    SpectatorParty party{};
    RoomId room{};
    const DecodeError prefix = decodeSpectatorPrefix(
        bytes, length, SpectatorMessageKind::Welcome, party, room);
    if (prefix != DecodeError::None)
        return prefix;
    if (bytes[27u] != 0u || !allZero(bytes + 104u, 8u))
        return DecodeError::BadReserved;
    const SpectatorStatus status =
        static_cast<SpectatorStatus>(bytes[26u]);
    if (!isValidSpectatorStatus(status))
        return DecodeError::BadStatus;

    SpectatorWelcome decoded{};
    decoded.room = room;
    decoded.party = party;
    decoded.status = status;
    decoded.registrationToken = {
        readLe64(bytes + 28u),
        readLe64(bytes + 36u),
    };
    decoded.viewerId = readLe64(bytes + 44u);
    decoded.sourceGeneration = readLe32(bytes + 52u);
    decoded.viewerCount = readLe16(bytes + 56u);
    decoded.viewerCapacity = readLe16(bytes + 58u);
    decoded.retryAfterMs = readLe32(bytes + 60u);
    decoded.sessionId = readLe64(bytes + 96u);

    const bool accepted = status == SpectatorStatus::Accepted
        || status == SpectatorStatus::Waiting;
    if (accepted != isValidId(decoded.registrationToken))
        return DecodeError::InvalidToken;
    const bool sourceKey = accepted && party == SpectatorParty::Source;
    if (sourceKey) {
        if (allZero(bytes + 64u, kGrantKeyBytes))
            return DecodeError::InvalidGrant;
        decoded.grantKey.assign(bytes + 64u, bytes + 64u + kGrantKeyBytes);
    } else if (!allZero(bytes + 64u, kGrantKeyBytes)) {
        return DecodeError::BadReserved;
    }
    if (party == SpectatorParty::Source && decoded.viewerId != 0u)
        return DecodeError::InvalidGrant;
    out = decoded;
    return DecodeError::None;
}

bool encodeSpectatorRegister(const SpectatorRegister& message,
                             std::vector<uint8_t>& out) {
    if (!isValidId(message.room) || !isValidParty(message.party)
        || !isValidId(message.token)) {
        return false;
    }
    if ((message.party == SpectatorParty::Viewer) != (message.viewerId != 0u))
        return false;

    std::vector<uint8_t> encoded(kSpectatorRegisterSize, 0u);
    encodeSpectatorHeader(
        encoded, SpectatorMessageKind::Register, message.room, message.party);
    writeLe32(encoded.data() + 28u, message.sourceGeneration);
    writeLe64(encoded.data() + 32u, message.token.low);
    writeLe64(encoded.data() + 40u, message.token.high);
    writeLe64(encoded.data() + 48u, message.viewerId);
    out.swap(encoded);
    return true;
}

DecodeError decodeSpectatorRegister(const uint8_t* const bytes,
                                    const size_t length,
                                    SpectatorRegister& out) {
    SpectatorParty party{};
    RoomId room{};
    const DecodeError prefix = decodeSpectatorPrefix(
        bytes, length, SpectatorMessageKind::Register, party, room);
    if (prefix != DecodeError::None)
        return prefix;
    if (!allZero(bytes + 26u, 2u) || !allZero(bytes + 56u, 8u))
        return DecodeError::BadReserved;

    SpectatorRegister decoded{};
    decoded.room = room;
    decoded.party = party;
    decoded.sourceGeneration = readLe32(bytes + 28u);
    decoded.token = {
        readLe64(bytes + 32u),
        readLe64(bytes + 40u),
    };
    decoded.viewerId = readLe64(bytes + 48u);
    if (!isValidId(decoded.token))
        return DecodeError::InvalidToken;
    if ((party == SpectatorParty::Viewer) != (decoded.viewerId != 0u))
        return DecodeError::InvalidGrant;
    out = decoded;
    return DecodeError::None;
}

bool encodeSpectatorReady(const SpectatorReady& message,
                          std::vector<uint8_t>& out) {
    if (!isValidId(message.room) || !isValidParty(message.party)
        || !isValidId(message.token)
        || !isValidEndpoint(message.peer)) {
        return false;
    }
    if (message.viewerId == 0u)
        return false;

    std::vector<uint8_t> encoded(kSpectatorReadySize, 0u);
    encodeSpectatorHeader(
        encoded, SpectatorMessageKind::Ready, message.room, message.party);
    writeLe32(encoded.data() + 28u, message.sourceGeneration);
    writeLe64(encoded.data() + 32u, message.token.low);
    writeLe64(encoded.data() + 40u, message.token.high);
    writeLe64(encoded.data() + 48u, message.viewerId);
    writeLe32(encoded.data() + 56u, message.peer.ipv4);
    writeLe16(encoded.data() + 60u, message.peer.port);
    out.swap(encoded);
    return true;
}

DecodeError decodeSpectatorReady(const uint8_t* const bytes,
                                 const size_t length,
                                 SpectatorReady& out) {
    SpectatorParty party{};
    RoomId room{};
    const DecodeError prefix = decodeSpectatorPrefix(
        bytes, length, SpectatorMessageKind::Ready, party, room);
    if (prefix != DecodeError::None)
        return prefix;
    if (!allZero(bytes + 26u, 2u) || !allZero(bytes + 62u, 10u))
        return DecodeError::BadReserved;

    SpectatorReady decoded{};
    decoded.room = room;
    decoded.party = party;
    decoded.sourceGeneration = readLe32(bytes + 28u);
    decoded.token = {
        readLe64(bytes + 32u),
        readLe64(bytes + 40u),
    };
    decoded.viewerId = readLe64(bytes + 48u);
    decoded.peer = {
        readLe32(bytes + 56u),
        readLe16(bytes + 60u),
    };
    if (!isValidId(decoded.token))
        return DecodeError::InvalidToken;
    if (decoded.viewerId == 0u)
        return DecodeError::InvalidGrant;
    if (!isValidEndpoint(decoded.peer))
        return DecodeError::InvalidEndpoint;
    out = decoded;
    return DecodeError::None;
}

bool encodeSpectatorDetach(const SpectatorDetach& message,
                           std::vector<uint8_t>& out) {
    if (!isValidId(message.room) || !isValidParty(message.party)
        || !isValidDetachReason(message.reason)
        || !isValidId(message.token)) {
        return false;
    }
    if (message.party == SpectatorParty::Viewer && message.viewerId == 0u)
        return false;
    std::vector<uint8_t> encoded(kSpectatorDetachSize, 0u);
    encodeSpectatorHeader(
        encoded, SpectatorMessageKind::Detach, message.room, message.party);
    writeLe16(encoded.data() + 26u, static_cast<uint16_t>(message.reason));
    writeLe32(encoded.data() + 28u, message.sourceGeneration);
    writeLe64(encoded.data() + 32u, message.token.low);
    writeLe64(encoded.data() + 40u, message.token.high);
    writeLe64(encoded.data() + 48u, message.viewerId);
    writeLe32(encoded.data() + 56u, message.retryAfterMs);
    out.swap(encoded);
    return true;
}

DecodeError decodeSpectatorDetach(const uint8_t* const bytes,
                                  const size_t length,
                                  SpectatorDetach& out) {
    SpectatorParty party{};
    RoomId room{};
    const DecodeError prefix = decodeSpectatorPrefix(
        bytes, length, SpectatorMessageKind::Detach, party, room);
    if (prefix != DecodeError::None)
        return prefix;
    if (!allZero(bytes + 60u, 4u))
        return DecodeError::BadReserved;
    const SpectatorDetachReason reason =
        static_cast<SpectatorDetachReason>(readLe16(bytes + 26u));
    if (!isValidDetachReason(reason))
        return DecodeError::BadStatus;

    SpectatorDetach decoded{};
    decoded.room = room;
    decoded.party = party;
    decoded.reason = reason;
    decoded.sourceGeneration = readLe32(bytes + 28u);
    decoded.token = {
        readLe64(bytes + 32u),
        readLe64(bytes + 40u),
    };
    decoded.viewerId = readLe64(bytes + 48u);
    decoded.retryAfterMs = readLe32(bytes + 56u);
    if (!isValidId(decoded.token))
        return DecodeError::InvalidToken;
    if (party == SpectatorParty::Viewer && decoded.viewerId == 0u)
        return DecodeError::InvalidGrant;
    out = decoded;
    return DecodeError::None;
}

bool encodeSpectatorFrameHeader(const SpectatorFrameHeader& header,
                                std::vector<uint8_t>& out) {
    if (!isValidParty(header.party) || header.viewerId == 0u)
        return false;
    std::vector<uint8_t> encoded(kSpectatorFrameHeaderSize, 0u);
    writeLe32(encoded.data() + kMagicOffset, kMagic);
    writeLe16(encoded.data() + kVersionOffset, kSpectatorVersion);
    encoded[kKindOffset] = kSpectatorFrameKind;
    encoded[kHeaderReservedOffset] = static_cast<uint8_t>(header.party);
    writeLe64(encoded.data() + 8u, header.viewerId);
    writeLe32(encoded.data() + 16u, header.sourceGeneration);
    out.swap(encoded);
    return true;
}

bool hasSpectatorFrameKind(const uint8_t* const bytes, const size_t length) {
    uint16_t version = 0u;
    return peekRelayVersion(bytes, length, version)
        && version == kSpectatorVersion
        && bytes[kKindOffset] == kSpectatorFrameKind;
}

DecodeError decodeSpectatorFrameHeader(const uint8_t* const bytes,
                                       const size_t length,
                                       SpectatorFrameHeader& out) {
    if (bytes == nullptr || length < kSpectatorFrameHeaderSize)
        return DecodeError::TooShort;
    if (readLe32(bytes + kMagicOffset) != kMagic)
        return DecodeError::BadMagic;
    if (readLe16(bytes + kVersionOffset) != kSpectatorVersion)
        return DecodeError::BadVersion;
    if (bytes[kKindOffset] != kSpectatorFrameKind)
        return DecodeError::BadKind;
    const SpectatorParty party =
        static_cast<SpectatorParty>(bytes[kHeaderReservedOffset]);
    if (!isValidParty(party))
        return DecodeError::InvalidParty;
    if (!allZero(bytes + 20u, 4u))
        return DecodeError::BadReserved;
    SpectatorFrameHeader decoded{};
    decoded.party = party;
    decoded.viewerId = readLe64(bytes + 8u);
    decoded.sourceGeneration = readLe32(bytes + 16u);
    if (decoded.viewerId == 0u)
        return DecodeError::InvalidGrant;
    out = decoded;
    return DecodeError::None;
}

namespace {

constexpr size_t kTopologyAdmissionOffset = 26u;
constexpr size_t kTopologyStatusOffset = 26u;

bool isKnownTopologyKind(const TopologyMessageKind kind) {
    switch (kind) {
        case TopologyMessageKind::Hello:
        case TopologyMessageKind::Welcome:
        case TopologyMessageKind::Register:
        case TopologyMessageKind::Ready:
        case TopologyMessageKind::Detach:
            return true;
    }
    return false;
}

void encodeTopologyHeader(std::vector<uint8_t>& bytes,
                          const TopologyMessageKind kind,
                          const RoomId& room,
                          const TopologyParty party) {
    writeLe32(bytes.data() + kMagicOffset, kMagic);
    writeLe16(bytes.data() + kVersionOffset, kTopologyVersion);
    bytes[kKindOffset] = static_cast<uint8_t>(kind);
    bytes[kHeaderReservedOffset] = 0u;
    writeLe64(bytes.data() + kRoomLowOffset, room.low);
    writeLe64(bytes.data() + kRoomHighOffset, room.high);
    bytes[kDomainOffset] = static_cast<uint8_t>(LaneDomain::Topology);
    bytes[kPartyOffset] = static_cast<uint8_t>(party);
}

DecodeError decodeTopologyPrefix(const uint8_t* const bytes,
                                 const size_t length,
                                 const TopologyMessageKind expected,
                                 TopologyParty& party,
                                 RoomId& room) {
    TopologyMessageKind kind{};
    const DecodeError header =
        inspectTopologyMessageKind(bytes, length, kind);
    if (header != DecodeError::None)
        return header;
    if (kind != expected)
        return DecodeError::BadKind;
    if (static_cast<LaneDomain>(bytes[kDomainOffset])
        != LaneDomain::Topology) {
        return DecodeError::BadDomain;
    }
    const TopologyParty decodedParty =
        static_cast<TopologyParty>(bytes[kPartyOffset]);
    if (!isValidTopologyParty(decodedParty))
        return DecodeError::InvalidParty;
    const RoomId decodedRoom = decodeRoom(bytes);
    if (!isValidId(decodedRoom))
        return DecodeError::InvalidRoom;
    party = decodedParty;
    room = decodedRoom;
    return DecodeError::None;
}

bool validTopologyBindingBody(const TopologyParty party,
                              const uint32_t authorityGeneration,
                              const RegistrationToken& token,
                              const uint64_t nodeId,
                              const bool authorityMayNameNode) {
    if (authorityGeneration == 0u || !isValidId(token))
        return false;
    if (party == TopologyParty::Node)
        return nodeId != 0u;
    return authorityMayNameNode || nodeId == 0u;
}

}

bool isValidTopologyParty(const TopologyParty party) {
    return party == TopologyParty::Authority
        || party == TopologyParty::Node;
}

bool isValidTopologyAdmission(const TopologyAdmission admission) {
    return admission == TopologyAdmission::None
        || admission == TopologyAdmission::Grant;
}

bool isValidTopologyStatus(const TopologyStatus status) {
    switch (status) {
        case TopologyStatus::Accepted:
        case TopologyStatus::Waiting:
        case TopologyStatus::RoomNotFound:
        case TopologyStatus::AuthorityUnavailable:
        case TopologyStatus::CapacityReached:
        case TopologyStatus::GrantRejected:
        case TopologyStatus::GrantExpired:
        case TopologyStatus::Superseded:
        case TopologyStatus::Unsupported:
        case TopologyStatus::RateLimited:
            return true;
    }
    return false;
}

bool isValidTopologyDetachReason(const TopologyDetachReason reason) {
    switch (reason) {
        case TopologyDetachReason::NodeRequested:
        case TopologyDetachReason::Expired:
        case TopologyDetachReason::AuthorityLost:
        case TopologyDetachReason::Superseded:
        case TopologyDetachReason::CapacityReclaimed:
        case TopologyDetachReason::ServiceStopping:
        case TopologyDetachReason::ProtocolViolation:
        case TopologyDetachReason::RateLimited:
        case TopologyDetachReason::EndpointMigrated:
            return true;
    }
    return false;
}

size_t topologyEncodedSize(const TopologyMessageKind kind) {
    switch (kind) {
        case TopologyMessageKind::Hello: return kTopologyHelloSize;
        case TopologyMessageKind::Welcome: return kTopologyWelcomeSize;
        case TopologyMessageKind::Register: return kTopologyRegisterSize;
        case TopologyMessageKind::Ready: return kTopologyReadySize;
        case TopologyMessageKind::Detach: return kTopologyDetachSize;
    }
    return 0u;
}

DecodeError inspectTopologyMessageKind(const uint8_t* const bytes,
                                       const size_t length,
                                       TopologyMessageKind& kind) {
    if (bytes == nullptr || length < kHeaderSize)
        return DecodeError::TooShort;
    if (readLe32(bytes + kMagicOffset) != kMagic)
        return DecodeError::BadMagic;
    if (readLe16(bytes + kVersionOffset) != kTopologyVersion)
        return DecodeError::BadVersion;
    const TopologyMessageKind decoded =
        static_cast<TopologyMessageKind>(bytes[kKindOffset]);
    if (!isKnownTopologyKind(decoded))
        return DecodeError::BadKind;
    if (length != topologyEncodedSize(decoded))
        return DecodeError::BadLength;
    if (bytes[kHeaderReservedOffset] != 0u)
        return DecodeError::BadReserved;
    kind = decoded;
    return DecodeError::None;
}

bool encodeTopologyHello(const TopologyHello& message,
                         std::vector<uint8_t>& out) {
    if (!isValidId(message.room) || !isValidTopologyParty(message.party)
        || message.sessionId == 0u) {
        return false;
    }
    if (message.party == TopologyParty::Authority) {
        if (message.admission != TopologyAdmission::None
            || !isValidId(message.authorityToken)
            || message.grantId != 0u || message.nodeId != 0u
            || message.expiryUnix != 0u
            || message.authorityGeneration != 0u
            || message.macLow != 0u || message.macHigh != 0u) {
            return false;
        }
    } else {
        if (message.admission != TopologyAdmission::Grant
            || isValidId(message.authorityToken)
            || message.grantId == 0u || message.nodeId == 0u
            || message.expiryUnix == 0u
            || message.authorityGeneration == 0u
            || (message.macLow == 0u && message.macHigh == 0u)) {
            return false;
        }
    }

    std::vector<uint8_t> encoded(kTopologyHelloSize, 0u);
    encodeTopologyHeader(
        encoded, TopologyMessageKind::Hello, message.room, message.party);
    encoded[kTopologyAdmissionOffset] =
        static_cast<uint8_t>(message.admission);
    writeLe64(encoded.data() + 28u, message.authorityToken.low);
    writeLe64(encoded.data() + 36u, message.authorityToken.high);
    writeLe64(encoded.data() + 44u, message.sessionId);
    writeLe64(encoded.data() + 52u, message.grantId);
    writeLe64(encoded.data() + 60u, message.nodeId);
    writeLe64(encoded.data() + 68u, message.expiryUnix);
    writeLe32(encoded.data() + 76u, message.authorityGeneration);
    writeLe64(encoded.data() + 80u, message.macLow);
    writeLe64(encoded.data() + 88u, message.macHigh);
    out.swap(encoded);
    return true;
}

DecodeError decodeTopologyHello(const uint8_t* const bytes,
                                const size_t length,
                                TopologyHello& out) {
    TopologyParty party{};
    RoomId room{};
    const DecodeError prefix = decodeTopologyPrefix(
        bytes, length, TopologyMessageKind::Hello, party, room);
    if (prefix != DecodeError::None)
        return prefix;
    if (bytes[27u] != 0u || !allZero(bytes + 96u, 4u))
        return DecodeError::BadReserved;

    TopologyHello decoded{};
    decoded.room = room;
    decoded.party = party;
    decoded.admission =
        static_cast<TopologyAdmission>(bytes[kTopologyAdmissionOffset]);
    if (!isValidTopologyAdmission(decoded.admission))
        return DecodeError::InvalidGrant;
    decoded.authorityToken = {
        readLe64(bytes + 28u),
        readLe64(bytes + 36u),
    };
    decoded.sessionId = readLe64(bytes + 44u);
    decoded.grantId = readLe64(bytes + 52u);
    decoded.nodeId = readLe64(bytes + 60u);
    decoded.expiryUnix = readLe64(bytes + 68u);
    decoded.authorityGeneration = readLe32(bytes + 76u);
    decoded.macLow = readLe64(bytes + 80u);
    decoded.macHigh = readLe64(bytes + 88u);
    if (decoded.sessionId == 0u)
        return DecodeError::InvalidGrant;

    if (party == TopologyParty::Authority) {
        if (decoded.admission != TopologyAdmission::None)
            return DecodeError::InvalidGrant;
        if (!isValidId(decoded.authorityToken))
            return DecodeError::InvalidToken;
        if (decoded.grantId != 0u || decoded.nodeId != 0u
            || decoded.expiryUnix != 0u
            || decoded.authorityGeneration != 0u
            || decoded.macLow != 0u || decoded.macHigh != 0u) {
            return DecodeError::InvalidGrant;
        }
    } else {
        if (decoded.admission != TopologyAdmission::Grant)
            return DecodeError::InvalidGrant;
        if (isValidId(decoded.authorityToken))
            return DecodeError::InvalidToken;
        if (decoded.grantId == 0u || decoded.nodeId == 0u
            || decoded.expiryUnix == 0u
            || decoded.authorityGeneration == 0u
            || (decoded.macLow == 0u && decoded.macHigh == 0u)) {
            return DecodeError::InvalidGrant;
        }
    }
    out = decoded;
    return DecodeError::None;
}

bool encodeTopologyWelcome(const TopologyWelcome& message,
                           std::vector<uint8_t>& out) {
    if (!isValidId(message.room) || !isValidTopologyParty(message.party)
        || !isValidTopologyStatus(message.status)) {
        return false;
    }
    const bool admitted = message.status == TopologyStatus::Accepted
        || message.status == TopologyStatus::Waiting;
    const bool carriesKey = message.party == TopologyParty::Authority
        && message.status == TopologyStatus::Accepted;
    if (admitted) {
        if (!isValidId(message.registrationToken)
            || message.sessionId == 0u
            || message.authorityGeneration == 0u) {
            return false;
        }
        if ((message.party == TopologyParty::Node)
            != (message.nodeId != 0u)) {
            return false;
        }
    } else {
        if (isValidId(message.registrationToken) || message.nodeId != 0u
            || message.sessionId != 0u
            || message.authorityGeneration != 0u
            || message.nodeCount != 0u || message.nodeCapacity != 0u) {
            return false;
        }
    }
    if (carriesKey != (message.grantKey.size() == kGrantKeyBytes))
        return false;
    if (!carriesKey && !message.grantKey.empty())
        return false;
    if (carriesKey
        && allZero(message.grantKey.data(), message.grantKey.size())) {
        return false;
    }

    std::vector<uint8_t> encoded(kTopologyWelcomeSize, 0u);
    encodeTopologyHeader(
        encoded, TopologyMessageKind::Welcome, message.room, message.party);
    encoded[kTopologyStatusOffset] = static_cast<uint8_t>(message.status);
    writeLe64(encoded.data() + 28u, message.registrationToken.low);
    writeLe64(encoded.data() + 36u, message.registrationToken.high);
    writeLe64(encoded.data() + 44u, message.nodeId);
    writeLe64(encoded.data() + 52u, message.sessionId);
    writeLe32(encoded.data() + 60u, message.authorityGeneration);
    writeLe16(encoded.data() + 64u, message.nodeCount);
    writeLe16(encoded.data() + 66u, message.nodeCapacity);
    writeLe32(encoded.data() + 68u, message.retryAfterMs);
    if (carriesKey) {
        std::copy(message.grantKey.begin(), message.grantKey.end(),
                  encoded.begin() + 72u);
    }
    out.swap(encoded);
    return true;
}

DecodeError decodeTopologyWelcome(const uint8_t* const bytes,
                                  const size_t length,
                                  TopologyWelcome& out) {
    TopologyParty party{};
    RoomId room{};
    const DecodeError prefix = decodeTopologyPrefix(
        bytes, length, TopologyMessageKind::Welcome, party, room);
    if (prefix != DecodeError::None)
        return prefix;
    if (bytes[27u] != 0u)
        return DecodeError::BadReserved;

    TopologyWelcome decoded{};
    decoded.room = room;
    decoded.party = party;
    decoded.status =
        static_cast<TopologyStatus>(bytes[kTopologyStatusOffset]);
    if (!isValidTopologyStatus(decoded.status))
        return DecodeError::BadRole;
    decoded.registrationToken = {
        readLe64(bytes + 28u),
        readLe64(bytes + 36u),
    };
    decoded.nodeId = readLe64(bytes + 44u);
    decoded.sessionId = readLe64(bytes + 52u);
    decoded.authorityGeneration = readLe32(bytes + 60u);
    decoded.nodeCount = readLe16(bytes + 64u);
    decoded.nodeCapacity = readLe16(bytes + 66u);
    decoded.retryAfterMs = readLe32(bytes + 68u);

    const bool admitted = decoded.status == TopologyStatus::Accepted
        || decoded.status == TopologyStatus::Waiting;
    const bool carriesKey = party == TopologyParty::Authority
        && decoded.status == TopologyStatus::Accepted;
    if (admitted) {
        if (!isValidId(decoded.registrationToken))
            return DecodeError::InvalidToken;
        if (decoded.sessionId == 0u
            || decoded.authorityGeneration == 0u
            || ((party == TopologyParty::Node)
                != (decoded.nodeId != 0u))) {
            return DecodeError::InvalidGrant;
        }
    } else {
        if (isValidId(decoded.registrationToken))
            return DecodeError::InvalidToken;
        if (decoded.nodeId != 0u || decoded.sessionId != 0u
            || decoded.authorityGeneration != 0u
            || decoded.nodeCount != 0u || decoded.nodeCapacity != 0u) {
            return DecodeError::InvalidGrant;
        }
    }
    if (carriesKey) {
        if (allZero(bytes + 72u, kGrantKeyBytes))
            return DecodeError::InvalidGrant;
        decoded.grantKey.assign(bytes + 72u, bytes + 72u + kGrantKeyBytes);
    } else if (!allZero(bytes + 72u, kGrantKeyBytes)) {
        return DecodeError::BadReserved;
    }
    out = decoded;
    return DecodeError::None;
}

bool encodeTopologyRegister(const TopologyRegister& message,
                            std::vector<uint8_t>& out) {
    if (!isValidId(message.room) || !isValidTopologyParty(message.party)
        || !validTopologyBindingBody(message.party,
                                     message.authorityGeneration,
                                     message.token, message.nodeId,
                                     false)) {
        return false;
    }
    std::vector<uint8_t> encoded(kTopologyRegisterSize, 0u);
    encodeTopologyHeader(
        encoded, TopologyMessageKind::Register, message.room, message.party);
    writeLe32(encoded.data() + 28u, message.authorityGeneration);
    writeLe64(encoded.data() + 32u, message.token.low);
    writeLe64(encoded.data() + 40u, message.token.high);
    writeLe64(encoded.data() + 48u, message.nodeId);
    out.swap(encoded);
    return true;
}

DecodeError decodeTopologyRegister(const uint8_t* const bytes,
                                   const size_t length,
                                   TopologyRegister& out) {
    TopologyParty party{};
    RoomId room{};
    const DecodeError prefix = decodeTopologyPrefix(
        bytes, length, TopologyMessageKind::Register, party, room);
    if (prefix != DecodeError::None)
        return prefix;
    if (!allZero(bytes + 26u, 2u))
        return DecodeError::BadReserved;

    TopologyRegister decoded{};
    decoded.room = room;
    decoded.party = party;
    decoded.authorityGeneration = readLe32(bytes + 28u);
    decoded.token = { readLe64(bytes + 32u), readLe64(bytes + 40u) };
    decoded.nodeId = readLe64(bytes + 48u);
    if (!isValidId(decoded.token))
        return DecodeError::InvalidToken;
    if (!validTopologyBindingBody(party, decoded.authorityGeneration,
                                  decoded.token, decoded.nodeId,
                                  false)) {
        return DecodeError::InvalidGrant;
    }
    out = decoded;
    return DecodeError::None;
}

bool encodeTopologyReady(const TopologyReady& message,
                         std::vector<uint8_t>& out) {
    if (!isValidId(message.room) || !isValidTopologyParty(message.party)
        || !isValidEndpoint(message.peer)
        || !validTopologyBindingBody(message.party,
                                     message.authorityGeneration,
                                     message.token, message.nodeId,
                                     true)) {
        return false;
    }
    std::vector<uint8_t> encoded(kTopologyReadySize, 0u);
    encodeTopologyHeader(
        encoded, TopologyMessageKind::Ready, message.room, message.party);
    writeLe32(encoded.data() + 28u, message.authorityGeneration);
    writeLe64(encoded.data() + 32u, message.token.low);
    writeLe64(encoded.data() + 40u, message.token.high);
    writeLe64(encoded.data() + 48u, message.nodeId);
    writeLe32(encoded.data() + 56u, message.peer.ipv4);
    writeLe16(encoded.data() + 60u, message.peer.port);
    out.swap(encoded);
    return true;
}

DecodeError decodeTopologyReady(const uint8_t* const bytes,
                                const size_t length,
                                TopologyReady& out) {
    TopologyParty party{};
    RoomId room{};
    const DecodeError prefix = decodeTopologyPrefix(
        bytes, length, TopologyMessageKind::Ready, party, room);
    if (prefix != DecodeError::None)
        return prefix;
    if (!allZero(bytes + 26u, 2u) || !allZero(bytes + 62u, 2u))
        return DecodeError::BadReserved;

    TopologyReady decoded{};
    decoded.room = room;
    decoded.party = party;
    decoded.authorityGeneration = readLe32(bytes + 28u);
    decoded.token = { readLe64(bytes + 32u), readLe64(bytes + 40u) };
    decoded.nodeId = readLe64(bytes + 48u);
    decoded.peer.ipv4 = readLe32(bytes + 56u);
    decoded.peer.port = readLe16(bytes + 60u);
    if (!isValidId(decoded.token))
        return DecodeError::InvalidToken;
    if (!isValidEndpoint(decoded.peer)
        || !validTopologyBindingBody(party, decoded.authorityGeneration,
                                     decoded.token, decoded.nodeId,
                                     true)) {
        return DecodeError::InvalidGrant;
    }
    out = decoded;
    return DecodeError::None;
}

bool encodeTopologyDetach(const TopologyDetach& message,
                          std::vector<uint8_t>& out) {
    if (!isValidId(message.room) || !isValidTopologyParty(message.party)
        || !isValidTopologyDetachReason(message.reason)
        || !validTopologyBindingBody(message.party,
                                     message.authorityGeneration,
                                     message.token, message.nodeId,
                                     true)) {
        return false;
    }
    std::vector<uint8_t> encoded(kTopologyDetachSize, 0u);
    encodeTopologyHeader(
        encoded, TopologyMessageKind::Detach, message.room, message.party);
    writeLe16(encoded.data() + 26u, static_cast<uint16_t>(message.reason));
    writeLe32(encoded.data() + 28u, message.authorityGeneration);
    writeLe64(encoded.data() + 32u, message.token.low);
    writeLe64(encoded.data() + 40u, message.token.high);
    writeLe64(encoded.data() + 48u, message.nodeId);
    writeLe32(encoded.data() + 56u, message.retryAfterMs);
    out.swap(encoded);
    return true;
}

DecodeError decodeTopologyDetach(const uint8_t* const bytes,
                                 const size_t length,
                                 TopologyDetach& out) {
    TopologyParty party{};
    RoomId room{};
    const DecodeError prefix = decodeTopologyPrefix(
        bytes, length, TopologyMessageKind::Detach, party, room);
    if (prefix != DecodeError::None)
        return prefix;
    if (!allZero(bytes + 60u, 4u))
        return DecodeError::BadReserved;

    TopologyDetach decoded{};
    decoded.room = room;
    decoded.party = party;
    decoded.reason =
        static_cast<TopologyDetachReason>(readLe16(bytes + 26u));
    decoded.authorityGeneration = readLe32(bytes + 28u);
    decoded.token = { readLe64(bytes + 32u), readLe64(bytes + 40u) };
    decoded.nodeId = readLe64(bytes + 48u);
    decoded.retryAfterMs = readLe32(bytes + 56u);
    if (!isValidTopologyDetachReason(decoded.reason))
        return DecodeError::BadRole;
    if (!isValidId(decoded.token))
        return DecodeError::InvalidToken;
    if (!validTopologyBindingBody(party, decoded.authorityGeneration,
                                  decoded.token, decoded.nodeId,
                                  true)) {
        return DecodeError::InvalidGrant;
    }
    out = decoded;
    return DecodeError::None;
}

bool encodeTopologyFrameHeader(const TopologyFrameHeader& header,
                               std::vector<uint8_t>& out) {
    if (!isValidTopologyParty(header.party) || header.nodeId == 0u
        || header.authorityGeneration == 0u) {
        return false;
    }
    std::vector<uint8_t> encoded(kTopologyFrameHeaderSize, 0u);
    writeLe32(encoded.data() + kMagicOffset, kMagic);
    writeLe16(encoded.data() + kVersionOffset, kTopologyVersion);
    encoded[kKindOffset] = kTopologyFrameKind;
    encoded[kHeaderReservedOffset] = static_cast<uint8_t>(header.party);
    writeLe64(encoded.data() + 8u, header.nodeId);
    writeLe32(encoded.data() + 16u, header.authorityGeneration);
    out.swap(encoded);
    return true;
}

DecodeError decodeTopologyFrameHeader(const uint8_t* const bytes,
                                      const size_t length,
                                      TopologyFrameHeader& out) {
    if (bytes == nullptr || length < kTopologyFrameHeaderSize)
        return DecodeError::TooShort;
    if (readLe32(bytes + kMagicOffset) != kMagic)
        return DecodeError::BadMagic;
    if (readLe16(bytes + kVersionOffset) != kTopologyVersion)
        return DecodeError::BadVersion;
    if (bytes[kKindOffset] != kTopologyFrameKind)
        return DecodeError::BadKind;
    const TopologyParty party =
        static_cast<TopologyParty>(bytes[kHeaderReservedOffset]);
    if (!isValidTopologyParty(party))
        return DecodeError::InvalidParty;
    if (!allZero(bytes + 20u, 4u))
        return DecodeError::BadReserved;
    TopologyFrameHeader decoded{};
    decoded.party = party;
    decoded.nodeId = readLe64(bytes + 8u);
    decoded.authorityGeneration = readLe32(bytes + 16u);
    if (decoded.nodeId == 0u || decoded.authorityGeneration == 0u)
        return DecodeError::InvalidGrant;
    out = decoded;
    return DecodeError::None;
}

bool hasTopologyFrameKind(const uint8_t* const bytes, const size_t length) {
    return bytes != nullptr && length >= kHeaderSize
        && readLe32(bytes + kMagicOffset) == kMagic
        && readLe16(bytes + kVersionOffset) == kTopologyVersion
        && bytes[kKindOffset] == kTopologyFrameKind;
}

}
