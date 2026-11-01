#include "TestHarness.h"

#include "../WireProtocol.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace qnetplay::wire;

constexpr uint64_t kTestSession = 0x3132333435363738ull;

SessionCookie testCookie() {
    return SessionCookie{
        0x0102030405060708ull,
        0x1112131415161718ull,
    };
}

qnetplay::compat::CompatibilityIdentityWire testIdentity(
    const char* const profileId = "qoh99-se1126") {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = kVersion;
    QNP_CHECK(qnetplay::compat::setProfileId(identity, profileId));
    for (size_t i = 0u;
         i < qnetplay::compat::kCompatibilitySha256Bytes; ++i) {
        identity.runtimeBuild.executableSha256[i] =
            static_cast<uint8_t>(0x10u + i);
        identity.simulationContent.simulationRootSha256[i] =
            static_cast<uint8_t>(0x40u + i);
        identity.simulationContent.validationRootSha256[i] =
            static_cast<uint8_t>(0x80u + i);
    }
    identity.runtimeBuild.executableSize = 0x0102030405060708ull;
    identity.runtimeBuild.peTimestamp = 0x11223344u;
    identity.simulationContent.manifestSchemaVersion = 0x01020304u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.simulationContent.entryCount = 0x10203040u;
    identity.simulationContent.requiredEntryCount = 0x01020304u;
    identity.stateSchemaId = 0x1112131415161718ull;
    identity.modSimulationId = 0x2122232425262728ull;
    identity.timingProfileId = 0x3132333435363738ull;
    return identity;
}

std::vector<uint8_t> helloDatagram() {
    HelloPayload hello{
        0x2122232425262728ull,
        PeerRole::Client,
        1,
        testIdentity(),
    };
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    QNP_CHECK(encodePayload(hello, payload));
    QNP_CHECK(encodeDatagram(MessageType::Hello, kTestSession, testCookie(),
                             0xA1B2C3D4u, payload, packet));
    return packet;
}

}

QNP_TEST(WireHelloGoldenLittleEndianAndRoundTrip) {
    using namespace qnetplay::wire;
    const std::vector<uint8_t> packet = helloDatagram();
    QNP_REQUIRE(packet.size() == kHeaderSize
                + payloadSize(MessageType::Hello));

    const SessionCookie expectedCookie = testCookie();
    const auto writeLe16 = [](uint8_t* const out, const uint16_t value) {
        out[0] = static_cast<uint8_t>(value);
        out[1] = static_cast<uint8_t>(value >> 8);
    };
    const auto writeLe32 = [](uint8_t* const out, const uint32_t value) {
        out[0] = static_cast<uint8_t>(value);
        out[1] = static_cast<uint8_t>(value >> 8);
        out[2] = static_cast<uint8_t>(value >> 16);
        out[3] = static_cast<uint8_t>(value >> 24);
    };
    const auto writeLe64 = [](uint8_t* const out, const uint64_t value) {
        for (unsigned i = 0u; i < 8u; ++i)
            out[i] = static_cast<uint8_t>(value >> (i * 8));
    };

    uint8_t expectedPrefix[40]{};
    writeLe32(expectedPrefix, kMagic);
    writeLe16(expectedPrefix + 4, kVersion);
    expectedPrefix[6] = static_cast<uint8_t>(MessageType::Hello);
    expectedPrefix[7] = 0u;
    writeLe16(expectedPrefix + 8,
              static_cast<uint16_t>(payloadSize(MessageType::Hello)));
    writeLe16(expectedPrefix + 10, 0u);
    writeLe64(expectedPrefix + 12, kTestSession);
    writeLe64(expectedPrefix + 20, expectedCookie.low);
    writeLe64(expectedPrefix + 28, expectedCookie.high);
    writeLe32(expectedPrefix + 36, 0xA1B2C3D4u);
    QNP_CHECK(std::equal(std::begin(expectedPrefix), std::end(expectedPrefix),
                         packet.begin()));
    const uint8_t* const payload = packet.data() + kHeaderSize;
    const uint8_t expectedHelloPrefix[] = {
        0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
        0x02, 0x01, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x0E, 0x00, 0x00, 0x00,
    };
    QNP_CHECK(std::equal(std::begin(expectedHelloPrefix),
                         std::end(expectedHelloPrefix), payload));
    const char expectedProfile[] = "qoh99-se1126";
    QNP_CHECK(std::equal(std::begin(expectedProfile),
                         std::end(expectedProfile) - 1, payload + 20));
    QNP_CHECK(payload[20 + sizeof(expectedProfile) - 1] == 0u);
    QNP_CHECK(payload[52] == 0x10u && payload[83] == 0x2Fu);
    QNP_CHECK(payload[84] == 0x08u && payload[91] == 0x01u);
    QNP_CHECK(payload[92] == 0x44u && payload[95] == 0x11u);
    QNP_CHECK(payload[96] == 0x04u && payload[99] == 0x01u);
    QNP_CHECK(payload[100] == 0x01u && payload[103] == 0x00u);
    QNP_CHECK(payload[104] == 0x40u && payload[135] == 0x5Fu);
    QNP_CHECK(payload[136] == 0x80u && payload[167] == 0x9Fu);
    QNP_CHECK(payload[168] == 0x40u && payload[171] == 0x10u);
    QNP_CHECK(payload[172] == 0x04u && payload[175] == 0x01u);
    QNP_CHECK(payload[176] == 0x18u && payload[183] == 0x11u);
    QNP_CHECK(payload[184] == 0x28u && payload[191] == 0x21u);
    QNP_CHECK(payload[192] == 0x38u && payload[199] == 0x31u);
    QNP_CHECK(payload[200] == 0u && payload[203] == 0u);

    Datagram decoded{};
    QNP_CHECK(decodeDatagram(packet.data(), packet.size(), kTestSession,
                             testCookie(), decoded)
              == DecodeError::None);
    QNP_CHECK(decoded.header.type == MessageType::Hello);
    QNP_CHECK(decoded.header.sessionId == kTestSession);
    QNP_CHECK(decoded.header.sequence == 0xA1B2C3D4u);
    HelloPayload hello{};
    QNP_CHECK(decodePayload(decoded.payload, hello));
    QNP_CHECK(hello.compatibilityId == 0x2122232425262728ull);
    QNP_CHECK(hello.role == PeerRole::Client && hello.player == 1);
    QNP_CHECK(qnetplay::compat::semanticallyEqual(
        hello.compatibilityIdentity, testIdentity()));
}

