#include "../RelayServerCore.h"

#include "../../net/RelayProtocol.h"
#include "../../netplay/tests/TestHarness.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace qnetplay;
using namespace qnetplay::relay::server;

namespace qsp = qnetplay::spectator;

namespace {

qnetplay::compat::CompatibilityIdentityWire relayFixtureIdentity() {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = wire::kVersion;
    (void)qnetplay::compat::setProfileId(identity, "fixture-profile");
    for (std::size_t index = 0u;
         index < qnetplay::compat::kCompatibilitySha256Bytes; ++index) {
        identity.runtimeBuild.executableSha256[index] =
            static_cast<std::uint8_t>(0x11u + index);
        identity.simulationContent.simulationRootSha256[index] =
            static_cast<std::uint8_t>(0x41u + index);
        identity.simulationContent.validationRootSha256[index] =
            static_cast<std::uint8_t>(0x81u + index);
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

qsp::SessionIdentity spectatorIdentity() {
    qsp::SessionIdentity identity{};
    identity.sessionId = 0x5151515151515151ull;
    identity.epoch = 3;
    identity.cookie.low = 0xA1A2A3A4A5A6A7A8ull;
    identity.cookie.high = 0xB1B2B3B4B5B6B7B8ull;
    return identity;
}

std::vector<uint8_t> viewerHelloDatagram(const uint64_t viewerId,
                                         const uint32_t generation) {
    qsp::HelloPayload hello{};
    hello.simulationContract = 0x0F0E0D0C0B0A0908ull;
    hello.compatibilityIdentity = relayFixtureIdentity();
    hello.requestedGameId = qsp::kLatestGameId;
    hello.viewerId = viewerId;
    hello.generation = generation;
    hello.viewerNonce = qsp::viewerGenerationNonce(viewerId, generation);
    hello.resumeRecord = 0;
    hello.resumeDigest = 0;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> encoded;
    if (!qsp::encodePayload(hello, payload)
        || !qsp::encodeDatagram(
            qsp::MessageType::Hello,
            spectatorIdentity(),
            11,
            payload,
            encoded)) {
        return {};
    }
    return encoded;
}

std::vector<uint8_t> sourceHeartbeatDatagram() {
    qsp::HeartbeatPayload heartbeat{};
    heartbeat.gameId = 0x1234;
    heartbeat.digest = 0x99;
    heartbeat.nextRecord = 12;
    heartbeat.checkpointRecord = 0;
    heartbeat.sourcePhase = qsp::SourcePhase::Active;
    heartbeat.viewerCount = 2;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> encoded;
    if (!qsp::encodePayload(heartbeat, payload)
        || !qsp::encodeDatagram(
            qsp::MessageType::Heartbeat,
            spectatorIdentity(),
            12,
            payload,
            encoded)) {
        return {};
    }
    return encoded;
}

std::vector<uint8_t> validHelloDatagram() {
    wire::HelloPayload hello{};
    hello.compatibilityId = 0x123456789ABCDEF0ull;
    hello.role = wire::PeerRole::Client;
    hello.player = 1;
    hello.compatibilityIdentity = relayFixtureIdentity();
    std::vector<uint8_t> payload;
    std::vector<uint8_t> encoded;
    if (!wire::encodePayload(hello, payload)
        || !wire::encodeDatagram(
            wire::MessageType::Hello,
            0x1122334455667788ull,
            wire::SessionCookie{
                0x0102030405060708ull,
                0x1112131415161718ull,
            },
            7,
            payload,
            encoded)) {
        return {};
    }
    return encoded;
}

namespace rwire = qnetplay::spectator::resilient::wire;
namespace rmodel = qnetplay::spectator::resilient;

void writeFixtureLe16(uint8_t* out, const uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void writeFixtureLe32(uint8_t* out, const uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out[shift / 8] = static_cast<uint8_t>(value >> shift);
}

void writeFixtureLe64(uint8_t* out, const uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out[shift / 8] = static_cast<uint8_t>(value >> shift);
}

std::vector<uint8_t> validInnerControlDatagram() {
    rwire::AuthorityState state{};
    if (!rmodel::makeStreamCompatibilityIdentity(
            relayFixtureIdentity(), 0x5151515151515151ull, 3u,
            0x4646464646464646ull, state.stream)) {
        return {};
    }
    state.authorityGeneration = 7u;
    state.rootNodeId = 1u;
    state.childNodeId = 2u;
    state.viewerId = 0x544F504F56494557ull;
    state.viewerGeneration = 1u;
    state.qspRouteGeneration = 1u;
    state.qsrIpv4BE = 0x0100007Fu;
    state.qsrPort = 40000u;
    state.qsrBindingGeneration = 1u;
    state.sequence = 9u;
    state.authorityExpiryMs = 60000u;
    state.eventTimeMs = 1234u;
    state.kind = rwire::AuthorityStateKind::Liveness;
    state.reason = rwire::AuthorityStateReason::None;
    state.livenessWindowMs = 1000u;
    rwire::ControlDatagram datagram{};
    datagram.type = rwire::ControlMessageType::AuthorityState;
    datagram.authorityState = state;
    std::vector<uint8_t> encoded;
    if (!rwire::encodeControlDatagram(datagram, encoded)) return {};
    return encoded;
}

std::vector<uint8_t> validTopologyEnvelope() {
    const std::vector<uint8_t> inner = validInnerControlDatagram();
    if (inner.empty()) return {};
    std::vector<uint8_t> out(
        rwire::kTopologyQtaEnvelopePrefixBytes + inner.size(), 0u);
    writeFixtureLe32(out.data(), rwire::kTopologyQtaEnvelopeMagic);
    writeFixtureLe16(out.data() + 4, rwire::kTopologyQtaEnvelopeVersion);
    out[6] = rwire::kTopologyQtaEnvelopeControlType;
    out[7] = 0u;
    writeFixtureLe16(out.data() + 8, static_cast<uint16_t>(inner.size()));
    writeFixtureLe16(out.data() + 10, 0u);
    writeFixtureLe64(out.data() + 12, 0x0101010101010101ull);
    writeFixtureLe64(out.data() + 20, 0x0202020202020202ull);
    writeFixtureLe64(out.data() + 28, 0x0303030303030303ull);
    for (size_t index = 0; index < rwire::kTopologyQtaEnvelopeMacBytes;
         ++index) {
        out[36 + index] = static_cast<uint8_t>(0x90 + index);
    }
    std::copy(inner.begin(), inner.end(),
              out.begin() + rwire::kTopologyQtaEnvelopePrefixBytes);
    return out;
}

}

QNP_TEST(relay_constant_time_token_comparison) {
    const std::array<uint8_t, 16> token{
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    };
    std::array<uint8_t, 16> differentFirst = token;
    std::array<uint8_t, 16> differentLast = token;
    differentFirst.front() ^= 0x80;
    differentLast.back() ^= 0x80;

    QNP_CHECK(constantTimeEqual(token.data(), token.data(), token.size()));
    QNP_CHECK(!constantTimeEqual(
        token.data(), differentFirst.data(), token.size()));
    QNP_CHECK(!constantTimeEqual(
        token.data(), differentLast.data(), token.size()));
    QNP_CHECK(constantTimeEqual(nullptr, nullptr, 0));
    QNP_CHECK(!constantTimeEqual(nullptr, token.data(), token.size()));
}

QNP_TEST(relay_token_bucket_bounds_burst_and_refill) {
    TokenBucket bucket(100, 200, 1000);
    QNP_CHECK(bucket.available(1000) == 200);
    QNP_CHECK(bucket.consume(150, 1000));
    QNP_CHECK(bucket.available(1000) == 50);
    QNP_CHECK(!bucket.consume(51, 1000));
    QNP_CHECK(bucket.consume(50, 1000));
    QNP_CHECK(bucket.available(1000) == 0);
    QNP_CHECK(bucket.available(1499) == 49);
    QNP_CHECK(bucket.available(1500) == 50);
    QNP_CHECK(bucket.available(100000) == 200);
    QNP_CHECK(!bucket.consume(201, 100000));

    bucket.reset(0, 0, 0);
    QNP_CHECK(!bucket.consume(1, 5000));
}

QNP_TEST(relay_accepts_only_complete_strict_qnp_player_datagrams) {
    const std::vector<uint8_t> hello = validHelloDatagram();
    QNP_REQUIRE(hello.size() >= wire::kHeaderSize);

    PlayerDatagramInfo info{};
    QNP_CHECK(inspectPlayerDatagram(
        hello.data(), hello.size(), info) == PlayerDatagramError::None);
    QNP_CHECK(info.sessionId == 0x1122334455667788ull);
    QNP_CHECK(info.cookie.low == 0x0102030405060708ull);
    QNP_CHECK(info.cookie.high == 0x1112131415161718ull);
    QNP_CHECK(info.type == wire::MessageType::Hello);
    QNP_CHECK(info.sequence == 7);
    QNP_CHECK(info.carriesRole);
    QNP_CHECK(info.role == wire::PeerRole::Client);

    std::vector<uint8_t> corrupt = hello;
    corrupt.back() ^= 1;
    PlayerDatagramInfo untouched{};
    untouched.sessionId = 99;
    QNP_CHECK(inspectPlayerDatagram(
        corrupt.data(), corrupt.size(), untouched)
        == PlayerDatagramError::InvalidFraming);
    QNP_CHECK(untouched.sessionId == 99);

    std::vector<uint8_t> zeroIdentity = hello;
    for (size_t i = 12; i < 20; ++i) zeroIdentity[i] = 0;
    QNP_CHECK(inspectPlayerDatagram(
        zeroIdentity.data(), zeroIdentity.size(), untouched)
        == PlayerDatagramError::InvalidIdentity);

    std::vector<uint8_t> malformedReadyPayload(
        wire::payloadSize(wire::MessageType::Ready), 0);
    std::vector<uint8_t> malformedReady;
    QNP_CHECK(wire::encodeDatagram(
        wire::MessageType::Ready,
        1,
        wire::SessionCookie{ 2, 3 },
        4,
        malformedReadyPayload,
        malformedReady));
    QNP_CHECK(inspectPlayerDatagram(
        malformedReady.data(), malformedReady.size(), untouched)
        == PlayerDatagramError::InvalidPayload);

    QNP_CHECK(inspectPlayerDatagram(
        hello.data(), wire::kHeaderSize - 1, untouched)
        == PlayerDatagramError::TooSmall);
    std::vector<uint8_t> oversized(wire::kMaxDatagramSize + 1, 0);
    QNP_CHECK(inspectPlayerDatagram(
        oversized.data(), oversized.size(), untouched)
        == PlayerDatagramError::TooLarge);
}

QNP_TEST(relay_forwards_session_event_and_rematch_choice_datagrams) {
    wire::SessionEventPayload event{};
    event.stage = wire::SessionEventStage::Quiesce;
    event.kind = 2;
    event.eventId = 5;
    event.value = 40000;
    std::vector<uint8_t> eventPayload;
    QNP_REQUIRE(wire::encodePayload(event, eventPayload));
    std::vector<uint8_t> eventDatagram;
    QNP_REQUIRE(wire::encodeDatagram(
        wire::MessageType::SessionEvent, 1,
        wire::SessionCookie{ 2, 3 }, 4, eventPayload, eventDatagram));
    PlayerDatagramInfo info{};
    QNP_CHECK(inspectPlayerDatagram(
        eventDatagram.data(), eventDatagram.size(), info)
        == PlayerDatagramError::None);
    QNP_CHECK(info.type == wire::MessageType::SessionEvent);
    QNP_CHECK(!info.carriesRole);

    wire::RematchChoicePayload choice{};
    choice.epoch = 3;
    choice.promptFrame = 40000;
    choice.side = 0;
    choice.choice = wire::kRematchChoiceRematch;
    choice.ready = 1;
    std::vector<uint8_t> choicePayload;
    QNP_REQUIRE(wire::encodePayload(choice, choicePayload));
    std::vector<uint8_t> choiceDatagram;
    QNP_REQUIRE(wire::encodeDatagram(
        wire::MessageType::RematchChoice, 1,
        wire::SessionCookie{ 2, 3 }, 5, choicePayload, choiceDatagram));
    QNP_CHECK(inspectPlayerDatagram(
        choiceDatagram.data(), choiceDatagram.size(), info)
        == PlayerDatagramError::None);
    QNP_CHECK(info.type == wire::MessageType::RematchChoice);

    std::vector<uint8_t> malformed(
        wire::payloadSize(wire::MessageType::RematchChoice), 0);
    malformed[9] = 9;
    std::vector<uint8_t> malformedDatagram;
    QNP_REQUIRE(wire::encodeDatagram(
        wire::MessageType::RematchChoice, 1,
        wire::SessionCookie{ 2, 3 }, 6, malformed, malformedDatagram));
    QNP_CHECK(inspectPlayerDatagram(
        malformedDatagram.data(), malformedDatagram.size(), info)
        == PlayerDatagramError::InvalidPayload);
}

QNP_TEST(relay_accepts_only_strict_qsp_spectator_datagrams) {
    const std::vector<uint8_t> hello = viewerHelloDatagram(0x77, 4);
    QNP_REQUIRE(hello.size() >= qsp::kSpectatorHeaderSize);

    SpectatorDatagramInfo info{};
    QNP_CHECK(inspectSpectatorDatagram(
        hello.data(), hello.size(), qsp::PeerRole::Viewer, info)
        == SpectatorDatagramError::None);
    QNP_CHECK(info.identity == spectatorIdentity());
    QNP_CHECK(info.type == qsp::MessageType::Hello);
    QNP_CHECK(info.senderRole == qsp::PeerRole::Viewer);
    QNP_CHECK(info.direction == qsp::Direction::ViewerToSource);
    QNP_CHECK(info.sequence == 11);
    QNP_CHECK(info.carriesViewerId);
    QNP_CHECK(info.viewerId == 0x77);
    QNP_CHECK(info.carriesGeneration);
    QNP_CHECK(info.generation == 4);

    SpectatorDatagramInfo untouched{};
    untouched.sequence = 4242;
    QNP_CHECK(inspectSpectatorDatagram(
        hello.data(), hello.size(), qsp::PeerRole::Source, untouched)
        == SpectatorDatagramError::WrongOrigin);
    QNP_CHECK(untouched.sequence == 4242);

    const std::vector<uint8_t> heartbeat = sourceHeartbeatDatagram();
    QNP_REQUIRE(!heartbeat.empty());
    QNP_CHECK(inspectSpectatorDatagram(
        heartbeat.data(), heartbeat.size(), qsp::PeerRole::Source, info)
        == SpectatorDatagramError::None);
    QNP_CHECK(!info.carriesViewerId);
    QNP_CHECK(inspectSpectatorDatagram(
        heartbeat.data(), heartbeat.size(), qsp::PeerRole::Viewer, untouched)
        == SpectatorDatagramError::WrongOrigin);

    std::vector<uint8_t> corrupt = hello;
    corrupt.back() ^= 1;
    QNP_CHECK(inspectSpectatorDatagram(
        corrupt.data(), corrupt.size(), qsp::PeerRole::Viewer, untouched)
        != SpectatorDatagramError::None);

    std::vector<uint8_t> zeroIdentity = hello;
    for (size_t i = 16; i < 48; ++i) zeroIdentity[i] = 0;
    QNP_CHECK(inspectSpectatorDatagram(
        zeroIdentity.data(), zeroIdentity.size(), qsp::PeerRole::Viewer,
        untouched)
        == SpectatorDatagramError::InvalidIdentity);

    QNP_CHECK(inspectSpectatorDatagram(
        hello.data(), qsp::kSpectatorHeaderSize - 1, qsp::PeerRole::Viewer,
        untouched)
        == SpectatorDatagramError::TooSmall);
    const std::vector<uint8_t> oversized(
        qsp::kMaxSpectatorDatagramSize + 1, 0);
    QNP_CHECK(inspectSpectatorDatagram(
        oversized.data(), oversized.size(), qsp::PeerRole::Viewer, untouched)
        == SpectatorDatagramError::TooLarge);
}

QNP_TEST(relay_player_and_spectator_datagrams_never_cross_lanes) {
    const std::vector<uint8_t> player = validHelloDatagram();
    const std::vector<uint8_t> spectator = viewerHelloDatagram(9, 1);
    QNP_REQUIRE(!player.empty());
    QNP_REQUIRE(!spectator.empty());

    PlayerDatagramInfo playerInfo{};
    QNP_CHECK(inspectPlayerDatagram(
        spectator.data(), spectator.size(), playerInfo)
        != PlayerDatagramError::None);

    SpectatorDatagramInfo spectatorInfo{};
    QNP_CHECK(inspectSpectatorDatagram(
        player.data(), player.size(), qsp::PeerRole::Viewer, spectatorInfo)
        != SpectatorDatagramError::None);
    QNP_CHECK(inspectSpectatorDatagram(
        player.data(), player.size(), qsp::PeerRole::Source, spectatorInfo)
        != SpectatorDatagramError::None);
}

QNP_TEST(relay_spectator_wire_extension_is_explicitly_versioned) {
    using namespace qnet::relay;

    SpectatorHello hello{};
    hello.room = RoomId{ 0x1122334455667788ull, 0x99AABBCCDDEEFF00ull };
    hello.party = SpectatorParty::Viewer;
    hello.route = SpectatorRouteKind::Relay;
    hello.sessionId = 0x4242;
    hello.grantId = 7;
    hello.viewerId = 9;
    hello.expiryUnix = 1'800'000'000ull;
    hello.sourceGeneration = 2;
    hello.resumeRecord = 31;
    hello.resumeDigest = 0xFEEDFACEull;
    hello.macLow = 0xAAAA;
    hello.macHigh = 0xBBBB;
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodeSpectatorHello(hello, encoded));
    QNP_CHECK(encoded.size() == kSpectatorHelloSize);

    uint16_t version = 0;
    QNP_CHECK(peekRelayVersion(encoded.data(), encoded.size(), version));
    QNP_CHECK(version == kSpectatorVersion);
    QNP_CHECK(version != kVersion);

    Hello playerHello{};
    QNP_CHECK(decodeHello(encoded.data(), encoded.size(), playerHello)
              == DecodeError::BadVersion);
    Register playerRegister{};
    QNP_CHECK(decodeRegister(encoded.data(), encoded.size(), playerRegister)
              == DecodeError::BadVersion);

    SpectatorHello decoded{};
    QNP_CHECK(decodeSpectatorHello(encoded.data(), encoded.size(), decoded)
              == DecodeError::None);
    QNP_CHECK(decoded.room == hello.room);
    QNP_CHECK(decoded.viewerId == 9);
    QNP_CHECK(decoded.grantId == 7);
    QNP_CHECK(decoded.macLow == 0xAAAA && decoded.macHigh == 0xBBBB);
    QNP_CHECK(decoded.route == SpectatorRouteKind::Relay);

    SpectatorHello smuggled = hello;
    smuggled.sourceToken = RegistrationToken{ 1, 2 };
    std::vector<uint8_t> rejected;
    QNP_CHECK(!encodeSpectatorHello(smuggled, rejected));

    SpectatorHello source{};
    source.room = hello.room;
    source.party = SpectatorParty::Source;
    source.admission = SpectatorAdmission::None;
    source.sessionId = 0x4242;
    source.sourceToken = RegistrationToken{ 5, 6 };
    QNP_REQUIRE(encodeSpectatorHello(source, encoded));
    SpectatorHello decodedSource{};
    QNP_CHECK(decodeSpectatorHello(
                  encoded.data(), encoded.size(), decodedSource)
              == DecodeError::None);
    QNP_CHECK(decodedSource.party == SpectatorParty::Source);
    QNP_CHECK(decodedSource.viewerId == 0);
    source.grantId = 3;
    QNP_CHECK(!encodeSpectatorHello(source, rejected));

    SpectatorHello publicViewer{};
    publicViewer.room = hello.room;
    publicViewer.party = SpectatorParty::Viewer;
    publicViewer.route = SpectatorRouteKind::Relay;
    publicViewer.admission = SpectatorAdmission::PublicToken;
    publicViewer.sourceToken =
        RegistrationToken{ 0x12345678ull, 0x90ABCDEFull };
    publicViewer.sessionId = 0x4242;
    publicViewer.resumeRecord = 15;
    publicViewer.resumeDigest = 0x1234ABCDull;
    QNP_REQUIRE(encodeSpectatorHello(publicViewer, encoded));
    SpectatorHello decodedPublic{};
    QNP_CHECK(decodeSpectatorHello(
                  encoded.data(), encoded.size(), decodedPublic)
              == DecodeError::None);
    QNP_CHECK(decodedPublic.admission
              == SpectatorAdmission::PublicToken);
    QNP_CHECK(decodedPublic.sourceToken
              == publicViewer.sourceToken);
    QNP_CHECK(decodedPublic.viewerId == 0);
    QNP_CHECK(decodedPublic.grantId == 0);
    QNP_CHECK(decodedPublic.sourceGeneration == 0);
    QNP_CHECK(decodedPublic.macLow == 0 && decodedPublic.macHigh == 0);

    SpectatorHello forgedPublic = publicViewer;
    forgedPublic.viewerId = 99;
    QNP_CHECK(!encodeSpectatorHello(forgedPublic, rejected));
    forgedPublic = publicViewer;
    forgedPublic.sourceGeneration = 2;
    QNP_CHECK(!encodeSpectatorHello(forgedPublic, rejected));
    forgedPublic = publicViewer;
    forgedPublic.macLow = 1;
    QNP_CHECK(!encodeSpectatorHello(forgedPublic, rejected));

    SpectatorFrameHeader frame{};
    frame.party = SpectatorParty::Source;
    frame.viewerId = 9;
    frame.sourceGeneration = 2;
    std::vector<uint8_t> frameBytes;
    QNP_REQUIRE(encodeSpectatorFrameHeader(frame, frameBytes));
    QNP_CHECK(frameBytes.size() == kSpectatorFrameHeaderSize);
    QNP_CHECK(hasSpectatorFrameKind(frameBytes.data(), frameBytes.size()));
    SpectatorMessageKind kind{};
    QNP_CHECK(inspectSpectatorMessageKind(
                  frameBytes.data(), frameBytes.size(), kind)
              == DecodeError::BadKind);
    SpectatorFrameHeader decodedFrame{};
    QNP_CHECK(decodeSpectatorFrameHeader(
                  frameBytes.data(), frameBytes.size(), decodedFrame)
              == DecodeError::None);
    QNP_CHECK(decodedFrame.viewerId == 9);
    QNP_CHECK(decodedFrame.party == SpectatorParty::Source);
}

QNP_TEST(relay_observed_endpoint_extension_is_explicitly_versioned) {
    using namespace qnet::relay;

    ObservedRequest request{};
    request.room = RoomId{ 0xDEADBEEFull, 0xFEEDFACEull };
    request.domain = LaneDomain::Player;
    request.role = RelayRole::Host;
    request.token = RegistrationToken{ 0x1111, 0x2222 };
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodeObservedRequest(request, encoded));
    QNP_CHECK(encoded.size() == kObservedRequestSize);

    uint16_t version = 0;
    QNP_CHECK(peekRelayVersion(encoded.data(), encoded.size(), version));
    QNP_CHECK(version == kObservedVersion);

    QNP_CHECK(kObservedRequestSize == kRegisterSize);
    Register asRegister{};
    QNP_CHECK(decodeRegister(encoded.data(), encoded.size(), asRegister)
              == DecodeError::BadVersion);

    ObservedRequest decodedRequest{};
    QNP_CHECK(decodeObservedRequest(
                  encoded.data(), encoded.size(), decodedRequest)
              == DecodeError::None);
    QNP_CHECK(decodedRequest.room == request.room);
    QNP_CHECK(decodedRequest.token == request.token);
    QNP_CHECK(decodedRequest.role == RelayRole::Host);

    ObservedEndpoint observed{};
    observed.room = request.room;
    observed.domain = LaneDomain::Player;
    observed.role = RelayRole::Host;
    observed.token = request.token;
    observed.observed = Ipv4Endpoint{ 0x0100007Fu, 40000 };
    QNP_REQUIRE(encodeObservedEndpoint(observed, encoded));
    QNP_CHECK(encoded.size() == kObservedSize);
    ObservedEndpoint decodedObserved{};
    QNP_CHECK(decodeObservedEndpoint(
                  encoded.data(), encoded.size(), decodedObserved)
              == DecodeError::None);
    QNP_CHECK(decodedObserved.observed == observed.observed);
    QNP_CHECK(decodedObserved.token == request.token);

    observed.observed = Ipv4Endpoint{ 0, 0 };
    std::vector<uint8_t> invalid;
    QNP_CHECK(!encodeObservedEndpoint(observed, invalid));

    ObservedRequest tokenless = request;
    tokenless.token = RegistrationToken{ 0, 0 };
    QNP_CHECK(!encodeObservedRequest(tokenless, invalid));
}

QNP_TEST(relay_topology_envelope_inspection_accepts_only_exact_shape) {
    const std::vector<uint8_t> envelope = validTopologyEnvelope();
    QNP_REQUIRE(!envelope.empty());

    TopologyDatagramInfo info{};
    QNP_CHECK(inspectTopologyControlDatagram(
                  envelope.data(), envelope.size(), info)
              == TopologyDatagramError::None);
    QNP_CHECK(info.authorityGeneration == 0x0101010101010101ull);
    QNP_CHECK(info.nodeId == 0x0202020202020202ull);
    QNP_CHECK(info.sequence == 0x0303030303030303ull);
    QNP_CHECK(info.innerType
              == rwire::ControlMessageType::AuthorityState);
    QNP_CHECK(info.innerLength
              == envelope.size() - rwire::kTopologyQtaEnvelopePrefixBytes);

    const auto untouched = [](const TopologyDatagramInfo& probe) {
        return probe.authorityGeneration == 0u && probe.nodeId == 0u
            && probe.sequence == 0u && probe.innerLength == 0u;
    };

    TopologyDatagramInfo failed{};
    QNP_CHECK(inspectTopologyControlDatagram(nullptr, envelope.size(), failed)
              == TopologyDatagramError::TooSmall);
    QNP_CHECK(inspectTopologyControlDatagram(
                  envelope.data(),
                  rwire::kTopologyQtaEnvelopePrefixBytes
                      + rwire::kResilientControlHeaderBytes - 1u,
                  failed)
              == TopologyDatagramError::TooSmall);
    std::vector<uint8_t> oversized = envelope;
    oversized.resize(rwire::kTopologyQtaEnvelopePrefixBytes
                         + rwire::kMaxResilientControlDatagramBytes + 1u,
                     0u);
    QNP_CHECK(inspectTopologyControlDatagram(
                  oversized.data(), oversized.size(), failed)
              == TopologyDatagramError::TooLarge);

    std::vector<uint8_t> corrupted = envelope;
    corrupted[0] ^= 0x01u;
    QNP_CHECK(inspectTopologyControlDatagram(
                  corrupted.data(), corrupted.size(), failed)
              == TopologyDatagramError::InvalidFraming);
    corrupted = envelope;
    corrupted[4] ^= 0x01u;
    QNP_CHECK(inspectTopologyControlDatagram(
                  corrupted.data(), corrupted.size(), failed)
              == TopologyDatagramError::InvalidFraming);
    corrupted = envelope;
    corrupted[6] ^= 0x01u;
    QNP_CHECK(inspectTopologyControlDatagram(
                  corrupted.data(), corrupted.size(), failed)
              == TopologyDatagramError::InvalidFraming);
    corrupted = envelope;
    corrupted[7] = 0x5Au;
    QNP_CHECK(inspectTopologyControlDatagram(
                  corrupted.data(), corrupted.size(), failed)
              == TopologyDatagramError::InvalidFraming);
    corrupted = envelope;
    corrupted[8] ^= 0x01u;
    QNP_CHECK(inspectTopologyControlDatagram(
                  corrupted.data(), corrupted.size(), failed)
              == TopologyDatagramError::InvalidFraming);

    corrupted = envelope;
    std::fill(corrupted.begin() + 12, corrupted.begin() + 20, uint8_t{0u});
    QNP_CHECK(inspectTopologyControlDatagram(
                  corrupted.data(), corrupted.size(), failed)
              == TopologyDatagramError::InvalidIdentity);
    corrupted = envelope;
    std::fill(corrupted.begin() + 20, corrupted.begin() + 28, uint8_t{0u});
    QNP_CHECK(inspectTopologyControlDatagram(
                  corrupted.data(), corrupted.size(), failed)
              == TopologyDatagramError::InvalidIdentity);
    corrupted = envelope;
    std::fill(corrupted.begin() + 28, corrupted.begin() + 36, uint8_t{0u});
    QNP_CHECK(inspectTopologyControlDatagram(
                  corrupted.data(), corrupted.size(), failed)
              == TopologyDatagramError::InvalidIdentity);
    corrupted = envelope;
    std::fill(corrupted.begin() + 36, corrupted.begin() + 52, uint8_t{0u});
    QNP_CHECK(inspectTopologyControlDatagram(
                  corrupted.data(), corrupted.size(), failed)
              == TopologyDatagramError::InvalidIdentity);

    corrupted = envelope;
    corrupted.back() ^= 0x01u;
    QNP_CHECK(inspectTopologyControlDatagram(
                  corrupted.data(), corrupted.size(), failed)
              == TopologyDatagramError::InvalidPayload);
    QNP_CHECK(untouched(failed));
}

QNP_TEST(relay_topology_datagrams_never_cross_lanes) {
    const std::vector<uint8_t> envelope = validTopologyEnvelope();
    const std::vector<uint8_t> player = validHelloDatagram();
    const std::vector<uint8_t> spectator =
        viewerHelloDatagram(0x77u, 1u);
    QNP_REQUIRE(!envelope.empty());
    QNP_REQUIRE(!player.empty());
    QNP_REQUIRE(!spectator.empty());

    PlayerDatagramInfo playerInfo{};
    QNP_CHECK(inspectPlayerDatagram(envelope.data(), envelope.size(),
                                    playerInfo)
              != PlayerDatagramError::None);
    SpectatorDatagramInfo spectatorInfo{};
    QNP_CHECK(inspectSpectatorDatagram(envelope.data(), envelope.size(),
                                       qsp::PeerRole::Viewer, spectatorInfo)
              != SpectatorDatagramError::None);
    QNP_CHECK(inspectSpectatorDatagram(envelope.data(), envelope.size(),
                                       qsp::PeerRole::Source, spectatorInfo)
              != SpectatorDatagramError::None);

    TopologyDatagramInfo topologyInfo{};
    QNP_CHECK(inspectTopologyControlDatagram(player.data(), player.size(),
                                             topologyInfo)
              != TopologyDatagramError::None);
    QNP_CHECK(inspectTopologyControlDatagram(
                  spectator.data(), spectator.size(), topologyInfo)
              != TopologyDatagramError::None);
    const std::vector<uint8_t> bare = validInnerControlDatagram();
    QNP_REQUIRE(!bare.empty());
    QNP_CHECK(inspectTopologyControlDatagram(bare.data(), bare.size(),
                                             topologyInfo)
              != TopologyDatagramError::None);
}

QNP_TEST(relay_topology_wire_extension_is_explicitly_versioned) {
    using namespace qnet::relay;
    const RoomId room{0x1111222233334444ull, 0x5555666677778888ull};
    const RegistrationToken token{0xAAAABBBBCCCCDDDDull,
                                  0xEEEEFFFF00001111ull};

    TopologyHello authorityHello{};
    authorityHello.room = room;
    authorityHello.party = TopologyParty::Authority;
    authorityHello.admission = TopologyAdmission::None;
    authorityHello.authorityToken = token;
    authorityHello.sessionId = 0x5151515151515151ull;
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodeTopologyHello(authorityHello, encoded));
    QNP_CHECK(encoded.size() == kTopologyHelloSize);
    uint16_t version = 0u;
    QNP_CHECK(peekRelayVersion(encoded.data(), encoded.size(), version));
    QNP_CHECK(version == kTopologyVersion);
    QNP_CHECK(version != kVersion);
    QNP_CHECK(version != kSpectatorVersion);
    TopologyHello decodedHello{};
    QNP_CHECK(decodeTopologyHello(encoded.data(), encoded.size(),
                                  decodedHello)
              == DecodeError::None);
    QNP_CHECK(decodedHello.room == room);
    QNP_CHECK(decodedHello.party == TopologyParty::Authority);
    QNP_CHECK(decodedHello.authorityToken == token);
    QNP_CHECK(decodedHello.sessionId == authorityHello.sessionId);

