#include "TestHarness.h"

#include "net/Protocol.h"
#include "net/RelayClient.h"
#include "net/RouteCandidates.h"
#include "net/SecureRandom.h"
#include "net/Sockets.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace qnet;

namespace {

constexpr uint32_t be(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return static_cast<uint32_t>(a)
         | (static_cast<uint32_t>(b) << 8)
         | (static_cast<uint32_t>(c) << 16)
         | (static_cast<uint32_t>(d) << 24);
}

CandidateEndpoint candidate(CandidateKind kind, uint32_t ipv4BE,
                            uint16_t hostPort) {
    CandidateEndpoint value;
    value.kind = kind;
    value.ipv4BE = ipv4BE;
    value.portBE = swapPort16(hostPort);
    return value;
}

SpectateAddress sampleAddress() {
    SpectateAddress address{};
    address.sessionId = 4242;
    address.controlPort = 7500;
    address.endpoints[0] =
        candidate(CandidateKind::PrivateLan, be(192, 168, 1, 20), 14764);
    address.endpoints[1] =
        candidate(CandidateKind::PrivateLan, be(10, 0, 0, 5), 14764);
    address.endpoints[2] =
        candidate(CandidateKind::PublicObserved, be(203, 0, 113, 7), 51000);
    address.endpoints[3] =
        candidate(CandidateKind::Relay, be(198, 51, 100, 2), 14764);
    address.endpointCount = 4;
    address.hasKey = true;
    address.keyHigh = 0x0123456789ABCDEFull;
    address.keyLow = 0xFEDCBA9876543210ull;
    address.hasRelayRoom = true;
    address.relayRoomHigh = 0x1020304050607080ull;
    address.relayRoomLow = 0x90A0B0C0D0E0F001ull;
    address.hasRelayAccessToken = true;
    address.relayAccessTokenHigh = 0x1122334455667788ull;
    address.relayAccessTokenLow = 0x99AABBCCDDEEFF01ull;
    return address;
}

std::string keyHexOf(const SpectateAddress& address) {
    char text[33];
    std::snprintf(text, sizeof(text), "%016llX%016llX",
                  static_cast<unsigned long long>(address.keyHigh),
                  static_cast<unsigned long long>(address.keyLow));
    return text;
}

bool allZero(const uint8_t* bytes, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (bytes[i] != 0) return false;
    }
    return true;
}

}

QNP_TEST(AddressingNumericIpv4ParseUnchanged) {
    struct Sample { const char* text; uint32_t expected; };
    const Sample accepted[] = {
        { "127.0.0.1",       be(127, 0, 0, 1) },
        { "127.255.255.254", be(127, 255, 255, 254) },
        { "128.0.0.1",       be(128, 0, 0, 1) },
        { "10.0.0.1",        be(10, 0, 0, 1) },
        { "10.255.255.254",  be(10, 255, 255, 254) },
        { "172.16.0.1",      be(172, 16, 0, 1) },
        { "172.31.255.254",  be(172, 31, 255, 254) },
        { "172.15.255.254",  be(172, 15, 255, 254) },
        { "172.32.0.1",      be(172, 32, 0, 1) },
        { "192.168.0.1",     be(192, 168, 0, 1) },
        { "192.169.0.1",     be(192, 169, 0, 1) },
        { "11.0.0.1",        be(11, 0, 0, 1) },
        { "169.254.0.1",     be(169, 254, 0, 1) },
        { "169.253.0.1",     be(169, 253, 0, 1) },
        { "0.0.0.0",         be(0, 0, 0, 0) },
        { "0.1.2.3",         be(0, 1, 2, 3) },
        { "224.0.0.1",       be(224, 0, 0, 1) },
        { "239.255.255.255", be(239, 255, 255, 255) },
        { "240.0.0.1",       be(240, 0, 0, 1) },
        { "255.255.255.255", be(255, 255, 255, 255) },
        { "8.8.8.8",         be(8, 8, 8, 8) },
        { "203.0.113.7",     be(203, 0, 113, 7) },
        { "198.51.100.2",    be(198, 51, 100, 2) },
    };
    for (const Sample& sample : accepted) {
        Endpoint parsed;
        QNP_CHECK(Endpoint::parse(sample.text, 14763, parsed));
        QNP_CHECK(parsed.sa.sin_family == AF_INET);
        QNP_CHECK(parsed.sa.sin_addr.s_addr == sample.expected);
        QNP_CHECK(parsed.sa.sin_port == htons(14763));
        QNP_CHECK(parsed.str() == std::string(sample.text) + ":14763");

        QNP_CHECK(formatIpv4BE(sample.expected) == sample.text);
        uint32_t roundTrip = 0;
        QNP_CHECK(parseIpv4Text(sample.text, std::strlen(sample.text),
                                roundTrip));
        QNP_CHECK(roundTrip == sample.expected);
    }

    const char* const rejected[] = {
        "", " ", "1.2.3", "1.2.3.4.5", "1.2.3.256", "1.2.3.-1", "1.2.3.4:5",
        "localhost", "example.com", "::1", "1.2.3.4 ", "abc", "...",
    };
    for (const char* text : rejected) {
        Endpoint parsed;
        QNP_CHECK(!Endpoint::parse(text, 14763, parsed));
    }

    uint32_t ignored = 0;
    QNP_CHECK(!parseIpv4Text("010.1.2.3", 9, ignored));
    QNP_CHECK(!parseIpv4Text("1.2.3.00", 8, ignored));
    QNP_CHECK(!parseIpv4Text("1.2.3", 5, ignored));
    QNP_CHECK(!parseIpv4Text("1.2.3.4.5", 9, ignored));
    QNP_CHECK(!parseIpv4Text("1.2.3.4 ", 8, ignored));
    QNP_CHECK(!parseIpv4Text(nullptr, 7, ignored));

    Endpoint lan;
    QNP_REQUIRE(Endpoint::parse("192.168.1.20", 1, lan));
    QNP_CHECK(isAdvertisablePrivateBE(lan.sa.sin_addr.s_addr));
    Endpoint linkLocal;
    QNP_REQUIRE(Endpoint::parse("169.254.12.1", 1, linkLocal));
    QNP_CHECK(isLinkLocalBE(linkLocal.sa.sin_addr.s_addr));
    QNP_CHECK(!isAdvertisablePrivateBE(linkLocal.sa.sin_addr.s_addr));
}

