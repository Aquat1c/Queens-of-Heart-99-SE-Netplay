#include "TestHarness.h"

#include "../../net/RelayProtocol.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace qnet::relay;

RoomId roomId() {
    return RoomId{
        0x0102030405060708ull,
        0x1112131415161718ull,
    };
}

RegistrationToken playerToken() {
    return RegistrationToken{
        0x2122232425262728ull,
        0x3132333435363738ull,
    };
}

RegistrationToken spectatorToken() {
    return RegistrationToken{
        0x4142434445464748ull,
        0x5152535455565758ull,
    };
}

RegistrationToken viewerToken() {
    return RegistrationToken{
        0x6162636465666768ull,
        0x7172737475767778ull,
    };
}

Welcome acceptedHostWelcome() {
    Welcome welcome{};
    welcome.room = roomId();
    welcome.domain = LaneDomain::Player;
    welcome.role = RelayRole::Host;
    welcome.status = WelcomeStatus::Accepted;
    welcome.registrationToken = playerToken();
    welcome.spectatorRegistrationToken = spectatorToken();
    welcome.viewerAccessToken = viewerToken();
    return welcome;
}

}

QNP_TEST(RelayRoomIdHexIsCanonicalAndAtomic) {
    const RoomId expected = roomId();
    QNP_CHECK(formatRoomId(expected)
              == "11121314151617180102030405060708");

    RoomId parsed{};
    QNP_CHECK(parseRoomId(
        "11121314151617180102030405060708", parsed));
    QNP_CHECK(parsed == expected);
    QNP_CHECK(parseRoomId(
        "111213141516171801020304050607aB", parsed));
    QNP_CHECK(parsed.high == expected.high);
    QNP_CHECK(parsed.low == 0x01020304050607ABull);

    const RoomId sentinel{
        0xAAAAAAAAAAAAAAAAull,
        0xBBBBBBBBBBBBBBBBull,
    };
    parsed = sentinel;
    QNP_CHECK(!parseRoomId("", parsed));
    QNP_CHECK(parsed == sentinel);
    QNP_CHECK(!parseRoomId(
        "0x11121314151617180102030405060708", parsed));
    QNP_CHECK(parsed == sentinel);
    QNP_CHECK(!parseRoomId(
        "1112131415161718010203040506070Z", parsed));
    QNP_CHECK(parsed == sentinel);
    QNP_CHECK(!parseRoomId(
        "00000000000000000000000000000000", parsed));
    QNP_CHECK(parsed == sentinel);
}

QNP_TEST(RelayHelloGoldenLittleEndianAndRoundTrip) {
    Hello hello{};
    hello.room = roomId();
    hello.domain = LaneDomain::Player;
    hello.role = RelayRole::Client;

    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodeHello(hello, encoded));
    QNP_REQUIRE(encoded.size() == kHelloSize);
    const std::array<uint8_t, kHelloSize> expected{{
        0x51, 0x52, 0x4C, 0x31,
        0x01, 0x00,
        0x01, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
        0x01, 0x02,
        0x00, 0x00,
    }};
    QNP_CHECK(std::equal(
        expected.begin(), expected.end(), encoded.begin()));

    MessageKind kind = MessageKind::Ready;
    QNP_CHECK(inspectMessageKind(
                  encoded.data(), encoded.size(), kind)
              == DecodeError::None);
    QNP_CHECK(kind == MessageKind::Hello);
    QNP_CHECK(hasRelayMagic(encoded.data(), encoded.size()));
    QNP_CHECK(encodedSize(kind) == encoded.size());
    QNP_CHECK(kTcpEnvelopeType == 0x70u);

    Hello decoded{};
    QNP_CHECK(decodeHello(
                  encoded.data(), encoded.size(), decoded)
              == DecodeError::None);
    QNP_CHECK(decoded.room == hello.room);
    QNP_CHECK(decoded.domain == hello.domain);
    QNP_CHECK(decoded.role == hello.role);
}

