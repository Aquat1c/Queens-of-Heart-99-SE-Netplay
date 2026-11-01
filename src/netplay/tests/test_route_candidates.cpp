#include "TestHarness.h"

#include "net/RouteCandidates.h"

using namespace qnet;

namespace {
constexpr uint32_t be(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return static_cast<uint32_t>(a)
         | (static_cast<uint32_t>(b) << 8)
         | (static_cast<uint32_t>(c) << 16)
         | (static_cast<uint32_t>(d) << 24);
}
constexpr uint16_t portBE(uint16_t hostPort) {
    return static_cast<uint16_t>((hostPort >> 8) | (hostPort << 8));
}
CandidateEndpoint make(CandidateKind kind, uint32_t ipv4BE,
                       uint16_t port = portBE(14763)) {
    CandidateEndpoint c;
    c.kind = kind;
    c.ipv4BE = ipv4BE;
    c.portBE = port;
    return c;
}
}

QNP_TEST(CandidateAddressClassification) {
    QNP_CHECK(isLoopbackBE(be(127, 0, 0, 1)));
    QNP_CHECK(isLoopbackBE(be(127, 255, 255, 254)));
    QNP_CHECK(!isLoopbackBE(be(128, 0, 0, 1)));

    QNP_CHECK(isPrivateBE(be(10, 0, 0, 1)));
    QNP_CHECK(isPrivateBE(be(10, 255, 255, 254)));
    QNP_CHECK(isPrivateBE(be(172, 16, 0, 1)));
    QNP_CHECK(isPrivateBE(be(172, 31, 255, 254)));
    QNP_CHECK(!isPrivateBE(be(172, 15, 255, 254)));
    QNP_CHECK(!isPrivateBE(be(172, 32, 0, 1)));
    QNP_CHECK(isPrivateBE(be(192, 168, 0, 1)));
    QNP_CHECK(!isPrivateBE(be(192, 169, 0, 1)));
    QNP_CHECK(!isPrivateBE(be(11, 0, 0, 1)));

    QNP_CHECK(isLinkLocalBE(be(169, 254, 0, 1)));
    QNP_CHECK(!isLinkLocalBE(be(169, 253, 0, 1)));

    QNP_CHECK(isNonUnicastBE(be(0, 0, 0, 0)));
    QNP_CHECK(isNonUnicastBE(be(0, 1, 2, 3)));
    QNP_CHECK(isNonUnicastBE(be(224, 0, 0, 1)));
    QNP_CHECK(isNonUnicastBE(be(239, 255, 255, 255)));
    QNP_CHECK(isNonUnicastBE(be(240, 0, 0, 1)));
    QNP_CHECK(isNonUnicastBE(be(255, 255, 255, 255)));
    QNP_CHECK(!isNonUnicastBE(be(8, 8, 8, 8)));

    QNP_CHECK(isAdvertisablePrivateBE(be(192, 168, 1, 20)));
    QNP_CHECK(isAdvertisablePrivateBE(be(127, 0, 0, 1)));
    QNP_CHECK(!isAdvertisablePrivateBE(be(169, 254, 12, 1)));
    QNP_CHECK(!isAdvertisablePrivateBE(be(8, 8, 8, 8)));
}

QNP_TEST(CandidateSetAcceptsBoundedWellFormedSet) {
    const CandidateEndpoint entries[4] = {
        make(CandidateKind::PrivateLan, be(192, 168, 1, 20)),
        make(CandidateKind::PrivateLan, be(10, 0, 0, 5)),
        make(CandidateKind::PublicObserved, be(203, 0, 113, 7)),
        make(CandidateKind::Relay, be(198, 51, 100, 2), portBE(14764)),
    };
    QNP_CHECK(validateCandidateSet(entries, 4) == CandidateSetError::None);

    const CandidateEndpoint loop[1] = {
        make(CandidateKind::PrivateLan, be(127, 0, 0, 1)),
    };
    QNP_CHECK(validateCandidateSet(loop, 1) == CandidateSetError::None);

    const CandidateEndpoint lanRelay[1] = {
        make(CandidateKind::Relay, be(192, 168, 1, 9)),
    };
    QNP_CHECK(validateCandidateSet(lanRelay, 1) == CandidateSetError::None);
}