    TopologyHello nodeHello{};
    nodeHello.room = room;
    nodeHello.party = TopologyParty::Node;
    nodeHello.admission = TopologyAdmission::Grant;
    nodeHello.sessionId = 0x5151515151515151ull;
    nodeHello.grantId = 0x1234u;
    nodeHello.nodeId = 0x77u;
    nodeHello.expiryUnix = 1700000300u;
    nodeHello.authorityGeneration = 2u;
    nodeHello.macLow = 0xA1u;
    nodeHello.macHigh = 0xB2u;
    QNP_REQUIRE(encodeTopologyHello(nodeHello, encoded));
    QNP_CHECK(decodeTopologyHello(encoded.data(), encoded.size(),
                                  decodedHello)
              == DecodeError::None);
    QNP_CHECK(decodedHello.grantId == nodeHello.grantId);
    QNP_CHECK(decodedHello.nodeId == nodeHello.nodeId);
    QNP_CHECK(decodedHello.authorityGeneration == 2u);

    TopologyHello smuggled = nodeHello;
    smuggled.authorityToken = token;
    std::vector<uint8_t> refused{0xA5u};
    QNP_CHECK(!encodeTopologyHello(smuggled, refused));
    QNP_CHECK(refused.size() == 1u && refused[0] == 0xA5u);
    smuggled = authorityHello;
    smuggled.grantId = 1u;
    QNP_CHECK(!encodeTopologyHello(smuggled, refused));
    smuggled = nodeHello;
    smuggled.macLow = 0u;
    smuggled.macHigh = 0u;
    QNP_CHECK(!encodeTopologyHello(smuggled, refused));
    smuggled = nodeHello;
    smuggled.admission = TopologyAdmission::None;
    QNP_CHECK(!encodeTopologyHello(smuggled, refused));