QNP_TEST(AddressingResolvesNumericLiteralsWithoutDns) {
    std::vector<ResolvedEndpoint> out;
    ResolveError err = ResolveError::SystemError;
    std::string detail = "unset";

    QNP_REQUIRE(resolveEndpoints("192.0.2.10", 14763, 1, out, err, detail));
    QNP_CHECK(err == ResolveError::None);
    QNP_CHECK(detail.empty());
    QNP_REQUIRE(out.size() == 1);
    QNP_CHECK(!out[0].wasHostname);
    QNP_CHECK(out[0].canonicalText == "192.0.2.10:14763");
    QNP_CHECK(out[0].endpoint.sa.sin_addr.s_addr == be(192, 0, 2, 10));
    QNP_CHECK(out[0].endpoint.sa.sin_port == htons(14763));

    QNP_REQUIRE(resolveEndpoints("  [192.0.2.10]\t", 7000, 1, out, err,
                                 detail));
    QNP_REQUIRE(out.size() == 1);
    QNP_CHECK(out[0].canonicalText == "192.0.2.10:7000");
    QNP_CHECK(!out[0].wasHostname);

    ResolvedEndpoint single;
    QNP_REQUIRE(resolveEndpoint("127.0.0.1", 1234, 1, single, err, detail));
    QNP_CHECK(single.canonicalText == "127.0.0.1:1234");
}

QNP_TEST(AddressingRejectsMalformedHostText) {
    std::vector<ResolvedEndpoint> out;
    ResolveError err = ResolveError::None;
    std::string detail;

    QNP_CHECK(!resolveEndpoints("", 1, 1, out, err, detail));
    QNP_CHECK(err == ResolveError::InvalidInput);
    QNP_CHECK(!detail.empty());
    QNP_CHECK(out.empty());
    QNP_CHECK(!resolveEndpoints("   \t\r\n ", 1, 1, out, err, detail));
    QNP_CHECK(err == ResolveError::InvalidInput);

    const std::string embedded("local\0host.example", 18);
    QNP_REQUIRE(embedded.size() == 18);
    QNP_CHECK(!resolveEndpoints(embedded, 1, 1, out, err, detail));
    QNP_CHECK(err == ResolveError::InvalidInput);
    QNP_CHECK(out.empty());

    QNP_CHECK(!resolveEndpoints(std::string(kMaxHostTextLength + 1, 'a'), 1, 1,
                                out, err, detail));
    QNP_CHECK(err == ResolveError::InvalidInput);
    QNP_CHECK(!resolveEndpoints(std::string(4096, 'a'), 1, 1, out, err,
                                detail));
    QNP_CHECK(err == ResolveError::InvalidInput);

    QNP_CHECK(!resolveEndpoints(std::string(64, 'a') + ".example", 1, 1, out,
                                err, detail));
    QNP_CHECK(err == ResolveError::InvalidInput);

    const char* const junk[] = {
        "host/path", "host:1234", "user@host", "host%eth0", "ho st",
        "..", "host..example", ".leading", "-leading", "h\x01st",
    };
    for (const char* text : junk) {
        QNP_CHECK(!resolveEndpoints(text, 1, 1, out, err, detail));
        QNP_CHECK(err == ResolveError::InvalidInput);
        QNP_CHECK(out.empty());
        QNP_CHECK(!detail.empty());
    }

    QNP_CHECK(std::string(resolveErrorText(ResolveError::InvalidInput))
              != std::string(resolveErrorText(ResolveError::NoAddress)));
}

QNP_TEST(AddressingResolvesLoopbackHostname) {
    std::vector<ResolvedEndpoint> out;
    ResolveError err = ResolveError::SystemError;
    std::string detail = "unset";

    QNP_REQUIRE(resolveEndpoints("localhost", 14763, 3000, out, err, detail));
    QNP_CHECK(err == ResolveError::None);
    QNP_REQUIRE(!out.empty());
    QNP_CHECK(out.size() <= kMaxResolvedEndpoints);
    for (const ResolvedEndpoint& resolved : out) {
        QNP_CHECK(resolved.wasHostname);
        QNP_CHECK(resolved.endpoint.sa.sin_family == AF_INET);
        QNP_CHECK(resolved.endpoint.sa.sin_port == htons(14763));
        QNP_CHECK(!resolved.canonicalText.empty());
        QNP_CHECK(resolved.canonicalText == resolved.endpoint.str());
        QNP_CHECK(isLoopbackBE(resolved.endpoint.sa.sin_addr.s_addr));
    }
    for (size_t i = 1; i < out.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            QNP_CHECK(out[i].endpoint.sa.sin_addr.s_addr
                      != out[j].endpoint.sa.sin_addr.s_addr);
        }
    }

    ResolvedEndpoint first;
    QNP_REQUIRE(resolveEndpoint("localhost", 99, 3000, first, err, detail));
    QNP_CHECK(first.wasHostname);
    QNP_CHECK(first.endpoint.sa.sin_port == htons(99));

    if (!resolveEndpoint("localhost.", 99, 3000, first, err, detail)) {
        QNP_CHECK(err != ResolveError::None);
        QNP_CHECK(err != ResolveError::InvalidInput);
        QNP_CHECK(!detail.empty());
    }
}

QNP_TEST(AddressingHostnameFailureIsBoundedAndTyped) {
    std::vector<ResolvedEndpoint> out;
    ResolveError err = ResolveError::None;
    std::string detail;

    const uint64_t startUs = nowUs();
    const bool ok = resolveEndpoints(
        "qoh99-no-such-host-b7a1.invalid", 14763, 1500, out, err, detail);
    const uint64_t elapsedUs = nowUs() - startUs;
    QNP_CHECK(!ok);
    QNP_CHECK(out.empty());
    QNP_CHECK(!detail.empty());
    QNP_CHECK(err == ResolveError::NotFound || err == ResolveError::Timeout
              || err == ResolveError::NoAddress
              || err == ResolveError::SystemError);
    QNP_CHECK(elapsedUs < 10ull * 1000ull * 1000ull);
}

