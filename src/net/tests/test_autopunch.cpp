#include "TestHarness.h"

#include "net/Autopunch.h"
#include "net/AutopunchPeerSet.h"
#include "net/AutopunchSession.h"

#include <cstring>
#include <string>
#include <vector>

using namespace qnet::autopunch;
using qnet::Endpoint;

QNP_TEST(AutopunchTargetsThePublicRelayAndNotALoopbackPlaceholder) {
    QNP_CHECK(std::string(kRelayHost) == "delthas.fr");
    QNP_CHECK(kRelayPort == 14763u);
    QNP_CHECK(std::string(kRelayHost).find("127.0.0.1") == std::string::npos);
    QNP_CHECK(std::string(kRelayHost).find("localhost") == std::string::npos);
    QNP_CHECK(std::string(kRelayHost).find_first_of("0123456789")
              == std::string::npos);
}

QNP_TEST(AutopunchRegisterIsTwoBigEndianPortBytes) {
    uint8_t out[kRegisterBytes] = { 0xFF, 0xFF };
    QNP_CHECK(kRegisterBytes == 2u);

    QNP_CHECK(encodeRegister(10800u, out));
    QNP_CHECK(out[0] == 0x2Au);
    QNP_CHECK(out[1] == 0x30u);

    QNP_CHECK(encodeRegister(7500u, out));
    QNP_CHECK(out[0] == 0x1Du);
    QNP_CHECK(out[1] == 0x4Cu);

    QNP_CHECK(encodeRegister(65535u, out));
    QNP_CHECK(out[0] == 0xFFu && out[1] == 0xFFu);
    QNP_CHECK(encodeRegister(49152u, out));
    QNP_CHECK(out[0] == 0xC0u && out[1] == 0x00u);

    QNP_CHECK(!encodeRegister(0u, out));
    QNP_CHECK(!encodeRegister(10800u, nullptr));
}

QNP_TEST(AutopunchLookupMatchesTheReferenceLayout) {
    uint8_t out[kLookupBytes] = {};
    QNP_CHECK(kLookupBytes == 8u);

    uint32_t target = 0;
    const uint8_t octets[4] = { 203u, 0u, 113u, 9u };
    std::memcpy(&target, octets, sizeof(target));

    QNP_CHECK(encodeLookup(10800u, target, 7500u, out));
    QNP_CHECK(out[0] == 0x2Au && out[1] == 0x30u);
    QNP_CHECK(out[2] == 203u);
    QNP_CHECK(out[3] == 0u);
    QNP_CHECK(out[4] == 113u);
    QNP_CHECK(out[5] == 9u);
    QNP_CHECK(out[6] == 0x1Du && out[7] == 0x4Cu);

    QNP_CHECK(!encodeLookup(0u, target, 7500u, out));
    QNP_CHECK(!encodeLookup(10800u, 0u, 7500u, out));
    QNP_CHECK(!encodeLookup(10800u, target, 0u, out));
    QNP_CHECK(!encodeLookup(10800u, target, 7500u, nullptr));
}

QNP_TEST(AutopunchSessionLooksUpThePortTheCallerMeant) {
    struct Capture {
        std::vector<std::vector<uint8_t>> sent;
    } capture;
    auto send = [](void* context, const Endpoint&, const void* bytes,
                   size_t length) -> bool {
        const uint8_t* p = static_cast<const uint8_t*>(bytes);
        static_cast<Capture*>(context)->sent.emplace_back(p, p + length);
        return true;
    };

    Endpoint relay{};
    QNP_REQUIRE(Endpoint::parse("203.0.113.9", 14763, relay));

    Endpoint advertised{};
    QNP_REQUIRE(Endpoint::parse("198.51.100.42", 7500, advertised));

    AutopunchSession session;
    session.configure(10800u, relay);
    session.setTarget(advertised.sa.sin_addr.s_addr,
                      ntohs(advertised.sa.sin_port));
    session.service(send, &capture, 1000u);

    const std::vector<uint8_t>* lookup = nullptr;
    for (const auto& packet : capture.sent)
        if (packet.size() == kLookupBytes) lookup = &packet;
    QNP_REQUIRE(lookup != nullptr);
    QNP_CHECK((*lookup)[0] == 0x2Au && (*lookup)[1] == 0x30u);
    QNP_CHECK((*lookup)[2] == 198u && (*lookup)[3] == 51u);
    QNP_CHECK((*lookup)[4] == 100u && (*lookup)[5] == 42u);
    QNP_CHECK((*lookup)[6] == 0x1Du && (*lookup)[7] == 0x4Cu);
}

