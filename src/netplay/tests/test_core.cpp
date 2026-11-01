#include "TestHarness.h"

#include "../NetplayCore.h"
#include "../../game/FrontendDelayPolicy.h"

#include <array>

#include <string>
#include <vector>

namespace {

using namespace qnetplay;
using namespace qnetplay::wire;

constexpr uint32_t kHostIp = 0x0A000001u;
constexpr uint32_t kClientIp = 0x0A000002u;
constexpr uint64_t kCompatibility = 0x514F483939000003ull;
constexpr uint64_t kSession = 0x0102030405060708ull;

QNP_TEST(FrontendDelayIsStableAcrossSubMillisecondLanJitter) {
    const uint32_t lanSamplesUs[] = {80u, 200u, 386u, 546u, 900u, 999u};
    const uint8_t expected =
        qgame::frontendDelayForRttMicros(lanSamplesUs[0], 1u);
    for (uint32_t sampleUs : lanSamplesUs)
        QNP_CHECK(qgame::frontendDelayForRttMicros(sampleUs, 1u)
                  == expected);
    QNP_CHECK(qgame::frontendDelayForRtt(0u, 1u)
              != qgame::frontendDelayForRtt(47u, 1u));
    QNP_CHECK(qgame::frontendDelayForRttMicros(47000u, 1u)
              == qgame::frontendDelayForRtt(47u, 1u));
    QNP_CHECK(qgame::frontendDelayForRttMicros(250000u, 1u)
              == qgame::frontendDelayForRtt(250u, 1u));
    for (uint32_t ms = 0; ms <= 300; ++ms) {
        QNP_CHECK(qgame::frontendDelayForRttMicros(ms * 1000u, 1u)
                  == qgame::frontendDelayForRtt(ms, 1u));
    }
}

QNP_TEST(FrontendDelayUsesMeasuredOneWayLeadPlusBoundedSafety) {
    QNP_CHECK(qgame::frontendDelayForRtt(0, 0) == 1);
    QNP_CHECK(qgame::frontendDelayForRtt(21, 1) == 2);
    QNP_CHECK(qgame::frontendDelayForRtt(165, 1) == 6);
    QNP_CHECK(qgame::frontendDelayForRtt(181, 1) == 8);
    QNP_CHECK(qgame::frontendDelayForRtt(219, 1) == 9);
    QNP_CHECK(qgame::frontendDelayForRtt(237, 1) == 10);
    QNP_CHECK(qgame::frontendDelayForRtt(237, 10) == 10);
    QNP_CHECK(qgame::frontendDelayForRtt(270, 1) == 11);
    QNP_CHECK(qgame::frontendDelayForRtt(1000, 1) == 15);
}

QNP_TEST(FrontendDelayLatchHoldsTheBaselineUntilTheWindowMeansSomething) {
    qgame::FrontendDelayLatch latch;
    latch.frames = 1u;
    for (int64_t t = 0; t < 5000000; t += 100000) {
        QNP_CHECK(latch.step(false, 15u,
                             2u, t, 150000, 300000) == 2u);
    }
    QNP_CHECK(latch.frames == 2u);
}

QNP_TEST(FrontendDelayLatchRampsInsteadOfJumpingToASpike) {
    qgame::FrontendDelayLatch latch;
    latch.frames = 1u;
    QNP_CHECK(latch.step(true, 15u, 1u, 0, 150000, 300000) == 1u);
    QNP_CHECK(latch.step(true, 2u, 1u, 16000, 150000, 300000) == 1u);
    QNP_CHECK(latch.frames == 1u);
}

QNP_TEST(FrontendDelayLatchStillCoversASustainedIncrease) {
    qgame::FrontendDelayLatch latch;
    latch.frames = 1u;
    const uint32_t target =
        qgame::frontendDelayForRttMicros(250000u, 0u);
    QNP_CHECK(target == 10u);
    int64_t t = 0;
    for (int i = 0; i < 400 && latch.frames < target; ++i) {
        t += 16000;
        latch.step(true, target, 1u, t, 150000, 300000);
    }
    QNP_CHECK(latch.frames == target);
    QNP_CHECK(t <= 2000000);
}

QNP_TEST(FrontendDelayLatchShedsInRealTimeAndStopsAtWhatTheLinkNeeds) {
    qgame::FrontendDelayLatch latch;
    latch.frames = 12u;
    int64_t t = 0;
    for (int i = 0; i < 2000 && latch.frames > 3u; ++i) {
        t += 16000;
        latch.step(true, 3u, 1u, t, 150000, 300000);
    }
    QNP_CHECK(latch.frames == 3u);
    QNP_CHECK(t <= 3500000);
    for (int i = 0; i < 100; ++i) {
        t += 16000;
        QNP_CHECK(latch.step(true, 3u, 1u, t, 150000, 300000) == 3u);
    }
}

QNP_TEST(FrontendDelayLatchNeverFallsBelowOneFrame) {
    qgame::FrontendDelayLatch latch;
    latch.frames = 4u;
    int64_t t = 0;
    for (int i = 0; i < 2000; ++i) {
        t += 16000;
        QNP_CHECK(latch.step(true, 0u, 0u, t,
                             150000, 300000) >= 1u);
    }
    QNP_CHECK(latch.frames == 1u);
}

SessionCookie cookie() {
    return SessionCookie{
        0x0123456789ABCDEFull,
        0xFEDCBA9876543210ull,
    };
}

qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity(
    const char* const profileId = "qoh99-se1126") {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = kVersion;
    QNP_CHECK(qnetplay::compat::setProfileId(identity, profileId));
    for (size_t i = 0u;
         i < qnetplay::compat::kCompatibilitySha256Bytes; ++i) {
        identity.runtimeBuild.executableSha256[i] =
            static_cast<uint8_t>(0x20u + i);
        identity.simulationContent.simulationRootSha256[i] =
            static_cast<uint8_t>(0x50u + i);
        identity.simulationContent.validationRootSha256[i] =
            static_cast<uint8_t>(0x90u + i);
    }
    identity.runtimeBuild.executableSize = 704512u;
    identity.runtimeBuild.peTimestamp = 0x3A19A8A9u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.simulationContent.entryCount = 16u;
    identity.simulationContent.requiredEntryCount = 12u;
    identity.stateSchemaId = 0x1011121314151617ull;
    identity.modSimulationId = 0x2021222324252627ull;
    identity.timingProfileId = 0x3031323334353637ull;
    return identity;
}

CoreConfig hostConfig() {
    CoreConfig config{};
    config.localRole = PeerRole::Host;
    config.localPlayer = 0;
    config.compatibilityId = kCompatibility;
    config.compatibilityIdentity = compatibilityIdentity();
    config.sessionId = kSession;
    config.cookie = cookie();
    config.expectedPeer = Endpoint{ kClientIp, 0 };
    config.learnPeerPort = true;
    return config;
}

CoreConfig clientConfig(uint16_t hostPort = 7000) {
    CoreConfig config{};
    config.localRole = PeerRole::Client;
    config.localPlayer = 1;
    config.compatibilityId = kCompatibility;
    config.compatibilityIdentity = compatibilityIdentity();
    config.sessionId = kSession;
    config.cookie = cookie();
    config.expectedPeer = Endpoint{ kHostIp, hostPort };
    return config;
}

std::vector<uint8_t> makeHello(const SessionCookie& session,
                               uint32_t sequence,
                               uint64_t compatibility = kCompatibility,
                               PeerRole role = PeerRole::Client,
                               uint8_t player = 1,
                               uint64_t wireSession = kSession,
                               qnetplay::compat::CompatibilityIdentityWire
                                   identity = compatibilityIdentity()) {
    HelloPayload hello{ compatibility, role, player, identity };
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    QNP_CHECK(encodePayload(hello, payload));
    QNP_CHECK(encodeDatagram(MessageType::Hello, wireSession, session, sequence,
                             payload, packet));
    return packet;
}

template<typename Payload>
std::vector<uint8_t> makeControl(MessageType type,
                                 uint32_t sequence,
                                 const Payload& control) {
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    QNP_CHECK(encodePayload(control, payload));
    QNP_CHECK(encodeDatagram(type, kSession, cookie(), sequence,
                             payload, packet));
    return packet;
}

std::vector<uint8_t> makeInputs(const SessionCookie& session,
                                uint32_t sequence,
                                uint32_t newestFrame,
                                uint32_t ackThrough,
                                uint32_t validMask,
                                const std::array<uint16_t, kInputWindowSize>& values,
                                uint32_t simFrontier = kNoFrame,
                                PressureReport pressure = {}) {
    InputsPayload inputs{};
    inputs.newestFrame = newestFrame;
    inputs.ackThrough = ackThrough;
    inputs.validMask = validMask;
    inputs.inputs = values;
    inputs.simFrontier = simFrontier;
    inputs.pressure = pressure;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    QNP_CHECK(encodePayload(inputs, payload));
    QNP_CHECK(encodeDatagram(MessageType::Inputs, kSession, session, sequence,
                             payload, packet));
    return packet;
}

std::vector<uint8_t> makeAckOnly(const SessionCookie& session,
                                 uint32_t sequence,
                                 uint32_t ackThrough) {
    std::array<uint16_t, kInputWindowSize> empty{};
    return makeInputs(session, sequence, 0, ackThrough, 0, empty);
}

const OutboundDatagram* findType(const std::vector<OutboundDatagram>& packets,
                                 MessageType type) {
    for (const OutboundDatagram& packet : packets)
        if (packet.type == type) return &packet;
    return nullptr;
}

bool establish(NetplayCore& host,
               NetplayCore& client,
               const Endpoint& hostEndpoint,
               const Endpoint& clientEndpoint,
               uint64_t nowMs) {
    const auto clientPackets = client.pollOutbound(nowMs);
    const OutboundDatagram* hello =
        findType(clientPackets, MessageType::Hello);
    if (!hello) return false;
    if (host.onDatagram(clientEndpoint, hello->bytes.data(),
                        hello->bytes.size(), nowMs)
        != IngressResult::Accepted) {
        return false;
    }

    const auto hostPackets = host.pollOutbound(nowMs);
    const OutboundDatagram* ack =
        findType(hostPackets, MessageType::HelloAck);
    if (!ack) return false;
    if (client.onDatagram(hostEndpoint, ack->bytes.data(),
                          ack->bytes.size(), nowMs)
        != IngressResult::Accepted) {
        return false;
    }
    return host.isEstablished() && client.isEstablished();
}

InputsPayload decodeInputs(const OutboundDatagram& packet,
                           const SessionCookie& session) {
    Datagram datagram{};
    InputsPayload inputs{};
    QNP_CHECK(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                             kSession, session, datagram)
              == DecodeError::None);
    QNP_CHECK(datagram.header.type == MessageType::Inputs);
    QNP_CHECK(decodePayload(datagram.payload, inputs));
    return inputs;
}