QNP_TEST(RelayWelcomeEnforcesRoleScopedCapabilities) {
    Welcome host = acceptedHostWelcome();
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodeWelcome(host, encoded));
    QNP_REQUIRE(encoded.size() == kWelcomeSize);
    QNP_CHECK(encoded[6] == static_cast<uint8_t>(
                  MessageKind::Welcome));
    QNP_CHECK(encoded[24] == static_cast<uint8_t>(
                  LaneDomain::Player));
    QNP_CHECK(encoded[25] == static_cast<uint8_t>(
                  RelayRole::Host));
    QNP_CHECK(encoded[26] == static_cast<uint8_t>(
                  WelcomeStatus::Accepted));
    const uint8_t expectedTokenPrefix[] = {
        0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
    };
    QNP_CHECK(std::equal(
        std::begin(expectedTokenPrefix),
        std::end(expectedTokenPrefix),
        encoded.begin() + 28));

    Welcome decoded{};
    QNP_CHECK(decodeWelcome(
                  encoded.data(), encoded.size(), decoded)
              == DecodeError::None);
    QNP_CHECK(decoded.room == host.room);
    QNP_CHECK(decoded.registrationToken
              == host.registrationToken);
    QNP_CHECK(decoded.spectatorRegistrationToken
              == host.spectatorRegistrationToken);
    QNP_CHECK(decoded.viewerAccessToken
              == host.viewerAccessToken);

    Welcome client = host;
    client.role = RelayRole::Client;
    client.spectatorRegistrationToken = {};
    client.viewerAccessToken = {};
    QNP_CHECK(encodeWelcome(client, encoded));
    QNP_CHECK(decodeWelcome(
                  encoded.data(), encoded.size(), decoded)
              == DecodeError::None);
    QNP_CHECK(decoded.role == RelayRole::Client);
    QNP_CHECK(!isValidId(decoded.spectatorRegistrationToken));
    QNP_CHECK(!isValidId(decoded.viewerAccessToken));

    std::vector<uint8_t> untouched{0xA5};
    Welcome invalid = host;
    invalid.spectatorRegistrationToken = {};
    QNP_CHECK(!encodeWelcome(invalid, untouched));
    QNP_CHECK(untouched == std::vector<uint8_t>({0xA5}));

    invalid = host;
    invalid.viewerAccessToken = invalid.registrationToken;
    QNP_CHECK(!encodeWelcome(invalid, untouched));
    QNP_CHECK(untouched == std::vector<uint8_t>({0xA5}));

    invalid = client;
    invalid.viewerAccessToken = viewerToken();
    QNP_CHECK(!encodeWelcome(invalid, untouched));
    QNP_CHECK(untouched == std::vector<uint8_t>({0xA5}));

    Welcome waiting{};
    waiting.room = roomId();
    waiting.domain = LaneDomain::Player;
    waiting.role = RelayRole::Host;
    waiting.status = WelcomeStatus::Waiting;
    QNP_CHECK(encodeWelcome(waiting, encoded));
    QNP_CHECK(decodeWelcome(
                  encoded.data(), encoded.size(), decoded)
              == DecodeError::None);
    waiting.registrationToken = playerToken();
    QNP_CHECK(!encodeWelcome(waiting, untouched));

    QNP_REQUIRE(encodeWelcome(host, encoded));
    QNP_REQUIRE(encoded.size() == kWelcomeSize);
    std::fill(encoded.begin() + 44, encoded.begin() + 60, uint8_t{0});
    decoded = client;
    QNP_CHECK(decodeWelcome(
                  encoded.data(), encoded.size(), decoded)
              == DecodeError::InvalidToken);
    QNP_CHECK(decoded.role == RelayRole::Client);
}

QNP_TEST(RelayUdpRegisterAndReadyRoundTrip) {
    Register registration{};
    registration.room = roomId();
    registration.domain = LaneDomain::Player;
    registration.role = RelayRole::Client;
    registration.token = playerToken();

    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodeRegister(registration, encoded));
    QNP_REQUIRE(encoded.size() == kRegisterSize);
    QNP_CHECK(encoded[6] == static_cast<uint8_t>(
                  MessageKind::Register));
    Register decodedRegistration{};
    QNP_CHECK(decodeRegister(
                  encoded.data(), encoded.size(), decodedRegistration)
              == DecodeError::None);
    QNP_CHECK(decodedRegistration.room == registration.room);
    QNP_CHECK(decodedRegistration.domain == registration.domain);
    QNP_CHECK(decodedRegistration.role == registration.role);
    QNP_CHECK(decodedRegistration.token == registration.token);

    Ready ready{};
    ready.room = roomId();
    ready.domain = LaneDomain::Player;
    ready.role = RelayRole::Client;
    ready.token = playerToken();
    ready.peer = Ipv4Endpoint{0x0100007Fu, 0x3456u};
    QNP_REQUIRE(encodeReady(ready, encoded));
    QNP_REQUIRE(encoded.size() == kReadySize);
    const uint8_t expectedEndpoint[] = {
        0x7F, 0x00, 0x00, 0x01, 0x56, 0x34,
    };
    QNP_CHECK(std::equal(
        std::begin(expectedEndpoint), std::end(expectedEndpoint),
        encoded.begin() + 44));
    Ready decodedReady{};
    QNP_CHECK(decodeReady(
                  encoded.data(), encoded.size(), decodedReady)
              == DecodeError::None);
    QNP_CHECK(decodedReady.room == ready.room);
    QNP_CHECK(decodedReady.domain == ready.domain);
    QNP_CHECK(decodedReady.role == ready.role);
    QNP_CHECK(decodedReady.token == ready.token);
    QNP_CHECK(decodedReady.peer == ready.peer);

    std::vector<uint8_t> untouched{0x5A};
    Ready invalid = ready;
    invalid.peer.port = 0;
    QNP_CHECK(!encodeReady(invalid, untouched));
    QNP_CHECK(untouched == std::vector<uint8_t>({0x5A}));
    invalid = ready;
    invalid.token = {};
    QNP_CHECK(!encodeReady(invalid, untouched));
    QNP_CHECK(untouched == std::vector<uint8_t>({0x5A}));
}

