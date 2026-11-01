// All addresses/ports are in network byte order (BE), matching the sockaddr fields used across src/net.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "net/RouteSelection.h"

namespace qnet {

constexpr uint32_t hostOrder(uint32_t addrBE) {
    return ((addrBE & 0x000000FFu) << 24)
         | ((addrBE & 0x0000FF00u) << 8)
         | ((addrBE & 0x00FF0000u) >> 8)
         | ((addrBE & 0xFF000000u) >> 24);
}

constexpr bool isLoopbackBE(uint32_t addrBE) {
    return (hostOrder(addrBE) & 0xFF000000u) == 0x7F000000u;
}

constexpr uint32_t kLoopbackIpv4BE = 0x0100007Fu;

constexpr bool isPrivateBE(uint32_t addrBE) {
    const uint32_t a = hostOrder(addrBE);
    return (a & 0xFF000000u) == 0x0A000000u
        || (a & 0xFFF00000u) == 0xAC100000u
        || (a & 0xFFFF0000u) == 0xC0A80000u;
}

constexpr bool isLinkLocalBE(uint32_t addrBE) {
    return (hostOrder(addrBE) & 0xFFFF0000u) == 0xA9FE0000u;
}

constexpr bool isNonUnicastBE(uint32_t addrBE) {
    const uint32_t a = hostOrder(addrBE);
    return (a & 0xFF000000u) == 0x00000000u
        || (a & 0xE0000000u) == 0xE0000000u
        || a == 0xFFFFFFFFu;
}

constexpr bool isAdvertisablePrivateBE(uint32_t addrBE) {
    return !isNonUnicastBE(addrBE)
        && (isPrivateBE(addrBE) || isLoopbackBE(addrBE));
}

enum class CandidateKind : uint8_t {
    None = 0,
    PrivateLan = 1,
    PublicObserved = 2,
    Relay = 3,
};

struct CandidateEndpoint {
    CandidateKind kind = CandidateKind::None;
    uint32_t ipv4BE = 0;
    uint16_t portBE = 0;
};

constexpr bool sameEndpoint(const CandidateEndpoint& a,
                            const CandidateEndpoint& b) {
    return a.ipv4BE == b.ipv4BE && a.portBE == b.portBE;
}

constexpr size_t kMaxPrivateCandidates = 4;
constexpr size_t kMaxRouteCandidates = kMaxPrivateCandidates + 2;

enum class CandidateSetError : uint8_t {
    None = 0,
    Empty,
    TooMany,
    ZeroEndpoint,
    BadKind,
    KindAddressMismatch,
    Duplicate,
    MultiplePublic,
    MultipleRelay,
};

inline CandidateSetError validateCandidateSet(const CandidateEndpoint* entries,
                                              size_t count) {
    if (count == 0) return CandidateSetError::Empty;
    if (count > kMaxRouteCandidates) return CandidateSetError::TooMany;
    size_t publicCount = 0;
    size_t relayCount = 0;
    size_t privateCount = 0;
    for (size_t i = 0; i < count; ++i) {
        const CandidateEndpoint& c = entries[i];
        if (c.ipv4BE == 0 || c.portBE == 0)
            return CandidateSetError::ZeroEndpoint;
        if (isNonUnicastBE(c.ipv4BE))
            return CandidateSetError::KindAddressMismatch;
        switch (c.kind) {
        case CandidateKind::PrivateLan:
            if (!isAdvertisablePrivateBE(c.ipv4BE))
                return CandidateSetError::KindAddressMismatch;
            ++privateCount;
            break;
        case CandidateKind::PublicObserved:
            if (isPrivateBE(c.ipv4BE) || isLoopbackBE(c.ipv4BE)
                || isLinkLocalBE(c.ipv4BE))
                return CandidateSetError::KindAddressMismatch;
            ++publicCount;
            break;
        case CandidateKind::Relay:
            if (isLinkLocalBE(c.ipv4BE))
                return CandidateSetError::KindAddressMismatch;
            ++relayCount;
            break;
        default:
            return CandidateSetError::BadKind;
        }
        for (size_t j = 0; j < i; ++j) {
            if (sameEndpoint(entries[j], c))
                return CandidateSetError::Duplicate;
        }
    }
    if (privateCount > kMaxPrivateCandidates) return CandidateSetError::TooMany;
    if (publicCount > 1) return CandidateSetError::MultiplePublic;
    if (relayCount > 1) return CandidateSetError::MultipleRelay;
    return CandidateSetError::None;
}

constexpr RouteLabel candidateBaseLabel(CandidateKind kind) {
    return kind == CandidateKind::PrivateLan   ? RouteLabel::LanDirect
         : kind == CandidateKind::PublicObserved ? RouteLabel::PublicDirect
         : kind == CandidateKind::Relay          ? RouteLabel::Relay
                                                 : RouteLabel::None;
}

inline size_t collectPrivateCandidates(const uint32_t* localAddrsBE,
                                       size_t localCount,
                                       uint16_t advertisedPortBE,
                                       CandidateEndpoint* out,
                                       size_t outCap) {
    if (out == nullptr || outCap == 0 || advertisedPortBE == 0) return 0;
    const size_t cap = outCap < kMaxPrivateCandidates ? outCap
                                                      : kMaxPrivateCandidates;
    size_t used = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const bool wantLoopback = pass == 1;
        for (size_t i = 0; i < localCount && used < cap; ++i) {
            const uint32_t addr = localAddrsBE[i];
            if (!isAdvertisablePrivateBE(addr)) continue;
            if (isLoopbackBE(addr) != wantLoopback) continue;
            bool duplicate = false;
            for (size_t j = 0; j < used; ++j) {
                if (out[j].ipv4BE == addr) { duplicate = true; break; }
            }
            if (duplicate) continue;
            out[used].kind = CandidateKind::PrivateLan;
            out[used].ipv4BE = addr;
            out[used].portBE = advertisedPortBE;
            ++used;
        }
    }
    return used;
}