template<typename Payload>
Payload decodeControl(const OutboundDatagram& packet, MessageType type) {
    Datagram datagram{};
    Payload payload{};
    QNP_CHECK(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                             kSession, cookie(), datagram)
              == DecodeError::None);
    QNP_CHECK(datagram.header.type == type);
    QNP_CHECK(decodePayload(datagram.payload, payload));
    return payload;
}

}

QNP_TEST(CoreCarriesF1ReturnOnReliableSessionEventChannel) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(
        host, client, hostEndpoint, clientEndpoint, 10));

    SessionEventPayload requested{};
    requested.stage = SessionEventStage::Request;
    requested.kind = 4;
    requested.eventId = 77;
    requested.value = 1200;
    QNP_REQUIRE(host.submitSessionEvent(requested));
    QNP_REQUIRE(host.submitLocalInput(0, 0x0101));

    const auto first = host.pollOutbound(20);
    QNP_REQUIRE(findType(first, MessageType::SessionEvent) != nullptr);
    QNP_REQUIRE(findType(first, MessageType::Inputs) != nullptr);
    const auto retry = host.pollOutbound(1020);
    const OutboundDatagram* event =
        findType(retry, MessageType::SessionEvent);
    QNP_REQUIRE(event != nullptr);
    QNP_REQUIRE(findType(retry, MessageType::Inputs) != nullptr);
    const SessionEventPayload encoded =
        decodeControl<SessionEventPayload>(
            *event, MessageType::SessionEvent);
    QNP_CHECK(encoded.kind == 4);
    QNP_CHECK(encoded.eventId == 77u);

    QNP_CHECK(client.onDatagram(
                  hostEndpoint, event->bytes.data(), event->bytes.size(), 1020)
              == IngressResult::Accepted);
    SessionEventPayload received{};
    QNP_REQUIRE(client.takeSessionEvent(received));
    QNP_CHECK(received.kind == 4);
    QNP_CHECK(received.eventId == 77u);
    QNP_CHECK(received.value == 1200u);

    host.clearSessionEvent();
    QNP_CHECK(findType(host.pollOutbound(2020), MessageType::SessionEvent)
              == nullptr);
}

QNP_TEST(CoreSendsATerminalCommitAckOnceAndNeverOnTheCadence) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(
        host, client, hostEndpoint, clientEndpoint, 10));

    SessionEventPayload ack{};
    ack.stage = SessionEventStage::CommitAck;
    ack.kind = 4;
    ack.eventId = 9;
    QNP_REQUIRE(client.submitSessionEvent(ack));
    QNP_REQUIRE(findType(client.pollOutbound(20),
                         MessageType::SessionEvent) != nullptr);
    QNP_CHECK(findType(client.pollOutbound(1020),
                       MessageType::SessionEvent) == nullptr);
    QNP_CHECK(findType(client.pollOutbound(5020),
                       MessageType::SessionEvent) == nullptr);

    SessionEventPayload quiesce{};
    quiesce.stage = SessionEventStage::Quiesce;
    quiesce.kind = 4;
    quiesce.eventId = 10;
    QNP_REQUIRE(client.submitSessionEvent(quiesce));
    QNP_REQUIRE(findType(client.pollOutbound(5040),
                         MessageType::SessionEvent) != nullptr);
    QNP_REQUIRE(findType(client.pollOutbound(6040),
                         MessageType::SessionEvent) != nullptr);
}

QNP_TEST(CoreRejectsBeforePeerLockWithoutMutatingState) {
    NetplayCore host(hostConfig());
    const Endpoint expectedClient{ kClientIp, 7100 };
    const Endpoint wrongIp{ 0x0A000099u, 7100 };

    const uint8_t tiny[] = { 1, 2, 3 };
    QNP_CHECK(host.onDatagram(expectedClient, tiny, sizeof(tiny), 100)
              == IngressResult::TooShort);
    QNP_CHECK(!host.peerLocked());
    QNP_CHECK(host.lastValidReceiveMs() == 0);

    SessionCookie wrongCookie = cookie();
    wrongCookie.low ^= 1;
    const auto wrongSession = makeHello(wrongCookie, 1);
    QNP_CHECK(host.onDatagram(expectedClient, wrongSession.data(),
                              wrongSession.size(), 101)
              == IngressResult::SessionMismatch);
    QNP_CHECK(!host.peerLocked());
    QNP_CHECK(host.lastValidReceiveMs() == 0);

    const auto hello = makeHello(cookie(), 2);
    QNP_REQUIRE(!hello.empty());
    auto corruptFromWrongSource = hello;
    corruptFromWrongSource.back() ^= 0x80;
    QNP_CHECK(host.onDatagram(wrongIp, corruptFromWrongSource.data(),
                              corruptFromWrongSource.size(), 102)
              == IngressResult::BadCrc);
    QNP_CHECK(!host.peerLocked());

    std::array<uint16_t, kInputWindowSize> values{};
    values[0] = 7;
    const auto inputs = makeInputs(cookie(), 3, 0, kNoFrame, 1, values);
    QNP_CHECK(host.onDatagram(wrongIp, inputs.data(), inputs.size(), 102)
              == IngressResult::WrongSource);
    QNP_CHECK(!host.peerLocked());
    QNP_CHECK(host.onDatagram(expectedClient, inputs.data(), inputs.size(), 103)
              == IngressResult::WrongSource);
    QNP_CHECK(!host.peerLocked());

    const auto incompatible =
        makeHello(cookie(), 4, kCompatibility + 1);
    QNP_CHECK(host.onDatagram(expectedClient, incompatible.data(),
                              incompatible.size(), 104)
              == IngressResult::CompatibilityMismatch);
    QNP_CHECK(!host.peerLocked());
    QNP_CHECK(host.lastValidReceiveMs() == 0);

    QNP_CHECK(host.onDatagram(expectedClient, hello.data(), hello.size(), 105)
              == IngressResult::Accepted);
    QNP_CHECK(host.peerLocked());
    QNP_CHECK(host.peerEndpoint() == expectedClient);
    QNP_CHECK(host.lastValidReceiveMs() == 105);

    const Endpoint wrongPort{ kClientIp, 7101 };
    QNP_CHECK(host.onDatagram(wrongPort, inputs.data(), inputs.size(), 106)
              == IngressResult::WrongSource);
    QNP_CHECK(host.lastValidReceiveMs() == 105);
    uint16_t input = 0;
    QNP_CHECK(!host.getRemoteInput(0, input));
}

QNP_TEST(CoreRejectsCrossProfileCompatibilityBeforeEstablishment) {
    NetplayCore host(hostConfig());
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    const auto decemberHello = makeHello(
        cookie(), 1, kCompatibility, PeerRole::Client, 1, kSession,
        compatibilityIdentity("qoh99-december-candidate"));
    QNP_REQUIRE(!decemberHello.empty());

    QNP_CHECK(host.onDatagram(
                  clientEndpoint, decemberHello.data(),
                  decemberHello.size(), 100)
              == IngressResult::CompatibilityMismatch);
    QNP_CHECK(!host.peerLocked());
    QNP_CHECK(!host.isEstablished());
    QNP_CHECK(host.lastValidReceiveMs() == 0u);
    QNP_CHECK(host.lastCompatibilityIdentityMismatch()
              == qnetplay::compat::IdentityMismatch::ProfileId);
    QNP_CHECK(std::string(host.lastCompatibilityIdentityMismatchReason())
              == "runtime profile mismatch");
}

