#include "TestHarness.h"

#include "net/ControlLink.h"
#include "net/Protocol.h"
#include "net/Sockets.h"
#include "netplay/WireProtocol.h"

#include <cstring>
#include <string>
#include <vector>

using qnet::ControlLink;
using qnet::Endpoint;
using qnet::LinkHello;
using qnet::UdpControlLink;
using qnet::UdpSock;

namespace {

struct NetInit {
    NetInit() { qnet::netInit(); }
    ~NetInit() { qnet::netShutdown(); }
};

bool loopback(uint16_t port, Endpoint& out) {
    return Endpoint::parse("127.0.0.1", port, out);
}

qnetplay::compat::CompatibilityIdentityWire controlTestIdentity() {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = qnet::kProtocolVersion;
    if (!qnetplay::compat::setProfileId(identity, "qoh99-se1126"))
        return {};
    std::memset(identity.runtimeBuild.executableSha256, 0x11,
                sizeof(identity.runtimeBuild.executableSha256));
    identity.runtimeBuild.executableSize = 704512u;
    identity.runtimeBuild.peTimestamp = 0x3A19A8A9u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    std::memset(identity.simulationContent.simulationRootSha256, 0x22,
                sizeof(identity.simulationContent.simulationRootSha256));
    std::memset(identity.simulationContent.validationRootSha256, 0x33,
                sizeof(identity.simulationContent.validationRootSha256));
    identity.simulationContent.entryCount = 68u;
    identity.simulationContent.requiredEntryCount = 68u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.stateSchemaId = 0x1001u;
    identity.modSimulationId = 0x2001u;
    identity.timingProfileId = 0x3001u;
    return identity;
}

qnet::RuntimeArtifactIdentityWire controlTestRuntimeArtifacts() {
    qnet::RuntimeArtifactIdentityWire artifacts{};
    artifacts.schemaVersion = qnet::kRuntimeArtifactIdentitySchemaVersion;
    std::memset(artifacts.casterSha256, 0x44, sizeof(artifacts.casterSha256));
    std::memset(artifacts.netplayDllSha256, 0x55,
                sizeof(artifacts.netplayDllSha256));
    std::memset(artifacts.ddrawSha256, 0x66, sizeof(artifacts.ddrawSha256));
    return artifacts;
}

template <typename T>
bool roundTripControlBody(const uint8_t type, const T& source, T& decoded) {
    uint8_t buffer[qnet::kMaxMsgSize]{};
    const size_t bytes = qnet::packMsg(type, &source, sizeof(source), buffer,
                                       sizeof(buffer));
    if (bytes == 0u) return false;
    uint8_t decodedType = qnet::MSG_NONE;
    const uint8_t* body = nullptr;
    size_t bodyBytes = 0u;
    if (!qnet::unpackMsg(buffer, bytes, decodedType, body, bodyBytes)
        || decodedType != type || bodyBytes != sizeof(T)) {
        return false;
    }
    std::memcpy(&decoded, body, sizeof(decoded));
    return true;
}

template <class Predicate>
bool pumpUntil(UdpControlLink& a, UdpControlLink& b, Predicate done,
               int budgetMs) {
    const uint64_t deadlineUs = qnet::nowUs()
                              + static_cast<uint64_t>(budgetMs) * 1000ull;
    while (qnet::nowUs() < deadlineUs) {
        ControlLink::Received rx;
        a.recvMsgEx(rx, 2);
        b.recvMsgEx(rx, 2);
        if (done()) return true;
    }
    return done();
}

struct Router {
    UdpSock socket;
    Endpoint left{}, right{};
    bool haveLeft = false, haveRight = false;
    int dropEveryNth = 0;
    int seen = 0;
    int forwarded = 0, dropped = 0;

    bool open(uint16_t port) { return socket.open(port); }

    void pump() {
        for (;;) {
            uint8_t buffer[2048];
            Endpoint from{};
            const int n = socket.recvFrom(buffer, sizeof(buffer), from);
            if (n <= 0) break;
            if (!haveLeft) { left = from; haveLeft = true; }
            else if (!haveRight && !(from == left)) { right = from; haveRight = true; }

            seen++;
            if (dropEveryNth > 0 && (seen % dropEveryNth) == 0) { dropped++; continue; }

            const Endpoint& to = (from == left) ? right : left;
            if ((from == left && haveRight) || (!(from == left) && haveLeft)) {
                socket.sendTo(to, buffer, static_cast<size_t>(n));
                forwarded++;
            }
        }
    }
};

template <class Predicate>
bool pumpRouted(UdpControlLink& a, UdpControlLink& b, Router& router,
                Predicate done, int budgetMs) {
    const uint64_t deadlineUs = qnet::nowUs()
                              + static_cast<uint64_t>(budgetMs) * 1000ull;
    while (qnet::nowUs() < deadlineUs) {
        ControlLink::Received rx;
        a.recvMsgEx(rx, 1);
        router.pump();
        b.recvMsgEx(rx, 1);
        router.pump();
        if (done()) return true;
    }
    return done();
}

}

QNP_TEST(ControlLinkEstablishesOverLoopback) {
    NetInit net;
    UdpSock hostSocket, clientSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(clientSocket.open(0));
    Endpoint hostEp{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));

    UdpControlLink host, client;
    const uint64_t now = qnet::nowUs() / 1000ull;
    host.startHostListen(hostSocket, now);
    client.startClient(clientSocket, hostEp, now);

    QNP_CHECK(pumpUntil(host, client,
                        [&] { return host.established() && client.established(); },
                        3000));
    QNP_CHECK(host.established());
    QNP_CHECK(client.established());
    QNP_CHECK(host.havePeer());
    QNP_CHECK(host.peer().sa.sin_port == htons(clientSocket.boundPort()));
    QNP_CHECK(std::string(host.transportName()) == "udp");
    QNP_CHECK(host.sharesDataSocket());
}