inline size_t collectPrivateCandidatesWithLoopback(
    const uint32_t* localAddrsBE,
    size_t localCount,
    uint16_t advertisedPortBE,
    CandidateEndpoint* out,
    size_t outCap) {
    if (out == nullptr || outCap == 0 || advertisedPortBE == 0) return 0;
    const size_t cap = outCap < kMaxPrivateCandidates ? outCap
                                                      : kMaxPrivateCandidates;
    size_t used = collectPrivateCandidates(localAddrsBE, localCount,
                                           advertisedPortBE, out, cap);
    for (size_t i = 0; i < used; ++i)
        if (out[i].ipv4BE == kLoopbackIpv4BE) return used;

    const size_t slot = used < cap ? used++ : cap - 1;
    out[slot].kind = CandidateKind::PrivateLan;
    out[slot].ipv4BE = kLoopbackIpv4BE;
    out[slot].portBE = advertisedPortBE;
    return used;
}

// These constants mirror qnet::ClientIntentKind in net/Protocol.h and must keep identical values.
constexpr uint8_t kClientIntentJoin = 0;
constexpr uint8_t kClientIntentSpectate = 1;
constexpr uint8_t kClientIntentSpectateRelease = 2;
constexpr uint8_t kClientIntentClientRoot = 3;
constexpr uint8_t kClientIntentStatus = 4;

constexpr bool validateClientIntent(uint8_t intent) {
    return intent == kClientIntentJoin
        || intent == kClientIntentSpectate
        || intent == kClientIntentSpectateRelease
        || intent == kClientIntentClientRoot
        || intent == kClientIntentStatus;
}

constexpr uint16_t swapPort16(uint16_t value) {
    return static_cast<uint16_t>((value >> 8) | (value << 8));
}