QNP_TEST(CoreRejectsSameProfileDifferentContentBeforeEstablishment) {
    NetplayCore host(hostConfig());
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    qnetplay::compat::CompatibilityIdentityWire changed =
        compatibilityIdentity();
    changed.simulationContent.simulationRootSha256[7] ^= 0x80u;
    const auto changedContentHello = makeHello(
        cookie(), 1, kCompatibility, PeerRole::Client, 1, kSession,
        changed);
    QNP_REQUIRE(!changedContentHello.empty());

    QNP_CHECK(host.onDatagram(
                  clientEndpoint, changedContentHello.data(),
                  changedContentHello.size(), 100)
              == IngressResult::CompatibilityMismatch);
    QNP_CHECK(!host.peerLocked());
    QNP_CHECK(!host.isEstablished());
    QNP_CHECK(host.lastValidReceiveMs() == 0u);
    QNP_CHECK(host.lastCompatibilityIdentityMismatch()
              == qnetplay::compat::IdentityMismatch::SimulationRoot);
    QNP_CHECK(std::string(host.lastCompatibilityIdentityMismatchReason())
              == "simulation-content root mismatch");
}

QNP_TEST(CoreAdmitsForensicPeTimestampDifferences) {
    CoreConfig hostConfiguration = hostConfig();
    CoreConfig clientConfiguration = clientConfig();
    clientConfiguration.compatibilityIdentity.runtimeBuild.peTimestamp = 0u;

    NetplayCore host(hostConfiguration);
    NetplayCore client(clientConfiguration);
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_CHECK(establish(host, client, hostEndpoint, clientEndpoint, 100u));
    QNP_CHECK(host.lastCompatibilityIdentityMismatch()
              == qnetplay::compat::IdentityMismatch::None);
    QNP_CHECK(client.lastCompatibilityIdentityMismatch()
              == qnetplay::compat::IdentityMismatch::None);
}

QNP_TEST(CoreUnlockedHostAdoptsAuthenticatedHelloFromAnyRoute) {
    NetplayCore host(hostConfig());
    const Endpoint lanClient{ kClientIp + 0x10u, 55881 };
    const auto hello = makeHello(cookie(), 2);
    QNP_REQUIRE(!hello.empty());
    QNP_CHECK(host.onDatagram(lanClient, hello.data(), hello.size(), 100)
              == IngressResult::Accepted);
    QNP_CHECK(host.peerLocked());
    const Endpoint controlClient{ kClientIp, 7100 };
    QNP_CHECK(host.onDatagram(controlClient, hello.data(), hello.size(), 101)
              == IngressResult::WrongSource);
}

QNP_TEST(CoreHandlesReorderDuplicatesAndConflictsExactly) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 10));
    host.setSimulationFrame(0);

    std::array<uint16_t, kInputWindowSize> values{};
    values[0] = 0x0106;
    auto packet = makeInputs(cookie(), 10, 2, kNoFrame, 1, values);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 20)
              == IngressResult::Accepted);
    QNP_CHECK(host.remoteAckThrough() == kNoFrame);

    values = {};
    values[0] = 10;
    packet = makeInputs(cookie(), 11, 0, kNoFrame, 1, values);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 21)
              == IngressResult::Accepted);
    QNP_CHECK(host.remoteAckThrough() == 0);

    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 22)
              == IngressResult::Accepted);
    QNP_CHECK(host.remoteAckThrough() == 0);

    values[0] = 0x0203;
    packet = makeInputs(cookie(), 12, 0, kNoFrame, 1, values);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 23)
              == IngressResult::ConflictingInput);
    QNP_CHECK(host.lastValidReceiveMs() == 22);
    uint16_t input = 0;
    QNP_CHECK(host.getRemoteInput(0, input) && input == 10);

    values[0] = 11;
    packet = makeInputs(cookie(), 13, 1, kNoFrame, 1, values);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 24)
              == IngressResult::Accepted);
    QNP_CHECK(host.remoteAckThrough() == 2);
    QNP_CHECK(host.getRemoteInput(1, input) && input == 11);
    QNP_CHECK(host.getRemoteInput(2, input) && input == 0x0106);

    values[0] = 0x0F0F;
    packet = makeInputs(cookie(), 14, 121, kNoFrame, 1, values);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 25)
              == IngressResult::FrameOutOfRange);
    QNP_CHECK(host.lastValidReceiveMs() == 24);
    QNP_CHECK(!host.getRemoteInput(121, input));
}

QNP_TEST(CoreRejectsMalformedPayloadAndFutureAckAtomically) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 10));
    QNP_CHECK(host.submitLocalInput(0, 0x0204));

    std::vector<uint8_t> malformed(payloadSize(MessageType::Inputs), 0);
    malformed[8] = 0x00;
    malformed[9] = 0x00;
    malformed[10] = 0x00;
    malformed[11] = 0x40;
    std::vector<uint8_t> packet;
    QNP_CHECK(encodeDatagram(MessageType::Inputs, kSession, cookie(), 20,
                             malformed, packet));
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 20)
              == IngressResult::BadPayload);
    QNP_CHECK(host.peerAckThrough() == kNoFrame);
    QNP_CHECK(host.lastValidReceiveMs() == 10);

    packet = makeAckOnly(cookie(), 21, 5);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 21)
              == IngressResult::FutureAck);
    QNP_CHECK(host.peerAckThrough() == kNoFrame);
    QNP_CHECK(host.lastValidReceiveMs() == 10);
}

QNP_TEST(CoreResendsInputsOnPumpTicksWhileSimulationStalls) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    host.setSimulationFrame(0);
    client.setSimulationFrame(0);
    QNP_CHECK(host.submitLocalInput(0, 0x0101));
    QNP_CHECK(client.submitLocalInput(0, 0x0202));

    auto hostPackets = host.pollOutbound(10);
    auto clientPackets = client.pollOutbound(10);
    QNP_CHECK(findType(hostPackets, MessageType::Inputs) != nullptr);
    QNP_CHECK(findType(clientPackets, MessageType::Inputs) != nullptr);
    QNP_CHECK(host.pollOutbound(59).empty());
    QNP_CHECK(client.pollOutbound(59).empty());

    hostPackets = host.pollOutbound(60);
    clientPackets = client.pollOutbound(60);
    const OutboundDatagram* fromHost =
        findType(hostPackets, MessageType::Inputs);
    const OutboundDatagram* fromClient =
        findType(clientPackets, MessageType::Inputs);
    QNP_REQUIRE(fromHost != nullptr && fromClient != nullptr);
    QNP_CHECK(client.onDatagram(hostEndpoint, fromHost->bytes.data(),
                                fromHost->bytes.size(), 60)
              == IngressResult::Accepted);
    QNP_CHECK(host.onDatagram(clientEndpoint, fromClient->bytes.data(),
                              fromClient->bytes.size(), 60)
              == IngressResult::Accepted);

    uint16_t input = 0;
    QNP_CHECK(client.getRemoteInput(0, input) && input == 0x0101);
    QNP_CHECK(host.getRemoteInput(0, input) && input == 0x0202);

    hostPackets = host.pollOutbound(60);
    clientPackets = client.pollOutbound(60);
    fromHost = findType(hostPackets, MessageType::Inputs);
    fromClient = findType(clientPackets, MessageType::Inputs);
    QNP_REQUIRE(fromHost != nullptr && fromClient != nullptr);
    QNP_CHECK(client.onDatagram(hostEndpoint, fromHost->bytes.data(),
                                fromHost->bytes.size(), 61)
              == IngressResult::Accepted);
    QNP_CHECK(host.onDatagram(clientEndpoint, fromClient->bytes.data(),
                              fromClient->bytes.size(), 61)
              == IngressResult::Accepted);
    QNP_CHECK(host.peerAckThrough() == 0);
    QNP_CHECK(client.peerAckThrough() == 0);
    QNP_CHECK(host.pollOutbound(61).empty());
    QNP_CHECK(client.pollOutbound(61).empty());

    QNP_CHECK(host.submitLocalInput(1, 0x0102));
    QNP_CHECK(client.submitLocalInput(1, 0x0203));
    QNP_CHECK(findType(host.pollOutbound(100), MessageType::Inputs) != nullptr);
    QNP_CHECK(findType(client.pollOutbound(100), MessageType::Inputs) != nullptr);
    QNP_CHECK(host.pollOutbound(149).empty());
    QNP_CHECK(client.pollOutbound(149).empty());
    QNP_CHECK(findType(host.pollOutbound(150), MessageType::Inputs) != nullptr);
    QNP_CHECK(findType(client.pollOutbound(150), MessageType::Inputs) != nullptr);
}

QNP_TEST(CoreRateLimitsDuplicateInputAcksButRecoversLostAck) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    host.setSimulationFrame(0);
    client.setSimulationFrame(0);
    QNP_CHECK(host.submitLocalInput(0, 0x0204));

    auto packets = host.pollOutbound(10);
    const OutboundDatagram* data =
        findType(packets, MessageType::Inputs);
    QNP_REQUIRE(data != nullptr);
    const std::vector<uint8_t> firstData = data->bytes;
    QNP_CHECK(client.onDatagram(hostEndpoint, firstData.data(),
                                firstData.size(), 10)
              == IngressResult::Accepted);

    packets = client.pollOutbound(10);
    QNP_REQUIRE(findType(packets, MessageType::Inputs) != nullptr);

    QNP_CHECK(client.onDatagram(hostEndpoint, firstData.data(),
                                firstData.size(), 11)
              == IngressResult::Accepted);
    QNP_CHECK(client.pollOutbound(11).empty());

    packets = host.pollOutbound(60);
    data = findType(packets, MessageType::Inputs);
    QNP_REQUIRE(data != nullptr);
    QNP_CHECK(client.onDatagram(hostEndpoint, data->bytes.data(),
                                data->bytes.size(), 60)
              == IngressResult::Accepted);

    packets = client.pollOutbound(60);
    const OutboundDatagram* recoveryAck =
        findType(packets, MessageType::Inputs);
    QNP_REQUIRE(recoveryAck != nullptr);
    QNP_CHECK(host.onDatagram(clientEndpoint,
                              recoveryAck->bytes.data(),
                              recoveryAck->bytes.size(), 61)
              == IngressResult::Accepted);
    QNP_CHECK(host.peerAckThrough() == 0);
    QNP_CHECK(host.pollOutbound(110).empty());
}

