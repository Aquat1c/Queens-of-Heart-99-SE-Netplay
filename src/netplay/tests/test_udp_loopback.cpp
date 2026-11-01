#include "TestHarness.h"

#include "../../game/DPlayTransport.h"
#include "../NetplayCore.h"
#include "../../net/Sockets.h"

#include <cstddef>
#include <cstdint>

namespace {

qnet::Endpoint socketEndpoint(uint16_t port) {
    qnet::Endpoint endpoint;
    QNP_CHECK(qnet::Endpoint::parse("127.0.0.1", port, endpoint));
    return endpoint;
}

qnetplay::Endpoint coreEndpoint(const qnet::Endpoint& endpoint) {
    return {
        endpoint.sa.sin_addr.s_addr,
        ntohs(endpoint.sa.sin_port),
    };
}

sockaddr_in transportEndpoint(uint16_t port) {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(0x7F000001u);
    endpoint.sin_port = htons(port);
    return endpoint;
}

qnetplay::compat::CompatibilityIdentityWire loopbackFixtureIdentity() {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = qnetplay::wire::kVersion;
    (void)qnetplay::compat::setProfileId(identity, "fixture-profile");
    for (std::size_t index = 0u;
         index < qnetplay::compat::kCompatibilitySha256Bytes; ++index) {
        identity.runtimeBuild.executableSha256[index] =
            static_cast<std::uint8_t>(0x13u + index);
        identity.simulationContent.simulationRootSha256[index] =
            static_cast<std::uint8_t>(0x43u + index);
        identity.simulationContent.validationRootSha256[index] =
            static_cast<std::uint8_t>(0x83u + index);
    }
    identity.runtimeBuild.executableSize = 1u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.simulationContent.entryCount = 1u;
    identity.simulationContent.requiredEntryCount = 1u;
    identity.stateSchemaId = 1u;
    identity.modSimulationId = 1u;
    identity.timingProfileId = 1u;
    return identity;
}

void sendPending(qnet::UdpSock& socket, qnetplay::NetplayCore& core,
                 uint64_t nowMs) {
    for (const auto& packet : core.pollOutbound(nowMs)) {
        qnet::Endpoint destination;
        destination.sa.sin_addr.s_addr = packet.destination.ipv4;
        destination.sa.sin_port = htons(packet.destination.port);
        QNP_CHECK(socket.sendTo(destination, packet.bytes.data(),
                               packet.bytes.size()));
    }
}

void receivePending(qnet::UdpSock& socket, qnetplay::NetplayCore& core,
                    uint64_t nowMs) {
    uint8_t bytes[qnetplay::wire::kMaxDatagramSize];
    for (;;) {
        qnet::Endpoint source;
        int received = socket.recvFrom(bytes, sizeof(bytes), source);
        QNP_CHECK(received >= 0);
        if (received <= 0) break;
        QNP_CHECK(core.onDatagram(coreEndpoint(source), bytes,
                                  static_cast<size_t>(received), nowMs)
                  == qnetplay::IngressResult::Accepted);
    }
}

}

QNP_TEST(UdpLoopbackLearnsExactClientPortWithCookie) {
    qnet::UdpSock hostSocket;
    qnet::UdpSock clientSocket;
    QNP_CHECK(hostSocket.open(0));
    QNP_CHECK(clientSocket.open(0));
    const auto hostEndpoint = socketEndpoint(hostSocket.boundPort());

    const qnetplay::wire::SessionCookie cookie{
        0x8877665544332211ull,
        0xFFEEDDCCBBAA0099ull,
    };
    constexpr uint64_t sessionId = 0x1122334455667788ull;
    qnetplay::CoreConfig hostConfig{};
    hostConfig.localRole = qnetplay::wire::PeerRole::Host;
    hostConfig.localPlayer = 0;
    hostConfig.compatibilityId = 0x123456789ABCDEF0ull;
    hostConfig.compatibilityIdentity = loopbackFixtureIdentity();
    hostConfig.sessionId = sessionId;
    hostConfig.cookie = cookie;
    hostConfig.expectedPeer = coreEndpoint(socketEndpoint(1));
    hostConfig.expectedPeer.port = 0;
    hostConfig.learnPeerPort = true;

    qnetplay::CoreConfig clientConfig{};
    clientConfig.localRole = qnetplay::wire::PeerRole::Client;
    clientConfig.localPlayer = 1;
    clientConfig.compatibilityId = hostConfig.compatibilityId;
    clientConfig.compatibilityIdentity = hostConfig.compatibilityIdentity;
    clientConfig.sessionId = sessionId;
    clientConfig.cookie = cookie;
    clientConfig.expectedPeer = coreEndpoint(hostEndpoint);

    qnetplay::NetplayCore host(hostConfig);
    qnetplay::NetplayCore client(clientConfig);

    for (uint64_t nowMs = 1;
         nowMs < 2000 && (!host.isEstablished() || !client.isEstablished());
         nowMs += 10) {
        sendPending(clientSocket, client, nowMs);
        receivePending(hostSocket, host, nowMs);
        sendPending(hostSocket, host, nowMs);
        receivePending(clientSocket, client, nowMs);
    }

    QNP_CHECK(host.isEstablished());
    QNP_CHECK(client.isEstablished());
    QNP_CHECK(host.peerEndpoint().port == clientSocket.boundPort());
    QNP_CHECK(client.peerEndpoint().port == hostSocket.boundPort());
    QNP_CHECK(host.peerEndpoint().ipv4 ==
              client.peerEndpoint().ipv4);
}