QNP_TEST(ControlLinkHostSpeaksFirstExactlyLikeTheStreamPath) {
    NetInit net;
    UdpSock hostSocket, clientSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(clientSocket.open(0));
    Endpoint hostEp{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));

    UdpControlLink host, client;
    const uint64_t now = qnet::nowUs() / 1000ull;
    host.startHostListen(hostSocket, now);
    client.startClient(clientSocket, hostEp, now);
    QNP_REQUIRE(pumpUntil(host, client,
                          [&] { return host.established() && client.established(); },
                          3000));

    const qnet::VersionConfig version{ qnet::kProtocolVersion };
    QNP_CHECK(host.sendMsg(qnet::MSG_VERSION_CONFIG, &version, sizeof(version)));

    ControlLink::Received rx;
    bool got = false;
    const uint64_t deadlineUs = qnet::nowUs() + 3000000ull;
    while (qnet::nowUs() < deadlineUs && !got) {
        if (client.recvMsgEx(rx, 20) == ControlLink::Rs::Msg) got = true;
        ControlLink::Received drain;
        host.recvMsgEx(drain, 2);
    }
    QNP_REQUIRE(got);
    QNP_CHECK(rx.type == qnet::MSG_VERSION_CONFIG);
    QNP_REQUIRE(rx.body.size() == sizeof(qnet::VersionConfig));
    qnet::VersionConfig decoded{};
    std::memcpy(&decoded, rx.body.data(), sizeof(decoded));
    QNP_CHECK(decoded.protocolVersion == qnet::kProtocolVersion);
}

QNP_TEST(ControlLinkConnectFailsCleanlyWhenNobodyAnswers) {
    NetInit net;
    UdpSock clientSocket;
    QNP_REQUIRE(clientSocket.open(0));
    Endpoint nowhere{};
    QNP_REQUIRE(loopback(1u, nowhere));

    UdpControlLink client;
    client.startClient(clientSocket, nowhere, qnet::nowUs() / 1000ull);
    const uint64_t startUs = qnet::nowUs();
    QNP_CHECK(!client.connect(300));
    const uint64_t elapsedMs = (qnet::nowUs() - startUs) / 1000ull;
    QNP_CHECK(elapsedMs >= 250 && elapsedMs < 3000);
    QNP_CHECK(client.state() == UdpControlLink::State::Closed);
    QNP_CHECK(!client.failureText().empty());
    QNP_CHECK(client.stats().hellosSent >= 2);
}

QNP_TEST(ControlLinkHandsForeignDatagramsBackInsteadOfEatingThem) {
    NetInit net;
    UdpSock hostSocket, clientSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(clientSocket.open(0));
    Endpoint hostEp{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));

    UdpControlLink host, client;
    const uint64_t now = qnet::nowUs() / 1000ull;
    host.startHostListen(hostSocket, now);
    client.startClient(clientSocket, hostEp, now);
    QNP_REQUIRE(pumpUntil(host, client,
                          [&] { return host.established() && client.established(); },
                          3000));

    std::vector<std::vector<uint8_t>> foreign;
    host.setForeignSink([&](const uint8_t* bytes, size_t length, const Endpoint&) {
        foreign.emplace_back(bytes, bytes + length);
    });

    uint8_t qnp[16] = {};
    const uint32_t qnpMagic = qnetplay::wire::kMagic;
    std::memcpy(qnp, &qnpMagic, sizeof(qnpMagic));
    clientSocket.sendTo(hostEp, qnp, sizeof(qnp));

    const uint8_t punch = 0x00;
    clientSocket.sendTo(hostEp, &punch, sizeof(punch));

    QNP_CHECK(pumpUntil(host, client, [&] { return foreign.size() >= 2; }, 2000));
    QNP_REQUIRE(foreign.size() >= 2);
    QNP_CHECK(foreign[0].size() == sizeof(qnp));
    QNP_CHECK(foreign[1].size() == 1);
    QNP_CHECK(host.stats().foreignDatagrams >= 2);
    QNP_CHECK(host.established());
}

QNP_TEST(ControlLinkIgnoresOurMagicWithSomebodyElsesType) {
    NetInit net;
    UdpSock hostSocket, clientSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(clientSocket.open(0));
    Endpoint hostEp{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));

    UdpControlLink host;
    host.startHostListen(hostSocket, qnet::nowUs() / 1000ull);
    std::vector<uint8_t> seen;
    host.setForeignSink([&](const uint8_t*, size_t length, const Endpoint&) {
        seen.push_back(static_cast<uint8_t>(length));
    });

    const qnet::VersionConfig version{ qnet::kProtocolVersion };
    uint8_t buffer[qnet::kMaxMsgSize];
    const size_t n = qnet::packMsg(qnet::MSG_VERSION_CONFIG, &version,
                                   sizeof(version), buffer, sizeof(buffer));
    QNP_REQUIRE(n > 0);
    clientSocket.sendTo(hostEp, buffer, n);

    const uint64_t deadlineUs = qnet::nowUs() + 1500000ull;
    while (qnet::nowUs() < deadlineUs && seen.empty()) {
        ControlLink::Received rx;
        host.recvMsgEx(rx, 10);
    }
    QNP_CHECK(seen.size() == 1);
    QNP_CHECK(host.state() == UdpControlLink::State::Connecting);
}