QNP_TEST(CoreSteadyInputsDoNotAmplifyBeyondOneAckWave) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    size_t inputPackets = 0;
    constexpr uint32_t frameCount = 120;
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        const uint64_t nowMs = 10u + static_cast<uint64_t>(frame) * 17u;
        host.setSimulationFrame(frame);
        client.setSimulationFrame(frame);
        QNP_REQUIRE(host.submitLocalInput(
            frame, static_cast<uint16_t>(0x1000u + frame)));
        QNP_REQUIRE(client.submitLocalInput(
            frame, static_cast<uint16_t>(0x2000u + frame)));

        auto hostPackets = host.pollOutbound(nowMs);
        auto clientPackets = client.pollOutbound(nowMs);
        for (const OutboundDatagram& packet : hostPackets) {
            if (packet.type == MessageType::Inputs) ++inputPackets;
            QNP_CHECK(client.onDatagram(
                          hostEndpoint, packet.bytes.data(),
                          packet.bytes.size(), nowMs)
                      == IngressResult::Accepted);
        }
        for (const OutboundDatagram& packet : clientPackets) {
            if (packet.type == MessageType::Inputs) ++inputPackets;
            QNP_CHECK(host.onDatagram(
                          clientEndpoint, packet.bytes.data(),
                          packet.bytes.size(), nowMs)
                      == IngressResult::Accepted);
        }

        hostPackets = host.pollOutbound(nowMs);
        clientPackets = client.pollOutbound(nowMs);
        for (const OutboundDatagram& packet : hostPackets) {
            if (packet.type == MessageType::Inputs) ++inputPackets;
            QNP_CHECK(client.onDatagram(
                          hostEndpoint, packet.bytes.data(),
                          packet.bytes.size(), nowMs)
                      == IngressResult::Accepted);
        }
        for (const OutboundDatagram& packet : clientPackets) {
            if (packet.type == MessageType::Inputs) ++inputPackets;
            QNP_CHECK(host.onDatagram(
                          clientEndpoint, packet.bytes.data(),
                          packet.bytes.size(), nowMs)
                      == IngressResult::Accepted);
        }
        QNP_CHECK(findType(host.pollOutbound(nowMs),
                           MessageType::Inputs) == nullptr);
        QNP_CHECK(findType(client.pollOutbound(nowMs),
                           MessageType::Inputs) == nullptr);
    }

    QNP_CHECK(inputPackets <= static_cast<size_t>(frameCount) * 4u);
}

QNP_TEST(CoreAnchorsThirtyFrameWindowAtOldestUnacked) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    const auto canonWord = [](uint32_t n) {
        return static_cast<uint16_t>((n & 0x0Fu) | ((n & 0xF0u) << 4));
    };
    for (uint32_t frame = 0; frame < 40; ++frame)
        QNP_CHECK(host.submitLocalInput(frame, canonWord(100 + frame)));

    auto packets = host.pollOutbound(10);
    const OutboundDatagram* first = findType(packets, MessageType::Inputs);
    QNP_REQUIRE(first != nullptr);
    const InputsPayload firstWindow = decodeInputs(*first, cookie());
    QNP_CHECK(firstWindow.newestFrame == 29);
    QNP_CHECK(firstWindow.validMask == 0x3FFFFFFFu);
    QNP_CHECK(firstWindow.inputs[0] == canonWord(129));
    QNP_CHECK(firstWindow.inputs[29] == canonWord(100));

    const auto ack = makeAckOnly(cookie(), 90, 29);
    QNP_CHECK(host.onDatagram(clientEndpoint, ack.data(), ack.size(), 11)
              == IngressResult::Accepted);
    packets = host.pollOutbound(11);
    const OutboundDatagram* second = findType(packets, MessageType::Inputs);
    QNP_REQUIRE(second != nullptr);
    const InputsPayload secondWindow = decodeInputs(*second, cookie());
    QNP_CHECK(secondWindow.newestFrame == 39);
    QNP_CHECK(secondWindow.validMask == 0x000003FFu);
    QNP_CHECK(secondWindow.inputs[0] == canonWord(139));
    QNP_CHECK(secondWindow.inputs[9] == canonWord(130));
}

QNP_TEST(CoreSendsPlayerInputBeforeEveryOtherEstablishedMessage) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    const ReadyPayload ready{ 11, 12, 1, 0, 0x1111222233334444ull };
    QNP_CHECK(host.submitLocalReady(ready));
    SyncHashPayload hash{};
    hash.tag = SyncHashTag{1, 2, 30};
    hash.rngGameplay = 31;
    hash.hp0 = 900;
    hash.hp1 = 850;
    hash.gameplayHash = 0x123456789ABCDEF0ull;
    QNP_CHECK(host.submitLocalSyncHash(hash));
    QNP_CHECK(host.submitLocalInput(0, 0x0101));

    const auto packets = host.pollOutbound(5000);
    QNP_REQUIRE(!packets.empty());
    QNP_CHECK(packets.front().type == MessageType::Inputs);

    int hashAt = -1, pingAt = -1;
    for (size_t i = 0; i < packets.size(); ++i) {
        if (packets[i].type == MessageType::SyncHash) hashAt = static_cast<int>(i);
        if (packets[i].type == MessageType::Ping) pingAt = static_cast<int>(i);
    }
    if (hashAt >= 0 && pingAt >= 0) QNP_CHECK(hashAt < pingAt);
}

QNP_TEST(CorePressureReportReachesThePeerAndSurvivesAStalledSender) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    PressureReport report{};
    report.inputProducedThrough = 40;
    report.confirmedFrontier = 18;
    report.predictionDepth = 9;
    report.correctionDebt = 3;
    report.runState = 4;
    report.holdGeneration = 2;
    host.setPressureReport(report);
    host.setSimulationFrame(25);
    QNP_CHECK(host.submitLocalInput(0, 0x0101));

    const auto packets = host.pollOutbound(10);
    const OutboundDatagram* inputs = findType(packets, MessageType::Inputs);
    QNP_REQUIRE(inputs != nullptr);
    QNP_CHECK(client.onDatagram(hostEndpoint, inputs->bytes.data(),
                                inputs->bytes.size(), 11)
              == IngressResult::Accepted);

    PressureReport seen{};
    uint64_t ageMs = 0;
    QNP_REQUIRE(client.peerPressureReport(seen, ageMs, 11));
    QNP_CHECK(seen.inputProducedThrough == 40);
    QNP_CHECK(seen.confirmedFrontier == 18);
    QNP_CHECK(seen.predictionDepth == 9);
    QNP_CHECK(seen.correctionDebt == 3);
    QNP_CHECK(seen.runState == 4);
    QNP_CHECK(seen.holdGeneration == 2);
    QNP_CHECK(ageMs == 0);

    report.inputProducedThrough = 52;
    report.runState = 2;
    host.setPressureReport(report);
    host.setSimulationFrame(25);
    QNP_CHECK(host.submitLocalInput(1, 0x0102));
    const auto second = host.pollOutbound(20);
    const OutboundDatagram* again = findType(second, MessageType::Inputs);
    QNP_REQUIRE(again != nullptr);
    QNP_CHECK(client.onDatagram(hostEndpoint, again->bytes.data(),
                                again->bytes.size(), 25)
              == IngressResult::Accepted);
    QNP_REQUIRE(client.peerPressureReport(seen, ageMs, 30));
    QNP_CHECK(seen.inputProducedThrough == 52);
    QNP_CHECK(seen.runState == 2);
    QNP_CHECK(ageMs == 5);
}

QNP_TEST(CorePressureReportRejectsOlderFrontierButRefreshesEqualHold) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    std::array<uint16_t, kInputWindowSize> values{};
    PressureReport report{};
    report.predictionDepth = 5;
    report.runState = 2;
    values[0] = 0x0101;
    auto packet = makeInputs(cookie(), 100, 0, kNoFrame, 1, values,
                             5, report);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 20)
              == IngressResult::Accepted);

    PressureReport seen{};
    uint64_t ageMs = 0;
    QNP_REQUIRE(host.peerPressureReport(seen, ageMs, 25));
    QNP_CHECK(seen.predictionDepth == 5);
    QNP_CHECK(ageMs == 5);

    report.predictionDepth = 2;
    values[0] = 0x0102;
    packet = makeInputs(cookie(), 101, 1, kNoFrame, 1, values,
                        3, report);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 30)
              == IngressResult::Accepted);
    QNP_REQUIRE(host.peerPressureReport(seen, ageMs, 30));
    QNP_CHECK(seen.predictionDepth == 5);
    QNP_CHECK(ageMs == 10);

    report.predictionDepth = 7;
    values[0] = 0x0103;
    packet = makeInputs(cookie(), 102, 2, kNoFrame, 1, values,
                        5, report);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 40)
              == IngressResult::Accepted);
    QNP_REQUIRE(host.peerPressureReport(seen, ageMs, 41));
    QNP_CHECK(seen.predictionDepth == 7);
    QNP_CHECK(ageMs == 1);

    report.predictionDepth = 1;
    values[0] = 0x0104;
    packet = makeInputs(cookie(), 99, 3, kNoFrame, 1, values,
                        5, report);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 50)
              == IngressResult::Accepted);
    QNP_REQUIRE(host.peerPressureReport(seen, ageMs, 50));
    QNP_CHECK(seen.predictionDepth == 7);
    QNP_CHECK(ageMs == 10);
}