QNP_TEST(AddressingIpv6OnlyReportsNoAddressWithSpecificMessage) {
    std::vector<ResolvedEndpoint> out;
    ResolveError err = ResolveError::None;
    std::string detail;

    const char* const literals[] = {
        "::1", "[::1]", "2001:db8::1", "[2001:db8::1]", "fe80::1",
    };
    for (const char* text : literals) {
        QNP_CHECK(!resolveEndpoints(text, 14763, 1, out, err, detail));
        QNP_CHECK(err == ResolveError::NoAddress);
        QNP_CHECK(detail == resolveIpv6UnsupportedText());
        QNP_CHECK(out.empty());
    }

    const ResolvedSample ipv6Only[] = {
        { AF_INET6, 0 },
        { AF_INET6, 0 },
    };
    QNP_CHECK(!filterResolvedSamples(ipv6Only, 2, 14763, out, err, detail));
    QNP_CHECK(err == ResolveError::NoAddress);
    QNP_CHECK(detail == resolveIpv6UnsupportedText());
    QNP_CHECK(out.empty());

    const ResolvedSample dual[] = {
        { AF_INET6, 0 },
        { AF_INET, be(203, 0, 113, 7) },
        { AF_INET6, 0 },
        { AF_INET, be(198, 51, 100, 2) },
        { AF_INET, be(203, 0, 113, 7) },
    };
    QNP_REQUIRE(filterResolvedSamples(dual, 5, 7000, out, err, detail));
    QNP_CHECK(err == ResolveError::None);
    QNP_REQUIRE(out.size() == 2);
    QNP_CHECK(out[0].endpoint.sa.sin_addr.s_addr == be(203, 0, 113, 7));
    QNP_CHECK(out[1].endpoint.sa.sin_addr.s_addr == be(198, 51, 100, 2));
    QNP_CHECK(out[0].canonicalText == "203.0.113.7:7000");
    QNP_CHECK(out[0].wasHostname);

    const ResolvedSample poisoned[] = {
        { AF_INET, 0 },
        { AF_INET, be(224, 0, 0, 1) },
        { AF_INET, be(255, 255, 255, 255) },
    };
    QNP_CHECK(!filterResolvedSamples(poisoned, 3, 1, out, err, detail));
    QNP_CHECK(err == ResolveError::NoAddress);
    QNP_CHECK(detail != resolveIpv6UnsupportedText());
    QNP_CHECK(!detail.empty());

    QNP_CHECK(!filterResolvedSamples(nullptr, 0, 1, out, err, detail));
    QNP_CHECK(err == ResolveError::NotFound);

    std::vector<ResolvedSample> many;
    for (unsigned i = 1; i <= 32; ++i)
        many.push_back(ResolvedSample{ AF_INET, be(10, 0, 0,
                                                   static_cast<uint8_t>(i)) });
    QNP_REQUIRE(filterResolvedSamples(many.data(), many.size(), 1, out, err,
                                      detail));
    QNP_CHECK(out.size() == kMaxResolvedEndpoints);

    QNP_CHECK(resolveFamilySupported(AF_INET));
    QNP_CHECK(!resolveFamilySupported(AF_INET6));
    QNP_CHECK(!resolveFamilySupported(AF_UNSPEC));
}

QNP_TEST(SecureRandomFillsReportsBackendAndFlagsOnlyTheLastResort) {
    QNP_CHECK(secureRandomBackend() != SecureRandomBackend::None);
    QNP_REQUIRE(secureRandomBackendName() != nullptr);
    QNP_CHECK(std::strlen(secureRandomBackendName()) > 0);
    QNP_CHECK(std::string(secureRandomBackendName())
              == secureRandomBackendName(secureRandomBackend()));

    std::set<std::string> seen;
    for (int draw = 0; draw < 256; ++draw) {
        uint8_t buffer[32];
        std::memset(buffer, 0, sizeof(buffer));
        bool degraded = true;
        QNP_REQUIRE(secureRandomBytes(buffer, sizeof(buffer), degraded));
        QNP_CHECK(!allZero(buffer, sizeof(buffer)));
        QNP_CHECK(degraded
                  == (secureRandomBackend() == SecureRandomBackend::LastResort));
        seen.insert(std::string(reinterpret_cast<const char*>(buffer),
                                sizeof(buffer)));
    }
    QNP_CHECK(seen.size() == 256);

    bool degraded = true;
    QNP_CHECK(secureRandomBytes(nullptr, 0, degraded));
    QNP_CHECK(!secureRandomBytes(nullptr, 8, degraded));

    for (int draw = 0; draw < 64; ++draw) {
        bool wasDegraded = true;
        const uint64_t value = secureRandom64(wasDegraded);
        QNP_CHECK(value != 0);
        QNP_CHECK(wasDegraded
                  == (secureRandomBackend()
                      == SecureRandomBackend::LastResort));
    }

    std::set<uint64_t> mixed;
    for (int draw = 0; draw < 256; ++draw) {
        uint64_t value = 0;
        bool wasDegraded = false;
        QNP_REQUIRE(secureRandomBytesFrom(SecureRandomBackend::LastResort,
                                          &value, sizeof(value), wasDegraded));
        QNP_CHECK(wasDegraded);
        QNP_CHECK(value != 0);
        mixed.insert(value);
    }
    QNP_CHECK(mixed.size() == 256);

    for (const SecureRandomBackend backend :
         { SecureRandomBackend::BCrypt, SecureRandomBackend::RtlGen }) {
        uint8_t buffer[16] = {};
        bool wasDegraded = true;
        if (secureRandomBytesFrom(backend, buffer, sizeof(buffer),
                                  wasDegraded)) {
            QNP_CHECK(!wasDegraded);
            QNP_CHECK(!allZero(buffer, sizeof(buffer)));
        }
    }

    uint8_t unused[8] = {};
    bool noneDegraded = true;
    QNP_CHECK(!secureRandomBytesFrom(SecureRandomBackend::None, unused,
                                     sizeof(unused), noneDegraded));
    QNP_CHECK(!noneDegraded);
    QNP_CHECK(allZero(unused, sizeof(unused)));

    qnet::relay::RoomId room{};
    std::string error = "unset";
    bool roomDegraded = true;
    QNP_CHECK(qnet::relay::generateRoomId(room, &error, &roomDegraded));
    QNP_CHECK(error.empty());
    QNP_CHECK(room.low != 0 || room.high != 0);
    QNP_CHECK(roomDegraded
              == (secureRandomBackend() == SecureRandomBackend::LastResort));
}