QNP_TEST(AutopunchPunchesForAPeerThatArrivesLongAfterHostingBegan) {
    struct Capture { uint32_t punches = 0; };
    Capture capture;
    auto send = [](void* context, const Endpoint&, const void*,
                   size_t length) -> bool {
        if (length == kPunchBytes) static_cast<Capture*>(context)->punches++;
        return true;
    };

    Endpoint relay{};
    QNP_REQUIRE(Endpoint::parse("203.0.113.9", 14763, relay));

    AutopunchSession session;
    session.configure(7500u, relay);

    uint64_t now = 1000u;
    for (int i = 0; i < 60; ++i) {
        session.service(send, &capture, now);
        now += 60000u;
    }
    QNP_CHECK(capture.punches == 0);
    QNP_CHECK(session.stats().registersSent > 1);

    uint8_t mapping[kMappingBytes] = {};
    mapping[0] = 0x1Du; mapping[1] = 0x4Cu;
    mapping[2] = 0xC0u; mapping[3] = 0x00u;
    const uint8_t octets[4] = { 198u, 51u, 100u, 42u };
    std::memcpy(mapping + 4, octets, sizeof(octets));
    QNP_CHECK(session.handleInbound(mapping, sizeof(mapping), relay, now));
    QNP_REQUIRE(session.havePeerMapping());

    session.service(send, &capture, now);
    QNP_CHECK(capture.punches == kPunchBurst);
    QNP_CHECK(session.stats().punchesSent == kPunchBurst);
}

QNP_TEST(AutopunchLookupRoundTripsThroughTheMappingDecoder) {
    uint8_t lookup[kLookupBytes] = {};
    uint32_t target = 0;
    const uint8_t octets[4] = { 198u, 51u, 100u, 42u };
    std::memcpy(&target, octets, sizeof(target));
    QNP_CHECK(encodeLookup(1234u, target, 5678u, lookup));

    uint8_t mapping[kMappingBytes] = {};
    mapping[0] = lookup[0];
    mapping[1] = lookup[1];
    mapping[2] = 0xC0u;
    mapping[3] = 0x00u;
    std::memcpy(mapping + 4, lookup + 2, 4);

    Mapping decoded{};
    QNP_CHECK(decodeMapping(mapping, sizeof(mapping), decoded));
    QNP_CHECK(decoded.internalPort == 1234u);
    QNP_CHECK(decoded.natPort == 49152u);
    QNP_CHECK(decoded.ipv4NetworkOrder == target);
}