    QNP_REQUIRE(encodeTopologyHello(nodeHello, encoded));
    encoded[25] = static_cast<uint8_t>(TopologyParty::Authority);
    QNP_CHECK(decodeTopologyHello(encoded.data(), encoded.size(),
                                  decodedHello)
              != DecodeError::None);

    TopologyWelcome welcome{};
    welcome.room = room;
    welcome.party = TopologyParty::Authority;
    welcome.status = TopologyStatus::Accepted;
    welcome.registrationToken = token;
    welcome.sessionId = 0x5151515151515151ull;
    welcome.authorityGeneration = 2u;
    welcome.nodeCount = 1u;
    welcome.nodeCapacity = 8u;
    welcome.grantKey.assign(kGrantKeyBytes, 0u);
    for (size_t index = 0u; index < kGrantKeyBytes; ++index)
        welcome.grantKey[index] = static_cast<uint8_t>(0x30u + index);
    QNP_REQUIRE(encodeTopologyWelcome(welcome, encoded));
    QNP_CHECK(encoded.size() == kTopologyWelcomeSize);
    TopologyWelcome decodedWelcome{};
    QNP_CHECK(decodeTopologyWelcome(encoded.data(), encoded.size(),
                                    decodedWelcome)
              == DecodeError::None);
    QNP_CHECK(decodedWelcome.grantKey == welcome.grantKey);
    QNP_CHECK(decodedWelcome.nodeCapacity == 8u);