QNP_TEST(ControlLinkServesOnePeerAndTurnsAwayASecondConnector) {
    NetInit net;
    UdpSock hostSocket, firstSocket, secondSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(firstSocket.open(0));
    QNP_REQUIRE(secondSocket.open(0));
    Endpoint hostEp{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));

    UdpControlLink host, first, second;
    const uint64_t now = qnet::nowUs() / 1000ull;
    host.startHostListen(hostSocket, now);
    first.startClient(firstSocket, hostEp, now);
    QNP_REQUIRE(pumpUntil(host, first,
                          [&] { return host.established() && first.established(); },
                          3000));
    const Endpoint pinned = host.peer();

    second.startClient(secondSocket, hostEp, qnet::nowUs() / 1000ull);
    QNP_CHECK(!pumpUntil(host, second, [&] { return second.established(); }, 600));

    QNP_CHECK(host.established());
    QNP_CHECK(host.peer() == pinned);
    QNP_CHECK(!second.established());
    QNP_CHECK(host.stats().rejected >= 1);

    const qnet::VersionConfig version{ qnet::kProtocolVersion };
    QNP_CHECK(host.sendMsg(qnet::MSG_VERSION_CONFIG, &version, sizeof(version)));
    ControlLink::Received rx;
    bool got = false;
    const uint64_t deadlineUs = qnet::nowUs() + 2000000ull;
    while (qnet::nowUs() < deadlineUs && !got) {
        if (first.recvMsgEx(rx, 20) == ControlLink::Rs::Msg) got = true;
        ControlLink::Received drain;
        host.recvMsgEx(drain, 2);
    }
    QNP_CHECK(got && rx.type == qnet::MSG_VERSION_CONFIG);
}

QNP_TEST(ControlLinkRejectsAnAckForSomebodyElsesAttempt) {
    NetInit net;
    UdpSock clientSocket, impostorSocket;
    QNP_REQUIRE(clientSocket.open(0));
    QNP_REQUIRE(impostorSocket.open(0));
    Endpoint clientEp{}, impostorEp{};
    QNP_REQUIRE(loopback(clientSocket.boundPort(), clientEp));
    QNP_REQUIRE(loopback(impostorSocket.boundPort(), impostorEp));

    UdpControlLink client;
    client.startClient(clientSocket, impostorEp, qnet::nowUs() / 1000ull);

    LinkHello wrong{ qnet::kProtocolVersion, 0xDEADBEEFDEADBEEFull };
    uint8_t buffer[qnet::kMaxMsgSize];
    const size_t n = qnet::packMsg(qnet::MSG_LINK_HELLO_ACK, &wrong,
                                   sizeof(wrong), buffer, sizeof(buffer));
    QNP_REQUIRE(n > 0);

    const uint64_t deadlineUs = qnet::nowUs() + 800000ull;
    while (qnet::nowUs() < deadlineUs) {
        impostorSocket.sendTo(clientEp, buffer, n);
        ControlLink::Received rx;
        client.recvMsgEx(rx, 20);
        uint8_t sink[2048];
        Endpoint from{};
        while (impostorSocket.recvFrom(sink, sizeof(sink), from) > 0) {}
    }
    QNP_CHECK(!client.established());
    QNP_CHECK(client.stats().rejected >= 1);
}

QNP_TEST(ControlLinkRefusesAnAckFromSomewhereItNeverGreeted) {
    NetInit net;
    UdpSock hostSocket, clientSocket, observerSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(clientSocket.open(0));
    QNP_REQUIRE(observerSocket.open(0));
    Endpoint hostEp{}, clientEp{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));
    QNP_REQUIRE(loopback(clientSocket.boundPort(), clientEp));

    UdpControlLink client;
    client.startClient(clientSocket, hostEp, qnet::nowUs() / 1000ull);

    uint64_t nonce = 0;
    const uint64_t sniffUs = qnet::nowUs() + 1500000ull;
    while (qnet::nowUs() < sniffUs && nonce == 0) {
        uint8_t buffer[2048];
        Endpoint from{};
        const int n = hostSocket.recvFrom(buffer, sizeof(buffer), from);
        if (n <= 0) { client.poll(2); continue; }
        uint8_t type = 0;
        const uint8_t* body = nullptr;
        size_t bodyLen = 0;
        if (qnet::unpackMsg(buffer, static_cast<size_t>(n), type, body, bodyLen)
            && type == qnet::MSG_LINK_HELLO && bodyLen == sizeof(LinkHello)) {
            LinkHello seen{};
            std::memcpy(&seen, body, sizeof(seen));
            nonce = seen.nonce;
        }
    }
    QNP_REQUIRE(nonce != 0);

    LinkHello forged{ qnet::kProtocolVersion, nonce };
    uint8_t framed[qnet::kMaxMsgSize];
    const size_t n = qnet::packMsg(qnet::MSG_LINK_HELLO_ACK, &forged,
                                   sizeof(forged), framed, sizeof(framed));
    QNP_REQUIRE(n > 0);
    const uint64_t deadlineUs = qnet::nowUs() + 800000ull;
    while (qnet::nowUs() < deadlineUs && !client.established()) {
        observerSocket.sendTo(clientEp, framed, n);
        client.poll(5);
        uint8_t sink[2048];
        Endpoint from{};
        while (hostSocket.recvFrom(sink, sizeof(sink), from) > 0) {}
    }
    QNP_CHECK(!client.established());
    QNP_CHECK(client.stats().rejected >= 1);
    QNP_CHECK(!client.greeted([&] {
        Endpoint observer{};
        loopback(observerSocket.boundPort(), observer);
        return observer;
    }()));
}

QNP_TEST(ControlLinkHostCanBeToldExactlyWhoIsComing) {
    NetInit net;
    UdpSock hostSocket, strangerSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(strangerSocket.open(0));
    Endpoint hostEp{}, expected{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));
    QNP_REQUIRE(loopback(static_cast<uint16_t>(strangerSocket.boundPort() ^ 0x1F),
                         expected));

    UdpControlLink host, stranger;
    const uint64_t now = qnet::nowUs() / 1000ull;
    host.startHostListen(hostSocket, now);
    host.expectPeer(expected);
    stranger.startClient(strangerSocket, hostEp, now);

    QNP_CHECK(!pumpUntil(host, stranger, [&] { return host.established(); }, 600));
    QNP_CHECK(!host.established());
    QNP_CHECK(host.stats().hellosReceived >= 1);
    QNP_CHECK(host.stats().rejected >= 1);
}