QNP_TEST(CoreRequiresSequentialImmutableInputsAndRejectsPrematureAcks) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 10));

    QNP_CHECK(!host.submitLocalInput(1, 0x0101));
    QNP_CHECK(host.submitLocalInput(0, 0x0100));
    QNP_CHECK(host.submitLocalInput(0, 0x0100));
    QNP_CHECK(!host.submitLocalInput(0, 0xFFFF));
    QNP_CHECK(!host.submitLocalInput(2, 0x0102));
    QNP_CHECK(host.submitLocalInput(1, 0x0101));
    QNP_CHECK(host.submitLocalInput(0, 0x0100));

    auto ack = makeAckOnly(cookie(), 100, 0);
    QNP_CHECK(host.onDatagram(clientEndpoint, ack.data(), ack.size(), 11)
              == IngressResult::FutureAck);
    QNP_CHECK(host.peerAckThrough() == kNoFrame);
    QNP_CHECK(host.lastValidReceiveMs() == 10);

    const auto packets = host.pollOutbound(20);
    const OutboundDatagram* inputs =
        findType(packets, MessageType::Inputs);
    QNP_REQUIRE(inputs != nullptr);
    const InputsPayload window = decodeInputs(*inputs, cookie());
    QNP_CHECK(window.newestFrame == 1 && window.validMask == 3u);

    ack = makeAckOnly(cookie(), 101, 1);
    QNP_CHECK(host.onDatagram(clientEndpoint, ack.data(), ack.size(), 21)
              == IngressResult::Accepted);
    QNP_CHECK(host.peerAckThrough() == 1);

    ack = makeAckOnly(cookie(), 102, 0);
    QNP_CHECK(host.onDatagram(clientEndpoint, ack.data(), ack.size(), 22)
              == IngressResult::Accepted);
    QNP_CHECK(host.peerAckThrough() == 1);
}

QNP_TEST(CoreBindsEveryPacketToExplicitSessionAndCookie) {
    NetplayCore host(hostConfig());
    const Endpoint clientEndpoint{ kClientIp, 7100 };

    const auto wrongSession =
        makeHello(cookie(), 1, kCompatibility, PeerRole::Client, 1,
                  kSession + 1);
    QNP_CHECK(host.onDatagram(clientEndpoint, wrongSession.data(),
                              wrongSession.size(), 10)
              == IngressResult::SessionMismatch);
    QNP_CHECK(!host.peerLocked());
    QNP_CHECK(host.lastValidReceiveMs() == 0);

    CoreConfig invalid = hostConfig();
    invalid.sessionId = 0;
    NetplayCore invalidCore(invalid);
    QNP_CHECK(invalidCore.hasTerminalError());
    QNP_CHECK(invalidCore.terminalState().code
              == TerminalErrorCode::InvalidConfiguration);
    QNP_CHECK(invalidCore.pollOutbound(100).empty());

    invalid = hostConfig();
    invalid.compatibilityIdentity = {};
    NetplayCore missingIdentityCore(invalid);
    QNP_CHECK(missingIdentityCore.hasTerminalError());
    QNP_CHECK(missingIdentityCore.terminalState().code
              == TerminalErrorCode::InvalidConfiguration);
}

QNP_TEST(CoreReadyHandshakeRetransmitsAndRejectsPrematureOrConflictingAck) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    const ReadyPayload hostReady{
        11, 12, 1, 0, 0x1111222233334444ull
    };
    const ReadyPayload clientReady{
        21, 22, 0, 0, 0x5555666677778888ull
    };
    QNP_CHECK(host.submitLocalReady(hostReady));
    QNP_CHECK(client.submitLocalReady(clientReady));
    QNP_CHECK(host.submitLocalReady(hostReady));
    ReadyPayload changed = hostReady;
    changed.selector ^= 1u;
    QNP_CHECK(!host.submitLocalReady(changed));
    changed = hostReady;
    changed.timerMode = 1u;
    QNP_CHECK(!host.submitLocalReady(changed));
    changed = hostReady;
    changed.aiDifficulty = 1u;
    QNP_CHECK(!host.submitLocalReady(changed));
    changed = hostReady;
    changed.roundsToWinMinusOne = 1u;
    QNP_CHECK(!host.submitLocalReady(changed));

    ReadyPayload forged = clientReady;
    forged.flags = kReadyAcknowledgesPeer;
    auto packet = makeControl(MessageType::Ready, 10, forged);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 1)
              == IngressResult::InvalidControl);
    ReadyPayload remote{};
    QNP_CHECK(!host.getRemoteReady(remote));
    QNP_CHECK(host.lastValidReceiveMs() == 0);

    auto packets = host.pollOutbound(20);
    const OutboundDatagram* fromHost =
        findType(packets, MessageType::Ready);
    QNP_REQUIRE(fromHost != nullptr);
    ReadyPayload sent =
        decodeControl<ReadyPayload>(*fromHost, MessageType::Ready);
    QNP_CHECK(sent.flags == 0);
    QNP_CHECK(client.onDatagram(hostEndpoint, fromHost->bytes.data(),
                                fromHost->bytes.size(), 20)
              == IngressResult::Accepted);

    packets = client.pollOutbound(20);
    const OutboundDatagram* fromClient =
        findType(packets, MessageType::Ready);
    QNP_REQUIRE(fromClient != nullptr);
    sent = decodeControl<ReadyPayload>(*fromClient, MessageType::Ready);
    QNP_CHECK(sent.flags == kReadyAcknowledgesPeer);
    QNP_CHECK(host.onDatagram(clientEndpoint, fromClient->bytes.data(),
                              fromClient->bytes.size(), 21)
              == IngressResult::Accepted);
    QNP_CHECK(host.readyComplete());
    QNP_CHECK(!client.readyComplete());

    packets = host.pollOutbound(21);
    fromHost = findType(packets, MessageType::Ready);
    QNP_REQUIRE(fromHost != nullptr);
    sent = decodeControl<ReadyPayload>(*fromHost, MessageType::Ready);
    QNP_CHECK(sent.flags == kReadyAcknowledgesPeer);
    QNP_CHECK(findType(host.pollOutbound(120), MessageType::Ready) == nullptr);
    packets = host.pollOutbound(121);
    fromHost = findType(packets, MessageType::Ready);
    QNP_REQUIRE(fromHost != nullptr);
    QNP_CHECK(client.onDatagram(hostEndpoint, fromHost->bytes.data(),
                                fromHost->bytes.size(), 121)
              == IngressResult::Accepted);
    QNP_CHECK(client.readyComplete());

    ReadyPayload conflict = clientReady;
    conflict.seedGameplay ^= 1u;
    conflict.flags = kReadyAcknowledgesPeer;
    packet = makeControl(MessageType::Ready, 11, conflict);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 122)
              == IngressResult::ConflictingControl);
    QNP_CHECK(host.lastValidReceiveMs() == 21);
}

QNP_TEST(CoreSyncHashIsImmutableOrderedAndReliablyAcknowledged) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    SyncHashPayload hash{};
    hash.tag = SyncHashTag{1, 2, 30};
    hash.rngGameplay = 31;
    hash.rngCosmetic = 32;
    hash.rngSelector = 33;
    hash.hp0 = 900;
    hash.hp1 = 850;
    hash.gameplayHash = 0x123456789ABCDEF0ull;
    QNP_CHECK(host.submitLocalSyncHashDetailed(hash)
              == SyncHashSubmitResult::Accepted);
    QNP_CHECK(host.submitLocalSyncHashDetailed(hash)
              == SyncHashSubmitResult::ExactDuplicate);
    SyncHashPayload conflict = hash;
    conflict.gameplayHash ^= 1u;
    QNP_CHECK(host.submitLocalSyncHashDetailed(conflict)
              == SyncHashSubmitResult::OutOfOrder);

    const SyncHashAckPayload prematureAck{hash.tag};
    auto packet =
        makeControl(MessageType::SyncHashAck, 40, prematureAck);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 1)
              == IngressResult::FutureAck);
    QNP_CHECK(host.lastValidReceiveMs() == 0);

    auto packets = host.pollOutbound(20);
    const OutboundDatagram* publication =
        findType(packets, MessageType::SyncHash);
    QNP_REQUIRE(publication != nullptr);
    QNP_CHECK(client.onDatagram(hostEndpoint, publication->bytes.data(),
                                publication->bytes.size(), 20)
              == IngressResult::Accepted);
    SyncHashPayload received{};
    QNP_CHECK(client.takeRemoteSyncHash(received));
    QNP_CHECK(received.tag == hash.tag
              && received.gameplayHash == hash.gameplayHash);
    QNP_CHECK(!client.takeRemoteSyncHash(received));

    packets = client.pollOutbound(20);
    QNP_CHECK(findType(packets, MessageType::SyncHashAck) != nullptr);
    QNP_CHECK(findType(host.pollOutbound(119), MessageType::SyncHash)
              == nullptr);
    packets = host.pollOutbound(120);
    publication = findType(packets, MessageType::SyncHash);
    QNP_REQUIRE(publication != nullptr);
    QNP_CHECK(client.onDatagram(hostEndpoint, publication->bytes.data(),
                                publication->bytes.size(), 120)
              == IngressResult::Accepted);
    QNP_CHECK(!client.takeRemoteSyncHash(received));
    packets = client.pollOutbound(120);
    const OutboundDatagram* ack =
        findType(packets, MessageType::SyncHashAck);
    QNP_REQUIRE(ack != nullptr);
    QNP_CHECK(host.onDatagram(clientEndpoint, ack->bytes.data(),
                              ack->bytes.size(), 121)
              == IngressResult::Accepted);
    QNP_CHECK(findType(host.pollOutbound(220), MessageType::SyncHash)
              == nullptr);

    packet = makeControl(MessageType::SyncHash, 41, conflict);
    QNP_CHECK(client.onDatagram(hostEndpoint, packet.data(), packet.size(), 122)
              == IngressResult::ConflictingControl);
    QNP_CHECK(client.lastValidReceiveMs() == 120);

    SyncHashPayload later = hash;
    later.tag.frame = 31;
    later.gameplayHash++;
    QNP_CHECK(host.submitLocalSyncHash(later));
    SyncHashPayload older = later;
    older.tag.frame = 29;
    QNP_CHECK(!host.submitLocalSyncHash(older));
}