    TopologyWelcome nodeWelcome = welcome;
    nodeWelcome.party = TopologyParty::Node;
    nodeWelcome.nodeId = 0x77u;
    QNP_CHECK(!encodeTopologyWelcome(nodeWelcome, refused));
    nodeWelcome.grantKey.clear();
    QNP_REQUIRE(encodeTopologyWelcome(nodeWelcome, encoded));
    QNP_CHECK(decodeTopologyWelcome(encoded.data(), encoded.size(),
                                    decodedWelcome)
              == DecodeError::None);
    QNP_CHECK(decodedWelcome.grantKey.empty());
    QNP_CHECK(decodedWelcome.nodeId == 0x77u);

    TopologyWelcome refusedWelcome{};
    refusedWelcome.room = room;
    refusedWelcome.party = TopologyParty::Node;
    refusedWelcome.status = TopologyStatus::GrantRejected;
    refusedWelcome.retryAfterMs = 250u;
    QNP_REQUIRE(encodeTopologyWelcome(refusedWelcome, encoded));
    QNP_CHECK(decodeTopologyWelcome(encoded.data(), encoded.size(),
                                    decodedWelcome)
              == DecodeError::None);
    QNP_CHECK(decodedWelcome.retryAfterMs == 250u);
    refusedWelcome.registrationToken = token;
    QNP_CHECK(!encodeTopologyWelcome(refusedWelcome, refused));