QNP_TEST(WireValidationOrderIsStable) {
    using namespace qnetplay::wire;
    const std::vector<uint8_t> valid = helloDatagram();
    QNP_REQUIRE(valid.size() == kHeaderSize
                + payloadSize(MessageType::Hello));
    Datagram decoded{};

    QNP_CHECK(decodeDatagram(valid.data(), kHeaderSize - 1, kTestSession,
                             testCookie(), decoded) == DecodeError::TooShort);

    std::vector<uint8_t> bad = valid;
    bad[0] ^= 0xFF;
    bad[12] ^= 0xFF;
    bad.back() ^= 0xFF;
    QNP_CHECK(decodeDatagram(bad.data(), bad.size(), kTestSession,
                             testCookie(), decoded)
              == DecodeError::BadMagic);

    bad = valid;
    bad[4] = 4;
    bad[12] ^= 0xFF;
    QNP_CHECK(decodeDatagram(bad.data(), bad.size(), kTestSession,
                             testCookie(), decoded)
              == DecodeError::BadVersion);

    bad = valid;
    bad[6] = 0x7F;
    bad[8] = 0;
    QNP_CHECK(decodeDatagram(bad.data(), bad.size(), kTestSession,
                             testCookie(), decoded)
              == DecodeError::BadType);

    bad = valid;
    bad[8] = 11;
    bad[12] ^= 0xFF;
    QNP_CHECK(decodeDatagram(bad.data(), bad.size(), kTestSession,
                             testCookie(), decoded)
              == DecodeError::BadLength);

    bad = valid;
    bad[7] = 1;
    bad[12] ^= 0xFF;
    QNP_CHECK(decodeDatagram(bad.data(), bad.size(), kTestSession,
                             testCookie(), decoded)
              == DecodeError::BadFlags);

    bad = valid;
    bad.back() ^= 0xFF;
    QNP_CHECK(decodeDatagram(bad.data(), bad.size(), kTestSession + 1,
                             testCookie(), decoded)
              == DecodeError::SessionMismatch);

    SessionCookie other = testCookie();
    other.high ^= 1;
    bad = valid;
    bad.back() ^= 0xFF;
    QNP_CHECK(decodeDatagram(bad.data(), bad.size(), kTestSession,
                             other, decoded)
              == DecodeError::SessionMismatch);

    QNP_CHECK(decodeDatagram(bad.data(), bad.size(), kTestSession,
                             testCookie(), decoded)
              == DecodeError::BadCrc);
}

QNP_TEST(WireHelloRejectsIncompleteCompatibilityIdentity) {
    using namespace qnetplay::wire;
    HelloPayload hello{};
    hello.compatibilityId = 0x2122232425262728ull;
    hello.role = PeerRole::Client;
    hello.player = 1;
    std::vector<uint8_t> payload;
    QNP_CHECK(!encodePayload(hello, payload));

    hello.compatibilityIdentity = testIdentity();
    hello.compatibilityIdentity.simulationContent
        .simulationRootSha256[0] = 0u;
    for (size_t i = 1u;
         i < qnetplay::compat::kCompatibilitySha256Bytes; ++i) {
        hello.compatibilityIdentity.simulationContent
            .simulationRootSha256[i] = 0u;
    }
    QNP_CHECK(!encodePayload(hello, payload));

    hello.compatibilityIdentity = testIdentity();
    QNP_REQUIRE(encodePayload(hello, payload));
    payload[12] = 0u;
    HelloPayload preserved{};
    preserved.compatibilityId = 0xDEADBEEFCAFEBABEull;
    QNP_CHECK(!decodePayload(payload, preserved));
    QNP_CHECK(preserved.compatibilityId == 0xDEADBEEFCAFEBABEull);
}

QNP_TEST(CompatibilityIdentityTreatsPeTimestampAsForensicMetadata) {
    using namespace qnetplay::wire;

    const qnetplay::compat::CompatibilityIdentityWire observed =
        testIdentity();
    qnetplay::compat::CompatibilityIdentityWire noTimestamp = observed;
    noTimestamp.runtimeBuild.peTimestamp = 0u;
    qnetplay::compat::CompatibilityIdentityWire differentTimestamp = observed;
    differentTimestamp.runtimeBuild.peTimestamp ^= 0xFFFFFFFFu;

    QNP_CHECK(qnetplay::compat::isValidCompatibilityIdentity(noTimestamp));
    QNP_CHECK(qnetplay::compat::compatibilityMismatch(
                  observed, noTimestamp)
              == qnetplay::compat::IdentityMismatch::None);
    QNP_CHECK(qnetplay::compat::semanticallyEqual(
        observed, differentTimestamp));

    HelloPayload hello{
        0x2122232425262728ull,
        PeerRole::Client,
        1,
        noTimestamp,
    };
    std::vector<uint8_t> payload;
    QNP_REQUIRE(encodePayload(hello, payload));
    HelloPayload decoded{};
    QNP_REQUIRE(decodePayload(payload, decoded));
    QNP_CHECK(decoded.compatibilityIdentity.runtimeBuild.peTimestamp == 0u);
    QNP_CHECK(qnetplay::compat::semanticallyEqual(
        observed, decoded.compatibilityIdentity));
}