QNP_TEST(CoreSyncHashSubmitDiagnosticsDistinguishBackpressure) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    SyncHashPayload hash{};
    hash.tag.layoutGeneration = 1;
    hash.tag.epoch = 1;
    hash.gameplayHash = 1;
    for (uint32_t frame = 0; frame < 64; ++frame) {
        hash.tag.frame = frame;
        hash.gameplayHash = frame + 1u;
        QNP_CHECK(host.submitLocalSyncHashDetailed(hash)
                  == SyncHashSubmitResult::Accepted);
    }
    QNP_CHECK(host.pendingLocalSyncHashCount() == 64u);
    hash.tag.frame = 64;
    hash.gameplayHash = 65;
    QNP_CHECK(host.submitLocalSyncHashDetailed(hash)
              == SyncHashSubmitResult::QueueFull);
    SyncHashTag last{};
    QNP_CHECK(host.lastLocalSyncHashTag(last));
    QNP_CHECK(last.frame == 63u);

    SyncHashPayload invalid = hash;
    invalid.tag.frame = kNoFrame;
    QNP_CHECK(client.submitLocalSyncHashDetailed(invalid)
              == SyncHashSubmitResult::InvalidPayload);
    client.setLocalTerminalError(
        TerminalErrorCode::Timeout, 123u);
    QNP_CHECK(client.submitLocalSyncHashDetailed(hash)
              == SyncHashSubmitResult::InvalidState);
}

QNP_TEST(CoreLayoutFingerprintIsReliableVisibleAndOrdersBeforeFrameZero) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    SyncHashPayload hostLayout{};
    hostLayout.tag = SyncHashTag{5, 7, kLayoutFingerprintFrame};
    hostLayout.gameplayHash = 0x1111222233334444ull;
    SyncHashPayload clientLayout = hostLayout;
    clientLayout.gameplayHash = 0xAAAABBBBCCCCDDDDull;

    QNP_CHECK(host.submitLocalSyncHash(hostLayout));
    QNP_CHECK(client.submitLocalSyncHash(clientLayout));
    SyncHashPayload frameZero{};
    frameZero.tag = SyncHashTag{5, 7, 0};
    frameZero.gameplayHash = 0x1234;
    QNP_CHECK(host.submitLocalSyncHash(frameZero));
    QNP_CHECK(client.submitLocalSyncHash(frameZero));
    QNP_CHECK(!host.localSyncHashAcknowledged(hostLayout.tag));
    QNP_CHECK(!client.localSyncHashAcknowledged(clientLayout.tag));

    const auto hostPackets = host.pollOutbound(10);
    const auto clientPackets = client.pollOutbound(10);
    const OutboundDatagram* hostPublication =
        findType(hostPackets, MessageType::SyncHash);
    const OutboundDatagram* clientPublication =
        findType(clientPackets, MessageType::SyncHash);
    QNP_REQUIRE(hostPublication != nullptr && clientPublication != nullptr);
    QNP_CHECK(
        decodeControl<SyncHashPayload>(
            *hostPublication, MessageType::SyncHash).tag.frame
        == kLayoutFingerprintFrame);
    QNP_CHECK(
        decodeControl<SyncHashPayload>(
            *clientPublication, MessageType::SyncHash).tag.frame
        == kLayoutFingerprintFrame);
    QNP_CHECK(client.onDatagram(
                  hostEndpoint, hostPublication->bytes.data(),
                  hostPublication->bytes.size(), 10)
              == IngressResult::Accepted);
    QNP_CHECK(host.onDatagram(
                  clientEndpoint, clientPublication->bytes.data(),
                  clientPublication->bytes.size(), 10)
              == IngressResult::Accepted);

    SyncHashPayload observedByHost{};
    SyncHashPayload observedByClient{};
    QNP_CHECK(host.peekRemoteSyncHash(observedByHost));
    QNP_CHECK(client.peekRemoteSyncHash(observedByClient));
    QNP_CHECK(host.takeRemoteSyncHash(observedByHost));
    QNP_CHECK(client.takeRemoteSyncHash(observedByClient));
    QNP_CHECK(observedByHost.tag == hostLayout.tag
              && observedByClient.tag == hostLayout.tag);
    QNP_CHECK(observedByHost.gameplayHash == clientLayout.gameplayHash);
    QNP_CHECK(observedByClient.gameplayHash == hostLayout.gameplayHash);
    QNP_CHECK(observedByHost.gameplayHash != hostLayout.gameplayHash);

    auto packets = host.pollOutbound(11);
    const OutboundDatagram* hostAck =
        findType(packets, MessageType::SyncHashAck);
    QNP_REQUIRE(hostAck != nullptr);
    QNP_CHECK(client.onDatagram(
                  hostEndpoint, hostAck->bytes.data(), hostAck->bytes.size(), 11)
              == IngressResult::Accepted);
    packets = client.pollOutbound(11);
    QNP_CHECK(findType(packets, MessageType::SyncHashAck) != nullptr);
    QNP_CHECK(client.localSyncHashAcknowledged(clientLayout.tag));
    QNP_CHECK(!host.localSyncHashAcknowledged(hostLayout.tag));

    QNP_CHECK(findType(host.pollOutbound(109), MessageType::SyncHash)
              == nullptr);
    packets = host.pollOutbound(110);
    hostPublication = findType(packets, MessageType::SyncHash);
    QNP_REQUIRE(hostPublication != nullptr);
    QNP_CHECK(client.onDatagram(
                  hostEndpoint, hostPublication->bytes.data(),
                  hostPublication->bytes.size(), 110)
              == IngressResult::Accepted);
    QNP_CHECK(!client.takeRemoteSyncHash(observedByClient));

    packets = client.pollOutbound(110);
    const OutboundDatagram* retryAck =
        findType(packets, MessageType::SyncHashAck);
    QNP_REQUIRE(retryAck != nullptr);
    QNP_CHECK(host.onDatagram(
                  clientEndpoint, retryAck->bytes.data(),
                  retryAck->bytes.size(), 111)
              == IngressResult::Accepted);
    QNP_CHECK(host.localSyncHashAcknowledged(hostLayout.tag));

    packets = host.pollOutbound(111);
    const OutboundDatagram* frameZeroPublication =
        findType(packets, MessageType::SyncHash);
    QNP_REQUIRE(frameZeroPublication != nullptr);
    QNP_CHECK(
        decodeControl<SyncHashPayload>(
            *frameZeroPublication, MessageType::SyncHash).tag.frame == 0);

    QNP_CHECK(!host.submitLocalSyncHash(hostLayout));

    SyncHashPayload withComponent = hostLayout;
    withComponent.rngSelector = 0x00020000u;
    NetplayCore componentFresh(hostConfig());
    QNP_CHECK(componentFresh.submitLocalSyncHash(withComponent));

    SyncHashPayload malformed = hostLayout;
    malformed.gameplayHash = 0;
    NetplayCore fresh(hostConfig());
    QNP_CHECK(!fresh.submitLocalSyncHash(malformed));
}