QNP_TEST(ClientIntentAcceptsExactlyTheKnownValues) {
    static_assert(kClientIntentJoin == static_cast<uint8_t>(INTENT_JOIN),
                  "kClientIntentJoin must mirror INTENT_JOIN");
    static_assert(kClientIntentSpectate
                      == static_cast<uint8_t>(INTENT_SPECTATE),
                  "kClientIntentSpectate must mirror INTENT_SPECTATE");
    static_assert(kClientIntentSpectateRelease
                      == static_cast<uint8_t>(INTENT_SPECTATE_RELEASE),
                  "release mirror must match INTENT_SPECTATE_RELEASE");
    static_assert(kClientIntentClientRoot
                      == static_cast<uint8_t>(INTENT_CLIENT_ROOT),
                  "client-root mirror must match INTENT_CLIENT_ROOT");
    static_assert(kClientIntentStatus
                      == static_cast<uint8_t>(INTENT_STATUS),
                  "status mirror must match INTENT_STATUS");
    static_assert(validateClientIntent(kClientIntentJoin), "join is known");
    static_assert(validateClientIntent(kClientIntentSpectate),
                  "spectate is known");
    static_assert(validateClientIntent(kClientIntentSpectateRelease),
                  "spectate release is known");
    static_assert(validateClientIntent(kClientIntentClientRoot),
                  "client-root claim is known");
    static_assert(validateClientIntent(kClientIntentStatus),
                  "status query is known");

    int accepted = 0;
    for (unsigned value = 0; value <= 0xFFu; ++value) {
        const bool ok = validateClientIntent(static_cast<uint8_t>(value));
        if (ok) ++accepted;
        QNP_CHECK(ok == (value == 0u || value == 1u || value == 2u
                         || value == 3u || value == 4u));
    }
    QNP_CHECK(accepted == 5);

    QNP_CHECK(validateClientIntent(2));
    QNP_CHECK(validateClientIntent(3));
    QNP_CHECK(validateClientIntent(4));
    QNP_CHECK(!validateClientIntent(5));
    QNP_CHECK(!validateClientIntent(0xFF));
}

QNP_TEST(SpectateUriRoundTripsEveryCandidateKind) {
    const SpectateAddress address = sampleAddress();
    std::string uri;
    SpectateUriError err = SpectateUriError::BadField;
    QNP_REQUIRE(formatSpectateUri(address, uri, err));
    QNP_CHECK(err == SpectateUriError::None);
    QNP_CHECK(uri.size() <= kMaxSpectateUriLength);
    QNP_CHECK(uri.rfind("qoh99://spectate/2?", 0) == 0);
    QNP_CHECK(uri.find("&cp=7500") != std::string::npos);
    QNP_CHECK(uri.find("lan=192.168.1.20:14764,10.0.0.5:14764")
              != std::string::npos);
    QNP_CHECK(uri.find("pub=203.0.113.7:51000") != std::string::npos);
    QNP_CHECK(uri.find("relay=198.51.100.2:14764") != std::string::npos);

    SpectateAddress parsed{};
    QNP_REQUIRE(parseSpectateUri(uri, parsed, err));
    QNP_CHECK(err == SpectateUriError::None);
    QNP_CHECK(parsed.version == kSpectateAddressVersion);
    QNP_CHECK(parsed.sessionId == address.sessionId);
    QNP_CHECK(parsed.controlPort == address.controlPort);
    QNP_CHECK(parsed.hasKey);
    QNP_CHECK(parsed.keyLow == address.keyLow);
    QNP_CHECK(parsed.keyHigh == address.keyHigh);
    QNP_CHECK(parsed.hasRelayRoom);
    QNP_CHECK(parsed.relayRoomLow == address.relayRoomLow);
    QNP_CHECK(parsed.relayRoomHigh == address.relayRoomHigh);
    QNP_CHECK(parsed.hasRelayAccessToken);
    QNP_CHECK(parsed.relayAccessTokenLow
              == address.relayAccessTokenLow);
    QNP_CHECK(parsed.relayAccessTokenHigh
              == address.relayAccessTokenHigh);
    QNP_REQUIRE(parsed.endpointCount == address.endpointCount);
    for (size_t i = 0; i < parsed.endpointCount; ++i) {
        QNP_CHECK(parsed.endpoints[i].kind == address.endpoints[i].kind);
        QNP_CHECK(parsed.endpoints[i].ipv4BE == address.endpoints[i].ipv4BE);
        QNP_CHECK(parsed.endpoints[i].portBE == address.endpoints[i].portBE);
    }

    std::string again;
    QNP_REQUIRE(formatSpectateUri(parsed, again, err));
    QNP_CHECK(again == uri);

    SpectateAddress lanOnly{};
    lanOnly.sessionId = 7;
    lanOnly.endpoints[0] =
        candidate(CandidateKind::PrivateLan, be(192, 168, 4, 4), 14764);
    lanOnly.endpointCount = 1;
    std::string lanUri;
    QNP_REQUIRE(formatSpectateUri(lanOnly, lanUri, err));
    QNP_CHECK(lanUri.find('#') == std::string::npos);
    QNP_CHECK(lanUri.find("kt=") == std::string::npos);
    SpectateAddress lanParsed{};
    QNP_REQUIRE(parseSpectateUri(lanUri, lanParsed, err));
    QNP_CHECK(!lanParsed.hasKey);
    QNP_CHECK(lanParsed.endpointCount == 1);
    QNP_CHECK(lanParsed.endpoints[0].ipv4BE == be(192, 168, 4, 4));
}