QNP_TEST(CompatibilityIdentityFailsClosedOnIncompleteManifestFlags) {
    using qnetplay::compat::isValidCompatibilityIdentity;
    using qoh99::content::ManifestFlagDuplicateNormalizedPath;
    using qoh99::content::ManifestFlagExtraScanIncomplete;
    using qoh99::content::ManifestFlagExtraSimulationResource;
    using qoh99::content::ManifestFlagUnreviewedPotentiallyRelevantExtra;
    using qoh99::content::ManifestFlagMissingRequired;
    using qoh99::content::ManifestFlagNativeValidationMismatch;
    using qoh99::content::ManifestFlagRootMismatch;
    using qoh99::content::ManifestFlagUnreadableRequired;

    qnetplay::compat::CompatibilityIdentityWire identity = testIdentity();
    QNP_REQUIRE(isValidCompatibilityIdentity(identity));

    identity.simulationContent.manifestFlags = 0u;
    QNP_CHECK(!isValidCompatibilityIdentity(identity));

    const uint32_t rejectedFlags[] = {
        ManifestFlagMissingRequired,
        ManifestFlagUnreadableRequired,
        ManifestFlagDuplicateNormalizedPath,
        ManifestFlagExtraSimulationResource,
        ManifestFlagUnreviewedPotentiallyRelevantExtra,
        ManifestFlagRootMismatch,
        ManifestFlagNativeValidationMismatch,
        ManifestFlagExtraScanIncomplete,
        1u << 31u,
    };
    for (const uint32_t flag : rejectedFlags) {
        identity = testIdentity();
        identity.simulationContent.manifestFlags |= flag;
        QNP_CHECK(!isValidCompatibilityIdentity(identity));
    }
}

QNP_TEST(CompatibilityIdentityReportsProfileAndContentMismatches) {
    qnetplay::compat::CompatibilityIdentityWire november = testIdentity();
    qnetplay::compat::CompatibilityIdentityWire december =
        testIdentity("qoh99-december-candidate");
    QNP_CHECK(qnetplay::compat::compatibilityMismatch(november, december)
              == qnetplay::compat::IdentityMismatch::ProfileId);
    QNP_CHECK(std::string(qnetplay::compat::identityMismatchReason(
                  qnetplay::compat::IdentityMismatch::ProfileId))
              == "runtime profile mismatch");

    qnetplay::compat::CompatibilityIdentityWire changedContent = november;
    changedContent.simulationContent.simulationRootSha256[31] ^= 0x01u;
    QNP_CHECK(qnetplay::compat::compatibilityMismatch(
                  november, changedContent)
              == qnetplay::compat::IdentityMismatch::SimulationRoot);
    QNP_CHECK(std::string(qnetplay::compat::identityMismatchReason(
                  qnetplay::compat::IdentityMismatch::SimulationRoot))
              == "simulation-content root mismatch");

    qnetplay::compat::CompatibilityIdentityWire futureSchema = november;
    futureSchema.schemaVersion = 2u;
    QNP_CHECK(qnetplay::compat::compatibilityMismatch(
                  november, futureSchema)
              == qnetplay::compat::IdentityMismatch::IdentitySchemaVersion);

    qnetplay::compat::CompatibilityIdentityWire otherManifest = november;
    otherManifest.simulationContent.manifestSchemaVersion = 2u;
    QNP_CHECK(qnetplay::compat::compatibilityMismatch(
                  november, otherManifest)
              == qnetplay::compat::IdentityMismatch::ManifestSchemaVersion);
}

QNP_TEST(WireInputsRoundTripWindowAckAndMask) {
    using namespace qnetplay::wire;
    InputsPayload inputs{};
    inputs.newestFrame = 99;
    inputs.ackThrough = 42;
    inputs.validMask = (1u << 0) | (1u << 3) | (1u << 29);
    inputs.inputs[0] = 0x0101;
    inputs.inputs[3] = 0x0202;
    inputs.inputs[29] = 0x0303;
    inputs.simFrontier = 97;

    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    QNP_REQUIRE(encodePayload(inputs, payload));
    QNP_REQUIRE(payload.size() == 92);
    QNP_REQUIRE(encodeDatagram(MessageType::Inputs, kTestSession,
                               testCookie(), 8,
                               payload, packet));

    Datagram decoded{};
    QNP_CHECK(decodeDatagram(packet.data(), packet.size(), kTestSession,
                             testCookie(), decoded)
              == DecodeError::None);
    InputsPayload roundTrip{};
    QNP_CHECK(decodePayload(decoded.payload, roundTrip));
    QNP_CHECK(roundTrip.newestFrame == 99);
    QNP_CHECK(roundTrip.ackThrough == 42);
    QNP_CHECK(roundTrip.validMask == inputs.validMask);
    QNP_CHECK(roundTrip.inputs[0] == 0x0101);
    QNP_CHECK(roundTrip.inputs[3] == 0x0202);
    QNP_CHECK(roundTrip.inputs[29] == 0x0303);
    QNP_CHECK(roundTrip.simFrontier == 97);

    inputs.simFrontier = kNoFrame;
    QNP_REQUIRE(encodePayload(inputs, payload));
    QNP_REQUIRE(encodeDatagram(MessageType::Inputs, kTestSession,
                               testCookie(), 9, payload, packet));
    QNP_CHECK(decodeDatagram(packet.data(), packet.size(), kTestSession,
                             testCookie(), decoded)
              == DecodeError::None);
    QNP_CHECK(decodePayload(decoded.payload, roundTrip));
    QNP_CHECK(roundTrip.simFrontier == kNoFrame);
}