QNP_TEST(CoreAcknowledgesButDoesNotRepublishDelayedOlderLayoutTraffic) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    SyncHashPayload current{};
    current.tag = SyncHashTag{9, 9, kLayoutFingerprintFrame};
    current.gameplayHash = 0x9999000011112222ull;
    auto packet = makeControl(MessageType::SyncHash, 30, current);
    QNP_CHECK(client.onDatagram(
                  hostEndpoint, packet.data(), packet.size(), 10)
              == IngressResult::Accepted);
    SyncHashPayload observed{};
    QNP_CHECK(client.takeRemoteSyncHash(observed));
    QNP_CHECK(observed.tag == current.tag);

    SyncHashPayload stale{};
    stale.tag = SyncHashTag{8, 8, kLayoutFingerprintFrame};
    stale.gameplayHash = 0x8888000011112222ull;
    packet = makeControl(MessageType::SyncHash, 31, stale);
    QNP_CHECK(client.onDatagram(
                  hostEndpoint, packet.data(), packet.size(), 11)
              == IngressResult::Accepted);
    QNP_CHECK(!client.takeRemoteSyncHash(observed));

    const auto acks = client.pollOutbound(11);
    size_t hashAckCount = 0;
    bool staleAcked = false;
    for (const OutboundDatagram& outbound : acks) {
        if (outbound.type != MessageType::SyncHashAck) continue;
        ++hashAckCount;
        const SyncHashAckPayload ack =
            decodeControl<SyncHashAckPayload>(
                outbound, MessageType::SyncHashAck);
        staleAcked = staleAcked || ack.tag == stale.tag;
    }
    QNP_CHECK(hashAckCount >= 2);
    QNP_CHECK(staleAcked);

    SyncHashPayload conflict = current;
    conflict.gameplayHash++;
    packet = makeControl(MessageType::SyncHash, 32, conflict);
    QNP_CHECK(client.onDatagram(
                  hostEndpoint, packet.data(), packet.size(), 12)
              == IngressResult::ConflictingControl);
}

QNP_TEST(CoreMeasuresRttTimesOutAndReliablyRepeatsTerminalError) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));
    QNP_CHECK(host.isAlive(0));
    QNP_CHECK(!host.hasRtt());

    auto packets = host.pollOutbound(1000);
    const OutboundDatagram* ping =
        findType(packets, MessageType::Ping);
    QNP_REQUIRE(ping != nullptr);
    const PingPayload pingPayload =
        decodeControl<PingPayload>(*ping, MessageType::Ping);
    QNP_CHECK(pingPayload.pingId != 0 && pingPayload.sentMs == 1000);
    QNP_CHECK(client.onDatagram(hostEndpoint, ping->bytes.data(),
                                ping->bytes.size(), 1000)
              == IngressResult::Accepted);

    packets = client.pollOutbound(1000);
    const OutboundDatagram* pong =
        findType(packets, MessageType::Pong);
    QNP_REQUIRE(pong != nullptr);
    QNP_CHECK(host.onDatagram(clientEndpoint, pong->bytes.data(),
                              pong->bytes.size(), 1042)
              == IngressResult::Accepted);
    QNP_CHECK(host.hasRtt() && host.rttMs() == 42);
    QNP_CHECK(host.isAlive(11042));
    QNP_CHECK(!host.isAlive(11043));

    packets = host.pollOutbound(11043);
    const OutboundDatagram* terminal =
        findType(packets, MessageType::Disconnect);
    QNP_REQUIRE(terminal != nullptr);
    QNP_CHECK(host.terminalState().active);
    QNP_CHECK(!host.terminalState().fromPeer);
    QNP_CHECK(host.terminalState().code == TerminalErrorCode::Timeout);
    QNP_CHECK(host.terminalState().reasonId != 0);
    QNP_CHECK(std::string(host.terminalState().reason.data())
              == "peer liveness timeout");
    host.setLocalTerminalError(TerminalErrorCode::ConfirmedDesync, 99);
    QNP_CHECK(host.terminalState().code == TerminalErrorCode::Timeout);

    QNP_CHECK(client.onDatagram(hostEndpoint, terminal->bytes.data(),
                                terminal->bytes.size(), 11043)
              == IngressResult::Accepted);
    QNP_CHECK(client.terminalState().active);
    QNP_CHECK(client.terminalState().fromPeer);
    QNP_CHECK(client.terminalState().code == TerminalErrorCode::Timeout);
    QNP_CHECK(client.terminalState().reasonId
              == host.terminalState().reasonId);
    QNP_CHECK(client.terminalState().reason
              == host.terminalState().reason);

    packets = client.pollOutbound(11043);
    QNP_CHECK(findType(packets, MessageType::DisconnectAck) != nullptr);
    QNP_CHECK(host.pollOutbound(11142).empty());
    packets = host.pollOutbound(11143);
    terminal = findType(packets, MessageType::Disconnect);
    QNP_REQUIRE(terminal != nullptr);
    QNP_CHECK(client.onDatagram(hostEndpoint, terminal->bytes.data(),
                                terminal->bytes.size(), 11143)
              == IngressResult::Accepted);
    packets = client.pollOutbound(11143);
    const OutboundDatagram* terminalAck =
        findType(packets, MessageType::DisconnectAck);
    QNP_REQUIRE(terminalAck != nullptr);
    QNP_CHECK(host.onDatagram(clientEndpoint, terminalAck->bytes.data(),
                              terminalAck->bytes.size(), 11144)
              == IngressResult::Accepted);
    QNP_CHECK(findType(host.pollOutbound(11244),
                       MessageType::Disconnect) == nullptr);
}

QNP_TEST(CoreTimesOutWaitingForHelloFromFirstPollEpoch) {
    CoreConfig config = hostConfig();
    config.pingIntervalMs = 100;
    config.livenessTimeoutMs = 1000;
    NetplayCore host(config);

    constexpr uint64_t firstPollMs = 9876543210ull;
    QNP_CHECK(host.pollOutbound(firstPollMs).empty());
    QNP_CHECK(!host.hasTerminalError());
    QNP_CHECK(host.lastValidReceiveMs() == 0);

    QNP_CHECK(host.pollOutbound(firstPollMs + 1000).empty());
    QNP_CHECK(!host.hasTerminalError());

    QNP_CHECK(host.pollOutbound(firstPollMs + 1001).empty());
    QNP_CHECK(host.hasTerminalError());
    QNP_CHECK(!host.terminalState().fromPeer);
    QNP_CHECK(host.terminalState().code == TerminalErrorCode::Timeout);
}

QNP_TEST(CoreTimesOutWaitingForHelloAckAndPublishesTerminal) {
    CoreConfig config = clientConfig();
    config.pingIntervalMs = 100;
    config.livenessTimeoutMs = 1000;
    NetplayCore client(config);

    constexpr uint64_t firstPollMs = 1234567890ull;
    auto packets = client.pollOutbound(firstPollMs);
    QNP_CHECK(findType(packets, MessageType::Hello) != nullptr);
    QNP_CHECK(findType(packets, MessageType::Disconnect) == nullptr);
    QNP_CHECK(!client.hasTerminalError());

    packets = client.pollOutbound(firstPollMs + 1000);
    QNP_CHECK(findType(packets, MessageType::Hello) != nullptr);
    QNP_CHECK(findType(packets, MessageType::Disconnect) == nullptr);
    QNP_CHECK(!client.hasTerminalError());

    packets = client.pollOutbound(firstPollMs + 1001);
    const OutboundDatagram* terminal =
        findType(packets, MessageType::Disconnect);
    QNP_REQUIRE(terminal != nullptr);
    QNP_CHECK(findType(packets, MessageType::Hello) == nullptr);
    QNP_CHECK(client.hasTerminalError());
    QNP_CHECK(!client.terminalState().fromPeer);
    QNP_CHECK(client.terminalState().code == TerminalErrorCode::Timeout);
    const DisconnectPayload payload =
        decodeControl<DisconnectPayload>(
            *terminal, MessageType::Disconnect);
    QNP_CHECK(payload.code == TerminalErrorCode::Timeout);
    QNP_CHECK(payload.detail == 0);
    QNP_CHECK(terminal->destination == config.expectedPeer);

    QNP_CHECK(client.pollOutbound(firstPollMs + 1100).empty());
    packets = client.pollOutbound(firstPollMs + 1101);
    QNP_CHECK(findType(packets, MessageType::Disconnect) != nullptr);
}

QNP_TEST(CoreDoesNotInflateRttUsingRetransmittedPing) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    QNP_CHECK(findType(host.pollOutbound(1000), MessageType::Ping)
              != nullptr);
    auto packets = host.pollOutbound(2000);
    const OutboundDatagram* ping =
        findType(packets, MessageType::Ping);
    QNP_REQUIRE(ping != nullptr);
    QNP_CHECK(client.onDatagram(hostEndpoint, ping->bytes.data(),
                                ping->bytes.size(), 2000)
              == IngressResult::Accepted);
    packets = client.pollOutbound(2000);
    const OutboundDatagram* pong =
        findType(packets, MessageType::Pong);
    QNP_REQUIRE(pong != nullptr);
    QNP_CHECK(host.onDatagram(clientEndpoint, pong->bytes.data(),
                              pong->bytes.size(), 2025)
              == IngressResult::Accepted);
    QNP_CHECK(!host.hasRtt());

    packets = host.pollOutbound(3000);
    ping = findType(packets, MessageType::Ping);
    QNP_REQUIRE(ping != nullptr);
    QNP_CHECK(client.onDatagram(hostEndpoint, ping->bytes.data(),
                                ping->bytes.size(), 3000)
              == IngressResult::Accepted);
    packets = client.pollOutbound(3000);
    pong = findType(packets, MessageType::Pong);
    QNP_REQUIRE(pong != nullptr);
    QNP_CHECK(host.onDatagram(clientEndpoint, pong->bytes.data(),
                              pong->bytes.size(), 3025)
              == IngressResult::Accepted);
    QNP_CHECK(host.hasRtt() && host.rttMs() == 25);
}