QNP_TEST(ControlLinkDeliversEveryMessageInOrderThroughALossyPath) {
    NetInit net;
    Router router;
    QNP_REQUIRE(router.open(0));
    router.dropEveryNth = 3;

    UdpSock hostSocket, clientSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(clientSocket.open(0));
    Endpoint routerEp{}, hostEp{};
    QNP_REQUIRE(loopback(router.socket.boundPort(), routerEp));
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));
    router.right = hostEp;
    router.haveRight = true;

    UdpControlLink host, client;
    const uint64_t now = qnet::nowUs() / 1000ull;
    host.startHostListen(hostSocket, now);
    client.startClient(clientSocket, routerEp, now);
    QNP_REQUIRE(pumpRouted(host, client, router,
                           [&] { return host.established() && client.established(); },
                           6000));

    constexpr int kCount = 12;
    for (int i = 0; i < kCount; ++i) {
        const qnet::VersionConfig payload{ static_cast<uint32_t>(1000 + i) };
        QNP_CHECK(host.sendMsg(qnet::MSG_VERSION_CONFIG, &payload, sizeof(payload)));
    }

    std::vector<uint32_t> received;
    const uint64_t deadlineUs = qnet::nowUs() + 12000000ull;
    while (qnet::nowUs() < deadlineUs
           && received.size() < static_cast<size_t>(kCount)) {
        ControlLink::Received rx;
        if (client.recvMsgEx(rx, 1) == ControlLink::Rs::Msg
            && rx.body.size() == sizeof(qnet::VersionConfig)) {
            qnet::VersionConfig decoded{};
            std::memcpy(&decoded, rx.body.data(), sizeof(decoded));
            received.push_back(decoded.protocolVersion);
        }
        router.pump();
        ControlLink::Received drain;
        host.recvMsgEx(drain, 1);
        router.pump();
    }

    QNP_CHECK(router.dropped > 0);
    QNP_REQUIRE(received.size() == static_cast<size_t>(kCount));
    for (int i = 0; i < kCount; ++i)
        QNP_CHECK(received[static_cast<size_t>(i)] == static_cast<uint32_t>(1000 + i));
}

QNP_TEST(ControlListenerServesAPlayerAndAVisitorAtOnce) {
    NetInit net;
    UdpSock hostSocket, playerSocket, viewerSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(playerSocket.open(0));
    QNP_REQUIRE(viewerSocket.open(0));
    Endpoint hostEp{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));

    qnet::UdpControlListener listener;
    listener.open(hostSocket);

    UdpControlLink player, viewer;
    const uint64_t now = qnet::nowUs() / 1000ull;
    player.startClient(playerSocket, hostEp, now);
    viewer.startClient(viewerSocket, hostEp, now);

    UdpControlLink* served[2] = { nullptr, nullptr };
    const uint64_t deadlineUs = qnet::nowUs() + 4000000ull;
    while (qnet::nowUs() < deadlineUs
           && (served[0] == nullptr || served[1] == nullptr)) {
        listener.pump(1);
        ControlLink::Received rx;
        player.recvMsgEx(rx, 1);
        viewer.recvMsgEx(rx, 1);
        UdpControlLink* accepted = listener.acceptOne();
        if (accepted != nullptr)
            served[served[0] == nullptr ? 0 : 1] = accepted;
    }
    QNP_REQUIRE(served[0] != nullptr && served[1] != nullptr);
    QNP_CHECK(served[0] != served[1]);
    QNP_CHECK(player.established());
    QNP_CHECK(viewer.established());
    QNP_CHECK(listener.liveLinks() == 2);

    const qnet::ClientIntent joinIntent{ qnet::INTENT_JOIN };
    QNP_CHECK(served[0]->sendMsg(qnet::MSG_CLIENT_INTENT, &joinIntent,
                                 sizeof(joinIntent)));
    bool delivered = false;
    const uint64_t sendDeadlineUs = qnet::nowUs() + 3000000ull;
    while (qnet::nowUs() < sendDeadlineUs && !delivered) {
        listener.pump(1);
        ControlLink::Received rx;
        if (player.recvMsgEx(rx, 1) == ControlLink::Rs::Msg
            && rx.type == qnet::MSG_CLIENT_INTENT)
            delivered = true;
        if (viewer.recvMsgEx(rx, 1) == ControlLink::Rs::Msg)
            QNP_CHECK(false);
    }
    QNP_CHECK(delivered);
}

QNP_TEST(AListenerOwnedLinkStillReceivesWhenPolledWithNoTimeout) {
    NetInit net;
    UdpSock hostSocket, clientSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(clientSocket.open(0));
    Endpoint hostEp{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));

    qnet::UdpControlListener listener;
    listener.open(hostSocket);
    UdpControlLink client;
    client.startClient(clientSocket, hostEp, qnet::nowUs() / 1000ull);

    UdpControlLink* served = nullptr;
    const uint64_t upUs = qnet::nowUs() + 3000000ull;
    while (qnet::nowUs() < upUs && served == nullptr) {
        listener.pump(1);
        ControlLink::Received rx;
        client.recvMsgEx(rx, 1);
        served = listener.acceptOne();
    }
    QNP_REQUIRE(served != nullptr);
    QNP_REQUIRE(served->established());

    const uint8_t body[4] = {1, 2, 3, 4};
    QNP_REQUIRE(client.sendMsg(qnet::MSG_SETTINGS_PENDING, body, sizeof(body)));

    bool got = false;
    const uint64_t readUs = qnet::nowUs() + 3000000ull;
    while (qnet::nowUs() < readUs && !got) {
        ControlLink::Received rx;
        if (served->recvMsgEx(rx, 0) == ControlLink::Rs::Msg
            && rx.type == qnet::MSG_SETTINGS_PENDING) {
            QNP_CHECK(rx.body.size() == sizeof(body));
            got = true;
        }
        ControlLink::Received clientRx;
        client.recvMsgEx(clientRx, 1);
    }
    QNP_CHECK(got);
}

