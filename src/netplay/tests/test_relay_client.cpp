#include "TestHarness.h"

#include "../../net/Protocol.h"
#include "../../net/RelayClient.h"
#include "../../net/RelayProtocol.h"

#include <cstdint>
#include <vector>

namespace {

using namespace qnet::relay;

qnet::Endpoint endpoint(const char* ipv4, uint16_t port) {
    qnet::Endpoint value;
    QNP_CHECK(qnet::Endpoint::parse(ipv4, port, value));
    return value;
}

bool listenOnTestPort(qnet::TcpListener& listener,
                      qnet::Endpoint& relayEndpoint) {
    for (uint16_t port = 43500; port < 43560; ++port) {
        if (listener.listen(port)) {
            relayEndpoint = endpoint("127.0.0.1", port);
            return true;
        }
    }
    return false;
}

struct CapturedDatagram {
    qnet::Endpoint destination;
    std::vector<uint8_t> bytes;
};

struct FakeUdpSender {
    bool succeed = true;
    std::vector<CapturedDatagram> sent;

    static bool send(void* context, const qnet::Endpoint& destination,
                     const void* bytes, size_t length) {
        auto& self = *static_cast<FakeUdpSender*>(context);
        if (!self.succeed) return false;
        const auto* first = static_cast<const uint8_t*>(bytes);
        self.sent.push_back(
            CapturedDatagram{ destination,
                              std::vector<uint8_t>(first, first + length) });
        return true;
    }
};

PairResult playerPair(const qnet::Endpoint& relayEndpoint) {
    PairResult pair{};
    pair.room = { 0x0102030405060708ull, 0x1112131415161718ull };
    pair.domain = LaneDomain::Player;
    pair.role = RelayRole::Client;
    pair.token = { 0x2122232425262728ull, 0x3132333435363738ull };
    pair.relayEndpoint = relayEndpoint;
    return pair;
}

std::vector<uint8_t> readyBytes(const PairResult& pair,
                                const qnet::Endpoint& peer,
                                const RegistrationToken& token) {
    Ready ready{};
    ready.room = pair.room;
    ready.domain = pair.domain;
    ready.role = pair.role;
    ready.token = token;
    ready.peer = {
        peer.sa.sin_addr.s_addr,
        ntohs(peer.sa.sin_port),
    };
    std::vector<uint8_t> bytes;
    QNP_CHECK(encodeReady(ready, bytes));
    return bytes;
}

}

QNP_TEST(RelayDecodeErrorsHaveStableSymbolicText) {
    struct Expected {
        DecodeError error;
        const char* text;
    };
    const Expected expected[] = {
        { DecodeError::None, "none" },
        { DecodeError::TooShort, "too short" },
        { DecodeError::BadMagic, "bad magic" },
        { DecodeError::BadVersion, "bad version" },
        { DecodeError::BadKind, "bad kind" },
        { DecodeError::BadLength, "bad length" },
        { DecodeError::BadReserved, "non-zero reserved field" },
        { DecodeError::BadDomain, "bad lane domain" },
        { DecodeError::BadRole, "bad relay role" },
        { DecodeError::BadStatus, "bad welcome status" },
        { DecodeError::InvalidRoom, "invalid room" },
        { DecodeError::InvalidToken, "invalid token" },
        { DecodeError::InvalidEndpoint, "invalid endpoint" },
        { DecodeError::InvalidGrant, "invalid spectator grant" },
        { DecodeError::InvalidParty, "invalid spectator party" },
    };
    for (const auto& entry : expected) {
        QNP_CHECK(std::string(decodeErrorText(entry.error)) == entry.text);
    }
    QNP_CHECK(std::string(decodeErrorText(
                  static_cast<DecodeError>(0x7FFFFFFF)))
              == "unknown decode error");
}

QNP_TEST(RelayRoomGenerationRoundTripsShareCode) {
    RoomId room{};
    std::string error;
    QNP_CHECK(generateRoomId(room, &error));
    QNP_CHECK(error.empty());
    QNP_CHECK(isValidId(room));

    const std::string code = formatRoomId(room);
    QNP_CHECK(code.size() == 32);
    RoomId parsed{};
    QNP_CHECK(parseRoomId(code, parsed));
    QNP_CHECK(parsed == room);
}