QNP_TEST(RelayCodecRejectsMalformedRecordsAtomically) {
    Register registration{};
    registration.room = roomId();
    registration.domain = LaneDomain::Player;
    registration.role = RelayRole::Host;
    registration.token = playerToken();
    std::vector<uint8_t> valid;
    QNP_REQUIRE(encodeRegister(registration, valid));
    QNP_REQUIRE(valid.size() == kRegisterSize);

    Register output{};
    output.room = RoomId{0xAA, 0xBB};
    output.domain = LaneDomain::Spectator;
    output.role = RelayRole::Client;
    output.token = RegistrationToken{0xCC, 0xDD};
    const Register sentinel = output;

    QNP_CHECK(decodeRegister(
                  valid.data(), kHeaderSize - 1u, output)
              == DecodeError::TooShort);
    QNP_CHECK(output.room == sentinel.room);

    std::vector<uint8_t> malformed = valid;
    malformed[0] ^= 0xFFu;
    malformed[4] ^= 0xFFu;
    QNP_CHECK(decodeRegister(
                  malformed.data(), malformed.size(), output)
              == DecodeError::BadMagic);
    QNP_CHECK(output.token == sentinel.token);

    malformed = valid;
    malformed[4] = 2u;
    malformed[7] = 1u;
    QNP_CHECK(decodeRegister(
                  malformed.data(), malformed.size(), output)
              == DecodeError::BadVersion);

    malformed = valid;
    malformed[6] = 0x7Fu;
    QNP_CHECK(decodeRegister(
                  malformed.data(), malformed.size(), output)
              == DecodeError::BadKind);

    malformed = valid;
    malformed.pop_back();
    QNP_CHECK(decodeRegister(
                  malformed.data(), malformed.size(), output)
              == DecodeError::BadLength);

    malformed = valid;
    malformed[7] = 1u;
    QNP_CHECK(decodeRegister(
                  malformed.data(), malformed.size(), output)
              == DecodeError::BadReserved);

    malformed = valid;
    malformed[26] = 1u;
    QNP_CHECK(decodeRegister(
                  malformed.data(), malformed.size(), output)
              == DecodeError::BadReserved);

    malformed = valid;
    malformed[24] = 0x7Fu;
    QNP_CHECK(decodeRegister(
                  malformed.data(), malformed.size(), output)
              == DecodeError::BadDomain);

    malformed = valid;
    malformed[25] = 0x7Fu;
    QNP_CHECK(decodeRegister(
                  malformed.data(), malformed.size(), output)
              == DecodeError::BadRole);

    malformed = valid;
    std::fill(
        malformed.begin() + 8, malformed.begin() + 24, uint8_t{0});
    QNP_CHECK(decodeRegister(
                  malformed.data(), malformed.size(), output)
              == DecodeError::InvalidRoom);

    malformed = valid;
    std::fill(
        malformed.begin() + 32, malformed.end(), uint8_t{0});
    QNP_CHECK(decodeRegister(
                  malformed.data(), malformed.size(), output)
              == DecodeError::InvalidToken);
    QNP_CHECK(output.room == sentinel.room);
    QNP_CHECK(output.domain == sentinel.domain);
    QNP_CHECK(output.role == sentinel.role);
    QNP_CHECK(output.token == sentinel.token);

    MessageKind kind = MessageKind::Ready;
    malformed = valid;
    malformed[7] = 1u;
    QNP_CHECK(inspectMessageKind(
                  malformed.data(), malformed.size(), kind)
              == DecodeError::BadReserved);
    QNP_CHECK(kind == MessageKind::Ready);

    Hello helloOutput{};
    std::vector<uint8_t> registerRecord;
    QNP_CHECK(encodeRegister(registration, registerRecord));
    QNP_CHECK(decodeHello(
                  registerRecord.data(), registerRecord.size(),
                  helloOutput)
              == DecodeError::BadKind);
}

QNP_TEST(RelayLaneDomainIsExplicitAndNeverInferredFromToken) {
    Register player{};
    player.room = roomId();
    player.domain = LaneDomain::Player;
    player.role = RelayRole::Host;
    player.token = playerToken();
    Register spectator = player;
    spectator.domain = LaneDomain::Spectator;

    std::vector<uint8_t> playerBytes;
    std::vector<uint8_t> spectatorBytes;
    QNP_REQUIRE(encodeRegister(player, playerBytes));
    QNP_REQUIRE(encodeRegister(spectator, spectatorBytes));
    QNP_REQUIRE(playerBytes.size() == kRegisterSize);
    QNP_REQUIRE(spectatorBytes.size() == kRegisterSize);
    QNP_CHECK(playerBytes[24] != spectatorBytes[24]);
    QNP_CHECK(std::equal(
        playerBytes.begin() + 32, playerBytes.end(),
        spectatorBytes.begin() + 32));

    Register decoded{};
    QNP_CHECK(decodeRegister(
                  playerBytes.data(), playerBytes.size(), decoded)
              == DecodeError::None);
    QNP_CHECK(decoded.domain == LaneDomain::Player);
    QNP_CHECK(decodeRegister(
                  spectatorBytes.data(), spectatorBytes.size(), decoded)
              == DecodeError::None);
    QNP_CHECK(decoded.domain == LaneDomain::Spectator);
}