QNP_TEST(CandidateSetRejectsMalformedSets) {
    QNP_CHECK(validateCandidateSet(nullptr, 0) == CandidateSetError::Empty);

    CandidateEndpoint tooMany[kMaxRouteCandidates + 1] = {};
    for (size_t i = 0; i < kMaxRouteCandidates + 1; ++i) {
        tooMany[i] = make(CandidateKind::PrivateLan,
                          be(10, 0, 0, static_cast<uint8_t>(1 + i)));
    }
    QNP_CHECK(validateCandidateSet(tooMany, kMaxRouteCandidates + 1)
              == CandidateSetError::TooMany);

    CandidateEndpoint tooManyPrivate[kMaxPrivateCandidates + 1] = {};
    for (size_t i = 0; i < kMaxPrivateCandidates + 1; ++i) {
        tooManyPrivate[i] = make(CandidateKind::PrivateLan,
                                 be(10, 0, 1, static_cast<uint8_t>(1 + i)));
    }
    QNP_CHECK(validateCandidateSet(tooManyPrivate, kMaxPrivateCandidates + 1)
              == CandidateSetError::TooMany);

    const CandidateEndpoint zeroAddr[1] = {
        make(CandidateKind::PrivateLan, 0),
    };
    QNP_CHECK(validateCandidateSet(zeroAddr, 1)
              == CandidateSetError::ZeroEndpoint);
    const CandidateEndpoint zeroPort[1] = {
        make(CandidateKind::PrivateLan, be(192, 168, 1, 20), 0),
    };
    QNP_CHECK(validateCandidateSet(zeroPort, 1)
              == CandidateSetError::ZeroEndpoint);

    const CandidateEndpoint badKind[1] = {
        make(static_cast<CandidateKind>(9), be(192, 168, 1, 20)),
    };
    QNP_CHECK(validateCandidateSet(badKind, 1) == CandidateSetError::BadKind);
    const CandidateEndpoint noneKind[1] = {
        make(CandidateKind::None, be(192, 168, 1, 20)),
    };
    QNP_CHECK(validateCandidateSet(noneKind, 1) == CandidateSetError::BadKind);

    const CandidateEndpoint publicAsPrivate[1] = {
        make(CandidateKind::PrivateLan, be(203, 0, 113, 7)),
    };
    QNP_CHECK(validateCandidateSet(publicAsPrivate, 1)
              == CandidateSetError::KindAddressMismatch);
    const CandidateEndpoint privateAsPublic[1] = {
        make(CandidateKind::PublicObserved, be(192, 168, 1, 20)),
    };
    QNP_CHECK(validateCandidateSet(privateAsPublic, 1)
              == CandidateSetError::KindAddressMismatch);
    const CandidateEndpoint loopbackPublic[1] = {
        make(CandidateKind::PublicObserved, be(127, 0, 0, 1)),
    };
    QNP_CHECK(validateCandidateSet(loopbackPublic, 1)
              == CandidateSetError::KindAddressMismatch);
    const CandidateEndpoint linkLocalPrivate[1] = {
        make(CandidateKind::PrivateLan, be(169, 254, 3, 4)),
    };
    QNP_CHECK(validateCandidateSet(linkLocalPrivate, 1)
              == CandidateSetError::KindAddressMismatch);
    const CandidateEndpoint linkLocalRelay[1] = {
        make(CandidateKind::Relay, be(169, 254, 3, 4)),
    };
    QNP_CHECK(validateCandidateSet(linkLocalRelay, 1)
              == CandidateSetError::KindAddressMismatch);
    const CandidateEndpoint multicastRelay[1] = {
        make(CandidateKind::Relay, be(224, 0, 0, 7)),
    };
    QNP_CHECK(validateCandidateSet(multicastRelay, 1)
              == CandidateSetError::KindAddressMismatch);

    const CandidateEndpoint duplicate[2] = {
        make(CandidateKind::PrivateLan, be(192, 168, 1, 20)),
        make(CandidateKind::PrivateLan, be(192, 168, 1, 20)),
    };
    QNP_CHECK(validateCandidateSet(duplicate, 2)
              == CandidateSetError::Duplicate);

    const CandidateEndpoint twoPublic[2] = {
        make(CandidateKind::PublicObserved, be(203, 0, 113, 7)),
        make(CandidateKind::PublicObserved, be(203, 0, 113, 8)),
    };
    QNP_CHECK(validateCandidateSet(twoPublic, 2)
              == CandidateSetError::MultiplePublic);
    const CandidateEndpoint twoRelay[2] = {
        make(CandidateKind::Relay, be(198, 51, 100, 2)),
        make(CandidateKind::Relay, be(198, 51, 100, 3)),
    };
    QNP_CHECK(validateCandidateSet(twoRelay, 2)
              == CandidateSetError::MultipleRelay);
}