QNP_TEST(AutopunchMappingRefusesEveryUnusableForm) {
    uint8_t mapping[kMappingBytes] = { 0x2Au, 0x30u, 0xC0u, 0x00u,
                                       203u, 0u, 113u, 9u };
    Mapping decoded{};
    QNP_CHECK(decodeMapping(mapping, sizeof(mapping), decoded));
    QNP_CHECK(decoded.internalPort == 10800u);
    QNP_CHECK(decoded.natPort == 49152u);

    QNP_CHECK(!decodeMapping(mapping, 7u, decoded));
    QNP_CHECK(!decodeMapping(mapping, 9u, decoded));
    QNP_CHECK(!decodeMapping(mapping, 0u, decoded));
    QNP_CHECK(!decodeMapping(nullptr, kMappingBytes, decoded));

    {
        uint8_t zeroInternal[kMappingBytes] = { 0, 0, 0xC0u, 0x00u,
                                                203u, 0u, 113u, 9u };
        QNP_CHECK(!decodeMapping(zeroInternal, kMappingBytes, decoded));
    }
    {
        uint8_t zeroNat[kMappingBytes] = { 0x2Au, 0x30u, 0, 0,
                                           203u, 0u, 113u, 9u };
        QNP_CHECK(!decodeMapping(zeroNat, kMappingBytes, decoded));
    }
    {
        uint8_t zeroHost[kMappingBytes] = { 0x2Au, 0x30u, 0xC0u, 0x00u,
                                            0, 0, 0, 0 };
        QNP_CHECK(!decodeMapping(zeroHost, kMappingBytes, decoded));
    }

    decoded.internalPort = 1u;
    decoded.natPort = 1u;
    decoded.ipv4NetworkOrder = 1u;
    QNP_CHECK(!decodeMapping(mapping, 3u, decoded));
    QNP_CHECK(decoded.internalPort == 0u);
    QNP_CHECK(decoded.natPort == 0u);
    QNP_CHECK(decoded.ipv4NetworkOrder == 0u);
}

QNP_TEST(AutopunchProbeIsExactlyOneZeroByte) {
    const uint8_t zero[1] = { 0x00u };
    QNP_CHECK(isPunchProbe(zero, 1u));
    QNP_CHECK(kPunchByte == 0x00u);
    QNP_CHECK(kPunchBytes == 1u);

    const uint8_t nonZero[1] = { 0x01u };
    QNP_CHECK(!isPunchProbe(nonZero, 1u));
    const uint8_t twoZeroes[2] = { 0x00u, 0x00u };
    QNP_CHECK(!isPunchProbe(twoZeroes, 2u));
    QNP_CHECK(!isPunchProbe(zero, 0u));
    QNP_CHECK(!isPunchProbe(nullptr, 1u));
}

QNP_TEST(AutopunchPacketSizesAreBounded) {
    QNP_CHECK(kMaxPacketBytes >= kRegisterBytes);
    QNP_CHECK(kMaxPacketBytes >= kLookupBytes);
    QNP_CHECK(kMaxPacketBytes >= kMappingBytes);
    QNP_CHECK(kMaxPacketBytes >= kPunchBytes);
    QNP_CHECK(kMaxPacketBytes == 8u);
}

namespace {

struct Recorder {
    struct Sent { Endpoint to; std::vector<uint8_t> bytes; };
    std::vector<Sent> sent;
    bool allow = true;

    static bool send(void* ctx, const Endpoint& to, const void* bytes,
                     size_t length) {
        Recorder* self = static_cast<Recorder*>(ctx);
        if (!self->allow) return false;
        Sent record;
        record.to = to;
        record.bytes.assign(static_cast<const uint8_t*>(bytes),
                            static_cast<const uint8_t*>(bytes) + length);
        self->sent.push_back(std::move(record));
        return true;
    }
    size_t countOfSize(size_t n) const {
        size_t c = 0;
        for (const Sent& s : sent) if (s.bytes.size() == n) ++c;
        return c;
    }
};

Endpoint makeEndpoint(uint32_t ipv4NetworkOrder, uint16_t port) {
    Endpoint e{};
    e.sa.sin_family = AF_INET;
    e.sa.sin_addr.s_addr = ipv4NetworkOrder;
    e.sa.sin_port = htons(port);
    return e;
}

uint32_t ipv4Of(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t octets[4] = { a, b, c, d };
    uint32_t v = 0;
    std::memcpy(&v, octets, sizeof(v));
    return v;
}

}

using qnet::autopunch::AutopunchSession;