QNP_TEST(SpectateUriLoggableFormNeverCarriesTheKey) {
    const SpectateAddress address = sampleAddress();
    std::string full;
    std::string loggable;
    SpectateUriError err = SpectateUriError::BadField;
    QNP_REQUIRE(formatSpectateUri(address, full, err));
    QNP_REQUIRE(formatSpectateUriLoggable(address, loggable, err));

    const std::string keyHex = keyHexOf(address);
    char relayTokenHexBuffer[33] = {};
    std::snprintf(
        relayTokenHexBuffer, sizeof(relayTokenHexBuffer),
        "%016llX%016llX",
        static_cast<unsigned long long>(
            address.relayAccessTokenHigh),
        static_cast<unsigned long long>(
            address.relayAccessTokenLow));
    const std::string relayTokenHex = relayTokenHexBuffer;
    QNP_REQUIRE(keyHex.size() == 32);
    QNP_REQUIRE(relayTokenHex.size() == 32);
    QNP_CHECK(full.find(keyHex) != std::string::npos);
    QNP_CHECK(full.find(relayTokenHex) != std::string::npos);

    QNP_CHECK(loggable.find(keyHex) == std::string::npos);
    QNP_CHECK(loggable.find(keyHex.substr(0, 16)) == std::string::npos);
    QNP_CHECK(loggable.find(keyHex.substr(16)) == std::string::npos);
    QNP_CHECK(loggable.find(relayTokenHex) == std::string::npos);
    QNP_CHECK(loggable.find(relayTokenHex.substr(0, 16))
              == std::string::npos);
    QNP_CHECK(loggable.find(relayTokenHex.substr(16))
              == std::string::npos);
    QNP_CHECK(loggable.find('#') == std::string::npos);
    QNP_CHECK(loggable.find("k=") == std::string::npos);

    QNP_REQUIRE(full.size() > loggable.size());
    QNP_CHECK(full.compare(0, loggable.size(), loggable) == 0);
    QNP_CHECK(full[loggable.size()] == '#');

    SpectateAddress parsed{};
    QNP_REQUIRE(parseSpectateUri(loggable, parsed, err));
    QNP_CHECK(!parsed.hasKey);
    QNP_CHECK(parsed.keyLow == 0 && parsed.keyHigh == 0);
    QNP_CHECK(parsed.sessionId == address.sessionId);
    QNP_CHECK(parsed.controlPort == address.controlPort);
    QNP_CHECK(parsed.endpointCount == address.endpointCount);

    const uint32_t tag = spectateKeyTag(address.keyLow, address.keyHigh);
    QNP_CHECK(tag != 0);
    QNP_CHECK(tag == spectateKeyTag(address.keyLow, address.keyHigh));
    QNP_CHECK(tag != spectateKeyTag(address.keyLow ^ 1ull, address.keyHigh));
    QNP_CHECK(tag != spectateKeyTag(address.keyLow, address.keyHigh ^ 1ull));
    QNP_CHECK(tag != static_cast<uint32_t>(address.keyLow));
    QNP_CHECK(tag != static_cast<uint32_t>(address.keyLow >> 32));
    QNP_CHECK(tag != static_cast<uint32_t>(address.keyHigh));
    QNP_CHECK(tag != static_cast<uint32_t>(address.keyHigh >> 32));

    QNP_CHECK(redactSpectateUri(full) == loggable + "#<redacted>");
    QNP_CHECK(redactSpectateUri("no-fragment") == "no-fragment");
}