QNP_TEST(ControlListenerReleasesASlotWhenAConversationEnds) {
    NetInit net;
    UdpSock hostSocket, clientSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(clientSocket.open(0));
    Endpoint hostEp{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));

    qnet::UdpControlListener listener;
    listener.open(hostSocket);
    UdpControlLink client;
    client.startClient(clientSocket, hostEp, qnet::nowUs() / 1000ull);

    UdpControlLink* served = nullptr;
    const uint64_t deadlineUs = qnet::nowUs() + 3000000ull;
    while (qnet::nowUs() < deadlineUs && served == nullptr) {
        listener.pump(1);
        ControlLink::Received rx;
        client.recvMsgEx(rx, 1);
        served = listener.acceptOne();
    }
    QNP_REQUIRE(served != nullptr);
    QNP_CHECK(listener.liveLinks() == 1);
    listener.release(served);
    QNP_CHECK(listener.liveLinks() == 0);
    QNP_CHECK(listener.acceptOne() == nullptr);
    QNP_CHECK(listener.isOpen());
}

QNP_TEST(ControlListenerIgnoresJunkWithoutSpendingASlot) {
    NetInit net;
    UdpSock hostSocket, senderSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(senderSocket.open(0));
    Endpoint hostEp{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));

    qnet::UdpControlListener listener;
    listener.open(hostSocket);
    int foreign = 0;
    listener.setForeignSink([&](const uint8_t*, size_t, const Endpoint&) {
        ++foreign;
    });

    const uint8_t junk[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    senderSocket.sendTo(hostEp, junk, sizeof(junk));
    LinkHello partial{ qnet::kProtocolVersion, 7 };
    uint8_t framed[qnet::kMaxMsgSize];
    const size_t n = qnet::packMsg(qnet::MSG_LINK_HELLO, &partial,
                                   sizeof(partial) - 4, framed, sizeof(framed));
    QNP_REQUIRE(n > 0);
    senderSocket.sendTo(hostEp, framed, n);

    const uint64_t deadlineUs = qnet::nowUs() + 1200000ull;
    while (qnet::nowUs() < deadlineUs) listener.pump(5);

    QNP_CHECK(listener.liveLinks() == 0);
    QNP_CHECK(listener.acceptOne() == nullptr);
    QNP_CHECK(foreign == 2);
}

QNP_TEST(ControlListenerInjectedIngressEstablishesWithoutReadingTheSocket) {
    NetInit net;
    UdpSock hostSocket, clientSocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(clientSocket.open(0));
    Endpoint hostEp{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));

    qnet::UdpControlListener listener;
    listener.openInjected(
        hostSocket,
        [&hostSocket](qnet::UdpControlListener& injected) {
            for (;;) {
                uint8_t bytes[2048]{};
                Endpoint from{};
                const int received =
                    hostSocket.recvFrom(bytes, sizeof(bytes), from);
                if (received <= 0) break;
                injected.offerDatagram(
                    bytes, static_cast<size_t>(received), from);
            }
        });

    UdpControlLink client;
    client.startClient(clientSocket, hostEp, qnet::nowUs() / 1000ull);
    UdpControlLink* served = nullptr;
    const uint64_t deadlineUs = qnet::nowUs() + 3000000ull;
    while (qnet::nowUs() < deadlineUs && served == nullptr) {
        listener.pump(1);
        ControlLink::Received ignored;
        (void)client.recvMsgEx(ignored, 1);
        served = listener.acceptOne();
    }
    QNP_REQUIRE(served != nullptr);
    QNP_CHECK(client.established());
    QNP_CHECK(served->established());
    QNP_CHECK(listener.datagramsReceived() >= 1u);
    QNP_CHECK(listener.helloDatagramsReceived() == 1u);
    listener.release(served);
}

QNP_TEST(ControlListenerCapsHowManyPeersItWillTrack) {
    NetInit net;
    UdpSock hostSocket;
    QNP_REQUIRE(hostSocket.open(0));
    Endpoint hostEp{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), hostEp));

    qnet::UdpControlListener listener;
    listener.open(hostSocket);

    constexpr size_t kAttempts = qnet::kLinkMaxPeers + 3;
    UdpSock sockets[kAttempts];
    UdpControlLink links[kAttempts];
    for (size_t i = 0; i < kAttempts; ++i) {
        QNP_REQUIRE(sockets[i].open(0));
        links[i].startClient(sockets[i], hostEp, qnet::nowUs() / 1000ull);
    }

    const uint64_t deadlineUs = qnet::nowUs() + 3000000ull;
    while (qnet::nowUs() < deadlineUs) {
        listener.pump(1);
        for (size_t i = 0; i < kAttempts; ++i) {
            ControlLink::Received rx;
            links[i].recvMsgEx(rx, 0);
        }
    }
    QNP_CHECK(listener.liveLinks() == qnet::kLinkMaxPeers);
    QNP_CHECK(listener.rejectedForCapacity() > 0);
}