QNP_TEST(RelayEndpointRoomAliasIsStableAndPortSpecific) {
    RoomId first{};
    RoomId again{};
    RoomId otherPort{};
    QNP_CHECK(deriveEndpointRoomId(endpoint("203.0.113.9", 7500), first));
    QNP_CHECK(deriveEndpointRoomId(endpoint("203.0.113.9", 7500), again));
    QNP_CHECK(deriveEndpointRoomId(
        endpoint("203.0.113.9", 7501), otherPort));
    QNP_CHECK(isValidId(first));
    QNP_CHECK(first == again);
    QNP_CHECK(first != otherPort);

    qnet::Endpoint invalid;
    QNP_CHECK(!deriveEndpointRoomId(invalid, again));
}

QNP_TEST(RelayPairStopsTcpDemuxExactlyAfterAcceptedWelcome) {
    qnet::TcpListener listener;
    qnet::Endpoint relayEndpoint;
    QNP_REQUIRE(listenOnTestPort(listener, relayEndpoint));

    const RoomId room{
        0x1020304050607080ull,
        0x90A0B0C0D0E0F001ull,
    };
    RelayPairClient pairing;
    qnet::TcpConn client;
    std::string error;
    QNP_REQUIRE(pairing.begin(client, relayEndpoint, room,
                              LaneDomain::Player, RelayRole::Client,
                              2000, &error));
    QNP_CHECK(error.empty());

    qnet::TcpConn server = listener.acceptOne(2000);
    QNP_REQUIRE(server.isOpen());
    qnet::TcpConn::Received helloFrame;
    QNP_REQUIRE(server.recvMsgEx(helloFrame, 2000)
                == qnet::TcpConn::Rs::Msg);
    QNP_CHECK(helloFrame.type == kTcpEnvelopeType);
    Hello hello{};
    QNP_CHECK(decodeHello(helloFrame.body.data(), helloFrame.body.size(),
                          hello) == DecodeError::None);
    QNP_CHECK(hello.room == room);
    QNP_CHECK(hello.role == RelayRole::Client);

    Welcome waiting{};
    waiting.room = room;
    waiting.domain = LaneDomain::Player;
    waiting.role = RelayRole::Client;
    waiting.status = WelcomeStatus::Waiting;
    std::vector<uint8_t> bytes;
    QNP_CHECK(encodeWelcome(waiting, bytes));
    QNP_CHECK(server.sendMsg(kTcpEnvelopeType, bytes.data(), bytes.size()));
    QNP_CHECK(pairing.poll(client, 2000, &error) == PairPoll::Waiting);
    QNP_CHECK(!pairing.ready());

    Welcome accepted = waiting;
    accepted.status = WelcomeStatus::Accepted;
    accepted.registrationToken = {
        0x2222222222222222ull,
        0x3333333333333333ull,
    };
    QNP_CHECK(encodeWelcome(accepted, bytes));
    QNP_CHECK(server.sendMsg(kTcpEnvelopeType, bytes.data(), bytes.size()));

    const qnet::VersionConfig version{ qnet::kProtocolVersion };
    QNP_CHECK(server.sendMsg(qnet::MSG_VERSION_CONFIG,
                             &version, sizeof(version)));
    QNP_CHECK(pairing.poll(client, 2000, &error) == PairPoll::Ready);
    QNP_CHECK(pairing.result().token == accepted.registrationToken);
    QNP_CHECK(pairing.poll(client, 0, &error) == PairPoll::Ready);

    qnet::TcpConn::Received peerFrame;
    QNP_CHECK(client.recvMsgEx(peerFrame, 2000) == qnet::TcpConn::Rs::Msg);
    QNP_CHECK(peerFrame.type == qnet::MSG_VERSION_CONFIG);
    QNP_CHECK(peerFrame.body.size() == sizeof(version));
}