QNP_TEST(CandidateBaseLabels) {
    QNP_CHECK(candidateBaseLabel(CandidateKind::PrivateLan)
              == RouteLabel::LanDirect);
    QNP_CHECK(candidateBaseLabel(CandidateKind::PublicObserved)
              == RouteLabel::PublicDirect);
    QNP_CHECK(candidateBaseLabel(CandidateKind::Relay) == RouteLabel::Relay);
    QNP_CHECK(candidateBaseLabel(CandidateKind::None) == RouteLabel::None);
}

QNP_TEST(CollectPrivateCandidatesFiltersDedupesAndBounds) {
    const uint32_t locals[] = {
        be(127, 0, 0, 1),
        be(192, 168, 1, 20),
        be(192, 168, 1, 20),
        be(169, 254, 9, 9),
        be(203, 0, 113, 7),
        be(10, 0, 0, 5),
        be(0, 0, 0, 0),
    };
    CandidateEndpoint out[kMaxPrivateCandidates] = {};
    const size_t n = collectPrivateCandidates(
        locals, sizeof(locals) / sizeof(locals[0]), portBE(14763), out,
        kMaxPrivateCandidates);
    QNP_REQUIRE(n == 3);
    QNP_CHECK(out[0].ipv4BE == be(192, 168, 1, 20));
    QNP_CHECK(out[1].ipv4BE == be(10, 0, 0, 5));
    QNP_CHECK(out[2].ipv4BE == be(127, 0, 0, 1));
    for (size_t i = 0; i < n; ++i) {
        QNP_CHECK(out[i].kind == CandidateKind::PrivateLan);
        QNP_CHECK(out[i].portBE == portBE(14763));
    }
    QNP_CHECK(validateCandidateSet(out, n) == CandidateSetError::None);

    const uint32_t many[] = {
        be(127, 0, 0, 1),   be(10, 0, 0, 1), be(10, 0, 0, 2),
        be(10, 0, 0, 3),    be(10, 0, 0, 4), be(10, 0, 0, 5),
    };
    const size_t m = collectPrivateCandidates(
        many, sizeof(many) / sizeof(many[0]), portBE(1), out,
        kMaxPrivateCandidates);
    QNP_REQUIRE(m == kMaxPrivateCandidates);
    for (size_t i = 0; i < m; ++i)
        QNP_CHECK(!isLoopbackBE(out[i].ipv4BE));

    QNP_CHECK(collectPrivateCandidates(locals, 7, 0, out,
                                       kMaxPrivateCandidates) == 0);
}

QNP_TEST(CollectPrivateCandidatesWithLoopbackGuaranteesSamePcRoute) {
    CandidateEndpoint out[kMaxPrivateCandidates] = {};
    const uint32_t noLoopback[] = {
        be(10, 0, 0, 1),
        be(10, 0, 0, 2),
        be(10, 0, 0, 3),
        be(10, 0, 0, 4),
        be(10, 0, 0, 5),
    };
    const size_t count = collectPrivateCandidatesWithLoopback(
        noLoopback, sizeof(noLoopback) / sizeof(noLoopback[0]),
        portBE(41000), out, kMaxPrivateCandidates);
    QNP_REQUIRE(count == kMaxPrivateCandidates);
    QNP_CHECK(out[count - 1].ipv4BE == kLoopbackIpv4BE);
    QNP_CHECK(out[count - 1].portBE == portBE(41000));
    QNP_CHECK(validateCandidateSet(out, count) == CandidateSetError::None);

    const size_t fallback = collectPrivateCandidatesWithLoopback(
        nullptr, 0, portBE(41001), out, kMaxPrivateCandidates);
    QNP_REQUIRE(fallback == 1);
    QNP_CHECK(out[0].kind == CandidateKind::PrivateLan);
    QNP_CHECK(out[0].ipv4BE == be(127, 0, 0, 1));
    QNP_CHECK(out[0].portBE == portBE(41001));

    const size_t one = collectPrivateCandidatesWithLoopback(
        noLoopback, 5, portBE(41002), out, 1);
    QNP_REQUIRE(one == 1);
    QNP_CHECK(out[0].ipv4BE == kLoopbackIpv4BE);

    QNP_CHECK(collectPrivateCandidatesWithLoopback(
                  noLoopback, 5, 0, out, kMaxPrivateCandidates) == 0);
}