    TopologyRegister nodeRegister{};
    nodeRegister.room = room;
    nodeRegister.party = TopologyParty::Node;
    nodeRegister.authorityGeneration = 2u;
    nodeRegister.token = token;
    nodeRegister.nodeId = 0x77u;
    QNP_REQUIRE(encodeTopologyRegister(nodeRegister, encoded));
    QNP_CHECK(encoded.size() == kTopologyRegisterSize);
    TopologyRegister decodedRegister{};
    QNP_CHECK(decodeTopologyRegister(encoded.data(), encoded.size(),
                                     decodedRegister)
              == DecodeError::None);
    QNP_CHECK(decodedRegister.nodeId == 0x77u);
    TopologyRegister badRegister = nodeRegister;
    badRegister.nodeId = 0u;
    QNP_CHECK(!encodeTopologyRegister(badRegister, refused));
    badRegister = nodeRegister;
    badRegister.party = TopologyParty::Authority;
    QNP_CHECK(!encodeTopologyRegister(badRegister, refused));
    badRegister = nodeRegister;
    badRegister.authorityGeneration = 0u;
    QNP_CHECK(!encodeTopologyRegister(badRegister, refused));

    TopologyReady ready{};
    ready.room = room;
    ready.party = TopologyParty::Authority;
    ready.authorityGeneration = 2u;
    ready.token = token;
    ready.nodeId = 0x77u;
    ready.peer = Ipv4Endpoint{0x0100007Fu, 40001u};
    QNP_REQUIRE(encodeTopologyReady(ready, encoded));
    QNP_CHECK(encoded.size() == kTopologyReadySize);
    TopologyReady decodedReady{};
    QNP_CHECK(decodeTopologyReady(encoded.data(), encoded.size(),
                                  decodedReady)
              == DecodeError::None);
    QNP_CHECK(decodedReady.peer == ready.peer);