QNP_TEST(WirePressureReportRoundTrips) {
    using namespace qnetplay::wire;
    InputsPayload inputs{};
    inputs.newestFrame = 500;
    inputs.simFrontier = 470;
    inputs.pressure.inputProducedThrough = 512;
    inputs.pressure.confirmedFrontier = 455;
    inputs.pressure.predictionDepth = 11;
    inputs.pressure.correctionDebt = 5;
    inputs.pressure.runState = 4;
    inputs.pressure.holdGeneration = 3;

    std::vector<uint8_t> payload;
    QNP_REQUIRE(encodePayload(inputs, payload));
    InputsPayload rt{};
    QNP_REQUIRE(decodePayload(payload, rt));
    QNP_CHECK(rt.pressure.inputProducedThrough == 512);
    QNP_CHECK(rt.pressure.confirmedFrontier == 455);
    QNP_CHECK(rt.pressure.predictionDepth == 11);
    QNP_CHECK(rt.pressure.correctionDebt == 5);
    QNP_CHECK(rt.pressure.runState == 4);
    QNP_CHECK(rt.pressure.holdGeneration == 3);
    QNP_CHECK(rt.newestFrame == 500 && rt.simFrontier == 470);
}

QNP_TEST(WirePressureReportDefaultsAreTheAbsentSentinels) {
    using namespace qnetplay::wire;
    InputsPayload inputs{};
    inputs.newestFrame = 1;
    std::vector<uint8_t> payload;
    QNP_REQUIRE(encodePayload(inputs, payload));
    InputsPayload rt{};
    QNP_REQUIRE(decodePayload(payload, rt));
    QNP_CHECK(rt.pressure.inputProducedThrough == kNoFrame);
    QNP_CHECK(rt.pressure.confirmedFrontier == kNoFrame);
    QNP_CHECK(rt.pressure.predictionDepth == 0);
    QNP_CHECK(rt.pressure.correctionDebt == 0);
    QNP_CHECK(rt.pressure.runState == 0);
    QNP_CHECK(rt.pressure.holdGeneration == 0);
}

QNP_TEST(WireRejectsOutOfDomainRunStateInBothDirections) {
    using namespace qnetplay::wire;
    InputsPayload inputs{};
    inputs.newestFrame = 7;
    inputs.pressure.runState = kMaxRunStateOrdinal + 1;
    std::vector<uint8_t> payload{0xAA};
    QNP_CHECK(!encodePayload(inputs, payload));
    QNP_CHECK(payload.size() == 1 && payload[0] == 0xAA);

    inputs.pressure.runState = kMaxRunStateOrdinal;
    QNP_REQUIRE(encodePayload(inputs, payload));
    InputsPayload rt{};
    QNP_REQUIRE(decodePayload(payload, rt));
    QNP_CHECK(rt.pressure.runState == kMaxRunStateOrdinal);

    const size_t runStateOffset =
        12u + qnetplay::wire::kInputWindowSize * 2u + 15u;
    QNP_CHECK(runStateOffset < payload.size());
    payload[runStateOffset] = 0xFF;
    QNP_CHECK(payload.size() == 92);
    InputsPayload untouched{};
    untouched.newestFrame = 4242;
    QNP_CHECK(!decodePayload(payload, untouched));
    QNP_CHECK(untouched.newestFrame == 4242);
}

QNP_TEST(WireRejectsNonCanonicalInputWindowsAtomically) {
    using namespace qnetplay::wire;

    InputsPayload inputs{};
    inputs.newestFrame = 9;
    inputs.validMask = 1u;
    inputs.inputs[0] = 0x0204;
    inputs.inputs[1] = 0x9999;
    std::vector<uint8_t> payload{0xAA};
    QNP_CHECK(!encodePayload(inputs, payload));
    QNP_CHECK(payload.size() == 1 && payload[0] == 0xAA);

    inputs.inputs[1] = 0;
    QNP_REQUIRE(encodePayload(inputs, payload));
    QNP_REQUIRE(payload.size() >= 16);
    payload[14] = 0x99;
    payload[15] = 0x99;
    InputsPayload unchanged{};
    unchanged.newestFrame = 77;
    unchanged.inputs[0] = 88;
    QNP_CHECK(!decodePayload(payload, unchanged));
    QNP_CHECK(unchanged.newestFrame == 77 && unchanged.inputs[0] == 88);

    inputs = {};
    inputs.newestFrame = 0;
    inputs.validMask = 1u << 1;
    inputs.inputs[1] = 1;
    QNP_CHECK(!encodePayload(inputs, payload));
}