QNP_TEST(DialReachesAListeningHostAndRunsTheRealHandshakeOpening) {
    NetInit net;
    UdpSock hostSocket, joinerSocket, relaySocket;
    QNP_REQUIRE(hostSocket.open(0));
    QNP_REQUIRE(joinerSocket.open(0));
    QNP_REQUIRE(relaySocket.open(0));
    Endpoint advertised{};
    Endpoint relay{};
    Endpoint joinerEndpoint{};
    QNP_REQUIRE(loopback(hostSocket.boundPort(), advertised));
    QNP_REQUIRE(loopback(relaySocket.boundPort(), relay));
    QNP_REQUIRE(loopback(joinerSocket.boundPort(), joinerEndpoint));

    qnet::UdpControlListener listener;
    listener.open(hostSocket);

    UdpControlLink joiner;
    struct HostPump {
        qnet::UdpControlListener* listener;
        volatile bool* stop;
        qnet::UdpControlLink** served;
    };
    volatile bool stop = false;
    qnet::UdpControlLink* served = nullptr;
    HostPump pumpArgs{ &listener, &stop, &served };
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID raw) -> DWORD {
            HostPump* args = static_cast<HostPump*>(raw);
            while (!*args->stop) {
                args->listener->pump(2);
                if (*args->served == nullptr)
                    *args->served = args->listener->acceptOne();
            }
            return 0;
        },
        &pumpArgs, 0, nullptr);
    QNP_REQUIRE(thread != nullptr);

    std::string error;
    const bool dialled = qnet::dialControlLink(joinerSocket, advertised,
                                               &relay, 5000, joiner, nullptr,
                                               error);
    QNP_CHECK(dialled);
    QNP_CHECK(error.empty());
    QNP_CHECK(joiner.established());

    const uint8_t latePunch = 0;
    QNP_REQUIRE(relaySocket.sendTo(joinerEndpoint, &latePunch,
                                   sizeof(latePunch)));
    ControlLink::Received lateRx;
    QNP_CHECK(joiner.recvMsgEx(lateRx, 20) == ControlLink::Rs::Timeout);
    QNP_CHECK(joiner.stats().foreignDatagrams >= 1u);

    if (dialled) {
        const uint64_t deadlineUs = qnet::nowUs() + 3000000ull;
        while (qnet::nowUs() < deadlineUs && served == nullptr) Sleep(1);
        QNP_CHECK(served != nullptr);
        if (served != nullptr) {
            const qnet::VersionConfig version{ qnet::kProtocolVersion };
            QNP_CHECK(served->sendMsg(qnet::MSG_VERSION_CONFIG, &version,
                                      sizeof(version)));
            ControlLink::Received rx;
            bool got = false;
            const uint64_t waitUs = qnet::nowUs() + 3000000ull;
            while (qnet::nowUs() < waitUs && !got)
                got = joiner.recvMsgEx(rx, 20) == ControlLink::Rs::Msg;
            QNP_CHECK(got);
            QNP_CHECK(rx.type == qnet::MSG_VERSION_CONFIG);

            QNP_CHECK(joiner.sendMsg(qnet::MSG_VERSION_CONFIG, &version,
                                     sizeof(version)));
            const qnet::ClientIntent intent{ qnet::INTENT_JOIN };
            QNP_CHECK(joiner.sendMsg(qnet::MSG_CLIENT_INTENT, &intent,
                                     sizeof(intent)));
            uint8_t seen[2] = {};
            size_t count = 0;
            const uint64_t backUs = qnet::nowUs() + 3000000ull;
            while (qnet::nowUs() < backUs && count < 2) {
                ControlLink::Received hostRx;
                if (served->recvMsgEx(hostRx, 20) == ControlLink::Rs::Msg)
                    seen[count++] = hostRx.type;
            }
            QNP_REQUIRE(count == 2);
            QNP_CHECK(seen[0] == qnet::MSG_VERSION_CONFIG);
            QNP_CHECK(seen[1] == qnet::MSG_CLIENT_INTENT);
        }
    }

    stop = true;
    WaitForSingleObject(thread, 3000);
    CloseHandle(thread);
}

QNP_TEST(DialUsesRelaySelectedSameHostSidecarControlBridge) {
    NetInit net;
    UdpSock bridgeSocket, joinerSocket, relaySocket, advertisedReservation;
    QNP_REQUIRE(bridgeSocket.open(0));
    QNP_REQUIRE(joinerSocket.open(0));
    QNP_REQUIRE(relaySocket.open(0));
    QNP_REQUIRE(advertisedReservation.open(0));

    const uint16_t advertisedPort = advertisedReservation.boundPort();
    advertisedReservation.close();
    QNP_REQUIRE(advertisedPort != bridgeSocket.boundPort());

    Endpoint advertised{}, relay{}, bridge{};
    QNP_REQUIRE(loopback(advertisedPort, advertised));
    QNP_REQUIRE(loopback(relaySocket.boundPort(), relay));
    QNP_REQUIRE(loopback(bridgeSocket.boundPort(), bridge));

    qnet::UdpControlListener listener;
    listener.open(bridgeSocket);
    struct BridgePump {
        UdpSock* relay;
        qnet::UdpControlListener* listener;
        Endpoint bridge;
        volatile bool* stop;
        qnet::UdpControlLink** served;
    };
    volatile bool stop = false;
    qnet::UdpControlLink* served = nullptr;
    BridgePump pumpArgs{ &relaySocket, &listener, bridge, &stop, &served };
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID raw) -> DWORD {
            BridgePump* args = static_cast<BridgePump*>(raw);
            while (!*args->stop) {
                uint8_t request[32]{};
                Endpoint requester{};
                const int received = args->relay->recvFrom(
                    request, sizeof(request), requester);
                if (received == 8) {
                    const uint16_t port =
                        ntohs(args->bridge.sa.sin_port);
                    uint8_t mapping[8] = {
                        static_cast<uint8_t>(port >> 8),
                        static_cast<uint8_t>(port & 0xFFu),
                        static_cast<uint8_t>(port >> 8),
                        static_cast<uint8_t>(port & 0xFFu),
                        0u, 0u, 0u, 0u
                    };
                    std::memcpy(mapping + 4,
                                &args->bridge.sa.sin_addr.s_addr, 4u);
                    args->relay->sendTo(requester, mapping,
                                        sizeof(mapping));
                }
                args->listener->pump(1);
                if (*args->served == nullptr)
                    *args->served = args->listener->acceptOne();
            }
            return 0;
        },
        &pumpArgs, 0, nullptr);
    QNP_REQUIRE(thread != nullptr);

    UdpControlLink joiner;
    std::string error;
    const bool dialled = qnet::dialControlLink(
        joinerSocket, advertised, &relay, 5000, joiner, nullptr, error);
    QNP_CHECK(dialled);
    QNP_CHECK(error.empty());
    QNP_CHECK(joiner.established());
    QNP_CHECK(joiner.peer() == bridge);

    stop = true;
    WaitForSingleObject(thread, 3000);
    CloseHandle(thread);
}