QNP_TEST(ModernSocketProtocolDomainsRejectCrossDelivery) {
    using qgame::transport_detail::ModernSocketDomain;
    using qgame::transport_detail::modernDatagramMatchesDomain;
    using qgame::transport_detail::modernRecvErrorEndsTurn;

    QNP_CHECK(modernDatagramMatchesDomain(
        ModernSocketDomain::Player, false));
    QNP_CHECK(!modernDatagramMatchesDomain(
        ModernSocketDomain::Player, true));
    QNP_CHECK(modernDatagramMatchesDomain(
        ModernSocketDomain::Spectator, true));
    QNP_CHECK(!modernDatagramMatchesDomain(
        ModernSocketDomain::Spectator, false));
    QNP_CHECK(modernRecvErrorEndsTurn(WSAEWOULDBLOCK));
    QNP_CHECK(modernRecvErrorEndsTurn(WSAECONNRESET));
    QNP_CHECK(!modernRecvErrorEndsTurn(WSAEMSGSIZE));
}

QNP_TEST(SpectatorParentRouteTransitionRejectsUnauthorizedSourcesAndCommitsAtomically) {
    using qgame::transport_detail::SpectatorParentRouteCursor;
    using qgame::transport_detail::SpectatorParentRouteIngress;
    using qgame::transport_detail::SpectatorParentRouteTransitionAuthorization;
    using qgame::transport_detail::SpectatorParentRouteTransitionGate;

    const sockaddr_in original = transportEndpoint(31001u);
    const sockaddr_in candidate = transportEndpoint(31002u);
    const sockaddr_in unauthorized = transportEndpoint(31003u);
    SpectatorParentRouteTransitionGate gate;
    QNP_CHECK(gate.reset(original));

    SpectatorParentRouteCursor current{};
    current.sourceGeneration = 7u;
    current.nextRecord = 42u;
    current.nextEvent = 5u;
    current.digest = 0xCAB005E5D15EA5E1ull;

    SpectatorParentRouteTransitionAuthorization activation{};
    activation.currentParentIpv4BE = original.sin_addr.s_addr;
    activation.currentParentPortBE = original.sin_port;
    activation.candidateIpv4BE = candidate.sin_addr.s_addr;
    activation.candidatePortBE = candidate.sin_port;
    activation.sourceGeneration = current.sourceGeneration;
    activation.routeGeneration = 1u;
    activation.nextRecord = current.nextRecord;
    activation.nextEvent = current.nextEvent;
    activation.digest = current.digest;
    activation.localDeadlineMs = 1000u;
    activation.activationId = 0xA11CE70000000001ull;

    QNP_CHECK(gate.classify(original, 100u)
              == SpectatorParentRouteIngress::Active);
    QNP_CHECK(gate.classify(candidate, 100u)
              == SpectatorParentRouteIngress::Rejected);
    QNP_CHECK(gate.classify(unauthorized, 100u)
              == SpectatorParentRouteIngress::Rejected);

    SpectatorParentRouteTransitionAuthorization wrongCursor = activation;
    ++wrongCursor.nextRecord;
    QNP_CHECK(!gate.armValidated(wrongCursor, current, 100u));
    QNP_CHECK(gate.classify(candidate, 100u)
              == SpectatorParentRouteIngress::Rejected);
    SpectatorParentRouteTransitionAuthorization wrongSource = activation;
    ++wrongSource.sourceGeneration;
    QNP_CHECK(!gate.armValidated(wrongSource, current, 100u));
    QNP_CHECK(gate.classify(candidate, 100u)
              == SpectatorParentRouteIngress::Rejected);
    SpectatorParentRouteTransitionAuthorization wrongParent = activation;
    wrongParent.currentParentPortBE = unauthorized.sin_port;
    QNP_CHECK(!gate.armValidated(wrongParent, current, 100u));
    QNP_CHECK(gate.classify(candidate, 100u)
              == SpectatorParentRouteIngress::Rejected);

    QNP_CHECK(gate.armValidated(activation, current, 100u));
    QNP_CHECK(gate.pending());
    QNP_CHECK(gate.classify(original, 101u)
              == SpectatorParentRouteIngress::Active);
    QNP_CHECK(gate.classify(candidate, 101u)
              == SpectatorParentRouteIngress::Pending);
    QNP_CHECK(gate.classify(unauthorized, 101u)
              == SpectatorParentRouteIngress::Rejected);

    QNP_CHECK(!gate.commitPendingAfterAcceptedIngress(original, 101u));
    QNP_CHECK(gate.pending());
    QNP_CHECK(gate.commitPendingAfterAcceptedIngress(candidate, 101u));
    QNP_CHECK(!gate.pending());
    QNP_CHECK(gate.classify(original, 102u)
              == SpectatorParentRouteIngress::Rejected);
    QNP_CHECK(gate.classify(candidate, 102u)
              == SpectatorParentRouteIngress::Active);
    QNP_CHECK(gate.classify(unauthorized, 102u)
              == SpectatorParentRouteIngress::Rejected);
}