QNP_TEST(RelayUdpRetriesAuthenticatesAndKeepsRawRelayQnpForCaller) {
    const qnet::Endpoint relayEndpoint = endpoint("127.0.0.1", 45670);
    const qnet::Endpoint directEndpoint = endpoint("127.0.0.2", 45671);
    const PairResult pair = playerPair(relayEndpoint);

    RelayUdpClient client;
    std::string error;
    QNP_CHECK(client.configure(pair, &error));
    FakeUdpSender sender;

    QNP_CHECK(client.serviceWith(&FakeUdpSender::send, &sender, 0, &error));
    QNP_REQUIRE(sender.sent.size() == 1);
    Register registration{};
    QNP_CHECK(decodeRegister(sender.sent[0].bytes.data(),
                             sender.sent[0].bytes.size(),
                             registration) == DecodeError::None);
    QNP_CHECK(registration.room == pair.room);
    QNP_CHECK(registration.token == pair.token);
    QNP_CHECK(sender.sent[0].destination == relayEndpoint);

    QNP_CHECK(client.serviceWith(&FakeUdpSender::send, &sender, 499, &error));
    QNP_CHECK(sender.sent.size() == 1);
    QNP_CHECK(client.serviceWith(&FakeUdpSender::send, &sender, 500, &error));
    QNP_CHECK(sender.sent.size() == 2);

    const auto validReady =
        readyBytes(pair, directEndpoint, pair.token);
    qnet::Endpoint wrongRelay = relayEndpoint;
    wrongRelay.sa.sin_port =
        htons(static_cast<uint16_t>(ntohs(wrongRelay.sa.sin_port) + 1));
    QNP_CHECK(client.handleInbound(validReady.data(), validReady.size(),
                                   wrongRelay, 501)
              == UdpInbound::NotHandled);
    QNP_CHECK(!client.ready());

    RegistrationToken wrongToken = pair.token;
    ++wrongToken.low;
    const auto forgedReady =
        readyBytes(pair, directEndpoint, wrongToken);
    QNP_CHECK(client.handleInbound(forgedReady.data(), forgedReady.size(),
                                   relayEndpoint, 501)
              == UdpInbound::Consumed);
    QNP_CHECK(!client.ready());

    uint8_t qnp[qnet::kMaxMsgSize] = {};
    const qnet::Ping ping{ 7, 99 };
    const size_t qnpSize = qnet::packMsg(
        qnet::MSG_PING, &ping, sizeof(ping), qnp, sizeof(qnp));
    QNP_CHECK(qnpSize != 0);
    QNP_CHECK(client.handleInbound(qnp, qnpSize, relayEndpoint, 501)
              == UdpInbound::NotHandled);

    QNP_CHECK(client.handleInbound(validReady.data(), validReady.size(),
                                   relayEndpoint, 501)
              == UdpInbound::Ready);
    QNP_CHECK(client.ready());
    QNP_CHECK(client.directPeer() == directEndpoint);

    QNP_CHECK(client.serviceWith(&FakeUdpSender::send, &sender, 501, &error));
    QNP_REQUIRE(sender.sent.size() == 3);
    QNP_CHECK(sender.sent.back().destination == directEndpoint);
    QNP_REQUIRE(sender.sent.back().bytes.size() == 1);
    QNP_CHECK(sender.sent.back().bytes[0] == kDirectPunchByte);
    QNP_CHECK(client.serviceWith(&FakeUdpSender::send, &sender, 625, &error));
    QNP_CHECK(sender.sent.size() == 3);
    QNP_CHECK(client.serviceWith(&FakeUdpSender::send, &sender, 626, &error));
    QNP_CHECK(sender.sent.size() == 4);

    const uint8_t punch = kDirectPunchByte;
    QNP_CHECK(client.handleInbound(&punch, sizeof(punch),
                                   wrongRelay, 627)
              == UdpInbound::NotHandled);
    QNP_CHECK(!client.directPeerObserved());
    QNP_CHECK(client.handleInbound(&punch, sizeof(punch),
                                   directEndpoint, 627)
              == UdpInbound::DirectPeerObserved);
    QNP_CHECK(client.directPeerObserved());
    QNP_CHECK(client.stats().registersSent == 2);
    QNP_CHECK(client.stats().directPunchesSent == 2);

    RelayUdpClient forced;
    QNP_CHECK(forced.configure(pair, &error));
    forced.setDirectPunchEnabled(false);
    FakeUdpSender forcedSender;
    QNP_CHECK(forced.serviceWith(
        &FakeUdpSender::send, &forcedSender, 0, &error));
    QNP_CHECK(forced.handleInbound(
        validReady.data(), validReady.size(), relayEndpoint, 1)
        == UdpInbound::Ready);
    QNP_CHECK(forced.serviceWith(
        &FakeUdpSender::send, &forcedSender, 1, &error));
    QNP_REQUIRE(forcedSender.sent.size() == 1);
    QNP_CHECK(forcedSender.sent.front().destination == relayEndpoint);
    QNP_CHECK(forced.stats().registersSent == 1);
    QNP_CHECK(forced.stats().directPunchesSent == 0);
}