QNP_TEST(AutopunchSessionRegistersEvenWithNoTarget) {
    AutopunchSession session;
    session.configure(10800u, makeEndpoint(ipv4Of(65, 108, 63, 185), 14763u));
    QNP_CHECK(session.configured());
    QNP_CHECK(!session.haveTarget());

    Recorder rec;
    session.service(&Recorder::send, &rec, 1000u);
    QNP_CHECK(rec.countOfSize(qnet::autopunch::kRegisterBytes) == 1u);
    QNP_CHECK(rec.countOfSize(qnet::autopunch::kLookupBytes) == 0u);

    session.service(&Recorder::send, &rec, 1010u);
    QNP_CHECK(rec.countOfSize(qnet::autopunch::kRegisterBytes) == 1u);
    session.service(&Recorder::send, &rec,
                    1000u + qnet::autopunch::kRegisterIntervalMs);
    QNP_CHECK(rec.countOfSize(qnet::autopunch::kRegisterBytes) == 2u);
}

QNP_TEST(AutopunchSessionLooksUpOnlyWhenItKnowsWhoToAskAbout) {
    AutopunchSession session;
    session.configure(10800u, makeEndpoint(ipv4Of(65, 108, 63, 185), 14763u));
    Recorder rec;
    session.service(&Recorder::send, &rec, 0u);
    QNP_CHECK(rec.countOfSize(qnet::autopunch::kLookupBytes) == 0u);

    session.setTarget(ipv4Of(198, 51, 100, 7), 7500u);
    QNP_CHECK(session.haveTarget());
    session.service(&Recorder::send, &rec, 5000u);
    QNP_CHECK(rec.countOfSize(qnet::autopunch::kLookupBytes) == 1u);
}

QNP_TEST(AutopunchSessionOnlyTrustsMappingsFromTheRelay) {
    const Endpoint relay = makeEndpoint(ipv4Of(65, 108, 63, 185), 14763u);
    AutopunchSession session;
    session.configure(10800u, relay);

    const uint8_t mapping[8] = { 0x2Au, 0x30u, 0xC0u, 0x00u, 198u, 51u, 100u, 7u };

    const Endpoint impostor = makeEndpoint(ipv4Of(203, 0, 113, 99), 14763u);
    QNP_CHECK(!session.handleInbound(mapping, sizeof(mapping), impostor, 100u));
    QNP_CHECK(!session.havePeerMapping());

    QNP_CHECK(session.handleInbound(mapping, sizeof(mapping), relay, 100u));
    QNP_CHECK(session.havePeerMapping());
    QNP_CHECK(session.peerMapping().sa.sin_addr.s_addr == ipv4Of(198, 51, 100, 7));
    QNP_CHECK(ntohs(session.peerMapping().sa.sin_port) == 49152u);
    QNP_CHECK(session.stats().mappingsReceived == 1u);
}

QNP_TEST(AutopunchSessionAcceptsRelaySelectedSidecarOnTheTargetHost) {
    const Endpoint relay = makeEndpoint(ipv4Of(65, 108, 63, 185), 14763u);
    const uint32_t hostIp = ipv4Of(198, 51, 100, 7);
    AutopunchSession session;
    session.configure(54000u, relay);
    session.setTarget(hostIp, 10800u);

    const uint8_t spectatorMapping[8] = {
        0xC7u, 0x0Cu, 0xF2u, 0x31u, 198u, 51u, 100u, 7u
    };
    QNP_CHECK(session.handleInbound(spectatorMapping,
                                    sizeof(spectatorMapping), relay, 10u));
    QNP_REQUIRE(session.havePeerMapping());
    QNP_CHECK(session.peerMapping().sa.sin_addr.s_addr == hostIp);
    QNP_CHECK(ntohs(session.peerMapping().sa.sin_port) == 62001u);
    QNP_CHECK(session.stats().mappingsReceived == 1u);
    QNP_CHECK(session.stats().mappingsRejected == 0u);
    QNP_CHECK(session.stats().mappingTargetMismatches == 0u);
    QNP_CHECK(session.stats().mappingSameIpAlternates == 1u);

    const uint8_t foreignMapping[8] = {
        0x2Au, 0x30u, 0xF2u, 0x32u, 203u, 0u, 113u, 9u
    };
    QNP_CHECK(session.handleInbound(foreignMapping,
                                    sizeof(foreignMapping), relay, 20u));
    QNP_REQUIRE(session.havePeerMapping());
    QNP_CHECK(session.peerMapping().sa.sin_addr.s_addr == hostIp);
    QNP_CHECK(ntohs(session.peerMapping().sa.sin_port) == 62001u);
    QNP_CHECK(session.stats().mappingsReceived == 2u);
    QNP_CHECK(session.stats().mappingsRejected == 1u);
    QNP_CHECK(session.stats().mappingTargetMismatches == 1u);
    QNP_CHECK(session.stats().mappingSameIpAlternates == 1u);
}