inline size_t formatIpv4BE(uint32_t addrBE, char* out, size_t cap) {
    if (out == nullptr || cap < 16) return 0;
    const unsigned octet[4] = {
        static_cast<unsigned>(addrBE & 0xFFu),
        static_cast<unsigned>((addrBE >> 8) & 0xFFu),
        static_cast<unsigned>((addrBE >> 16) & 0xFFu),
        static_cast<unsigned>((addrBE >> 24) & 0xFFu),
    };
    size_t used = 0;
    for (int i = 0; i < 4; ++i) {
        if (i != 0) out[used++] = '.';
        unsigned value = octet[i];
        char digits[3];
        int count = 0;
        do {
            digits[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value != 0);
        while (count > 0) out[used++] = digits[--count];
    }
    out[used] = '\0';
    return used;
}

inline std::string formatIpv4BE(uint32_t addrBE) {
    char text[16];
    const size_t used = formatIpv4BE(addrBE, text, sizeof(text));
    return std::string(text, used);
}

inline bool parseIpv4Text(const char* text, size_t len, uint32_t& addrBE) {
    if (text == nullptr || len < 7 || len > 15) return false;
    uint32_t value = 0;
    size_t index = 0;
    for (int octetIndex = 0; octetIndex < 4; ++octetIndex) {
        if (octetIndex != 0) {
            if (index >= len || text[index] != '.') return false;
            ++index;
        }
        const size_t start = index;
        unsigned octet = 0;
        while (index < len && text[index] >= '0' && text[index] <= '9') {
            octet = octet * 10u + static_cast<unsigned>(text[index] - '0');
            if (octet > 255u) return false;
            if (++index - start > 3) return false;
        }
        if (index == start) return false;
        if (index - start > 1 && text[start] == '0') return false;
        value |= static_cast<uint32_t>(octet) << (8 * octetIndex);
    }
    if (index != len) return false;
    addrBE = value;
    return true;
}

inline bool parsePortText(const char* text, size_t len, uint16_t& portHost) {
    if (text == nullptr || len == 0 || len > 5) return false;
    if (len > 1 && text[0] == '0') return false;
    uint32_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10u + static_cast<uint32_t>(text[i] - '0');
        if (value > 65535u) return false;
    }
    if (value == 0) return false;
    portHost = static_cast<uint16_t>(value);
    return true;
}

constexpr uint8_t kLegacySpectateAddressVersion = 1;
constexpr uint8_t kSpectateAddressVersion = 2;
constexpr size_t kMaxSpectateEndpoints = kMaxRouteCandidates;
constexpr size_t kMaxSpectateUriLength = 512;

struct SpectateAddress {
    uint8_t version = kSpectateAddressVersion;
    uint32_t sessionId = 0;
    uint16_t controlPort = 0;
    CandidateEndpoint endpoints[kMaxSpectateEndpoints]{};
    size_t endpointCount = 0;
    bool hasKey = false;
    uint64_t keyLow = 0;
    uint64_t keyHigh = 0;

    bool hasRelayRoom = false;
    uint64_t relayRoomLow = 0;
    uint64_t relayRoomHigh = 0;
    bool hasRelayAccessToken = false;
    uint64_t relayAccessTokenLow = 0;
    uint64_t relayAccessTokenHigh = 0;
};

enum class SpectateUriError : uint8_t {
    None = 0,
    Empty,
    TooLong,
    BadScheme,
    BadVersion,
    BadField,
    BadSession,
    BadEndpoint,
    TooManyEndpoints,
    BadCandidateSet,
    BadKey,
    KeyTagMismatch,
    BadRelayBinding,
    RelayTagMismatch,
};

inline const char* spectateUriErrorText(SpectateUriError error) {
    switch (error) {
    case SpectateUriError::None:             return "ok";
    case SpectateUriError::Empty:            return "empty spectate address";
    case SpectateUriError::TooLong:          return "spectate address is too long";
    case SpectateUriError::BadScheme:        return "not a qoh99 spectate address";
    case SpectateUriError::BadVersion:       return "unsupported spectate address version";
    case SpectateUriError::BadField:         return "malformed spectate address field";
    case SpectateUriError::BadSession:       return "missing or invalid session id";
    case SpectateUriError::BadEndpoint:      return "malformed spectate endpoint";
    case SpectateUriError::TooManyEndpoints: return "too many spectate endpoints";
    case SpectateUriError::BadCandidateSet:  return "spectate endpoints failed validation";
    case SpectateUriError::BadKey:           return "malformed spectate key";
    case SpectateUriError::KeyTagMismatch:   return "spectate key does not match its tag";
    case SpectateUriError::BadRelayBinding:  return "malformed spectator relay binding";
    case SpectateUriError::RelayTagMismatch: return "spectator relay token does not match its tag";
    }
    return "unknown spectate address error";
}

inline uint32_t spectateKeyTag(uint64_t keyLow, uint64_t keyHigh) {
    uint64_t mix = 0x9E3779B97F4A7C15ull;
    mix ^= keyHigh + 0x165667B19E3779F9ull;
    mix = (mix ^ (mix >> 30)) * 0xBF58476D1CE4E5B9ull;
    mix ^= keyLow + 0x27D4EB2F165667C5ull;
    mix = (mix ^ (mix >> 27)) * 0x94D049BB133111EBull;
    mix ^= mix >> 31;
    const uint32_t tag = static_cast<uint32_t>(mix ^ (mix >> 32));
    return tag == 0 ? 1u : tag;
}