    QNP_CHECK(encoded.size() == kSpectatorRegisterSize);
    SpectatorRegister collided{};
    QNP_CHECK(decodeSpectatorRegister(encoded.data(), encoded.size(),
                                      collided)
              == DecodeError::BadVersion);

    TopologyDetach detach{};
    detach.room = room;
    detach.party = TopologyParty::Authority;
    detach.reason = TopologyDetachReason::ServiceStopping;
    detach.authorityGeneration = 2u;
    detach.token = token;
    detach.nodeId = 0u;
    detach.retryAfterMs = 500u;
    QNP_REQUIRE(encodeTopologyDetach(detach, encoded));
    QNP_CHECK(encoded.size() == kTopologyDetachSize);
    QNP_CHECK(encoded.size() == kSpectatorDetachSize);
    SpectatorDetach collidedDetach{};
    QNP_CHECK(decodeSpectatorDetach(encoded.data(), encoded.size(),
                                    collidedDetach)
              == DecodeError::BadVersion);
    TopologyDetach decodedDetach{};
    QNP_CHECK(decodeTopologyDetach(encoded.data(), encoded.size(),
                                   decodedDetach)
              == DecodeError::None);
    QNP_CHECK(decodedDetach.reason == TopologyDetachReason::ServiceStopping);
    QNP_CHECK(decodedDetach.nodeId == 0u);