QNP_TEST(DialGivesUpOnItsOwnBudgetWhenNothingIsListening) {
    NetInit net;
    UdpSock socket;
    QNP_REQUIRE(socket.open(0));
    Endpoint nowhere{};
    QNP_REQUIRE(loopback(1u, nowhere));

    UdpControlLink link;
    std::string error;
    const uint64_t startUs = qnet::nowUs();
    QNP_CHECK(!qnet::dialControlLink(socket, nowhere, nullptr, 400, link,
                                     nullptr, error));
    const uint64_t elapsedMs = (qnet::nowUs() - startUs) / 1000ull;
    QNP_CHECK(elapsedMs >= 350 && elapsedMs < 4000);
    QNP_CHECK(!error.empty());
    QNP_CHECK(!link.established());
}

QNP_TEST(DialStopsWhenTheCallerCancels) {
    NetInit net;
    UdpSock socket;
    QNP_REQUIRE(socket.open(0));
    Endpoint nowhere{};
    QNP_REQUIRE(loopback(1u, nowhere));

    UdpControlLink link;
    std::string error;
    int ticks = 0;
    const uint64_t startUs = qnet::nowUs();
    const bool ok = qnet::dialControlLink(
        socket, nowhere, nullptr, 30000, link,
        [&](const qnet::ControlDialStatus&) { return ++ticks < 3; }, error);
    QNP_CHECK(!ok);
    QNP_CHECK(error == "Cancelled");
    QNP_CHECK((qnet::nowUs() - startUs) / 1000ull < 5000);
}

QNP_TEST(ControlLinkZeroTimeoutIsOneNonBlockingPoll) {
    NetInit net;
    UdpSock hostSocket;
    QNP_REQUIRE(hostSocket.open(0));
    UdpControlLink host;
    host.startHostListen(hostSocket, qnet::nowUs() / 1000ull);

    const uint64_t startUs = qnet::nowUs();
    ControlLink::Received rx;
    for (int i = 0; i < 50; ++i)
        QNP_CHECK(host.recvMsgEx(rx, 0) == ControlLink::Rs::Timeout);
    QNP_CHECK((qnet::nowUs() - startUs) < 500000ull);
    QNP_CHECK(rx.type == qnet::MSG_NONE);
}

QNP_TEST(ControlLinkRefusesToSendBeforeItIsEstablished) {
    NetInit net;
    UdpSock socket;
    QNP_REQUIRE(socket.open(0));
    UdpControlLink link;
    const qnet::VersionConfig version{ qnet::kProtocolVersion };

    QNP_CHECK(!link.sendMsg(qnet::MSG_VERSION_CONFIG, &version, sizeof(version)));
    link.startHostListen(socket, qnet::nowUs() / 1000ull);
    QNP_CHECK(!link.sendMsg(qnet::MSG_VERSION_CONFIG, &version, sizeof(version)));
    link.close();
    QNP_CHECK(!link.sendMsg(qnet::MSG_VERSION_CONFIG, &version, sizeof(version)));
    QNP_CHECK(!link.isOpen());
}

QNP_TEST(ControlLinkCarriesEveryHandshakeMessageSize) {
    constexpr size_t kEnvelope = sizeof(qnet::MsgHeader) + sizeof(qnet::RelData);
    QNP_CHECK(kEnvelope + sizeof(qnet::VersionConfig)  <= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::InitialConfig)  <= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::NetplayConfig)  <= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::TuningProposal) <= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::ConfirmConfig)  <= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::RouteCommit)    <= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::RouteCandidates)<= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::RuntimeSync)    <= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::ErrorText)      <= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::SpectateInfo)   <= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::ClientIntent)   <= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::PingStats)      <= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::SettingsPending)<= qnet::kLinkMaxDatagram);
    QNP_CHECK(kEnvelope + sizeof(qnet::Ping)           <= qnet::kLinkMaxDatagram);
}