inline uint32_t spectateRelayAccessTag(uint64_t tokenLow,
                                      uint64_t tokenHigh) {
    return spectateKeyTag(tokenLow ^ 0xA56F2C38D71904Ebull,
                          tokenHigh ^ 0x6D8B31F4C25097A1ull);
}

namespace spectate_uri_detail {

inline void appendHex64(std::string& out, uint64_t value) {
    static const char kDigits[] = "0123456789ABCDEF";
    for (int shift = 60; shift >= 0; shift -= 4)
        out.push_back(kDigits[(value >> shift) & 0xFu]);
}

inline void appendHex32(std::string& out, uint32_t value) {
    static const char kDigits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4)
        out.push_back(kDigits[(value >> shift) & 0xFu]);
}

inline void appendDecimal(std::string& out, uint32_t value) {
    char digits[10];
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value != 0);
    while (count > 0) out.push_back(digits[--count]);
}

inline bool parseHex(const char* text, size_t len, uint64_t& out) {
    if (text == nullptr || len == 0 || len > 16) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = text[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A') + 10u;
        else return false;
        value = (value << 4) | digit;
    }
    out = value;
    return true;
}

inline bool parseDecimal32(const char* text, size_t len, uint32_t& out) {
    if (text == nullptr || len == 0 || len > 10) return false;
    if (len > 1 && text[0] == '0') return false;
    uint64_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10ull + static_cast<uint64_t>(text[i] - '0');
        if (value > 0xFFFFFFFFull) return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

inline bool parseEndpointText(const std::string& text, CandidateKind kind,
                              CandidateEndpoint& out) {
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    uint32_t addrBE = 0;
    uint16_t portHost = 0;
    if (!parseIpv4Text(text.data(), colon, addrBE)) return false;
    if (!parsePortText(text.data() + colon + 1, text.size() - colon - 1,
                       portHost))
        return false;
    out.kind = kind;
    out.ipv4BE = addrBE;
    out.portBE = swapPort16(portHost);
    return true;
}

inline void appendEndpointText(std::string& out,
                               const CandidateEndpoint& endpoint) {
    char address[16];
    out.append(address, formatIpv4BE(endpoint.ipv4BE, address,
                                     sizeof(address)));
    out.push_back(':');
    appendDecimal(out, swapPort16(endpoint.portBE));
}

constexpr char kSpectateScheme[] = "qoh99://spectate/";

}