QNP_TEST(SpectateUriRejectsMalformedAndOversizedInput) {
    const SpectateAddress address = sampleAddress();
    std::string valid;
    SpectateUriError err = SpectateUriError::None;
    QNP_REQUIRE(formatSpectateUri(address, valid, err));

    SpectateAddress parsed{};
    struct Case { const char* text; SpectateUriError expected; };
    const std::string oversized =
        "qoh99://spectate/1?s=1&lan=" + std::string(kMaxSpectateUriLength, '9');

    QNP_CHECK(!parseSpectateUri("", parsed, err));
    QNP_CHECK(err == SpectateUriError::Empty);
    QNP_CHECK(!parseSpectateUri(oversized, parsed, err));
    QNP_CHECK(err == SpectateUriError::TooLong);

    const Case cases[] = {
        { "http://spectate/1?s=1&lan=10.0.0.1:1", SpectateUriError::BadScheme },
        { "qoh99://join/1?s=1&lan=10.0.0.1:1",    SpectateUriError::BadScheme },
        { "qoh99://spectate/1",                   SpectateUriError::BadScheme },
        { "qoh99://spectate/3?s=1&lan=10.0.0.1:1", SpectateUriError::BadVersion },
        { "qoh99://spectate/01?s=1&lan=10.0.0.1:1", SpectateUriError::BadVersion },
        { "qoh99://spectate/?s=1&lan=10.0.0.1:1", SpectateUriError::BadVersion },
        { "qoh99://spectate/1?lan=10.0.0.1:1",    SpectateUriError::BadSession },
        { "qoh99://spectate/1?s=0&lan=10.0.0.1:1", SpectateUriError::BadSession },
        { "qoh99://spectate/1?s=x&lan=10.0.0.1:1", SpectateUriError::BadSession },
        { "qoh99://spectate/1?s=99999999999&lan=10.0.0.1:1",
          SpectateUriError::BadSession },
        { "qoh99://spectate/1?s=1&s=2&lan=10.0.0.1:1",
          SpectateUriError::BadField },
        { "qoh99://spectate/2?s=1&cp=0&lan=10.0.0.1:1",
          SpectateUriError::BadField },
        { "qoh99://spectate/2?s=1&cp=70000&lan=10.0.0.1:1",
          SpectateUriError::BadField },
        { "qoh99://spectate/2?s=1&cp=7500&cp=7501&lan=10.0.0.1:1",
          SpectateUriError::BadField },
        { "qoh99://spectate/1?s=1&cp=7500&lan=10.0.0.1:1",
          SpectateUriError::BadField },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:1&bogus=1",
          SpectateUriError::BadField },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:1&lan=10.0.0.2:1",
          SpectateUriError::BadField },
        { "qoh99://spectate/1?s=1&&lan=10.0.0.1:1",
          SpectateUriError::BadField },
        { "qoh99://spectate/1?s=1&lan=", SpectateUriError::BadField },
        { "qoh99://spectate/1?s=1&=10.0.0.1:1", SpectateUriError::BadField },
        { "qoh99://spectate/1?s=1",              SpectateUriError::BadEndpoint },
        { "qoh99://spectate/1?s=1&lan=10.0.0.256:1",
          SpectateUriError::BadEndpoint },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:0",
          SpectateUriError::BadEndpoint },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:70000",
          SpectateUriError::BadEndpoint },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1",
          SpectateUriError::BadEndpoint },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:1,",
          SpectateUriError::BadEndpoint },
        { "qoh99://spectate/1?s=1&lan=010.0.0.1:1",
          SpectateUriError::BadEndpoint },
        { "qoh99://spectate/1?s=1&lan=203.0.113.7:1",
          SpectateUriError::BadCandidateSet },
        { "qoh99://spectate/1?s=1&pub=192.168.1.5:1",
          SpectateUriError::BadCandidateSet },
        { "qoh99://spectate/1?s=1&lan=169.254.1.1:1",
          SpectateUriError::BadCandidateSet },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:1,10.0.0.1:1",
          SpectateUriError::BadCandidateSet },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:1&relay=10.0.0.1:1",
          SpectateUriError::BadCandidateSet },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:1,10.0.0.2:1,10.0.0.3:1,"
          "10.0.0.4:1,10.0.0.5:1,10.0.0.6:1,10.0.0.7:1",
          SpectateUriError::TooManyEndpoints },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:1#", SpectateUriError::BadKey },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:1#k=", SpectateUriError::BadKey },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:1#k=00",
          SpectateUriError::BadKey },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:1"
          "#k=0000000000000000000000000000000",
          SpectateUriError::BadKey },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:1"
          "#k=000000000000000000000000000000ZZ",
          SpectateUriError::BadKey },
        { "qoh99://spectate/1?s=1&lan=10.0.0.1:1"
          "#k=00000000000000000000000000000000",
          SpectateUriError::BadKey },
    };
    for (const Case& item : cases) {
        SpectateAddress rejected{};
        rejected.sessionId = 0xDEAD;
        QNP_CHECK(!parseSpectateUri(item.text, rejected, err));
        QNP_CHECK(err == item.expected);
        QNP_CHECK(rejected.sessionId == 0xDEAD);
    }

    const size_t hash = valid.find('#');
    QNP_REQUIRE(hash != std::string::npos);
    std::string tampered = valid;
    const size_t tagAt = tampered.find("&kt=");
    QNP_REQUIRE(tagAt != std::string::npos);
    tampered[tagAt + 4] = tampered[tagAt + 4] == 'A' ? 'B' : 'A';
    QNP_CHECK(!parseSpectateUri(tampered, parsed, err));
    QNP_CHECK(err == SpectateUriError::KeyTagMismatch);

    std::string untagged = valid.substr(0, tagAt) + valid.substr(hash);
    QNP_CHECK(!parseSpectateUri(untagged, parsed, err));
    QNP_CHECK(err == SpectateUriError::KeyTagMismatch);

    QNP_CHECK(!parseSpectateUri(
        "qoh99://spectate/2?s=1&relay=198.51.100.2:14764",
        parsed, err));
    QNP_CHECK(err == SpectateUriError::BadRelayBinding);
    QNP_CHECK(!parseSpectateUri(
        "qoh99://spectate/2?s=1&lan=10.0.0.1:1"
        "&rr=102030405060708090A0B0C0D0E0F001",
        parsed, err));
    QNP_CHECK(err == SpectateUriError::BadRelayBinding);
    std::string badRelayTag = valid;
    const size_t relayTagAt = badRelayTag.find("&rat=");
    QNP_REQUIRE(relayTagAt != std::string::npos);
    badRelayTag[relayTagAt + 5] =
        badRelayTag[relayTagAt + 5] == 'A' ? 'B' : 'A';
    QNP_CHECK(!parseSpectateUri(badRelayTag, parsed, err));
    QNP_CHECK(err == SpectateUriError::RelayTagMismatch);

    std::string embedded = valid;
    embedded[10] = '\0';
    QNP_CHECK(!parseSpectateUri(embedded, parsed, err));

    SpectateAddress bad = address;
    bad.version = 3;
    QNP_CHECK(!formatSpectateUri(bad, valid, err));
    QNP_CHECK(err == SpectateUriError::BadVersion);
    bad = address;
    bad.sessionId = 0;
    QNP_CHECK(!formatSpectateUri(bad, valid, err));
    QNP_CHECK(err == SpectateUriError::BadSession);
    bad = address;
    bad.endpointCount = 0;
    QNP_CHECK(!formatSpectateUri(bad, valid, err));
    QNP_CHECK(err == SpectateUriError::TooManyEndpoints);
    bad = address;
    bad.endpoints[0].kind = CandidateKind::PublicObserved;
    QNP_CHECK(!formatSpectateUri(bad, valid, err));
    QNP_CHECK(err == SpectateUriError::BadCandidateSet);
    bad = address;
    bad.keyLow = 0;
    bad.keyHigh = 0;
    QNP_CHECK(!formatSpectateUri(bad, valid, err));
    QNP_CHECK(err == SpectateUriError::BadKey);

    QNP_CHECK(std::string(spectateUriErrorText(SpectateUriError::BadKey))
              != std::string(spectateUriErrorText(SpectateUriError::None)));
}