QNP_TEST(CoreDiagnosticEnumNamesAreExhaustiveAndStable) {
    for (unsigned value = 0;
         value < static_cast<unsigned>(IngressResult::Count);
         ++value) {
        QNP_CHECK(std::string(ingressResultName(
                      static_cast<IngressResult>(value)))
                  != "unknown");
    }
    QNP_CHECK(std::string(ingressResultName(
                  static_cast<IngressResult>(255)))
              == "unknown");

    const SyncHashSubmitResult syncResults[] = {
        SyncHashSubmitResult::Accepted,
        SyncHashSubmitResult::ExactDuplicate,
        SyncHashSubmitResult::InvalidState,
        SyncHashSubmitResult::InvalidPayload,
        SyncHashSubmitResult::AlreadyAcknowledged,
        SyncHashSubmitResult::OutOfOrder,
        SyncHashSubmitResult::QueueFull,
    };
    for (const SyncHashSubmitResult result : syncResults) {
        QNP_CHECK(std::string(syncHashSubmitResultName(result))
                  != "unknown");
    }
}

QNP_TEST(CorePeerSimFrontierIsMonotonicFreshnessStamped) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig());
    const Endpoint hostEndpoint{ kHostIp, 7000 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };
    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 10));

    QNP_CHECK(!host.hasPeerSimFrontier());
    QNP_CHECK(host.peerSimFrontier() == kNoFrame);
    QNP_CHECK(host.peerFrontierAgeMs(100) == ~0ull);

    std::array<uint16_t, kInputWindowSize> values{};
    values[0] = 10;
    auto packet = makeInputs(cookie(), 10, 0, kNoFrame, 1, values,
                             5);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 20)
              == IngressResult::Accepted);
    QNP_CHECK(host.hasPeerSimFrontier());
    QNP_CHECK(host.peerSimFrontier() == 5);
    QNP_CHECK(host.peerFrontierAgeMs(24) == 4);

    values[0] = 11;
    packet = makeInputs(cookie(), 11, 1, kNoFrame, 1, values, 3);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 30)
              == IngressResult::Accepted);
    QNP_CHECK(host.peerSimFrontier() == 5);
    QNP_CHECK(host.peerFrontierAgeMs(30) == 10);

    values[0] = 12;
    packet = makeInputs(cookie(), 12, 2, kNoFrame, 1, values, 5);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 40)
              == IngressResult::Accepted);
    QNP_CHECK(host.peerSimFrontier() == 5);
    QNP_CHECK(host.peerFrontierAgeMs(41) == 1);

    values[0] = 13;
    packet = makeInputs(cookie(), 13, 3, kNoFrame, 1, values, 9);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 50)
              == IngressResult::Accepted);
    QNP_CHECK(host.peerSimFrontier() == 9);

    values[0] = 0x0E;
    packet = makeInputs(cookie(), 14, 4, kNoFrame, 1, values, kNoFrame);
    QNP_CHECK(host.onDatagram(clientEndpoint, packet.data(), packet.size(), 60)
              == IngressResult::Accepted);
    QNP_CHECK(host.peerSimFrontier() == 9);

    QNP_CHECK(client.setSimulationFrame(7));
    uint16_t word = 0;
    QNP_CHECK(client.submitLocalInput(0, 0x0105));
    const auto out = client.pollOutbound(100);
    QNP_REQUIRE(!out.empty());
    bool sawFrontier = false;
    for (const auto& datagram : out) {
        Datagram decoded{};
        if (decodeDatagram(datagram.bytes.data(), datagram.bytes.size(),
                           kSession, cookie(), decoded)
                == DecodeError::None
            && decoded.header.type == MessageType::Inputs) {
            InputsPayload inputs{};
            QNP_CHECK(decodePayload(decoded.payload, inputs));
            QNP_CHECK(inputs.simFrontier == 7);
            sawFrontier = true;
        }
    }
    QNP_CHECK(sawFrontier);
    (void)word;
}

QNP_TEST(CorePaletteChannelResendsAndValidatesTheTransparentKey) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig(7100));
    const Endpoint hostEndpoint{ kHostIp, 7100 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };

    PalettePayload palette{};
    palette.epoch = 3;
    palette.side = 1;
    palette.variant = 2;
    palette.present = 1;
    for (size_t variant = 0; variant < 4; ++variant) {
        for (size_t colour = 1; colour < 16; ++colour) {
            uint8_t* entry = palette.table + variant * 64 + colour * 4;
            entry[0] = static_cast<uint8_t>(colour * 3 + variant);
            entry[1] = static_cast<uint8_t>(colour * 5 + variant);
            entry[2] = static_cast<uint8_t>(colour * 7 + variant);
        }
    }

    const std::vector<uint8_t> early =
        makeControl(MessageType::Palette, 1, palette);
    QNP_CHECK(host.onDatagram(clientEndpoint, early.data(), early.size(), 0)
              != IngressResult::Accepted);
    PalettePayload none{};
    QNP_CHECK(!host.takePalette(none));

    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    QNP_REQUIRE(client.submitPalette(palette));
    const auto firstPoll = client.pollOutbound(10);
    const OutboundDatagram* sent = findType(firstPoll, MessageType::Palette);
    QNP_REQUIRE(sent != nullptr);

    QNP_CHECK(findType(client.pollOutbound(20), MessageType::Palette)
              == nullptr);
    QNP_CHECK(findType(client.pollOutbound(10 + 500), MessageType::Palette)
              != nullptr);

    QNP_CHECK(host.onDatagram(clientEndpoint, sent->bytes.data(),
                              sent->bytes.size(), 30)
              == IngressResult::Accepted);
    PalettePayload taken{};
    QNP_REQUIRE(host.takePalette(taken));
    QNP_CHECK(taken.epoch == 3);
    QNP_CHECK(taken.side == 1);
    QNP_CHECK(taken.variant == 2);
    QNP_CHECK(taken.present == 1);
    QNP_CHECK(std::memcmp(taken.table, palette.table,
                          kPaletteTableWireBytes) == 0);
    QNP_CHECK(!host.takePalette(taken));

    std::vector<uint8_t> opaqueKey = sent->bytes;
    opaqueKey[wire::kHeaderSize + 8] = 0x7F;
    QNP_CHECK(host.onDatagram(clientEndpoint, opaqueKey.data(),
                              opaqueKey.size(), 60)
              != IngressResult::Accepted);
    QNP_CHECK(!host.takePalette(taken));

    std::vector<uint8_t> badVariant = sent->bytes;
    badVariant[wire::kHeaderSize + 5] = 4;
    QNP_CHECK(host.onDatagram(clientEndpoint, badVariant.data(),
                              badVariant.size(), 70)
              != IngressResult::Accepted);
    QNP_CHECK(!host.takePalette(taken));
}

QNP_TEST(CoreRematchChoiceIsUnreliableLatestWins) {
    NetplayCore host(hostConfig());
    NetplayCore client(clientConfig(7100));
    const Endpoint hostEndpoint{ kHostIp, 7100 };
    const Endpoint clientEndpoint{ kClientIp, 7100 };

    RematchChoicePayload choice{};
    choice.epoch = 2;
    choice.promptFrame = 41000;
    choice.side = 1;
    choice.choice = kRematchChoiceRematch;
    choice.ready = 1;
    const std::vector<uint8_t> early =
        makeControl(MessageType::RematchChoice, 1, choice);
    QNP_CHECK(host.onDatagram(clientEndpoint, early.data(), early.size(), 0)
              != IngressResult::Accepted);
    RematchChoicePayload none{};
    QNP_CHECK(!host.takeRematchChoice(none));

    QNP_REQUIRE(establish(host, client, hostEndpoint, clientEndpoint, 0));

    QNP_REQUIRE(client.submitRematchChoice(choice));
    const auto firstPoll = client.pollOutbound(10);
    const OutboundDatagram* sent =
        findType(firstPoll, MessageType::RematchChoice);
    QNP_REQUIRE(sent != nullptr);
    const auto quietPoll = client.pollOutbound(20);
    QNP_CHECK(findType(quietPoll, MessageType::RematchChoice) == nullptr);

    QNP_CHECK(host.onDatagram(clientEndpoint, sent->bytes.data(),
                              sent->bytes.size(), 30)
              == IngressResult::Accepted);
    RematchChoicePayload newer = choice;
    newer.ready = 0;
    newer.choice = kRematchChoiceUndecided;
    QNP_REQUIRE(client.submitRematchChoice(newer));
    const auto secondPoll = client.pollOutbound(40);
    const OutboundDatagram* second =
        findType(secondPoll, MessageType::RematchChoice);
    QNP_REQUIRE(second != nullptr);
    QNP_CHECK(host.onDatagram(clientEndpoint, second->bytes.data(),
                              second->bytes.size(), 50)
              == IngressResult::Accepted);
    RematchChoicePayload taken{};
    QNP_REQUIRE(host.takeRematchChoice(taken));
    QNP_CHECK(taken.ready == 0);
    QNP_CHECK(taken.choice == kRematchChoiceUndecided);
    QNP_CHECK(taken.promptFrame == 41000);
    QNP_CHECK(!host.takeRematchChoice(taken));

    std::vector<uint8_t> corrupted = second->bytes;
    corrupted[wire::kHeaderSize + 9] = 9;
    QNP_CHECK(host.onDatagram(clientEndpoint, corrupted.data(),
                              corrupted.size(), 60)
              != IngressResult::Accepted);
    QNP_CHECK(!host.takeRematchChoice(taken));
}