QNP_TEST(ControlDescriptorsRoundTripFullCompatibilityIdentity) {
    const qnetplay::compat::CompatibilityIdentityWire identity =
        controlTestIdentity();
    QNP_REQUIRE(qnet::isValidControlCompatibilityIdentity(identity));

    qnet::InitialConfig initial{};
    initial.name[0] = 'A';
    initial.nameCodePage = 932u;
    initial.isHost = 1u;
    initial.dataPort = 7500u;
    initial.roundsToWin = 2u;
    initial.compatibilityIdentity = identity;
    initial.runtimeArtifacts = controlTestRuntimeArtifacts();
    qnet::InitialConfig initialDecoded{};
    QNP_REQUIRE(roundTripControlBody(qnet::MSG_INITIAL_CONFIG, initial,
                                     initialDecoded));
    QNP_CHECK(qnet::isValidControlCompatibilityIdentity(
        initialDecoded.compatibilityIdentity));
    QNP_CHECK(qnetplay::compat::semanticallyEqual(
        identity, initialDecoded.compatibilityIdentity));
    QNP_CHECK(qnet::isValidRuntimeArtifactIdentity(
        initialDecoded.runtimeArtifacts));
    QNP_CHECK(qnet::runtimeArtifactMismatch(
        initial.runtimeArtifacts, initialDecoded.runtimeArtifacts)
              == qnet::RuntimeArtifactMismatch::None);
    QNP_CHECK(initialDecoded.roundsToWin == 2u);

    qnet::TuningProposal proposal{};
    proposal.sessionId = 0x10203040u;
    proposal.clientTuning = {2u, 4u, 8u, 10u};
    proposal.routeKind = qnet::ROUTE_DIRECT;
    proposal.routeLabel = static_cast<uint8_t>(qnet::RouteLabel::LanDirect);
    proposal.routeId = 7u;
    qnet::TuningProposal proposalDecoded{};
    QNP_REQUIRE(roundTripControlBody(qnet::MSG_TUNING_PROPOSAL, proposal,
                                     proposalDecoded));
    QNP_CHECK(proposalDecoded.sessionId == proposal.sessionId);
    QNP_CHECK(proposalDecoded.clientTuning.delay == 2u);
    QNP_CHECK(proposalDecoded.clientTuning.rollback == 4u);
    QNP_CHECK(proposalDecoded.clientTuning.maxDelay == 8u);
    QNP_CHECK(proposalDecoded.clientTuning.maxRollback == 10u);
    QNP_CHECK(proposalDecoded.routeKind == qnet::ROUTE_DIRECT);
    QNP_CHECK(proposalDecoded.routeId == 7u);

    qnet::NetplayConfig netplay{};
    netplay.sessionId = 0x10203040u;
    netplay.compatibilityId = 0x1020304050607080ull;
    netplay.compatibilityIdentity = identity;
    qnet::NetplayConfig netplayDecoded{};
    QNP_REQUIRE(roundTripControlBody(qnet::MSG_NETPLAY_CONFIG, netplay,
                                     netplayDecoded));
    QNP_CHECK(netplayDecoded.compatibilityId == netplay.compatibilityId);
    QNP_CHECK(qnetplay::compat::semanticallyEqual(
        identity, netplayDecoded.compatibilityIdentity));

    qnet::RuntimeSync runtime{};
    runtime.sessionId = netplay.sessionId;
    runtime.compatibilityId = netplay.compatibilityId;
    runtime.compatibilityIdentity = identity;
    qnet::RuntimeSync runtimeDecoded{};
    QNP_REQUIRE(roundTripControlBody(qnet::MSG_RUNTIME_READY, runtime,
                                     runtimeDecoded));
    QNP_CHECK(runtimeDecoded.compatibilityId == runtime.compatibilityId);
    QNP_CHECK(qnetplay::compat::semanticallyEqual(
        identity, runtimeDecoded.compatibilityIdentity));

    qnet::SpectateInfo spectate{};
    spectate.state = qnet::SPECTATE_LIVE;
    spectate.sessionId = netplay.sessionId;
    spectate.simulationContract = netplay.compatibilityId;
    spectate.compatibilityIdentity = identity;
    spectate.presence.schemaVersion = qnet::kLivePresenceSchemaVersion;
    spectate.presence.state = qnet::LIVE_PRESENCE_MATCH;
    spectate.presence.flags = qnet::kLivePresenceRosterValid
                            | qnet::kLivePresenceRoundScoreValid
                            | qnet::kLivePresenceSetScoreValid;
    spectate.presence.sessionId = netplay.sessionId;
    spectate.presence.roundsToWin = 2u;
    spectate.presence.activeFighterMask = 0x03u;
    spectate.presence.characterIds[0] = 4u;
    spectate.presence.characterIds[1] = 18u;
    spectate.presence.characterIds[2] = 0xFFu;
    spectate.presence.characterIds[3] = 0xFFu;
    spectate.presence.roundWins[0] = 1u;
    spectate.presence.matchWins[1] = 3u;
    std::memcpy(spectate.presence.playerNames[0], "Aquatic", 8u);
    std::memcpy(spectate.presence.playerNames[1], "Player 2", 9u);
    qnet::SpectateInfo spectateDecoded{};
    QNP_REQUIRE(roundTripControlBody(qnet::MSG_SPECTATE_INFO, spectate,
                                     spectateDecoded));
    QNP_CHECK(spectateDecoded.simulationContract
              == spectate.simulationContract);
    QNP_CHECK(qnetplay::compat::semanticallyEqual(
        identity, spectateDecoded.compatibilityIdentity));
    QNP_CHECK(spectateDecoded.presence.schemaVersion
              == qnet::kLivePresenceSchemaVersion);
    QNP_CHECK(spectateDecoded.presence.characterIds[1] == 18u);
    QNP_CHECK(spectateDecoded.presence.matchWins[1] == 3u);
    QNP_CHECK(std::strcmp(
                  spectateDecoded.presence.playerNames[0], "Aquatic") == 0);

    initialDecoded.compatibilityIdentity = {};
    netplayDecoded.compatibilityIdentity = {};
    runtimeDecoded.compatibilityIdentity = {};
    spectateDecoded.compatibilityIdentity = {};
    QNP_CHECK(!qnet::isValidControlCompatibilityIdentity(
        initialDecoded.compatibilityIdentity));
    QNP_CHECK(!qnet::isValidControlCompatibilityIdentity(
        netplayDecoded.compatibilityIdentity));
    QNP_CHECK(!qnet::isValidControlCompatibilityIdentity(
        runtimeDecoded.compatibilityIdentity));
    QNP_CHECK(!qnet::isValidControlCompatibilityIdentity(
        spectateDecoded.compatibilityIdentity));
}

QNP_TEST(ControlLinkEstablishmentTypesDoNotCollideWithTheGameWire) {
    QNP_CHECK(qnet::kMagic != qnetplay::wire::kMagic);
    QNP_CHECK(qnet::MSG_LINK_HELLO == 35);
    QNP_CHECK(qnet::MSG_LINK_HELLO_ACK == 36);
    QNP_CHECK(qnet::MSG_LINK_HELLO > qnet::MSG_SPECTATE_INFO);
    QNP_CHECK(sizeof(LinkHello) == 12);
}