    TopologyFrameHeader frame{};
    frame.party = TopologyParty::Node;
    frame.nodeId = 0x77u;
    frame.authorityGeneration = 2u;
    QNP_REQUIRE(encodeTopologyFrameHeader(frame, encoded));
    QNP_CHECK(encoded.size() == kTopologyFrameHeaderSize);
    QNP_CHECK(hasTopologyFrameKind(encoded.data(), encoded.size()));
    QNP_CHECK(!hasSpectatorFrameKind(encoded.data(), encoded.size()));
    TopologyFrameHeader decodedFrame{};
    QNP_CHECK(decodeTopologyFrameHeader(encoded.data(), encoded.size(),
                                        decodedFrame)
              == DecodeError::None);
    QNP_CHECK(decodedFrame.nodeId == 0x77u);
    TopologyMessageKind controlKind{};
    QNP_CHECK(inspectTopologyMessageKind(encoded.data(), encoded.size(),
                                         controlKind)
              == DecodeError::BadKind);
    TopologyFrameHeader badFrame = frame;
    badFrame.nodeId = 0u;
    QNP_CHECK(!encodeTopologyFrameHeader(badFrame, refused));
    badFrame = frame;
    badFrame.authorityGeneration = 0u;
    QNP_CHECK(!encodeTopologyFrameHeader(badFrame, refused));

    QNP_REQUIRE(encodeTopologyHello(nodeHello, encoded));
    MessageKind playerKind{};
    QNP_CHECK(inspectMessageKind(encoded.data(), encoded.size(), playerKind)
              == DecodeError::BadVersion);
    SpectatorMessageKind spectatorKind{};
    QNP_CHECK(inspectSpectatorMessageKind(encoded.data(), encoded.size(),
                                          spectatorKind)
              == DecodeError::BadVersion);
}