QNP_TEST(WireSessionEventRoundTripAndRejection) {
    using namespace qnetplay::wire;
    SessionEventPayload event{};
    event.stage = SessionEventStage::Report;
    event.kind = 0;
    event.flags = 0x01;
    event.eventId = 7;
    event.value = 1;
    event.boundary = 0;
    event.nextFrame = 120;
    event.assignedTailExclusive = 124;
    event.oldestRestorable = 90;
    event.spectatorFrontier = 118;
    event.lifecycleKey = 0xABCD;
    std::vector<uint8_t> payload;
    QNP_REQUIRE(encodePayload(event, payload));
    QNP_REQUIRE(payload.size() == payloadSize(MessageType::SessionEvent));
    SessionEventPayload roundTrip{};
    QNP_REQUIRE(decodePayload(payload, roundTrip));
    QNP_CHECK(roundTrip.stage == SessionEventStage::Report);
    QNP_CHECK(roundTrip.eventId == 7);
    QNP_CHECK(roundTrip.assignedTailExclusive == 124);
    QNP_CHECK(roundTrip.lifecycleKey == 0xABCD);
    QNP_CHECK(roundTrip.flags == 0x01);
    SessionEventPayload bad = event;
    bad.eventId = 0;
    QNP_CHECK(!encodePayload(bad, payload));
    bad = event;
    bad.flags = 0x80;
    QNP_CHECK(!encodePayload(bad, payload));
    QNP_REQUIRE(encodePayload(event, payload));
    payload[0] = 99;
    SessionEventPayload rejected{};
    QNP_CHECK(!decodePayload(payload, rejected));
    QNP_REQUIRE(encodePayload(event, payload));
    payload[3] = 1;
    QNP_CHECK(!decodePayload(payload, rejected));
    SessionEventPayload rematch = event;
    rematch.stage = SessionEventStage::Quiesce;
    rematch.kind = 2;
    rematch.flags = 0;
    rematch.value = 54321;
    QNP_REQUIRE(encodePayload(rematch, payload));
    SessionEventPayload rematchOut{};
    QNP_REQUIRE(decodePayload(payload, rematchOut));
    QNP_CHECK(rematchOut.kind == 2 && rematchOut.value == 54321);
    rematch.kind = 3;
    QNP_REQUIRE(encodePayload(rematch, payload));
    QNP_REQUIRE(decodePayload(payload, rematchOut));
    QNP_CHECK(rematchOut.kind == 3 && rematchOut.value == 54321);
    rematch.kind = 4;
    QNP_REQUIRE(encodePayload(rematch, payload));
    QNP_REQUIRE(decodePayload(payload, rematchOut));
    QNP_CHECK(rematchOut.kind == 4 && rematchOut.value == 54321);
    rematch.kind = 5;
    QNP_CHECK(!encodePayload(rematch, payload));
    rematch.kind = 4;
    QNP_REQUIRE(encodePayload(rematch, payload));
    payload[1] = 5;
    QNP_CHECK(!decodePayload(payload, rematchOut));
}

QNP_TEST(WireRematchChoiceRoundTripAndRejection) {
    using namespace qnetplay::wire;
    RematchChoicePayload choice{};
    choice.epoch = 4;
    choice.promptFrame = 123456;
    choice.side = 1;
    choice.choice = kRematchChoiceRematch;
    choice.ready = 1;
    std::vector<uint8_t> payload;
    QNP_REQUIRE(encodePayload(choice, payload));
    QNP_REQUIRE(payload.size() == payloadSize(MessageType::RematchChoice));
    RematchChoicePayload roundTrip{};
    QNP_REQUIRE(decodePayload(payload, roundTrip));
    QNP_CHECK(roundTrip.epoch == 4);
    QNP_CHECK(roundTrip.promptFrame == 123456);
    QNP_CHECK(roundTrip.side == 1);
    QNP_CHECK(roundTrip.choice == kRematchChoiceRematch);
    QNP_CHECK(roundTrip.ready == 1);
    RematchChoicePayload bad = choice;
    bad.side = 2;
    QNP_CHECK(!encodePayload(bad, payload));
    bad = choice;
    bad.choice = 3;
    QNP_CHECK(!encodePayload(bad, payload));
    bad = choice;
    bad.ready = 2;
    QNP_CHECK(!encodePayload(bad, payload));
    bad = choice;
    bad.reserved = 1;
    QNP_CHECK(!encodePayload(bad, payload));
    QNP_REQUIRE(encodePayload(choice, payload));
    payload[9] = 3;
    RematchChoicePayload rejected{};
    rejected.epoch = 77;
    QNP_CHECK(!decodePayload(payload, rejected));
    QNP_CHECK(rejected.epoch == 77);
    QNP_REQUIRE(encodePayload(choice, payload));
    payload[11] = 1;
    QNP_CHECK(!decodePayload(payload, rejected));
    QNP_REQUIRE(encodePayload(choice, payload));
    payload.pop_back();
    QNP_CHECK(!decodePayload(payload, rejected));
}