QNP_TEST(AutopunchSessionAcceptsTheOfficialExternalPortFallback) {
    const Endpoint relay = makeEndpoint(ipv4Of(65, 108, 63, 185), 14763u);
    const uint32_t hostIp = ipv4Of(198, 51, 100, 8);
    AutopunchSession session;
    session.configure(54000u, relay);
    session.setTarget(hostIp, 10800u);

    const uint8_t externalAlias[8] = {
        0xEBu, 0x3Eu, 0x2Au, 0x30u, 198u, 51u, 100u, 8u
    };
    QNP_CHECK(session.handleInbound(externalAlias, sizeof(externalAlias),
                                    relay, 10u));
    QNP_REQUIRE(session.havePeerMapping());
    QNP_CHECK(ntohs(session.peerMapping().sa.sin_port) == 10800u);
    QNP_CHECK(session.stats().mappingsRejected == 0u);
    QNP_CHECK(session.stats().mappingTargetMismatches == 0u);
    QNP_CHECK(session.stats().mappingExternalPortAliases == 1u);
    QNP_CHECK(session.stats().lastMappingInternalPort == 60222u);
    QNP_CHECK(session.stats().lastMappingNatPort == 10800u);
    QNP_CHECK(session.stats().lastMappingIpv4NetworkOrder == hostIp);
}

QNP_TEST(AutopunchSessionPunchesTheMappingAndConfirmsOnTheReturn) {
    const Endpoint relay = makeEndpoint(ipv4Of(65, 108, 63, 185), 14763u);
    AutopunchSession session;
    session.configure(10800u, relay);
    Recorder rec;
    session.service(&Recorder::send, &rec, 0u);

    const uint8_t mapping[8] = { 0x2Au, 0x30u, 0xC0u, 0x00u, 198u, 51u, 100u, 7u };
    QNP_CHECK(session.handleInbound(mapping, sizeof(mapping), relay, 10u));

    session.service(&Recorder::send, &rec, 20u);
    QNP_CHECK(rec.countOfSize(qnet::autopunch::kPunchBytes)
              == qnet::autopunch::kPunchBurst);
    const Endpoint expected = makeEndpoint(ipv4Of(198, 51, 100, 7), 49152u);
    bool aimedCorrectly = false;
    for (const Recorder::Sent& s : rec.sent) {
        if (s.bytes.size() == 1u
            && s.to.sa.sin_addr.s_addr == expected.sa.sin_addr.s_addr
            && s.to.sa.sin_port == expected.sa.sin_port) {
            aimedCorrectly = true;
        }
    }
    QNP_CHECK(aimedCorrectly);
    QNP_CHECK(!session.peerReachable());

    const uint8_t probe[1] = { 0x00u };
    QNP_CHECK(session.handleInbound(probe, sizeof(probe), expected, 30u));
    QNP_CHECK(session.peerReachable());
    QNP_CHECK(session.stats().punchesReceived == 1u);

    const size_t before = rec.countOfSize(qnet::autopunch::kPunchBytes);
    session.service(&Recorder::send, &rec, 5000u);
    QNP_CHECK(rec.countOfSize(qnet::autopunch::kPunchBytes) == before);
}