QNP_TEST(SpectatorParentRouteTransitionExpiresWithoutReplacingTheOldRoute) {
    using qgame::transport_detail::SpectatorParentRouteCursor;
    using qgame::transport_detail::SpectatorParentRouteIngress;
    using qgame::transport_detail::SpectatorParentRouteTransitionAuthorization;
    using qgame::transport_detail::SpectatorParentRouteTransitionGate;

    const sockaddr_in original = transportEndpoint(31101u);
    const sockaddr_in candidate = transportEndpoint(31102u);
    SpectatorParentRouteTransitionGate gate;
    QNP_CHECK(gate.reset(original));

    SpectatorParentRouteCursor current{};
    current.sourceGeneration = 3u;
    current.nextRecord = 9u;
    current.nextEvent = 2u;
    current.digest = 0xE011DAD00D0C0DEull;

    SpectatorParentRouteTransitionAuthorization activation{};
    activation.currentParentIpv4BE = original.sin_addr.s_addr;
    activation.currentParentPortBE = original.sin_port;
    activation.candidateIpv4BE = candidate.sin_addr.s_addr;
    activation.candidatePortBE = candidate.sin_port;
    activation.sourceGeneration = current.sourceGeneration;
    activation.routeGeneration = 4u;
    activation.nextRecord = current.nextRecord;
    activation.nextEvent = current.nextEvent;
    activation.digest = current.digest;
    activation.localDeadlineMs = 200u;
    activation.activationId = 0xA11CE70000000004ull;
    QNP_CHECK(gate.armValidated(activation, current, 199u));

    QNP_CHECK(gate.classify(candidate, 200u)
              == SpectatorParentRouteIngress::Rejected);
    QNP_CHECK(gate.classify(original, 200u)
              == SpectatorParentRouteIngress::Active);
    activation.localDeadlineMs = 400u;
    QNP_CHECK(!gate.armValidated(activation, current, 201u));
}

QNP_TEST(SpectatorParentRouteTransitionCancellationOnlyAcceptsTheCurrentActivation) {
    using qgame::transport_detail::SpectatorParentRouteCursor;
    using qgame::transport_detail::SpectatorParentRouteIngress;
    using qgame::transport_detail::SpectatorParentRouteTransitionAuthorization;
    using qgame::transport_detail::SpectatorParentRouteTransitionGate;

    const sockaddr_in original = transportEndpoint(31201u);
    const sockaddr_in candidate = transportEndpoint(31202u);
    SpectatorParentRouteTransitionGate gate;
    QNP_CHECK(gate.reset(original));

    SpectatorParentRouteCursor current{};
    current.sourceGeneration = 11u;
    current.nextRecord = 14u;
    current.nextEvent = 2u;
    current.digest = 0xC0FFEE0000D15EA5ull;

    SpectatorParentRouteTransitionAuthorization activation{};
    activation.currentParentIpv4BE = original.sin_addr.s_addr;
    activation.currentParentPortBE = original.sin_port;
    activation.candidateIpv4BE = candidate.sin_addr.s_addr;
    activation.candidatePortBE = candidate.sin_port;
    activation.sourceGeneration = current.sourceGeneration;
    activation.routeGeneration = 8u;
    activation.nextRecord = current.nextRecord;
    activation.nextEvent = current.nextEvent;
    activation.digest = current.digest;
    activation.localDeadlineMs = 900u;
    activation.activationId = 0xA11CE70000000008ull;
    QNP_CHECK(gate.armValidated(activation, current, 100u));

    QNP_CHECK(!gate.cancelPendingActivation(activation.activationId + 1u));
    QNP_CHECK(gate.pending());
    QNP_CHECK(gate.classify(candidate, 101u)
              == SpectatorParentRouteIngress::Pending);
    QNP_CHECK(gate.cancelPendingActivation(activation.activationId));
    QNP_CHECK(!gate.pending());
    QNP_CHECK(gate.classify(original, 101u)
              == SpectatorParentRouteIngress::Active);
    QNP_CHECK(gate.classify(candidate, 101u)
              == SpectatorParentRouteIngress::Rejected);

    ++activation.routeGeneration;
    ++activation.activationId;
    activation.localDeadlineMs = 1000u;
    QNP_CHECK(gate.armValidated(activation, current, 102u));
    QNP_CHECK(!gate.cancelPendingActivation(activation.activationId - 1u));
    QNP_CHECK(gate.pending());
    QNP_CHECK(gate.cancelPendingActivation(activation.activationId));
}