QNP_TEST(WireRoundTripsReadyHashAckAndTerminalControls) {
    using namespace qnetplay::wire;

    ReadyPayload ready{
        1, 2, 1, kReadyAcknowledgesPeer, 0x1122334455667788ull,
        2, 4, 0, 2
    };
    std::vector<uint8_t> payload;
    QNP_REQUIRE(encodePayload(ready, payload));
    ReadyPayload readyOut{};
    QNP_CHECK(decodePayload(payload, readyOut));
    QNP_CHECK(readyOut.seedGameplay == 1
              && readyOut.seedCosmetic == 2
              && readyOut.selector == 1
              && readyOut.flags == kReadyAcknowledgesPeer
              && readyOut.bootstrapHash == 0x1122334455667788ull
              && readyOut.timerMode == 2
              && readyOut.aiDifficulty == 4
              && readyOut.lifeMultiplier == 0
              && readyOut.roundsToWinMinusOne == 2);
    ready.flags |= 2u;
    QNP_CHECK(!encodePayload(ready, payload));
    ready.flags = 0;
    ready.bootstrapHash = 0;
    QNP_CHECK(!encodePayload(ready, payload));

    ready.bootstrapHash = 1;
    const ReadyPayload validReady = ready;
    ReadyPayload invalidReady = validReady;
    invalidReady.selector = 2;
    QNP_CHECK(!encodePayload(invalidReady, payload));
    invalidReady = validReady;
    invalidReady.timerMode = 3;
    QNP_CHECK(!encodePayload(invalidReady, payload));
    invalidReady = validReady;
    invalidReady.aiDifficulty = 5;
    QNP_CHECK(!encodePayload(invalidReady, payload));
    invalidReady = validReady;
    invalidReady.lifeMultiplier = 1;
    QNP_CHECK(!encodePayload(invalidReady, payload));
    invalidReady = validReady;
    invalidReady.roundsToWinMinusOne = 3;
    QNP_CHECK(!encodePayload(invalidReady, payload));

    QNP_REQUIRE(encodePayload(validReady, payload));
    QNP_REQUIRE(payload.size() == payloadSize(MessageType::Ready));
    const struct InvalidReadyField {
        size_t offset;
        uint32_t value;
    } invalidFields[] = {
        {8, 2}, {24, 3}, {28, 5}, {32, 1}, {36, 3},
    };
    for (const InvalidReadyField& field : invalidFields) {
        std::vector<uint8_t> invalidPayload = payload;
        for (unsigned byte = 0; byte < 4; ++byte) {
            invalidPayload[field.offset + byte] =
                static_cast<uint8_t>(field.value >> (byte * 8));
        }
        ReadyPayload untouched{};
        untouched.selector = 0xDEADBEEFu;
        QNP_CHECK(!decodePayload(invalidPayload, untouched));
        QNP_CHECK(untouched.selector == 0xDEADBEEFu);
    }

    SyncHashPayload hash{};
    hash.tag = SyncHashTag{7, 8, 9};
    hash.rngGameplay = 10;
    hash.rngCosmetic = 11;
    hash.rngSelector = 12;
    hash.hp0 = -13;
    hash.hp1 = 14;
    hash.gameplayHash = 0x1020304050607080ull;
    QNP_CHECK(encodePayload(hash, payload));
    SyncHashPayload hashOut{};
    QNP_CHECK(decodePayload(payload, hashOut));
    QNP_CHECK(hashOut.tag == hash.tag
              && hashOut.hp0 == -13
              && hashOut.hp1 == 14
              && hashOut.gameplayHash == hash.gameplayHash);

    SyncHashAckPayload ack{hash.tag};
    QNP_CHECK(encodePayload(ack, payload));
    SyncHashAckPayload ackOut{};
    QNP_CHECK(decodePayload(payload, ackOut));
    QNP_CHECK(ackOut.tag == hash.tag);

    DisconnectPayload terminal{};
    terminal.code = TerminalErrorCode::ConfirmedDesync;
    terminal.detail = 0xAABBCCDDu;
    QNP_REQUIRE(setTerminalReason(
        terminal.reason, "confirmed state hash mismatch"));
    terminal.reasonId = terminalReasonId(terminal.reason.data());
    QNP_CHECK(encodePayload(terminal, payload));
    QNP_CHECK(payload.size() == 12u + kTerminalReasonBytes);
    DisconnectPayload terminalOut{};
    QNP_CHECK(decodePayload(payload, terminalOut));
    QNP_CHECK(terminalOut.code == terminal.code
              && terminalOut.detail == terminal.detail
              && terminalOut.reasonId == terminal.reasonId
              && terminalOut.reason == terminal.reason);

    DisconnectAckPayload terminalAck{
        terminal.code,
        terminal.detail,
        terminal.reasonId,
    };
    QNP_CHECK(encodePayload(terminalAck, payload));
    DisconnectAckPayload terminalAckOut{};
    QNP_CHECK(decodePayload(payload, terminalAckOut));
    QNP_CHECK(terminalAckOut.code == terminal.code
              && terminalAckOut.detail == terminal.detail
              && terminalAckOut.reasonId == terminal.reasonId);
}

QNP_TEST(WireTerminalDiagnosticsAreBoundedNamedAndValidated) {
    using namespace qnetplay::wire;
    const TerminalErrorCode terminalCodes[] = {
        TerminalErrorCode::PeerRequested,
        TerminalErrorCode::ProtocolViolation,
        TerminalErrorCode::TransportFailure,
        TerminalErrorCode::Timeout,
        TerminalErrorCode::InternalFailure,
        TerminalErrorCode::ConfirmedDesync,
        TerminalErrorCode::InvalidConfiguration,
    };
    for (const TerminalErrorCode code : terminalCodes)
        QNP_CHECK(std::string(terminalErrorCodeName(code)) != "unknown");
    for (unsigned value = 0;
         value <= static_cast<unsigned>(DecodeError::BadCrc);
         ++value) {
        QNP_CHECK(std::string(decodeErrorName(
                      static_cast<DecodeError>(value)))
                  != "unknown");
    }
    for (unsigned value =
             static_cast<unsigned>(MessageType::Hello);
         value <= static_cast<unsigned>(MessageType::RematchChoice);
         ++value) {
        QNP_CHECK(std::string(messageTypeName(
                      static_cast<MessageType>(value)))
                  != "unknown");
    }
    QNP_CHECK(std::string(terminalErrorCodeName(
                  static_cast<TerminalErrorCode>(999)))
              == "unknown");

    DisconnectPayload terminal{};
    terminal.code = TerminalErrorCode::InvalidConfiguration;
    terminal.detail = 2364844963u;
    QNP_REQUIRE(setTerminalReason(
        terminal.reason,
        "post-load gameplay layout/content differs from peer"));
    terminal.reasonId = terminalReasonId(terminal.reason.data());
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodePayload(terminal, encoded));

    std::vector<uint8_t> badId = encoded;
    badId[8] ^= 1u;
    DisconnectPayload unchanged{};
    unchanged.detail = 77u;
    QNP_CHECK(!decodePayload(badId, unchanged));
    QNP_CHECK(unchanged.detail == 77u);

    std::vector<uint8_t> controlText = encoded;
    controlText[12] = '\n';
    QNP_CHECK(!decodePayload(controlText, unchanged));

    DisconnectPayload missing{};
    missing.code = TerminalErrorCode::Timeout;
    missing.detail = 1u;
    QNP_CHECK(!encodePayload(missing, encoded));
}