inline bool formatSpectateUri(const SpectateAddress& address,
                              bool includeKey, std::string& out,
                              SpectateUriError& err) {
    out.clear();
    err = SpectateUriError::None;
    if (address.version != kSpectateAddressVersion
        && address.version != kLegacySpectateAddressVersion) {
        err = SpectateUriError::BadVersion;
        return false;
    }
    if (address.sessionId == 0) {
        err = SpectateUriError::BadSession;
        return false;
    }
    if (address.endpointCount == 0
        || address.endpointCount > kMaxSpectateEndpoints) {
        err = SpectateUriError::TooManyEndpoints;
        return false;
    }
    if (validateCandidateSet(address.endpoints, address.endpointCount)
        != CandidateSetError::None) {
        err = SpectateUriError::BadCandidateSet;
        return false;
    }
    if (address.hasKey && address.keyLow == 0 && address.keyHigh == 0) {
        err = SpectateUriError::BadKey;
        return false;
    }
    bool hasRelayEndpoint = false;
    for (size_t i = 0; i < address.endpointCount; ++i)
        hasRelayEndpoint |=
            address.endpoints[i].kind == CandidateKind::Relay;
    const bool relayRoomValid =
        address.relayRoomLow != 0 || address.relayRoomHigh != 0;
    const bool relayAccessValid =
        address.relayAccessTokenLow != 0
        || address.relayAccessTokenHigh != 0;
    if (address.version == kLegacySpectateAddressVersion) {
        if (address.controlPort != 0) {
            err = SpectateUriError::BadField;
            return false;
        }
        if (address.hasRelayRoom || relayRoomValid
            || address.hasRelayAccessToken || relayAccessValid) {
            err = SpectateUriError::BadRelayBinding;
            return false;
        }
    } else if (hasRelayEndpoint != address.hasRelayRoom
               || address.hasRelayRoom != relayRoomValid
               || address.hasRelayAccessToken != relayAccessValid
               || (address.hasRelayAccessToken && !address.hasRelayRoom)) {
        err = SpectateUriError::BadRelayBinding;
        return false;
    }

    std::string text(spectate_uri_detail::kSpectateScheme);
    spectate_uri_detail::appendDecimal(text, address.version);
    text += "?s=";
    spectate_uri_detail::appendDecimal(text, address.sessionId);
    if (address.controlPort != 0) {
        text += "&cp=";
        spectate_uri_detail::appendDecimal(text, address.controlPort);
    }

    struct KindField { CandidateKind kind; const char* name; };
    const KindField fields[3] = {
        { CandidateKind::PrivateLan, "&lan=" },
        { CandidateKind::PublicObserved, "&pub=" },
        { CandidateKind::Relay, "&relay=" },
    };
    for (const KindField& field : fields) {
        bool first = true;
        for (size_t i = 0; i < address.endpointCount; ++i) {
            if (address.endpoints[i].kind != field.kind) continue;
            text += first ? field.name : ",";
            first = false;
            spectate_uri_detail::appendEndpointText(text, address.endpoints[i]);
        }
    }
    if (address.hasRelayRoom) {
        text += "&rr=";
        spectate_uri_detail::appendHex64(text, address.relayRoomHigh);
        spectate_uri_detail::appendHex64(text, address.relayRoomLow);
    }
    if (address.hasKey) {
        text += "&kt=";
        spectate_uri_detail::appendHex32(text,
                            spectateKeyTag(address.keyLow, address.keyHigh));
    }
    if (address.hasRelayAccessToken) {
        text += "&rat=";
        spectate_uri_detail::appendHex32(
            text, spectateRelayAccessTag(address.relayAccessTokenLow,
                                         address.relayAccessTokenHigh));
    }
    if (includeKey
        && (address.hasKey || address.hasRelayAccessToken)) {
        text += "#";
        bool firstSecret = true;
        if (address.hasKey) {
            text += "k=";
            spectate_uri_detail::appendHex64(text, address.keyHigh);
            spectate_uri_detail::appendHex64(text, address.keyLow);
            firstSecret = false;
        }
        if (address.hasRelayAccessToken) {
            if (!firstSecret) text += "&";
            text += "ra=";
            spectate_uri_detail::appendHex64(
                text, address.relayAccessTokenHigh);
            spectate_uri_detail::appendHex64(
                text, address.relayAccessTokenLow);
        }
    }
    if (text.size() > kMaxSpectateUriLength) {
        err = SpectateUriError::TooLong;
        return false;
    }
    out.swap(text);
    return true;
}

inline bool formatSpectateUri(const SpectateAddress& address, std::string& out,
                              SpectateUriError& err) {
    return formatSpectateUri(address, true, out, err);
}

inline bool formatSpectateUriLoggable(const SpectateAddress& address,
                                      std::string& out,
                                      SpectateUriError& err) {
    return formatSpectateUri(address, false, out, err);
}

inline std::string redactSpectateUri(const std::string& text) {
    const size_t hash = text.find('#');
    if (hash == std::string::npos) return text;
    return text.substr(0, hash) + "#<redacted>";
}