QNP_TEST(AutopunchSessionConsumesProbesSoTheyNeverReachTheGame) {
    AutopunchSession session;
    session.configure(10800u,
                      makeEndpoint(ipv4Of(65, 108, 63, 185), 14763u));
    const uint8_t probe[1] = { 0x00u };
    const Endpoint stranger = makeEndpoint(ipv4Of(203, 0, 113, 5), 1234u);
    QNP_CHECK(session.handleInbound(probe, sizeof(probe), stranger, 0u));

    const uint8_t game[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    QNP_CHECK(!session.handleInbound(game, sizeof(game), stranger, 0u));
}

namespace {

void encodeMappingFixture(uint16_t internalPort, uint16_t observedPort,
                          uint32_t ipv4NetworkOrder,
                          uint8_t out[kMappingBytes]) {
    out[0] = static_cast<uint8_t>((internalPort >> 8) & 0xFFu);
    out[1] = static_cast<uint8_t>(internalPort & 0xFFu);
    out[2] = static_cast<uint8_t>((observedPort >> 8) & 0xFFu);
    out[3] = static_cast<uint8_t>(observedPort & 0xFFu);
    std::memcpy(out + 4, &ipv4NetworkOrder, sizeof(ipv4NetworkOrder));
}

}

QNP_TEST(AutopunchPeerSetAsksTheRelayAboutAnAdmittedViewerAndPunchesAtIt) {
    const Endpoint relay = makeEndpoint(ipv4Of(65, 108, 63, 185), 14763u);
    AutopunchPeerSet peers;
    peers.configure(relay);
    peers.setLocalPort(61513u);
    Recorder recorder;

    const uint32_t viewerIp = ipv4Of(203, 0, 113, 77);
    QNP_REQUIRE(peers.addLookupTarget(viewerIp, 52147u, 1000u));
    const Endpoint viewerData = makeEndpoint(viewerIp, 52147u);
    QNP_REQUIRE(peers.addKnownPeer(viewerData, 1000u));
    QNP_CHECK(peers.stats().seededPeers == 1u);

    peers.service(&Recorder::send, &recorder, 1000u);

    size_t lookups = 0;
    for (const Recorder::Sent& sent : recorder.sent) {
        if (sent.bytes.size() != kLookupBytes) continue;
        if (!(sent.to == relay)) continue;
        uint8_t expected[kLookupBytes]{};
        QNP_REQUIRE(encodeLookup(61513u, viewerIp, 52147u, expected));
        bool same = true;
        for (size_t i = 0; i < kLookupBytes; ++i)
            if (sent.bytes[i] != expected[i]) same = false;
        if (same) ++lookups;
    }
    QNP_CHECK(lookups == 1u);
    QNP_CHECK(peers.stats().lookupsSent == 1u);

    size_t punches = 0;
    for (const Recorder::Sent& sent : recorder.sent) {
        if (sent.bytes.size() == kPunchBytes && sent.to == viewerData)
            ++punches;
    }
    QNP_CHECK(punches == kPunchBurst);

    QNP_CHECK(!peers.addLookupTarget(relay.sa.sin_addr.s_addr, 14763u, 1000u));
    QNP_CHECK(!peers.addLookupTarget(0u, 52147u, 1000u));
    QNP_CHECK(!peers.addLookupTarget(viewerIp, 0u, 1000u));

    QNP_REQUIRE(peers.addLookupTarget(viewerIp, 52147u, 2000u));
    QNP_REQUIRE(peers.addKnownPeer(viewerData, 2000u));
    QNP_CHECK(peers.size() == 1u);
    QNP_CHECK(peers.stats().seededPeers == 1u);

    Recorder later;
    peers.service(&Recorder::send, &later,
                  2000u + kSourceLookupWindowMs + 1u);
    size_t lateLookups = 0;
    for (const Recorder::Sent& sent : later.sent)
        if (sent.bytes.size() == kLookupBytes) ++lateLookups;
    QNP_CHECK(lateLookups == 0u);
}

QNP_TEST(AutopunchPeerSetKeepsTwoMappingsFromOneBatchIndependent) {
    const Endpoint relay = makeEndpoint(ipv4Of(65, 108, 63, 185), 14763u);
    AutopunchPeerSet peers;
    peers.configure(relay);
    Recorder recorder;

    uint8_t a[kMappingBytes] = {};
    uint8_t b[kMappingBytes] = {};
    encodeMappingFixture(54000u, 61001u, ipv4Of(198, 51, 100, 10), a);
    encodeMappingFixture(7501u, 48127u, ipv4Of(203, 0, 113, 20), b);
    PunchMappingResult result{};
    QNP_REQUIRE(peers.handleInboundMapping(a, sizeof(a), relay, 100u,
                                            result));
    QNP_CHECK(result == PunchMappingResult::AcceptedNew);
    QNP_REQUIRE(peers.handleInboundMapping(b, sizeof(b), relay, 100u,
                                            result));
    QNP_CHECK(result == PunchMappingResult::AcceptedNew);
    QNP_CHECK(peers.size() == 2u);

    peers.service(&Recorder::send, &recorder, 100u);
    const Endpoint aEndpoint = makeEndpoint(ipv4Of(198, 51, 100, 10), 61001u);
    const Endpoint bEndpoint = makeEndpoint(ipv4Of(203, 0, 113, 20), 48127u);
    size_t aPunches = 0;
    size_t bPunches = 0;
    for (const Recorder::Sent& sent : recorder.sent) {
        if (sent.bytes.size() != kPunchBytes) continue;
        if (sent.to == aEndpoint) ++aPunches;
        if (sent.to == bEndpoint) ++bPunches;
    }
    QNP_CHECK(aPunches == kPunchBurst);
    QNP_CHECK(bPunches == kPunchBurst);
    QNP_REQUIRE(peers.find(aEndpoint) != nullptr);
    QNP_REQUIRE(peers.find(bEndpoint) != nullptr);
    QNP_CHECK(peers.find(aEndpoint)->burstsSent == 1u);
    QNP_CHECK(peers.find(bEndpoint)->burstsSent == 1u);
}

QNP_TEST(AutopunchPeerSetSuccessTimeoutAndDuplicateAreIndependent) {
    const Endpoint relay = makeEndpoint(ipv4Of(65, 108, 63, 185), 14763u);
    AutopunchPeerSet peers;
    peers.configure(relay);
    Recorder recorder;
    uint8_t a[kMappingBytes] = {};
    uint8_t b[kMappingBytes] = {};
    encodeMappingFixture(54000u, 61001u, ipv4Of(198, 51, 100, 10), a);
    encodeMappingFixture(7501u, 48127u, ipv4Of(203, 0, 113, 20), b);
    PunchMappingResult result{};
    QNP_REQUIRE(peers.handleInboundMapping(a, sizeof(a), relay, 100u,
                                            result));
    QNP_REQUIRE(peers.handleInboundMapping(b, sizeof(b), relay, 100u,
                                            result));
    peers.service(&Recorder::send, &recorder, 100u);

    QNP_REQUIRE(peers.handleInboundMapping(a, sizeof(a), relay, 200u,
                                            result));
    QNP_CHECK(result == PunchMappingResult::Refreshed);
    QNP_CHECK(peers.size() == 2u);

    const Endpoint aEndpoint = makeEndpoint(ipv4Of(198, 51, 100, 10), 61001u);
    const uint8_t probe[kPunchBytes] = {kPunchByte};
    QNP_CHECK(peers.handleProbe(probe, sizeof(probe), aEndpoint, 300u));
    const uint32_t before = peers.stats().punchesSent;
    peers.service(&Recorder::send, &recorder, 100u + kPunchIntervalMs);
    QNP_CHECK(peers.stats().punchesSent == before + kPunchBurst);

    peers.expire(100u + kPunchPeerLifetimeMs + 1u);
    QNP_CHECK(peers.size() == 1u);
    QNP_CHECK(peers.find(aEndpoint) != nullptr);
}

QNP_TEST(AutopunchPeerSetRebindingUpdatesOnlyTheBoundNode) {
    const Endpoint relay = makeEndpoint(ipv4Of(65, 108, 63, 185), 14763u);
    AutopunchPeerSet peers;
    peers.configure(relay);
    uint8_t first[kMappingBytes] = {};
    uint8_t rebound[kMappingBytes] = {};
    uint8_t other[kMappingBytes] = {};
    encodeMappingFixture(7501u, 48127u, ipv4Of(203, 0, 113, 20), first);
    encodeMappingFixture(7501u, 48128u, ipv4Of(203, 0, 113, 20), rebound);
    encodeMappingFixture(7502u, 48129u, ipv4Of(203, 0, 113, 20), other);
    PunchMappingResult result{};
    QNP_REQUIRE(peers.handleInboundMapping(first, sizeof(first), relay, 1u,
                                            result));
    const Endpoint firstEndpoint = makeEndpoint(ipv4Of(203, 0, 113, 20),
                                                 48127u);
    QNP_REQUIRE(peers.bindNode(firstEndpoint, 0xA11CEu));
    QNP_REQUIRE(peers.handleInboundMapping(other, sizeof(other), relay, 2u,
                                            result));
    QNP_REQUIRE(peers.handleInboundMapping(rebound, sizeof(rebound), relay,
                                            3u, result, 0xA11CEu));
    QNP_CHECK(result == PunchMappingResult::Rebound);
    const Endpoint reboundEndpoint = makeEndpoint(ipv4Of(203, 0, 113, 20),
                                                   48128u);
    QNP_CHECK(peers.size() == 2u);
    QNP_REQUIRE(peers.find(reboundEndpoint) != nullptr);
    QNP_CHECK(peers.find(reboundEndpoint)->routeGeneration == 2u);
    QNP_REQUIRE(peers.find(makeEndpoint(ipv4Of(203, 0, 113, 20), 48129u))
                != nullptr);
}

QNP_TEST(AutopunchPeerSetFailsClosedAndBoundsSameNatPeers) {
    const Endpoint relay = makeEndpoint(ipv4Of(65, 108, 63, 185), 14763u);
    const Endpoint impostor = makeEndpoint(ipv4Of(203, 0, 113, 9), 14763u);
    AutopunchPeerSet peers;
    peers.configure(relay);
    PunchMappingResult result{};
    uint8_t malformed[3] = {};
    QNP_CHECK(!peers.handleInboundMapping(malformed, sizeof(malformed),
                                           impostor, 1u, result));
    QNP_CHECK(peers.size() == 0u);
    QNP_REQUIRE(peers.handleInboundMapping(malformed, sizeof(malformed),
                                            relay, 1u, result));
    QNP_CHECK(result == PunchMappingResult::RejectedMalformed);
    QNP_CHECK(peers.size() == 0u);

    for (uint16_t i = 0; i < kMaxSourcePunchPeersPerIpv4; ++i) {
        uint8_t mapping[kMappingBytes] = {};
        encodeMappingFixture(static_cast<uint16_t>(7501u + i),
                             static_cast<uint16_t>(48127u + i),
                             ipv4Of(203, 0, 113, 20), mapping);
        QNP_REQUIRE(peers.handleInboundMapping(mapping, sizeof(mapping), relay,
                                                10u + i, result));
        QNP_CHECK(result == PunchMappingResult::AcceptedNew);
    }
    uint8_t overLimit[kMappingBytes] = {};
    encodeMappingFixture(7600u, 49000u, ipv4Of(203, 0, 113, 20), overLimit);
    QNP_REQUIRE(peers.handleInboundMapping(overLimit, sizeof(overLimit), relay,
                                            20u, result));
    QNP_CHECK(result == PunchMappingResult::RejectedPerIpCapacity);
    QNP_CHECK(peers.size() == kMaxSourcePunchPeersPerIpv4);
}