QNP_TEST(WireReservesTypedLayoutFingerprintImmediatelyBeforeFrameZero) {
    using namespace qnetplay::wire;

    static_assert(kLayoutFingerprintFrame == 0xFFFFFFFEu);
    static_assert(kLayoutFingerprintFrame + 1u == kNoFrame);

    SyncHashPayload layout{};
    layout.tag = SyncHashTag{3, 4, kLayoutFingerprintFrame};
    layout.gameplayHash = 0xA1B2C3D4E5F60718ull;

    std::vector<uint8_t> payload;
    QNP_CHECK(isValidSyncHashPayload(layout));
    QNP_REQUIRE(encodePayload(layout, payload));
    QNP_REQUIRE(payload.size() == payloadSize(MessageType::SyncHash));

    SyncHashPayload decoded{};
    QNP_CHECK(decodePayload(payload, decoded));
    QNP_CHECK(decoded.tag == layout.tag);
    QNP_CHECK(decoded.gameplayHash == layout.gameplayHash);
    SyncHashPayload withParts = layout;
    withParts.rngGameplay = 0xA1B2C3D4u;
    withParts.rngCosmetic = 0x0F1E2D3Cu;
    withParts.rngSelector = 0x00020000u;
    withParts.hp0 = static_cast<int32_t>(0x9988776655443322ull & 0xFFFFFFFFu);
    withParts.hp1 = static_cast<int32_t>(0xCF040D9Bu);
    QNP_CHECK(isValidSyncHashPayload(withParts));
    QNP_REQUIRE(encodePayload(withParts, payload));
    SyncHashPayload parts{};
    QNP_CHECK(decodePayload(payload, parts));
    QNP_CHECK(parts.rngGameplay == withParts.rngGameplay);
    QNP_CHECK(parts.rngCosmetic == withParts.rngCosmetic);
    QNP_CHECK(parts.rngSelector == withParts.rngSelector);
    QNP_CHECK(parts.hp0 == withParts.hp0);
    QNP_CHECK(parts.hp1 == withParts.hp1);

    SyncHashPayload malformed = layout;
    malformed.gameplayHash = 0;
    QNP_CHECK(!isValidSyncHashPayload(malformed));
    QNP_CHECK(!encodePayload(malformed, payload));

    SyncHashPayload zeroDigest = layout;
    zeroDigest.gameplayHash = 0;
    QNP_CHECK(!isValidSyncHashPayload(zeroDigest));
    SyncHashPayload unchanged{};
    const SyncHashTag unchangedTag{9, 9, 9};
    unchanged.tag = unchangedTag;
    unchanged.gameplayHash = 9;
    QNP_CHECK(!encodePayload(zeroDigest, payload));
    QNP_CHECK(unchanged.tag == unchangedTag
              && unchanged.gameplayHash == 9);

    SyncHashPayload frameZero{};
    frameZero.tag = SyncHashTag{3, 4, 0};
    frameZero.rngGameplay = 1;
    frameZero.hp0 = -1;
    QNP_CHECK(isValidSyncHashPayload(frameZero));
    QNP_CHECK(encodePayload(frameZero, payload));
}

QNP_TEST(WireRejectsZeroSessionAndPreservesDecodeOutputOnFailure) {
    using namespace qnetplay::wire;
    HelloPayload hello{1, PeerRole::Client, 1, testIdentity()};
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet{0x5A};
    QNP_CHECK(encodePayload(hello, payload));
    QNP_CHECK(!encodeDatagram(MessageType::Hello, 0, testCookie(),
                              1, payload, packet));
    QNP_CHECK(packet.size() == 1 && packet[0] == 0x5A);

    QNP_CHECK(encodeDatagram(MessageType::Hello, kTestSession,
                             testCookie(), 1, payload, packet));
    Datagram output{};
    output.header.sequence = 0xDEADBEEFu;
    output.payload = {1, 2, 3};
    QNP_CHECK(decodeDatagram(packet.data(), packet.size(),
                             kTestSession + 1, testCookie(), output)
              == DecodeError::SessionMismatch);
    QNP_CHECK(output.header.sequence == 0xDEADBEEFu);
    QNP_CHECK(output.payload == std::vector<uint8_t>({1, 2, 3}));
}