QNP_TEST(ObservedRegistrationBindsToTheRegistrationAndDegradesExplicitly) {
    using namespace qnet::relay;

    Endpoint relayEndpoint;
    QNP_REQUIRE(Endpoint::parse("198.51.100.2", 14764, relayEndpoint));

    PairResult pair{};
    pair.room = { 0x0102030405060708ull, 0x1112131415161718ull };
    pair.domain = LaneDomain::Player;
    pair.role = RelayRole::Host;
    pair.token = { 0x2122232425262728ull, 0x3132333435363738ull };
    pair.relayEndpoint = relayEndpoint;

    RelayUdpClient client;
    QNP_REQUIRE(client.configure(pair));
    QNP_CHECK(!client.observedRegistration().valid);
    QNP_CHECK(!client.observedRegistration().relaySupportsObserved);
    QNP_CHECK(!client.observedRegistration().usableAsPublicCandidate);

    Endpoint publicMapping;
    QNP_REQUIRE(Endpoint::parse("203.0.113.9", 51000, publicMapping));

    RegistrationToken otherToken{ 1, 2 };
    std::string error;
    QNP_CHECK(!client.applyObservedRegistration(otherToken, publicMapping,
                                                &error));
    QNP_CHECK(!error.empty());
    QNP_CHECK(!client.observedRegistration().valid);
    RegistrationToken zeroToken{};
    QNP_CHECK(!client.applyObservedRegistration(zeroToken, publicMapping));

    Endpoint zeroAddress;
    QNP_CHECK(!client.applyObservedRegistration(pair.token, zeroAddress));
    Endpoint zeroPort;
    QNP_REQUIRE(Endpoint::parse("203.0.113.9", 0, zeroPort));
    QNP_CHECK(!client.applyObservedRegistration(pair.token, zeroPort));
    Endpoint linkLocal;
    QNP_REQUIRE(Endpoint::parse("169.254.3.4", 51000, linkLocal));
    QNP_CHECK(!client.applyObservedRegistration(pair.token, linkLocal));
    Endpoint multicast;
    QNP_REQUIRE(Endpoint::parse("224.0.0.7", 51000, multicast));
    QNP_CHECK(!client.applyObservedRegistration(pair.token, multicast));
    QNP_CHECK(!client.observedRegistration().valid);

    QNP_REQUIRE(client.applyObservedRegistration(pair.token, publicMapping,
                                                 &error));
    QNP_CHECK(error.empty());
    QNP_CHECK(client.observedRegistration().valid);
    QNP_CHECK(client.observedRegistration().relaySupportsObserved);
    QNP_CHECK(client.observedRegistration().usableAsPublicCandidate);
    QNP_CHECK(client.observedRegistration().observedPublic == publicMapping);
    QNP_CHECK(client.applyObservedRegistration(pair.token, publicMapping));
    QNP_CHECK(client.observedRegistration().valid);

    Endpoint conflicting;
    QNP_REQUIRE(Endpoint::parse("203.0.113.10", 51000, conflicting));
    QNP_CHECK(!client.applyObservedRegistration(pair.token, conflicting,
                                                &error));
    QNP_CHECK(!error.empty());
    QNP_CHECK(!client.observedRegistration().valid);

    RelayUdpClient lanClient;
    QNP_REQUIRE(lanClient.configure(pair));
    Endpoint lanMapping;
    QNP_REQUIRE(Endpoint::parse("192.168.1.9", 51000, lanMapping));
    QNP_REQUIRE(lanClient.applyObservedRegistration(pair.token, lanMapping));
    QNP_CHECK(lanClient.observedRegistration().valid);
    QNP_CHECK(lanClient.observedRegistration().relaySupportsObserved);
    QNP_CHECK(!lanClient.observedRegistration().usableAsPublicCandidate);

    lanClient.reset();
    QNP_CHECK(!lanClient.observedRegistration().valid);
    QNP_CHECK(!lanClient.observedRegistration().relaySupportsObserved);
    QNP_CHECK(!lanClient.applyObservedRegistration(pair.token, lanMapping));

    QNP_CHECK(isAcceptableObservedEndpoint(publicMapping));
    QNP_CHECK(isPublicObservedCandidate(publicMapping));
    QNP_CHECK(isAcceptableObservedEndpoint(lanMapping));
    QNP_CHECK(!isPublicObservedCandidate(lanMapping));
    Endpoint loopback;
    QNP_REQUIRE(Endpoint::parse("127.0.0.1", 51000, loopback));
    QNP_CHECK(isAcceptableObservedEndpoint(loopback));
    QNP_CHECK(!isPublicObservedCandidate(loopback));
    QNP_CHECK(!isAcceptableObservedEndpoint(linkLocal));
    QNP_CHECK(!isAcceptableObservedEndpoint(zeroAddress));
    QNP_CHECK(!isPublicObservedCandidate(zeroAddress));

    CandidateEndpoint asCandidate =
        candidate(CandidateKind::PublicObserved,
                  publicMapping.sa.sin_addr.s_addr, 51000);
    QNP_CHECK(validateCandidateSet(&asCandidate, 1) == CandidateSetError::None);
}

QNP_TEST(ObservedWelcomeReaderBindsToTheExtensionWhenItExists) {
    using namespace qnet::relay;

    struct NoExtension { RoomId room{}; };
    struct WithObservedPublic { Ipv4Endpoint observedPublic{}; };
    struct WithObservedEndpoint { Ipv4Endpoint observedEndpoint{}; };

    Ipv4Endpoint out{};
    out.ipv4 = 0xDEADBEEFu;
    out.port = 4242;
    const NoExtension older{};
    QNP_CHECK(!observed_ext::readWelcomeObserved(older, out));
    QNP_CHECK(out.ipv4 == 0xDEADBEEFu && out.port == 4242);

    WithObservedPublic newer{};
    newer.observedPublic.ipv4 = 0x0971000Cu;
    newer.observedPublic.port = 51000;
    QNP_CHECK(observed_ext::readWelcomeObserved(newer, out));
    QNP_CHECK(out.ipv4 == 0x0971000Cu && out.port == 51000);

    WithObservedEndpoint alternate{};
    alternate.observedEndpoint.ipv4 = 0x0A71000Cu;
    alternate.observedEndpoint.port = 51001;
    QNP_CHECK(observed_ext::readWelcomeObserved(alternate, out));
    QNP_CHECK(out.ipv4 == 0x0A71000Cu && out.port == 51001);

    Welcome live{};
    Ipv4Endpoint fromLive{};
    const bool protocolCarriesObserved =
        observed_ext::readWelcomeObserved(live, fromLive);
    QNP_CHECK(protocolCarriesObserved || fromLive.ipv4 == 0);
}