QNP_TEST(IndependentUdpQueuesKeepPlayerReadableDuringSpectatorAndRelayFlood) {
    qnet::UdpSock playerSocket;
    qnet::UdpSock spectatorSocket;
    qnet::UdpSock relaySocket;
    qnet::UdpSock senderSocket;
    QNP_CHECK(playerSocket.open(0));
    QNP_CHECK(spectatorSocket.open(0));
    QNP_CHECK(relaySocket.open(0));
    QNP_CHECK(senderSocket.open(0));
    QNP_CHECK(playerSocket.boundPort() != 0);
    QNP_CHECK(spectatorSocket.boundPort() != 0);
    QNP_CHECK(relaySocket.boundPort() != 0);
    QNP_CHECK(playerSocket.boundPort()
              != spectatorSocket.boundPort());
    QNP_CHECK(playerSocket.boundPort() != relaySocket.boundPort());
    QNP_CHECK(spectatorSocket.boundPort() != relaySocket.boundPort());

    const auto playerEndpoint =
        socketEndpoint(playerSocket.boundPort());
    const auto spectatorEndpoint =
        socketEndpoint(spectatorSocket.boundPort());
    const auto relayEndpoint = socketEndpoint(relaySocket.boundPort());
    constexpr uint32_t spectatorPayload =
        qnetplay::spectator::kSpectatorMagic;
    constexpr uint32_t relayPayload = 0x51A7E5EEu;
    constexpr uint32_t playerPayload = 0x13579BDFu;
    constexpr size_t spectatorPackets =
        qgame::transport_detail::kModernSpectatorIngressBudget + 16;

    for (size_t packet = 0;
         packet < spectatorPackets; ++packet) {
        QNP_CHECK(senderSocket.sendTo(
            spectatorEndpoint, &spectatorPayload,
            sizeof(spectatorPayload)));
    }
    for (size_t packet = 0;
         packet < spectatorPackets; ++packet) {
        QNP_CHECK(senderSocket.sendTo(
            relayEndpoint, &relayPayload, sizeof(relayPayload)));
    }
    QNP_CHECK(senderSocket.sendTo(
        playerEndpoint, &playerPayload, sizeof(playerPayload)));

    QNP_CHECK(playerSocket.waitReadable(1000));
    uint32_t receivedPayload = 0;
    qnet::Endpoint source;
    QNP_CHECK(playerSocket.recvFrom(
                  &receivedPayload, sizeof(receivedPayload), source)
              == sizeof(receivedPayload));
    QNP_CHECK(receivedPayload == playerPayload);

    size_t consumed = 0;
    for (; consumed
           < qgame::transport_detail::kModernSpectatorIngressBudget;
         ++consumed) {
        QNP_CHECK(spectatorSocket.waitReadable(1000));
        receivedPayload = 0;
        QNP_CHECK(spectatorSocket.recvFrom(
                      &receivedPayload, sizeof(receivedPayload),
                      source)
                  == sizeof(receivedPayload));
        QNP_CHECK(receivedPayload == spectatorPayload);
    }
    QNP_CHECK(
        consumed
        == qgame::transport_detail::kModernSpectatorIngressBudget);
    QNP_CHECK(spectatorSocket.waitReadable(1000));
    QNP_CHECK(spectatorSocket.recvFrom(
                  &receivedPayload, sizeof(receivedPayload), source)
              == sizeof(receivedPayload));

    consumed = 0;
    for (; consumed
           < qgame::transport_detail::kModernSpectatorIngressBudget;
         ++consumed) {
        QNP_CHECK(relaySocket.waitReadable(1000));
        receivedPayload = 0;
        QNP_CHECK(relaySocket.recvFrom(
                      &receivedPayload, sizeof(receivedPayload), source)
                  == sizeof(receivedPayload));
        QNP_CHECK(receivedPayload == relayPayload);
    }
    QNP_CHECK(
        consumed
        == qgame::transport_detail::kModernSpectatorIngressBudget);
    QNP_CHECK(relaySocket.waitReadable(1000));
    QNP_CHECK(relaySocket.recvFrom(
                  &receivedPayload, sizeof(receivedPayload), source)
              == sizeof(receivedPayload));
}