QNP_TEST(WireRouteProbeRoundTripAndValidation) {
    using namespace qnetplay::wire;
    RouteProbePayload probe{};
    probe.sequence = 0x01020304u;
    probe.generation = 7;
    probe.stampMicros = 0x1112131415161718ull;
    probe.routeId = 3;
    probe.role = PeerRole::Client;

    std::vector<uint8_t> payload;
    QNP_REQUIRE(encodePayload(probe, payload));
    QNP_CHECK(payload.size() == payloadSize(MessageType::RouteProbe));
    QNP_CHECK(payload[8] == 0x18 && payload[15] == 0x11);

    RouteProbePayload decoded{};
    QNP_REQUIRE(decodePayload(payload, decoded));
    QNP_CHECK(decoded.sequence == probe.sequence);
    QNP_CHECK(decoded.generation == probe.generation);
    QNP_CHECK(decoded.stampMicros == probe.stampMicros);
    QNP_CHECK(decoded.routeId == probe.routeId);
    QNP_CHECK(decoded.role == probe.role);

    RouteProbePayload invalid = probe;
    invalid.sequence = 0;
    QNP_CHECK(!encodePayload(invalid, payload));
    invalid = probe;
    invalid.generation = 0;
    QNP_CHECK(!encodePayload(invalid, payload));
    invalid = probe;
    invalid.role = static_cast<PeerRole>(9);
    QNP_CHECK(!encodePayload(invalid, payload));

    QNP_REQUIRE(encodePayload(probe, payload));
    payload[17] = 9;
    RouteProbePayload rejected{};
    QNP_CHECK(!decodePayload(payload, rejected));
    QNP_REQUIRE(encodePayload(probe, payload));
    payload[18] = 1;
    QNP_CHECK(!decodePayload(payload, rejected));
    payload.assign(payloadSize(MessageType::RouteProbe) - 1, 0);
    QNP_CHECK(!decodePayload(payload, rejected));
}

QNP_TEST(WireRouteProbeAckRoundTripAndValidation) {
    using namespace qnetplay::wire;
    RouteProbeAckPayload ack{};
    ack.sequence = 0x0A0B0C0Du;
    ack.generation = 2;
    ack.echoStampMicros = 0x2122232425262728ull;
    ack.dwellMicros = 1234;
    ack.routeId = 1;
    ack.role = PeerRole::Host;

    std::vector<uint8_t> payload;
    QNP_REQUIRE(encodePayload(ack, payload));
    QNP_CHECK(payload.size() == payloadSize(MessageType::RouteProbeAck));

    RouteProbeAckPayload decoded{};
    QNP_REQUIRE(decodePayload(payload, decoded));
    QNP_CHECK(decoded.sequence == ack.sequence);
    QNP_CHECK(decoded.generation == ack.generation);
    QNP_CHECK(decoded.echoStampMicros == ack.echoStampMicros);
    QNP_CHECK(decoded.dwellMicros == ack.dwellMicros);
    QNP_CHECK(decoded.routeId == ack.routeId);
    QNP_CHECK(decoded.role == ack.role);

    RouteProbeAckPayload invalid = ack;
    invalid.sequence = 0;
    QNP_CHECK(!encodePayload(invalid, payload));
    invalid = ack;
    invalid.generation = 0;
    QNP_CHECK(!encodePayload(invalid, payload));

    QNP_REQUIRE(encodePayload(ack, payload));
    payload[22] = 1;
    RouteProbeAckPayload rejected{};
    QNP_CHECK(!decodePayload(payload, rejected));

    std::vector<uint8_t> packet;
    QNP_REQUIRE(encodePayload(ack, payload));
    QNP_REQUIRE(encodeDatagram(MessageType::RouteProbeAck, kTestSession,
                               testCookie(), 5, payload, packet));
    Datagram datagram{};
    QNP_CHECK(decodeDatagram(packet.data(), packet.size(), kTestSession,
                             testCookie(), datagram)
              == DecodeError::None);
    QNP_CHECK(datagram.header.type == MessageType::RouteProbeAck);
    RouteProbeAckPayload fromWire{};
    QNP_CHECK(decodePayload(datagram.payload, fromWire));
    QNP_CHECK(fromWire.echoStampMicros == ack.echoStampMicros);
}

QNP_TEST(EveryCompatibilityMismatchExplainsItselfToAPlayer) {
    using qnetplay::compat::IdentityMismatch;
    static constexpr IdentityMismatch kAll[] = {
        IdentityMismatch::LocalIdentityInvalid,
        IdentityMismatch::PeerIdentityInvalid,
        IdentityMismatch::IdentitySchemaVersion,
        IdentityMismatch::ProtocolVersion,
        IdentityMismatch::ProfileId,
        IdentityMismatch::ExecutableSha256,
        IdentityMismatch::ExecutableSize,
        IdentityMismatch::ManifestSchemaVersion,
        IdentityMismatch::SimulationRoot,
        IdentityMismatch::ValidationRoot,
        IdentityMismatch::EntryCount,
        IdentityMismatch::RequiredEntryCount,
        IdentityMismatch::ManifestFlags,
        IdentityMismatch::StateSchema,
        IdentityMismatch::ModSimulation,
        IdentityMismatch::TimingProfile,
    };
    for (const IdentityMismatch mismatch : kAll) {
        const std::string player =
            qnetplay::compat::identityMismatchPlayerText(mismatch);
        const std::string technical =
            qnetplay::compat::identityMismatchReason(mismatch);
        QNP_CHECK(player.size() > 24u);
        QNP_CHECK(player != technical);
        QNP_CHECK(player
                  != "the two sides could not agree on what they are running");
        QNP_CHECK(player.find("mismatch") == std::string::npos);
        QNP_CHECK(player.find("SHA-256") == std::string::npos);
        QNP_CHECK(player.find("manifest") == std::string::npos);
        QNP_CHECK(player.find("schema") == std::string::npos);
        QNP_CHECK(player.find("root") == std::string::npos);
        QNP_CHECK(player.find("identity") == std::string::npos);
    }
    QNP_CHECK(std::string(qnetplay::compat::identityMismatchPlayerText(
                  IdentityMismatch::None))
              == "everything matches");
}