QNP_TEST(ObservedDiscoveryRequestsAndConsumesTheRelayAnswer) {
    using namespace qnet::relay;

    struct FakeSender {
        std::vector<std::pair<Endpoint, std::vector<uint8_t>>> sent;
        static bool send(void* context, const Endpoint& destination,
                         const void* bytes, size_t length) {
            auto& self = *static_cast<FakeSender*>(context);
            const auto* first = static_cast<const uint8_t*>(bytes);
            self.sent.emplace_back(destination,
                                   std::vector<uint8_t>(first, first + length));
            return true;
        }
    };

    Endpoint relayEndpoint;
    QNP_REQUIRE(Endpoint::parse("198.51.100.2", 14764, relayEndpoint));
    PairResult host{};
    host.room = { 0x0102030405060708ull, 0x1112131415161718ull };
    host.domain = LaneDomain::Player;
    host.role = RelayRole::Host;
    host.token = { 0x2122232425262728ull, 0x3132333435363738ull };
    host.relayEndpoint = relayEndpoint;

    RelayUdpClient client;
    std::string error;
    QNP_REQUIRE(client.configure(host, &error));
    QNP_CHECK(client.observedDiscoveryEnabled());
    QNP_CHECK(!client.observedDiscoveryExhausted());

    FakeSender sender;
    QNP_REQUIRE(client.serviceWith(&FakeSender::send, &sender, 0, &error));
    QNP_REQUIRE(sender.sent.size() == 2);
    QNP_CHECK(sender.sent[1].first == relayEndpoint);
    QNP_CHECK(client.stats().registersSent == 1);
    QNP_CHECK(client.stats().observedRequestsSent == 1);
    ObservedRequest decodedRequest{};
    QNP_CHECK(decodeObservedRequest(sender.sent[1].second.data(),
                                    sender.sent[1].second.size(),
                                    decodedRequest) == DecodeError::None);
    QNP_CHECK(decodedRequest.room == host.room);
    QNP_CHECK(decodedRequest.token == host.token);
    QNP_CHECK(decodedRequest.role == RelayRole::Host);

    auto answerBytes = [&](const RegistrationToken& token,
                           const char* ipv4, uint16_t port) {
        Endpoint mapped;
        QNP_CHECK(Endpoint::parse(ipv4, port, mapped));
        ObservedEndpoint answer{};
        answer.room = host.room;
        answer.domain = host.domain;
        answer.role = host.role;
        answer.token = token;
        answer.observed = { mapped.sa.sin_addr.s_addr, port };
        std::vector<uint8_t> bytes;
        QNP_CHECK(encodeObservedEndpoint(answer, bytes));
        return bytes;
    };

    RegistrationToken forged = host.token;
    ++forged.low;
    const std::vector<uint8_t> forgedAnswer =
        answerBytes(forged, "203.0.113.99", 51000);
    QNP_CHECK(client.handleInbound(forgedAnswer.data(), forgedAnswer.size(),
                                   relayEndpoint, 1)
              == UdpInbound::Consumed);
    QNP_CHECK(!client.observedRegistration().valid);
    QNP_CHECK(client.stats().observedRejected == 1);
    Endpoint impostor;
    QNP_REQUIRE(Endpoint::parse("203.0.113.50", 14764, impostor));
    const std::vector<uint8_t> good =
        answerBytes(host.token, "203.0.113.9", 51000);
    QNP_CHECK(client.handleInbound(good.data(), good.size(), impostor, 1)
              == UdpInbound::NotHandled);
    QNP_CHECK(!client.observedRegistration().valid);

    QNP_CHECK(client.handleInbound(good.data(), good.size(), relayEndpoint, 1)
              == UdpInbound::Consumed);
    QNP_CHECK(client.stats().observedAccepted == 1);
    QNP_REQUIRE(client.observedRegistration().valid);
    QNP_CHECK(client.observedRegistration().relaySupportsObserved);
    QNP_CHECK(client.observedRegistration().usableAsPublicCandidate);
    QNP_CHECK(client.observedRegistration().observedPublic.str()
              == "203.0.113.9:51000");

    const size_t afterAnswer = sender.sent.size();
    QNP_REQUIRE(client.serviceWith(&FakeSender::send, &sender, 5000, &error));
    QNP_CHECK(client.stats().observedRequestsSent == 1);
    QNP_CHECK(sender.sent.size() == afterAnswer + 1);

    RelayUdpClient silent;
    FakeSender silentSender;
    QNP_REQUIRE(silent.configure(host, &error));
    uint64_t nowMs = 0;
    for (int tick = 0; tick < 40 && !silent.observedDiscoveryExhausted();
         ++tick, nowMs += kObservedRequestIntervalMs) {
        QNP_REQUIRE(silent.serviceWith(&FakeSender::send, &silentSender, nowMs,
                                       &error));
    }
    QNP_CHECK(silent.observedDiscoveryExhausted());
    QNP_CHECK(silent.stats().observedRequestsSent == kMaxObservedRequests);
    QNP_CHECK(!silent.observedRegistration().valid);
    QNP_CHECK(!silent.observedRegistration().relaySupportsObserved);

    PairResult joiner = host;
    joiner.role = RelayRole::Client;
    RelayUdpClient clientRole;
    FakeSender clientSender;
    QNP_REQUIRE(clientRole.configure(joiner, &error));
    QNP_CHECK(!clientRole.observedDiscoveryEnabled());
    QNP_REQUIRE(clientRole.serviceWith(&FakeSender::send, &clientSender, 0,
                                       &error));
    QNP_CHECK(clientSender.sent.size() == 1);
    QNP_CHECK(clientRole.stats().observedRequestsSent == 0);

    clientRole.setObservedDiscoveryEnabled(true);
    QNP_CHECK(clientRole.observedDiscoveryEnabled());
    clientRole.reset();
    QNP_CHECK(!clientRole.observedDiscoveryEnabled());
}