inline bool parseSpectateUri(const std::string& text, SpectateAddress& out,
                             SpectateUriError& err) {
    err = SpectateUriError::None;
    if (text.empty()) { err = SpectateUriError::Empty; return false; }
    if (text.size() > kMaxSpectateUriLength) {
        err = SpectateUriError::TooLong;
        return false;
    }
    if (text.find('\0') != std::string::npos) {
        err = SpectateUriError::BadField;
        return false;
    }

    const std::string scheme(spectate_uri_detail::kSpectateScheme);
    if (text.compare(0, scheme.size(), scheme) != 0) {
        err = SpectateUriError::BadScheme;
        return false;
    }
    const size_t query = text.find('?', scheme.size());
    if (query == std::string::npos) {
        err = SpectateUriError::BadScheme;
        return false;
    }
    uint32_t version = 0;
    if (!spectate_uri_detail::parseDecimal32(text.data() + scheme.size(),
                                query - scheme.size(), version)
        || (version != kSpectateAddressVersion
            && version != kLegacySpectateAddressVersion)) {
        err = SpectateUriError::BadVersion;
        return false;
    }

    const size_t hash = text.find('#', query);
    const std::string queryText =
        text.substr(query + 1, (hash == std::string::npos ? text.size() : hash)
                                   - query - 1);

    SpectateAddress parsed{};
    parsed.version = static_cast<uint8_t>(version);
    bool haveSession = false;
    bool haveControlPort = false;
    bool haveTag = false;
    bool haveRelayRoom = false;
    bool haveRelayTag = false;
    uint32_t tag = 0;
    uint32_t relayTag = 0;
    bool seenKind[4] = { false, false, false, false };

    size_t cursor = 0;
    while (cursor <= queryText.size()) {
        size_t amp = queryText.find('&', cursor);
        if (amp == std::string::npos) amp = queryText.size();
        const std::string field = queryText.substr(cursor, amp - cursor);
        cursor = amp + 1;
        if (field.empty()) { err = SpectateUriError::BadField; return false; }
        const size_t equals = field.find('=');
        if (equals == std::string::npos || equals == 0
            || equals + 1 == field.size()) {
            err = SpectateUriError::BadField;
            return false;
        }
        const std::string name = field.substr(0, equals);
        const std::string value = field.substr(equals + 1);

        if (name == "s") {
            if (haveSession) { err = SpectateUriError::BadField; return false; }
            haveSession = true;
            if (!spectate_uri_detail::parseDecimal32(value.data(), value.size(),
                                        parsed.sessionId)
                || parsed.sessionId == 0) {
                err = SpectateUriError::BadSession;
                return false;
            }
            continue;
        }
        if (name == "kt") {
            if (haveTag) { err = SpectateUriError::BadField; return false; }
            haveTag = true;
            uint64_t wide = 0;
            if (value.size() != 8
                || !spectate_uri_detail::parseHex(value.data(), 8, wide)) {
                err = SpectateUriError::BadField;
                return false;
            }
            tag = static_cast<uint32_t>(wide);
            continue;
        }
        if (name == "cp") {
            if (version != kSpectateAddressVersion || haveControlPort
                || !parsePortText(
                    value.data(), value.size(), parsed.controlPort)) {
                err = SpectateUriError::BadField;
                return false;
            }
            haveControlPort = true;
            continue;
        }
        if (name == "rr") {
            if (version != kSpectateAddressVersion || haveRelayRoom
                || value.size() != 32) {
                err = SpectateUriError::BadRelayBinding;
                return false;
            }
            haveRelayRoom = true;
            if (!spectate_uri_detail::parseHex(
                    value.data(), 16, parsed.relayRoomHigh)
                || !spectate_uri_detail::parseHex(
                    value.data() + 16, 16, parsed.relayRoomLow)
                || (parsed.relayRoomLow == 0
                    && parsed.relayRoomHigh == 0)) {
                err = SpectateUriError::BadRelayBinding;
                return false;
            }
            parsed.hasRelayRoom = true;
            continue;
        }
        if (name == "rat") {
            if (version != kSpectateAddressVersion || haveRelayTag) {
                err = SpectateUriError::BadRelayBinding;
                return false;
            }
            haveRelayTag = true;
            uint64_t wide = 0;
            if (value.size() != 8
                || !spectate_uri_detail::parseHex(
                    value.data(), 8, wide)) {
                err = SpectateUriError::BadRelayBinding;
                return false;
            }
            relayTag = static_cast<uint32_t>(wide);
            continue;
        }

        CandidateKind kind = CandidateKind::None;
        if (name == "lan") kind = CandidateKind::PrivateLan;
        else if (name == "pub") kind = CandidateKind::PublicObserved;
        else if (name == "relay") kind = CandidateKind::Relay;
        else { err = SpectateUriError::BadField; return false; }

        const size_t kindIndex = static_cast<size_t>(kind);
        if (seenKind[kindIndex]) {
            err = SpectateUriError::BadField;
            return false;
        }
        seenKind[kindIndex] = true;

        size_t item = 0;
        while (item <= value.size()) {
            size_t comma = value.find(',', item);
            if (comma == std::string::npos) comma = value.size();
            const std::string one = value.substr(item, comma - item);
            item = comma + 1;
            if (one.empty()) { err = SpectateUriError::BadEndpoint; return false; }
            if (parsed.endpointCount >= kMaxSpectateEndpoints) {
                err = SpectateUriError::TooManyEndpoints;
                return false;
            }
            if (!spectate_uri_detail::parseEndpointText(
                    one, kind, parsed.endpoints[parsed.endpointCount])) {
                err = SpectateUriError::BadEndpoint;
                return false;
            }
            ++parsed.endpointCount;
            if (comma == value.size()) break;
        }
        if (amp == queryText.size()) break;
    }

    if (!haveSession) { err = SpectateUriError::BadSession; return false; }
    if (parsed.endpointCount == 0) {
        err = SpectateUriError::BadEndpoint;
        return false;
    }
    if (validateCandidateSet(parsed.endpoints, parsed.endpointCount)
        != CandidateSetError::None) {
        err = SpectateUriError::BadCandidateSet;
        return false;
    }

    if (hash != std::string::npos) {
        const std::string fragment = text.substr(hash + 1);
        bool haveFragmentKey = false;
        bool haveFragmentRelay = false;
        size_t secretCursor = 0;
        while (secretCursor <= fragment.size()) {
            size_t amp = fragment.find('&', secretCursor);
            if (amp == std::string::npos) amp = fragment.size();
            const std::string field =
                fragment.substr(secretCursor, amp - secretCursor);
            secretCursor = amp + 1;
            const size_t equals = field.find('=');
            if (equals == std::string::npos || equals == 0
                || equals + 1 == field.size()) {
                err = SpectateUriError::BadKey;
                return false;
            }
            const std::string name = field.substr(0, equals);
            const std::string value = field.substr(equals + 1);
            if (name == "k") {
                if (haveFragmentKey || value.size() != 32
                    || !spectate_uri_detail::parseHex(
                        value.data(), 16, parsed.keyHigh)
                    || !spectate_uri_detail::parseHex(
                        value.data() + 16, 16, parsed.keyLow)
                    || (parsed.keyLow == 0 && parsed.keyHigh == 0)) {
                    err = SpectateUriError::BadKey;
                    return false;
                }
                haveFragmentKey = true;
                parsed.hasKey = true;
            } else if (name == "ra") {
                if (version != kSpectateAddressVersion
                    || haveFragmentRelay || value.size() != 32
                    || !spectate_uri_detail::parseHex(
                        value.data(), 16, parsed.relayAccessTokenHigh)
                    || !spectate_uri_detail::parseHex(
                        value.data() + 16, 16,
                        parsed.relayAccessTokenLow)
                    || (parsed.relayAccessTokenLow == 0
                        && parsed.relayAccessTokenHigh == 0)) {
                    err = SpectateUriError::BadRelayBinding;
                    return false;
                }
                haveFragmentRelay = true;
                parsed.hasRelayAccessToken = true;
            } else {
                err = SpectateUriError::BadField;
                return false;
            }
            if (amp == fragment.size()) break;
        }
    }

    if (parsed.hasKey && !haveTag) {
        err = SpectateUriError::KeyTagMismatch;
        return false;
    }
    if (parsed.hasKey
        && spectateKeyTag(parsed.keyLow, parsed.keyHigh) != tag) {
        err = SpectateUriError::KeyTagMismatch;
        return false;
    }

    bool hasRelayEndpoint = false;
    for (size_t i = 0; i < parsed.endpointCount; ++i)
        hasRelayEndpoint |=
            parsed.endpoints[i].kind == CandidateKind::Relay;
    if (version == kSpectateAddressVersion
        && (hasRelayEndpoint != parsed.hasRelayRoom
            || (parsed.hasRelayAccessToken && !parsed.hasRelayRoom)
            || (parsed.hasRelayAccessToken && !haveRelayTag))) {
        err = SpectateUriError::BadRelayBinding;
        return false;
    }
    if (parsed.hasRelayAccessToken
        && spectateRelayAccessTag(parsed.relayAccessTokenLow,
                                  parsed.relayAccessTokenHigh)
               != relayTag) {
        err = SpectateUriError::RelayTagMismatch;
        return false;
    }

    out = parsed;
    return true;
}

}
