#include "../SpectatorRoute.h"

#include "../../../ipc/IpcConfig.h"
#include "../../../net/Autopunch.h"
#include "../../../net/Protocol.h"
#include "../../../netplay/WireProtocol.h"
#include "../../../netplay/tests/TestHarness.h"

#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace qclient::spectator;
namespace qsp = qnetplay::spectator;

namespace {

constexpr uint32_t kHostIpv4BE = 0x0100A8C0u;
constexpr uint32_t kRelayIpv4BE = 0x0A00A8C0u;
constexpr uint16_t kSourcePort = 41000;
constexpr uint16_t kRelayPort = 42000;
constexpr uint64_t kContract = 0xC0FFEE0000BEEFull;

qnetplay::compat::CompatibilityIdentity testCompatibilityIdentity() {
    qnetplay::compat::CompatibilityIdentity value{};
    value.schemaVersion = qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    value.protocolVersion = qnetplay::wire::kVersion;
    if (!qnetplay::compat::setProfileId(value, "qoh99-se1126"))
        return value;
    for (size_t index = 0; index < qnetplay::compat::kCompatibilitySha256Bytes;
         ++index) {
        value.runtimeBuild.executableSha256[index] =
            static_cast<uint8_t>(index + 1u);
        value.simulationContent.simulationRootSha256[index] =
            static_cast<uint8_t>(0x41u + index);
        value.simulationContent.validationRootSha256[index] =
            static_cast<uint8_t>(0x81u + index);
    }
    value.runtimeBuild.executableSize = 704512u;
    value.runtimeBuild.peTimestamp = 0x3A19A8A9u;
    value.simulationContent.manifestSchemaVersion = 1u;
    value.simulationContent.manifestFlags = 1u;
    value.simulationContent.entryCount = 68u;
    value.simulationContent.requiredEntryCount = 68u;
    value.stateSchemaId = 0x1001u;
    value.modSimulationId = 0x1002u;
    value.timingProfileId = 0x1003u;
    if (!qnetplay::compat::isValidCompatibilityIdentity(value))
        return {};
    return value;
}

uint64_t endpointKey(const RouteEndpoint& endpoint) {
    return (static_cast<uint64_t>(endpoint.ipv4BE) << 16)
         | static_cast<uint64_t>(endpoint.port);
}

enum class Answer {
    Silence,
    GameStart,
    WaitingHeartbeat,
    ExpiredHeartbeat,
    RejectCapacity,
    RejectGrantExpired,
    RejectArchiveUnavailable,
    RejectContract,
    ForeignCookie,
};

struct ScriptedRouteIo final : RouteIo {
    uint64_t clockMs = 1000;
    qsp::SessionIdentity identity{};
    ControlAnswer control{};
    bool controlExchangeCalled = false;
    std::vector<uint8_t> controlRequestedRoutes;
    size_t controlTransientFailures = 0;
    std::map<uint8_t, ControlAnswer> controlByRoute;
    bool controlReleaseOk = true;
    size_t controlReleaseCount = 0;
    RelayReservation relay{};
    bool relayReserveCalled = false;
    size_t relayReserveCount = 0;
    size_t relayRetryableFailures = 0;
    uint64_t waitedMs = 0;
    unsigned maximumWaitRequestMs = 0;
    std::vector<qsp::SpectatorGrant> relayGrants;
    bool relayReleased = false;
    bool socketOpen = false;
    bool openFails = false;
    uint16_t bound = 51000;

    std::map<uint64_t, Answer> answers;
    std::set<uint64_t> punchEcho;

    struct Sent {
        RouteEndpoint to{};
        std::vector<uint8_t> bytes;
    };
    std::vector<Sent> sent;
    size_t helloCount = 0;
    size_t detachCount = 0;
    std::deque<Sent> inbox;

    bool socketDetached = false;
    bool answerAutopunchLookup = false;
    bool autopunchMappingQueued = false;
    RouteEndpoint autopunchRelay{};
    RouteEndpoint autopunchMapping{};
    RouteEndpoint requiredAutopunchLookup{};
    bool overrideGameStartCompatibilityIdentity = false;
    qnetplay::compat::CompatibilityIdentityWire
        gameStartCompatibilityIdentity{};
    std::vector<RouteEndpoint> autopunchLookupTargets;
    size_t autopunchRegisterCount = 0;
    uint16_t autopunchInternalPort = 0;

    uintptr_t detachViewerSocket() override {
        if (!socketOpen) return ~uintptr_t(0);
        socketDetached = true;
        socketOpen = false;
        return 0x1234;
    }

    uint64_t nowMs() override { return clockMs; }
    void waitMs(unsigned ms) override {
        const unsigned elapsed = ms == 0 ? 1u : ms;
        waitedMs += elapsed;
        if (elapsed > maximumWaitRequestMs)
            maximumWaitRequestMs = elapsed;
        clockMs += elapsed;
    }

    bool resolveHost(const std::string& text, uint16_t port,
                     RouteEndpoint& out, std::string& error) override {
        if (text == "192.168.0.1") {
            out.ipv4BE = kHostIpv4BE;
            out.port = port;
            return true;
        }
        if (text == "192.168.0.10") {
            out.ipv4BE = kRelayIpv4BE;
            out.port = port;
            return true;
        }
        error = "unresolvable host";
        return false;
    }

    size_t localIpv4BE(uint32_t* out, size_t cap) override {
        if (cap == 0) return 0;
        out[0] = 0x0200A8C0u;
        return 1;
    }

    bool openViewerSocket(uint16_t& boundPort, std::string& error) override {
        if (openFails) {
            error = "scripted bind failure";
            return false;
        }
        socketOpen = true;
        boundPort = bound;
        return true;
    }
    void closeViewerSocket() override { socketOpen = false; }

    bool sendTo(const RouteEndpoint& to, const uint8_t* bytes,
                size_t length) override {
        Sent record{};
        record.to = to;
        record.bytes.assign(bytes, bytes + length);
        sent.push_back(record);
        if (to == autopunchRelay
            && length == qnet::autopunch::kRegisterBytes) {
            ++autopunchRegisterCount;
            return true;
        }
        RouteEndpoint lookupTarget{};
        const bool isAutopunchLookup =
            to == autopunchRelay
            && length == qnet::autopunch::kLookupBytes;
        if (isAutopunchLookup) {
            std::memcpy(&lookupTarget.ipv4BE, bytes + 2,
                        sizeof(lookupTarget.ipv4BE));
            lookupTarget.port = static_cast<uint16_t>(
                (static_cast<uint16_t>(bytes[6]) << 8)
                | static_cast<uint16_t>(bytes[7]));
            autopunchLookupTargets.push_back(lookupTarget);
        }
        if (answerAutopunchLookup && !autopunchMappingQueued
            && isAutopunchLookup
            && (!requiredAutopunchLookup.valid()
                || lookupTarget == requiredAutopunchLookup)
            && autopunchMapping.valid()) {
            Sent mapping{};
            mapping.to = autopunchRelay;
            mapping.bytes.resize(qnet::autopunch::kMappingBytes);
            const uint16_t internalPort =
                autopunchInternalPort != 0
                    ? autopunchInternalPort : lookupTarget.port;
            mapping.bytes[0] =
                static_cast<uint8_t>((internalPort >> 8) & 0xFFu);
            mapping.bytes[1] =
                static_cast<uint8_t>(internalPort & 0xFFu);
            mapping.bytes[2] = static_cast<uint8_t>(
                (autopunchMapping.port >> 8) & 0xFFu);
            mapping.bytes[3] = static_cast<uint8_t>(
                autopunchMapping.port & 0xFFu);
            std::memcpy(mapping.bytes.data() + 4,
                        &autopunchMapping.ipv4BE,
                        sizeof(autopunchMapping.ipv4BE));
            inbox.push_back(mapping);
            autopunchMappingQueued = true;
            return true;
        }
        if (length == 1) {
            if (punchEcho.count(endpointKey(to))) {
                Sent echo{};
                echo.to = to;
                echo.bytes.assign(1, 0);
                inbox.push_back(echo);
            }
            return true;
        }
        qsp::Datagram datagram{};
        if (qsp::decodeDatagram(bytes, length, identity, datagram)
            != qsp::DecodeError::None)
            return true;
        if (datagram.header.type == qsp::MessageType::Detach) {
            ++detachCount;
            return true;
        }
        if (datagram.header.type != qsp::MessageType::Hello) return true;
        ++helloCount;
        qsp::HelloPayload hello{};
        if (!qsp::decodePayload(datagram.payload, hello)) return true;
        respond(to, hello);
        return true;
    }

    int recvFrom(uint8_t* buffer, size_t capacity,
                 RouteEndpoint& from) override {
        if (!socketOpen) return -1;
        if (inbox.empty()) return 0;
        Sent next = inbox.front();
        inbox.pop_front();
        if (next.bytes.size() > capacity) return 0;
        std::memcpy(buffer, next.bytes.data(), next.bytes.size());
        from = next.to;
        return static_cast<int>(next.bytes.size());
    }

    void controlExchange(const SpectateTarget& target,
                         ControlAnswer& out) override {
        controlExchangeCalled = true;
        controlRequestedRoutes.push_back(target.requestedRoute);
        if (controlTransientFailures != 0) {
            --controlTransientFailures;
            out = ControlAnswer{};
            out.outcome = ControlOutcome::Unreachable;
            out.error = "scripted transient control failure";
            return;
        }
        const auto specific = controlByRoute.find(target.requestedRoute);
        out = specific != controlByRoute.end()
            ? specific->second : control;
        if (specific == controlByRoute.end() && out.hasGrant) {
            out.grant.route = target.requestedRoute;
            out.grant.grantId += controlRequestedRoutes.size() - 1u;
        }
    }

    bool controlRelease(const SpectateTarget&,
                        uint64_t,
                        std::string& error) override {
        ++controlReleaseCount;
        if (controlReleaseOk) {
            error.clear();
            return true;
        }
        error = "scripted release was not acknowledged";
        return false;
    }

    void relayReserve(const SpectateTarget&, const qsp::SpectatorGrant& grant,
                      RelayReservation& out) override {
        relayReserveCalled = true;
        ++relayReserveCount;
        relayGrants.push_back(grant);
        if (relayRetryableFailures != 0) {
            --relayRetryableFailures;
            out = {};
            out.retryable = true;
            out.retryAfterMs = 50;
            out.error = "scripted source is still starting";
            return;
        }
        out = relay;
    }
    void relayRelease() override { relayReleased = true; }

    void push(const RouteEndpoint& to, qsp::MessageType type,
              const std::vector<uint8_t>& payload,
              const qsp::SessionIdentity& useIdentity) {
        Sent record{};
        record.to = to;
        if (!qsp::encodeDatagram(type, useIdentity, 1, payload, record.bytes))
            return;
        inbox.push_back(record);
    }

    void respond(const RouteEndpoint& to, const qsp::HelloPayload& hello) {
        const auto found = answers.find(endpointKey(to));
        const Answer answer =
            found == answers.end() ? Answer::Silence : found->second;
        switch (answer) {
        case Answer::Silence:
            return;
        case Answer::GameStart: {
            qsp::GameStartPayload start{};
            start.gameId = 1;
            start.simulationContract = kContract;
            start.compatibilityIdentity =
                overrideGameStartCompatibilityIdentity
                    ? gameStartCompatibilityIdentity
                    : hello.compatibilityIdentity;
            start.initialStateHash = 0x5150;
            start.initialDigest = 0x99;
            start.resumeDigest = hello.resumeDigest;
            start.viewerId = hello.viewerId;
            start.generation = hello.generation;
            start.viewerNonce =
                qsp::viewerGenerationNonce(hello.viewerId, hello.generation);
            start.resumeRecord = hello.resumeRecord;
            start.firstTimelineFrame = 0;
            start.playerOneDelay = 2;
            start.playerTwoDelay = 2;
            start.rollbackWindow = 8;
            std::vector<uint8_t> payload;
            if (qsp::encodePayload(start, payload))
                push(to, qsp::MessageType::GameStart, payload, identity);
            return;
        }
        case Answer::WaitingHeartbeat:
        case Answer::ExpiredHeartbeat: {
            qsp::HeartbeatPayload beat{};
            beat.gameId = 0;
            beat.sourcePhase = answer == Answer::WaitingHeartbeat
                                   ? qsp::SourcePhase::Waiting
                                   : qsp::SourcePhase::Expired;
            std::vector<uint8_t> payload;
            if (qsp::encodePayload(beat, payload))
                push(to, qsp::MessageType::Heartbeat, payload, identity);
            return;
        }
        case Answer::RejectCapacity:
        case Answer::RejectGrantExpired:
        case Answer::RejectArchiveUnavailable:
        case Answer::RejectContract: {
            qsp::RejectPayload reject{};
            reject.requestedGameId = qsp::kLatestGameId;
            reject.availableGameId = qsp::kLatestGameId;
            reject.viewerId = hello.viewerId;
            reject.generation = hello.generation;
            reject.viewerNonce =
                qsp::viewerGenerationNonce(hello.viewerId, hello.generation);
            reject.reason =
                answer == Answer::RejectCapacity
                    ? qsp::RejectReason::CapacityReached
                    : answer == Answer::RejectGrantExpired
                          ? qsp::RejectReason::GrantExpired
                          : answer == Answer::RejectArchiveUnavailable
                                ? qsp::RejectReason::ArchiveUnavailable
                                : qsp::RejectReason::ContractMismatch;
            std::vector<uint8_t> payload;
            if (qsp::encodePayload(reject, payload))
                push(to, qsp::MessageType::Reject, payload, identity);
            return;
        }
        case Answer::ForeignCookie: {
            qsp::SessionIdentity foreign = identity;
            foreign.cookie.low ^= 0xFFFFFFFFull;
            qsp::HeartbeatPayload beat{};
            beat.sourcePhase = qsp::SourcePhase::Active;
            std::vector<uint8_t> payload;
            if (qsp::encodePayload(beat, payload))
                push(to, qsp::MessageType::Heartbeat, payload, foreign);
            return;
        }
        }
    }
};

SpectateTarget baseTarget() {
    SpectateTarget target{};
    target.hostText = "192.168.0.1";
    target.hostPort = 40000;
    target.directUdpPort = kSourcePort;
    target.sessionId = 7;
    target.epoch = 3;
    target.keyLow = 0x0123456789ABCDEFull;
    target.keyHigh = 0xFEDCBA9876543210ull;
    target.simulationContract = kContract;
    target.compatibilityIdentity = testCompatibilityIdentity();
    target.viewerId = 0x1122334455667788ull;
    target.generation = 1;
    target.allowPunch = true;
    target.allowRelayFallback = false;
    target.directRaceMs = 200;
    target.helloIntervalMs = 50;
    target.punchIntervalMs = 50;
    return target;
}

ControlAnswer okControl(const SpectateTarget& target) {
    ControlAnswer answer{};
    answer.outcome = ControlOutcome::Ok;
    answer.sourceDirect.ipv4BE = kHostIpv4BE;
    answer.sourceDirect.port = kSourcePort;
    answer.sourceUdpPort = kSourcePort;
    answer.sourceConfirmedLive = true;
    answer.identity = target.identity();
    answer.simulationContract = kContract;
    answer.compatibilityIdentity = target.compatibilityIdentity;
    answer.hasGrant = true;
    answer.viewerId = target.viewerId;
    answer.grant.grantId = 0xA110C00000000001ull;
    answer.grant.viewerId = target.viewerId;
    answer.grant.sessionId = target.sessionId;
    answer.grant.expiryUnix = 0x00000001FFFFFFFFull;
    answer.grant.sourceGeneration = 1;
    answer.grant.resumeRecord = target.resumeRecord;
    answer.grant.resumeDigest = target.resumeDigest;
    answer.grant.route = target.requestedRoute != 0
        ? target.requestedRoute : qnet::SPECTATE_ROUTE_DIRECT;
    answer.grant.macLow = 0x0123456789ABCDEFull;
    answer.grant.macHigh = 0xFEDCBA9876543210ull;
    return answer;
}

ScriptedRouteIo makeIo(const SpectateTarget& target) {
    ScriptedRouteIo io;
    io.identity = target.identity();
    io.control = okControl(target);
    return io;
}

const std::function<bool()> kNeverCancelled = [] { return false; };

RouteEndpoint sourceEndpoint() {
    RouteEndpoint endpoint{};
    endpoint.ipv4BE = kHostIpv4BE;
    endpoint.port = kSourcePort;
    return endpoint;
}

RouteEndpoint relayEndpoint() {
    RouteEndpoint endpoint{};
    endpoint.ipv4BE = kRelayIpv4BE;
    endpoint.port = kRelayPort;
    return endpoint;
}

RouteEndpoint loopbackEndpoint(uint16_t port = kSourcePort) {
    RouteEndpoint endpoint{};
    endpoint.ipv4BE = qnet::kLoopbackIpv4BE;
    endpoint.port = port;
    return endpoint;
}

qnet::CandidateEndpoint candidate(qnet::CandidateKind kind,
                                  uint32_t ipv4BE,
                                  uint16_t port) {
    qnet::CandidateEndpoint out{};
    out.kind = kind;
    out.ipv4BE = ipv4BE;
    out.portBE = qnet::swapPort16(port);
    return out;
}

qsp::RelayViewerBinding relayBinding(const SpectateTarget& target) {
    qsp::RelayViewerBinding binding{};
    binding.enabled = true;
    binding.room = {0x1111222233334444ull, 0x5555666677778888ull};
    binding.token = {0x0102030405060708ull, 0x1112131415161718ull};
    binding.viewerId = target.viewerId;
    binding.sourceGeneration = 9;
    binding.identity = target.identity();
    return binding;
}

std::vector<uint8_t> viewerHelloDatagram(const SpectateTarget& target) {
    qsp::HelloPayload hello{};
    hello.simulationContract = target.simulationContract;
    hello.compatibilityIdentity = target.compatibilityIdentity;
    hello.requestedGameId = qsp::kLatestGameId;
    hello.viewerId = target.viewerId;
    hello.generation = target.generation;
    hello.viewerNonce =
        qsp::viewerGenerationNonce(target.viewerId, target.generation);
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    if (!qsp::encodePayload(hello, payload)
        || !qsp::encodeDatagram(qsp::MessageType::Hello, target.identity(),
                                1, payload, datagram)) {
        return {};
    }
    return datagram;
}

template <typename Payload>
bool appendViewerDatagram(
    std::vector<std::pair<qsp::MessageType, std::vector<uint8_t>>>& out,
    qsp::MessageType type,
    const qsp::SessionIdentity& identity,
    uint32_t sequence,
    const Payload& value) {
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    if (!qsp::encodePayload(value, payload)
        || !qsp::encodeDatagram(
               type, identity, sequence, payload, datagram)) {
        return false;
    }
    out.emplace_back(type, std::move(datagram));
    return true;
}

std::vector<std::pair<qsp::MessageType, std::vector<uint8_t>>>
viewerIdentityDatagrams(const qsp::SessionIdentity& identity,
                        uint64_t viewerId) {
    std::vector<std::pair<qsp::MessageType, std::vector<uint8_t>>> out;
    uint32_t sequence = 1;

    qsp::HelloPayload hello{};
    hello.simulationContract = kContract;
    hello.compatibilityIdentity = testCompatibilityIdentity();
    hello.requestedGameId = qsp::kLatestGameId;
    hello.viewerNonce = qsp::viewerGenerationNonce(viewerId, 1);
    hello.viewerId = viewerId;
    hello.generation = 1;
    if (!appendViewerDatagram(
            out, qsp::MessageType::Hello, identity, sequence++, hello)) {
        return {};
    }

    qsp::InitAckPayload initAck{};
    initAck.gameId = 1;
    initAck.viewerNonce = qsp::viewerGenerationNonce(viewerId, 1);
    initAck.viewerId = viewerId;
    initAck.generation = 1;
    if (!appendViewerDatagram(
            out, qsp::MessageType::InitAck, identity, sequence++, initAck)) {
        return {};
    }

    qsp::AckPayload ack{};
    ack.gameId = 1;
    ack.viewerId = viewerId;
    ack.generation = 1;
    if (!appendViewerDatagram(
            out, qsp::MessageType::Ack, identity, sequence++, ack)) {
        return {};
    }

    qsp::TerminalAckPayload terminalAck{};
    terminalAck.gameId = 1;
    terminalAck.viewerId = viewerId;
    terminalAck.generation = 1;
    terminalAck.reason = qsp::TerminalReason::MatchEnded;
    if (!appendViewerDatagram(
            out, qsp::MessageType::TerminalAck, identity, sequence++,
            terminalAck)) {
        return {};
    }

    qsp::DetachPayload detach{};
    detach.viewerId = viewerId;
    detach.generation = 1;
    if (!appendViewerDatagram(
            out, qsp::MessageType::Detach, identity, sequence++, detach)) {
        return {};
    }

    qsp::MigratePayload migrate{};
    migrate.viewerId = viewerId;
    migrate.oldGeneration = 1;
    migrate.newGeneration = 2;
    if (!appendViewerDatagram(
            out, qsp::MessageType::Migrate, identity, sequence++, migrate)) {
        return {};
    }
    return out;
}

std::vector<uint8_t> sourceHeartbeatDatagram(
    const qsp::SessionIdentity& identity) {
    qsp::HeartbeatPayload heartbeat{};
    heartbeat.gameId = 1;
    heartbeat.sourcePhase = qsp::SourcePhase::Active;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    if (!qsp::encodePayload(heartbeat, payload)
        || !qsp::encodeDatagram(qsp::MessageType::Heartbeat, identity,
                                2, payload, datagram)) {
        return {};
    }
    return datagram;
}

std::vector<uint8_t> relayFrame(qnet::relay::SpectatorParty party,
                                uint64_t viewerId,
                                uint32_t sourceGeneration,
                                const std::vector<uint8_t>& inner) {
    qnet::relay::SpectatorFrameHeader header{};
    header.party = party;
    header.viewerId = viewerId;
    header.sourceGeneration = sourceGeneration;
    std::vector<uint8_t> framed;
    if (!qnet::relay::encodeSpectatorFrameHeader(header, framed)) return {};
    framed.insert(framed.end(), inner.begin(), inner.end());
    return framed;
}

}

QNP_TEST(spectate_key_parses_lowercase_uppercase_and_prefix) {
    uint64_t high = 0;
    uint64_t low = 0;
    QNP_CHECK(parseSpectateKeyHex(
        "0123456789abcdeffedcba9876543210", high, low));
    QNP_CHECK(high == 0x0123456789ABCDEFull);
    QNP_CHECK(low == 0xFEDCBA9876543210ull);

    high = low = 0;
    QNP_CHECK(parseSpectateKeyHex(
        "0123456789ABCDEFFEDCBA9876543210", high, low));
    QNP_CHECK(high == 0x0123456789ABCDEFull);
    QNP_CHECK(low == 0xFEDCBA9876543210ull);

    high = low = 0;
    QNP_CHECK(parseSpectateKeyHex(
        "0x0123456789AbCdEfFeDcBa9876543210", high, low));
    QNP_CHECK(high == 0x0123456789ABCDEFull);
    QNP_CHECK(low == 0xFEDCBA9876543210ull);

    high = low = 0;
    QNP_CHECK(parseSpectateKeyHex(
        "0X0123456789abcdeffedcba9876543210", high, low));
    QNP_CHECK(high == 0x0123456789ABCDEFull);
    QNP_CHECK(low == 0xFEDCBA9876543210ull);
}

QNP_TEST(spectate_key_boundaries) {
    uint64_t high = 0;
    uint64_t low = 0;
    QNP_CHECK(parseSpectateKeyHex(
        "00000000000000000000000000000001", high, low));
    QNP_CHECK(high == 0);
    QNP_CHECK(low == 1);

    high = low = 0;
    QNP_CHECK(parseSpectateKeyHex(
        "10000000000000000000000000000000", high, low));
    QNP_CHECK(high == 0x1000000000000000ull);
    QNP_CHECK(low == 0);

    high = low = 0;
    QNP_CHECK(parseSpectateKeyHex(
        "ffffffffffffffffffffffffffffffff", high, low));
    QNP_CHECK(high == 0xFFFFFFFFFFFFFFFFull);
    QNP_CHECK(low == 0xFFFFFFFFFFFFFFFFull);

    high = low = 0;
    QNP_CHECK(parseSpectateKeyHex(
        "00000000000000010000000000000002", high, low));
    QNP_CHECK(high == 1);
    QNP_CHECK(low == 2);
}

QNP_TEST(spectate_key_rejects_all_zero) {
    uint64_t high = 0xAAAA;
    uint64_t low = 0xBBBB;
    QNP_CHECK(!parseSpectateKeyHex(
        "00000000000000000000000000000000", high, low));
    QNP_CHECK(high == 0xAAAA);
    QNP_CHECK(low == 0xBBBB);
    QNP_CHECK(!parseSpectateKeyHex(
        "0x00000000000000000000000000000000", high, low));
    QNP_CHECK(high == 0xAAAA);
    QNP_CHECK(low == 0xBBBB);
}

QNP_TEST(spectate_key_rejects_wrong_length_and_leaves_outputs_untouched) {
    const uint64_t sentinelHigh = 0x1111111111111111ull;
    const uint64_t sentinelLow = 0x2222222222222222ull;
    const char* rejected[] = {
        "",
        "0",
        "0x",
        "0123456789abcdeffedcba987654321",
        "0123456789abcdeffedcba98765432100",
        "0x0123456789abcdeffedcba987654321",
        "0x0123456789abcdeffedcba98765432100",
        " 0123456789abcdeffedcba9876543210",
        "0123456789abcdeffedcba9876543210 ",
        "0y0123456789abcdeffedcba9876543210",
    };
    for (const char* text : rejected) {
        uint64_t high = sentinelHigh;
        uint64_t low = sentinelLow;
        QNP_CHECK(!parseSpectateKeyHex(text, high, low));
        QNP_CHECK(high == sentinelHigh);
        QNP_CHECK(low == sentinelLow);
    }
}

QNP_TEST(spectate_key_rejects_invalid_digits_at_every_position) {
    const uint64_t sentinelHigh = 0xDEADBEEFDEADBEEFull;
    const uint64_t sentinelLow = 0xFEEDFACEFEEDFACEull;
    const std::string valid = "0123456789abcdeffedcba9876543210";
    for (size_t i = 0; i < valid.size(); ++i) {
        for (char bad : {'g', 'G', 'z', ' ', '-', '+', '\0', '/', ':', '@'}) {
            std::string text = valid;
            text[i] = bad;
            uint64_t high = sentinelHigh;
            uint64_t low = sentinelLow;
            QNP_CHECK(!parseSpectateKeyHex(text, high, low));
            QNP_CHECK(high == sentinelHigh);
            QNP_CHECK(low == sentinelLow);
        }
    }
}

QNP_TEST(spectate_key_round_trips_every_nibble_position) {
    for (size_t position = 0; position < 32; ++position) {
        std::string text(32, '0');
        text[position] = 'd';
        uint64_t high = 0;
        uint64_t low = 0;
        QNP_REQUIRE(parseSpectateKeyHex(text, high, low));
        const unsigned shift =
            static_cast<unsigned>(60 - 4 * (position % 16));
        const uint64_t expected = 0xDull << shift;
        if (position < 16) {
            QNP_CHECK(high == expected);
            QNP_CHECK(low == 0);
        } else {
            QNP_CHECK(high == 0);
            QNP_CHECK(low == expected);
        }
    }
}

QNP_TEST(spectate_live_route_candidates_decode_fail_closed) {
    qnet::RouteCandidates wire{};
    wire.sessionId = 7;
    wire.count = 2;
    wire.entries[0].kind =
        static_cast<uint8_t>(qnet::CandidateKind::PrivateLan);
    wire.entries[0].routeId = 1;
    wire.entries[0].ipv4BE = qnet::kLoopbackIpv4BE;
    wire.entries[0].portBE = qnet::swapPort16(41000);
    wire.entries[1].kind =
        static_cast<uint8_t>(qnet::CandidateKind::PublicObserved);
    wire.entries[1].routeId = 2;
    wire.entries[1].ipv4BE = 0x076400CBu;
    wire.entries[1].portBE = qnet::swapPort16(41000);

    qnet::CandidateEndpoint decoded[qnet::kMaxRouteCandidates]{};
    uint8_t count = 0;
    std::string error;
    QNP_REQUIRE(decodeSpectateRouteCandidates(
        wire, 7, decoded, qnet::kMaxRouteCandidates, count, error));
    QNP_CHECK(count == 2);
    QNP_CHECK(error.empty());
    QNP_CHECK(decoded[0].kind == qnet::CandidateKind::PrivateLan);
    QNP_CHECK(decoded[0].ipv4BE == qnet::kLoopbackIpv4BE);
    QNP_CHECK(decoded[1].kind == qnet::CandidateKind::PublicObserved);

    qnet::RouteCandidates bad = wire;
    bad.sessionId = 8;
    QNP_CHECK(!decodeSpectateRouteCandidates(
        bad, 7, decoded, qnet::kMaxRouteCandidates, count, error));
    QNP_CHECK(count == 0);

    bad = wire;
    bad.reserved[1] = 1;
    QNP_CHECK(!decodeSpectateRouteCandidates(
        bad, 7, decoded, qnet::kMaxRouteCandidates, count, error));

    bad = wire;
    bad.entries[1].routeId = bad.entries[0].routeId;
    QNP_CHECK(!decodeSpectateRouteCandidates(
        bad, 7, decoded, qnet::kMaxRouteCandidates, count, error));

    bad = wire;
    bad.entries[0].routeId = 0;
    QNP_CHECK(!decodeSpectateRouteCandidates(
        bad, 7, decoded, qnet::kMaxRouteCandidates, count, error));

    bad = wire;
    bad.entries[1].kind =
        static_cast<uint8_t>(qnet::CandidateKind::PublicObserved);
    bad.entries[1].ipv4BE = kHostIpv4BE;
    QNP_CHECK(!decodeSpectateRouteCandidates(
        bad, 7, decoded, qnet::kMaxRouteCandidates, count, error));

    QNP_CHECK(!decodeSpectateRouteCandidates(
        wire, 7, decoded, 1, count, error));
}

QNP_TEST(preflight_direct_success_returns_manifest_and_no_process) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(result.ok);
    QNP_CHECK(result.failure == PreflightFailure::None);
    QNP_CHECK(result.failureReason.empty());
    QNP_CHECK(result.hasManifest);
    QNP_CHECK(result.manifestDigest != 0);
    QNP_CHECK(result.gameId == 1);
    QNP_CHECK(result.viewerId == target.viewerId);
    QNP_CHECK(result.confirmedEndpoint == sourceEndpoint());
    QNP_CHECK(result.statusText == "streaming");
    QNP_CHECK(!result.waitingForStart);
    QNP_CHECK(!result.processCreated);
    QNP_CHECK(io.socketOpen);
}

QNP_TEST(preflight_races_every_validated_uri_candidate) {
    SpectateTarget target = baseTarget();
    target.initialCandidates[0] =
        candidate(qnet::CandidateKind::PrivateLan, kRelayIpv4BE, 43001);
    target.initialCandidates[1] =
        candidate(qnet::CandidateKind::PrivateLan, kHostIpv4BE, 43002);
    target.initialCandidateCount = 2;

    ScriptedRouteIo io = makeIo(target);
    RouteEndpoint winning{};
    winning.ipv4BE = kHostIpv4BE;
    winning.port = 43002;
    io.answers[endpointKey(winning)] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_REQUIRE(result.ok);
    QNP_CHECK(result.confirmedEndpoint == winning);
    QNP_CHECK(result.confirmedKind == SpectateCandidateKind::Lan);
    bool sawFirst = false;
    bool sawSecond = false;
    for (const ScriptedRouteIo::Sent& sent : io.sent) {
        if (sent.to.ipv4BE == kRelayIpv4BE && sent.to.port == 43001)
            sawFirst = true;
        if (sent.to == winning) sawSecond = true;
    }
    QNP_CHECK(sawFirst);
    QNP_CHECK(sawSecond);
}

QNP_TEST(preflight_rejects_a_malformed_uri_candidate_set_before_io) {
    SpectateTarget target = baseTarget();
    target.initialCandidates[0] =
        candidate(qnet::CandidateKind::PublicObserved, kHostIpv4BE, 43001);
    target.initialCandidateCount = 1;
    ScriptedRouteIo io = makeIo(target);

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::InvalidTarget);
    QNP_CHECK(!io.controlExchangeCalled);
    QNP_CHECK(!io.socketOpen);
    QNP_CHECK(io.sent.empty());
}

QNP_TEST(preflight_races_authenticated_control_candidates) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.control.candidates[0] =
        candidate(qnet::CandidateKind::PrivateLan, kRelayIpv4BE, 43003);
    io.control.candidateCount = 1;
    RouteEndpoint winning{};
    winning.ipv4BE = kRelayIpv4BE;
    winning.port = 43003;
    io.answers[endpointKey(winning)] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_REQUIRE(result.ok);
    QNP_CHECK(result.confirmedEndpoint == winning);
    QNP_CHECK(result.confirmedKind == SpectateCandidateKind::Lan);
}

QNP_TEST(preflight_authenticated_live_source_adds_bounded_loopback) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = true;
    io.control.sourceUdpPort = 43004;
    io.answers[endpointKey(loopbackEndpoint(43004))] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_REQUIRE(result.ok);
    QNP_CHECK(result.sourceConfirmedLive);
    QNP_CHECK(result.confirmedEndpoint == loopbackEndpoint(43004));
    QNP_CHECK(result.confirmedKind == SpectateCandidateKind::Lan);
    QNP_CHECK(result.routeLabel
              == static_cast<uint16_t>(SpectatorRouteLabel::LanDirect));
}

QNP_TEST(preflight_copied_address_fallback_defers_identity_to_qsp_game_start) {
    SpectateTarget target = baseTarget();
    target.initialCandidates[0] = candidate(
        qnet::CandidateKind::PrivateLan, kHostIpv4BE, kSourcePort);
    target.initialCandidateCount = 1u;

    ScriptedRouteIo io = makeIo(target);
    io.control = ControlAnswer{};
    io.control.outcome = ControlOutcome::Ok;
    io.control.identity.sessionId = target.sessionId;
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_REQUIRE(result.ok);
    QNP_CHECK(!result.sourceConfirmedLive);
    QNP_CHECK(result.hasManifest);
    QNP_CHECK(qnetplay::compat::semanticallyEqual(
        result.manifest.compatibilityIdentity, target.compatibilityIdentity));
    QNP_CHECK(io.helloCount == 1u);
}

QNP_TEST(preflight_copied_address_rejects_qsp_gamestart_compatibility_mismatch) {
    SpectateTarget target = baseTarget();
    target.initialCandidates[0] = candidate(
        qnet::CandidateKind::PrivateLan, kHostIpv4BE, kSourcePort);
    target.initialCandidateCount = 1u;
    target.allowRelayFallback = true;
    target.relayHostText = "192.168.0.10";
    target.relayPort = kRelayPort;

    ScriptedRouteIo io = makeIo(target);
    io.control = ControlAnswer{};
    io.control.outcome = ControlOutcome::Ok;
    io.control.identity.sessionId = target.sessionId;
    io.overrideGameStartCompatibilityIdentity = true;
    io.gameStartCompatibilityIdentity = target.compatibilityIdentity;
    io.gameStartCompatibilityIdentity.runtimeBuild.executableSha256[0] ^= 1u;
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;
    io.relay.ok = true;
    io.relay.allocated = relayEndpoint();
    io.relay.viewerId = target.viewerId;
    io.answers[endpointKey(relayEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::ManifestInvalid);
    QNP_CHECK(!io.relayReserveCalled);
}

QNP_TEST(preflight_authenticated_live_control_requires_complete_identity) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = true;
    io.control.compatibilityIdentity = {};
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::ManifestInvalid);
    QNP_CHECK(io.helloCount == 0u);
    QNP_CHECK(!io.socketOpen);
}

QNP_TEST(preflight_rejects_unconfirmed_grant_without_compatibility_identity) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = false;
    io.control.compatibilityIdentity = {};

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::ManifestInvalid);
    QNP_CHECK(io.helloCount == 0u);
    QNP_CHECK(!io.socketOpen);
}

QNP_TEST(preflight_success_hands_the_proved_socket_to_the_caller) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);
    QNP_REQUIRE(result.ok);

    QNP_CHECK(io.socketOpen);
    const uintptr_t handle = io.detachViewerSocket();
    QNP_CHECK(handle != ~uintptr_t(0));
    QNP_CHECK(io.socketDetached);
    QNP_CHECK(!io.socketOpen);
}

QNP_TEST(preflight_uses_one_viewer_identity_across_candidate_hellos) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);
    QNP_REQUIRE(result.ok);

    QNP_CHECK(target.viewerId != 0);
    QNP_CHECK(result.viewerId == target.viewerId);
    QNP_CHECK(io.helloCount >= 1u);
    for (const ScriptedRouteIo::Sent& sent : io.sent) {
        qsp::Datagram datagram{};
        if (qsp::decodeDatagram(
                sent.bytes.data(), sent.bytes.size(), target.identity(),
                datagram) != qsp::DecodeError::None
            || datagram.header.type != qsp::MessageType::Hello) {
            continue;
        }
        qsp::HelloPayload hello{};
        QNP_REQUIRE(qsp::decodePayload(datagram.payload, hello));
        QNP_CHECK(hello.viewerId == target.viewerId);
        QNP_CHECK(hello.generation == target.generation);
    }
    QNP_CHECK(io.detachCount == 0);
}

QNP_TEST(preflight_mints_an_unpredictable_identity_when_given_none) {
    SpectateTarget a = baseTarget();
    SpectateTarget b = baseTarget();
    a.viewerId = 0;
    b.viewerId = 0;
    ScriptedRouteIo ioA = makeIo(a);
    ScriptedRouteIo ioB = makeIo(b);
    ioA.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;
    ioB.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;
    const SpectatePreflightResult ra =
        preflightSpectateWith(a, 5000, kNeverCancelled, ioA);
    const SpectatePreflightResult rb =
        preflightSpectateWith(b, 5000, kNeverCancelled, ioB);
    QNP_REQUIRE(ra.ok && rb.ok);
    QNP_CHECK(ra.viewerId != 0 && rb.viewerId != 0);
    QNP_CHECK(ra.viewerId != rb.viewerId);
}

QNP_TEST(preflight_waiting_heartbeat_is_a_valid_launch_decision) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.answers[endpointKey(sourceEndpoint())] = Answer::WaitingHeartbeat;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(result.ok);
    QNP_CHECK(result.waitingForStart);
    QNP_CHECK(result.statusText == "waiting for match start");
    QNP_CHECK(!result.hasManifest);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_expired_retention_is_not_a_waiting_state) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.answers[endpointKey(sourceEndpoint())] = Answer::ExpiredHeartbeat;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::ArchiveUnavailable);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_stale_session_is_distinct_from_unreachable) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.control.identity.sessionId = 999;
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::StaleSession);
    QNP_CHECK(!result.failureReason.empty());
    QNP_CHECK(io.helloCount == 0);
    QNP_CHECK(result.reservationsReleased);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_wrong_key_is_distinct_from_unreachable) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.punchEcho.insert(endpointKey(sourceEndpoint()));
    io.answers[endpointKey(sourceEndpoint())] = Answer::Silence;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 400, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::WrongKey);
    QNP_CHECK(io.helloCount > 0);
    QNP_CHECK(io.detachCount > 0);
    QNP_CHECK(result.reservationsReleased);
    QNP_CHECK(!io.socketOpen);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_foreign_cookie_answer_also_reads_as_wrong_key) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.answers[endpointKey(sourceEndpoint())] = Answer::ForeignCookie;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 400, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::WrongKey);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_capacity_full_fails_in_the_caster) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.answers[endpointKey(sourceEndpoint())] = Answer::RejectCapacity;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::CapacityFull);
    QNP_CHECK(io.detachCount > 0);
    QNP_CHECK(result.reservationsReleased);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_reject_reasons_map_one_to_one) {
    struct Case {
        Answer answer;
        PreflightFailure failure;
    };
    const Case cases[] = {
        {Answer::RejectGrantExpired, PreflightFailure::GrantExpired},
        {Answer::RejectArchiveUnavailable, PreflightFailure::ArchiveUnavailable},
        {Answer::RejectContract, PreflightFailure::ManifestInvalid},
    };
    for (const Case& item : cases) {
        SpectateTarget target = baseTarget();
        ScriptedRouteIo io = makeIo(target);
        io.answers[endpointKey(sourceEndpoint())] = item.answer;
        const SpectatePreflightResult result =
            preflightSpectateWith(target, 5000, kNeverCancelled, io);
        QNP_CHECK(!result.ok);
        QNP_CHECK(result.failure == item.failure);
        QNP_CHECK(!result.failureReason.empty());
    }
}

QNP_TEST(preflight_unreachable_route_reports_unreachable) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = false;
    io.control.hasGrant = false;
    io.control.grant = {};
    const SpectatePreflightResult result =
        preflightSpectateWith(target, 400, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::Unreachable);
    QNP_CHECK(!result.reservationsReleased);
    QNP_CHECK(!io.socketOpen);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_authenticated_live_but_unreachable_data_is_definitive) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = false;
    io.control.hasGrant = false;
    io.control.grant = {};
    io.control.sourceConfirmedLive = true;
    io.control.sourceUdpPort = kSourcePort;
    const SpectatePreflightResult result =
        preflightSpectateWith(target, 400, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::LiveRouteUnavailable);
    QNP_CHECK(result.sourceConfirmedLive);
    QNP_CHECK(!result.inconclusive);
    QNP_CHECK(isDefinitivePreflightFailure(result.failure));
    QNP_CHECK(!result.reservationsReleased);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_bad_target_fails_before_any_io) {
    ScriptedRouteIo io;
    SpectateTarget target = baseTarget();
    target.hostPort = 0;
    target.directUdpPort = 0;
    const SpectatePreflightResult a =
        preflightSpectateWith(target, 1000, kNeverCancelled, io);
    QNP_CHECK(a.failure == PreflightFailure::InvalidTarget);

    target = baseTarget();
    target.keyLow = 0;
    target.keyHigh = 0;
    const SpectatePreflightResult b =
        preflightSpectateWith(target, 1000, kNeverCancelled, io);
    QNP_CHECK(b.failure == PreflightFailure::InvalidTarget);

    target = baseTarget();
    target.sessionId = 0;
    const SpectatePreflightResult c =
        preflightSpectateWith(target, 1000, kNeverCancelled, io);
    QNP_CHECK(c.failure == PreflightFailure::InvalidTarget);

    QNP_CHECK(io.helloCount == 0);
    QNP_CHECK(!io.socketOpen);
    QNP_CHECK(!io.relayReserveCalled);
}

QNP_TEST(preflight_control_outcomes_map_to_distinct_reasons) {
    struct Case {
        ControlOutcome outcome;
        PreflightFailure failure;
    };
    const Case cases[] = {
        {ControlOutcome::NoMatch, PreflightFailure::SourceUnavailable},
        {ControlOutcome::Refused, PreflightFailure::HostDeclined},
        {ControlOutcome::CapacityFull, PreflightFailure::CapacityFull},
        {ControlOutcome::Unreachable, PreflightFailure::Unreachable},
        {ControlOutcome::Stale, PreflightFailure::StaleSession},
        {ControlOutcome::Expired, PreflightFailure::GrantExpired},
        {ControlOutcome::RateLimited, PreflightFailure::RateLimited},
        {ControlOutcome::Error, PreflightFailure::InternalError},
    };
    for (const Case& item : cases) {
        SpectateTarget target = baseTarget();
        ScriptedRouteIo io = makeIo(target);
        io.control.outcome = item.outcome;
        const SpectatePreflightResult result =
            preflightSpectateWith(target, 1000, kNeverCancelled, io);
        QNP_CHECK(!result.ok);
        QNP_CHECK(result.failure == item.failure);
        QNP_CHECK(!result.processCreated);
        QNP_CHECK(io.helloCount == 0);
    }
}

QNP_TEST(preflight_failure_texts_are_unique_and_non_empty) {
    const PreflightFailure all[] = {
        PreflightFailure::Cancelled,       PreflightFailure::InvalidTarget,
        PreflightFailure::WrongKey,        PreflightFailure::GrantInvalid,
        PreflightFailure::GrantExpired,    PreflightFailure::StaleSession,
        PreflightFailure::SourceUnavailable,
        PreflightFailure::ArchiveUnavailable,
        PreflightFailure::CapacityFull,    PreflightFailure::RateLimited,
        PreflightFailure::Unreachable,     PreflightFailure::RelayUnavailable,
        PreflightFailure::ManifestInvalid, PreflightFailure::HostDeclined,
        PreflightFailure::Timeout,         PreflightFailure::InternalError,
        PreflightFailure::LiveRouteUnavailable,
    };
    std::set<std::string> seen;
    for (PreflightFailure failure : all) {
        const std::string text = preflightFailureText(failure);
        QNP_CHECK(!text.empty());
        QNP_CHECK(seen.insert(text).second);
    }
    QNP_CHECK(std::string(preflightFailureText(PreflightFailure::None)).empty());
}

QNP_TEST(preflight_definitive_and_inconclusive_are_split_exactly) {
    const PreflightFailure definitive[] = {
        PreflightFailure::InvalidTarget,
        PreflightFailure::WrongKey,
        PreflightFailure::GrantInvalid,
        PreflightFailure::GrantExpired,
        PreflightFailure::StaleSession,
        PreflightFailure::ArchiveUnavailable,
        PreflightFailure::CapacityFull,
        PreflightFailure::RateLimited,
        PreflightFailure::ManifestInvalid,
        PreflightFailure::HostDeclined,
        PreflightFailure::LiveRouteUnavailable,
    };
    for (PreflightFailure failure : definitive)
        QNP_CHECK(isDefinitivePreflightFailure(failure));

    const PreflightFailure inconclusive[] = {
        PreflightFailure::SourceUnavailable,
        PreflightFailure::Unreachable,
        PreflightFailure::RelayUnavailable,
        PreflightFailure::Timeout,
        PreflightFailure::InternalError,
        PreflightFailure::Cancelled,
        PreflightFailure::None,
    };
    for (PreflightFailure failure : inconclusive)
        QNP_CHECK(!isDefinitivePreflightFailure(failure));
}

QNP_TEST(preflight_silence_is_inconclusive_not_a_refusal) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = false;
    io.control.hasGrant = false;
    io.control.grant = {};
    const SpectatePreflightResult result =
        preflightSpectateWith(target, 400, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::Unreachable);
    QNP_CHECK(result.inconclusive);
    QNP_CHECK(!result.failureReason.empty());
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_unreachable_relay_after_silence_stays_inconclusive) {
    SpectateTarget target = baseTarget();
    target.allowRelayFallback = true;
    target.relayHostText = "192.168.0.10";
    target.relayPort = kRelayPort;
    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = false;
    io.control.hasGrant = false;
    io.control.grant = {};
    io.relay.ok = false;
    io.relay.error = "could not reach the relay";

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::RelayUnavailable);
    QNP_CHECK(result.inconclusive);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_forced_relay_failure_never_fabricates_route_proof) {
    SpectateTarget target = baseTarget();
    target.requestedRoute = qnet::SPECTATE_ROUTE_RELAY;
    target.directRaceMs = 0;
    target.allowPunch = false;
    target.allowRelayFallback = true;
    target.relayHostText = "192.168.0.10";
    target.relayPort = kRelayPort;
    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = false;
    io.control.hasGrant = false;
    io.control.grant = {};
    io.relay.ok = false;
    io.relay.error = "could not reach the relay";

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 500, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::RelayUnavailable);
    QNP_CHECK(result.inconclusive);
    QNP_CHECK(result.routeLabel
              == static_cast<uint16_t>(SpectatorRouteLabel::None));
    QNP_CHECK(io.relayReserveCalled);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_definitive_reject_is_never_masked_by_a_dead_relay) {
    SpectateTarget target = baseTarget();
    target.allowRelayFallback = true;
    target.relayHostText = "192.168.0.10";
    target.relayPort = kRelayPort;
    ScriptedRouteIo io = makeIo(target);
    io.punchEcho.insert(endpointKey(sourceEndpoint()));
    io.answers[endpointKey(sourceEndpoint())] = Answer::Silence;
    io.relay.ok = false;
    io.relay.error = "could not reach the relay";

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::WrongKey);
    QNP_CHECK(!result.inconclusive);
    QNP_CHECK(result.reservationsReleased);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_definitive_rejects_are_marked_definitive) {
    struct Case {
        Answer answer;
        PreflightFailure failure;
    };
    const Case cases[] = {
        {Answer::RejectCapacity, PreflightFailure::CapacityFull},
        {Answer::RejectGrantExpired, PreflightFailure::GrantExpired},
        {Answer::RejectArchiveUnavailable, PreflightFailure::ArchiveUnavailable},
        {Answer::RejectContract, PreflightFailure::ManifestInvalid},
    };
    for (const Case& item : cases) {
        SpectateTarget target = baseTarget();
        ScriptedRouteIo io = makeIo(target);
        io.answers[endpointKey(sourceEndpoint())] = item.answer;
        const SpectatePreflightResult result =
            preflightSpectateWith(target, 5000, kNeverCancelled, io);
        QNP_CHECK(!result.ok);
        QNP_CHECK(result.failure == item.failure);
        QNP_CHECK(!result.inconclusive);
    }
}

QNP_TEST(preflight_success_is_never_marked_inconclusive) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.answers[endpointKey(sourceEndpoint())] = Answer::WaitingHeartbeat;
    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);
    QNP_CHECK(result.ok);
    QNP_CHECK(!result.inconclusive);
    QNP_CHECK(result.waitingForStart);
}

QNP_TEST(preflight_accepts_mapping_for_source_with_different_viewer_port) {
    SpectateTarget target = baseTarget();
    target.requestedRoute = qnet::SPECTATE_ROUTE_PUNCHED;
    target.directRaceMs = 0;
    target.allowPunch = true;
    target.allowRelayFallback = false;
    target.punchRelayIpv4BE = kRelayIpv4BE;
    target.punchRelayPort = kRelayPort;

    RouteEndpoint mapped{};
    mapped.ipv4BE = sourceEndpoint().ipv4BE;
    mapped.port = 43000;
    ScriptedRouteIo io = makeIo(target);
    io.answerAutopunchLookup = true;
    io.autopunchRelay = relayEndpoint();
    io.autopunchMapping = mapped;
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;
    io.answers[endpointKey(mapped)] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);
    QNP_CHECK(io.autopunchMappingQueued);
    QNP_CHECK(io.helloCount >= 1);
    QNP_CHECK(result.failure == PreflightFailure::None);
    QNP_CHECK(result.failureReason.empty());
    QNP_REQUIRE(result.ok);
    QNP_CHECK(result.routeLabel
              == static_cast<uint16_t>(SpectatorRouteLabel::Punched));
    QNP_CHECK(result.confirmedEndpoint == mapped);

    size_t originalHellos = 0;
    size_t mappedHellos = 0;
    for (const ScriptedRouteIo::Sent& sent : io.sent) {
        qsp::Datagram datagram{};
        if (qsp::decodeDatagram(
                sent.bytes.data(), sent.bytes.size(),
                target.identity(), datagram) != qsp::DecodeError::None
            || datagram.header.type != qsp::MessageType::Hello) {
            continue;
        }
        if (sent.to == sourceEndpoint()) ++originalHellos;
        if (sent.to == mapped) ++mappedHellos;
    }
    QNP_CHECK(originalHellos == 0);
    QNP_CHECK(mappedHellos >= 1);
}

QNP_TEST(preflight_forced_punch_looks_up_every_direct_source_alias) {
    SpectateTarget target = baseTarget();
    target.requestedRoute = qnet::SPECTATE_ROUTE_PUNCHED;
    target.directRaceMs = 0;
    target.allowPunch = true;
    target.allowRelayFallback = false;
    target.punchRelayIpv4BE = kRelayIpv4BE;
    target.punchRelayPort = kRelayPort;
    target.initialCandidates[0] =
        candidate(qnet::CandidateKind::PrivateLan,
                  kHostIpv4BE, kSourcePort);
    target.initialCandidates[1] =
        candidate(qnet::CandidateKind::PrivateLan,
                  qnet::kLoopbackIpv4BE, kSourcePort);
    target.initialCandidates[2] =
        candidate(qnet::CandidateKind::Relay,
                  kRelayIpv4BE, kRelayPort);
    target.initialCandidateCount = 3;

    RouteEndpoint mapped{};
    mapped.ipv4BE = loopbackEndpoint().ipv4BE;
    mapped.port = 43000;
    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = true;
    io.answerAutopunchLookup = true;
    io.autopunchRelay = relayEndpoint();
    io.requiredAutopunchLookup = loopbackEndpoint();
    io.autopunchMapping = mapped;
    io.answers[endpointKey(mapped)] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_REQUIRE(result.ok);
    QNP_CHECK(result.confirmedEndpoint == mapped);
    QNP_CHECK(result.routeLabel
              == static_cast<uint16_t>(SpectatorRouteLabel::Punched));
    QNP_CHECK(io.autopunchRegisterCount == 1);

    size_t sourceLookups = 0;
    size_t loopbackLookups = 0;
    size_t trafficRelayLookups = 0;
    for (const RouteEndpoint& endpoint : io.autopunchLookupTargets) {
        if (endpoint == sourceEndpoint()) ++sourceLookups;
        if (endpoint == loopbackEndpoint()) ++loopbackLookups;
        if (endpoint == relayEndpoint()) ++trafficRelayLookups;
    }
    QNP_CHECK(sourceLookups == 1);
    QNP_CHECK(loopbackLookups == 1);
    QNP_CHECK(trafficRelayLookups == 0);
}

QNP_TEST(preflight_autopunch_lookup_retries_are_paced) {
    SpectateTarget target = baseTarget();
    target.requestedRoute = qnet::SPECTATE_ROUTE_PUNCHED;
    target.directRaceMs = 0;
    target.allowPunch = true;
    target.allowRelayFallback = false;
    target.punchRelayIpv4BE = kRelayIpv4BE;
    target.punchRelayPort = kRelayPort;

    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = false;
    io.control.hasGrant = false;
    io.control.grant = {};
    io.autopunchRelay = relayEndpoint();
    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::Unreachable);
    QNP_CHECK(io.autopunchRegisterCount == 5);
    QNP_CHECK(io.autopunchLookupTargets.size() == 5);
    QNP_CHECK(io.maximumWaitRequestMs <= 25);
}

QNP_TEST(preflight_rejects_an_autopunch_mapping_for_another_source_target) {
    SpectateTarget target = baseTarget();
    target.requestedRoute = qnet::SPECTATE_ROUTE_PUNCHED;
    target.directRaceMs = 0;
    target.allowPunch = true;
    target.allowRelayFallback = false;
    target.punchRelayIpv4BE = kRelayIpv4BE;
    target.punchRelayPort = kRelayPort;

    RouteEndpoint mapped{};
    mapped.ipv4BE = sourceEndpoint().ipv4BE;
    mapped.port = 43000;
    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = false;
    io.control.hasGrant = false;
    io.control.grant = {};
    io.answerAutopunchLookup = true;
    io.autopunchRelay = relayEndpoint();
    io.autopunchMapping = mapped;
    io.autopunchInternalPort =
        static_cast<uint16_t>(sourceEndpoint().port + 1);
    io.answers[endpointKey(mapped)] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);
    QNP_CHECK(io.autopunchMappingQueued);
    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::Unreachable);
    QNP_CHECK(result.confirmedEndpoint != mapped);
    QNP_CHECK(io.helloCount == 0);
}

QNP_TEST(preflight_auto_direct_only_fallback_records_direct_unproved) {
    SpectateTarget target = baseTarget();
    target.allowRelayFallback = true;
    target.relayHostText = "192.168.0.10";
    target.relayPort = kRelayPort;
    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = false;
    io.control.hasGrant = false;
    io.control.grant = {};
    io.relay.ok = true;
    io.relay.allocated = relayEndpoint();
    io.relay.viewerId = target.viewerId;
    io.answers[endpointKey(relayEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(result.ok);
    QNP_CHECK(io.relayReserveCalled);
    QNP_REQUIRE(io.controlRequestedRoutes.size() == 1);
    QNP_CHECK(io.controlRequestedRoutes[0]
              == qnet::SPECTATE_ROUTE_DIRECT);
    QNP_CHECK(io.controlReleaseCount == 0);
    QNP_REQUIRE(io.relayGrants.size() == 1);
    QNP_CHECK(io.relayGrants[0].grantId == 0);
    QNP_CHECK(result.routeLabel
              == static_cast<uint16_t>(SpectatorRouteLabel::Relay));
    QNP_CHECK(result.confirmedEndpoint == relayEndpoint());
    QNP_CHECK(result.fallbackReason == "direct-unproved");
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_auto_punch_fallback_records_direct_and_punch_unproved) {
    SpectateTarget target = baseTarget();
    target.allowRelayFallback = true;
    target.relayHostText = "192.168.0.10";
    target.relayPort = kRelayPort;
    target.punchRelayIpv4BE = kRelayIpv4BE;
    target.punchRelayPort = kRelayPort;

    ScriptedRouteIo io = makeIo(target);
    io.autopunchRelay = relayEndpoint();
    io.relay.ok = true;
    io.relay.allocated = relayEndpoint();
    io.relay.viewerId = target.viewerId;
    io.answers[endpointKey(relayEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_REQUIRE(result.ok);
    QNP_CHECK(io.autopunchRegisterCount > 0u);
    QNP_CHECK(!io.autopunchLookupTargets.empty());
    QNP_CHECK(result.routeLabel
              == static_cast<uint16_t>(SpectatorRouteLabel::Relay));
    QNP_CHECK(result.fallbackReason == "direct-and-punch-unproved");
}

QNP_TEST(preflight_copied_no_control_detaches_direct_hello_before_relay_fallback) {
    SpectateTarget target = baseTarget();
    target.initialCandidates[0] = candidate(
        qnet::CandidateKind::PrivateLan, kHostIpv4BE, kSourcePort);
    target.initialCandidateCount = 1u;
    target.allowRelayFallback = true;
    target.relayHostText = "192.168.0.10";
    target.relayPort = kRelayPort;
    target.relayRoom = {0x1111222233334444ull, 0x5555666677778888ull};
    target.relayViewerAccessToken = {
        0x9999AAAABBBBCCCCull, 0xDDDDEEEEFFFF0001ull};

    ScriptedRouteIo io = makeIo(target);
    io.control = ControlAnswer{};
    io.control.outcome = ControlOutcome::Ok;
    io.control.identity.sessionId = target.sessionId;
    io.relay.ok = true;
    io.relay.allocated = relayEndpoint();
    io.relay.viewerId = 0x0123456789ABCDEFull;
    io.answers[endpointKey(relayEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_REQUIRE(result.ok);
    size_t directHello = io.sent.size();
    size_t directDetach = io.sent.size();
    size_t relayHello = io.sent.size();
    for (size_t index = 0; index < io.sent.size(); ++index) {
        qsp::Datagram datagram{};
        if (qsp::decodeDatagram(
                io.sent[index].bytes.data(), io.sent[index].bytes.size(),
                target.identity(), datagram) != qsp::DecodeError::None) {
            continue;
        }
        if (io.sent[index].to == sourceEndpoint()
            && datagram.header.type == qsp::MessageType::Hello) {
            directHello = index;
        }
        if (io.sent[index].to == sourceEndpoint()
            && datagram.header.type == qsp::MessageType::Detach) {
            directDetach = index;
        }
        if (io.sent[index].to == relayEndpoint()
            && datagram.header.type == qsp::MessageType::Hello) {
            relayHello = index;
        }
    }
    QNP_CHECK(directHello < directDetach);
    QNP_CHECK(directDetach < relayHello);
    QNP_CHECK(io.controlReleaseCount == 0u);
    QNP_CHECK(result.viewerId == io.relay.viewerId);
}

QNP_TEST(preflight_retries_only_the_relay_source_startup_race) {
    SpectateTarget target = baseTarget();
    target.requestedRoute = qnet::SPECTATE_ROUTE_RELAY;
    target.allowRelayFallback = true;
    target.relayHostText = "192.168.0.10";
    target.relayPort = kRelayPort;
    ScriptedRouteIo io = makeIo(target);
    io.relayRetryableFailures = 3;
    io.relay.ok = true;
    io.relay.allocated = relayEndpoint();
    io.relay.viewerId = target.viewerId;
    io.answers[endpointKey(relayEndpoint())] = Answer::GameStart;

    const uint64_t before = io.nowMs();
    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_REQUIRE(result.ok);
    QNP_CHECK(io.relayReserveCount == 4);
    QNP_CHECK(io.nowMs() >= before + 150);
    QNP_CHECK(io.waitedMs >= 150);
    QNP_CHECK(io.maximumWaitRequestMs <= 25);
    QNP_CHECK(result.routeLabel
              == static_cast<uint16_t>(SpectatorRouteLabel::Relay));
}

QNP_TEST(preflight_auto_with_relay_available_keeps_direct_admission_on_success) {
    SpectateTarget target = baseTarget();
    target.allowRelayFallback = true;
    target.relayHostText = "192.168.0.10";
    target.relayPort = kRelayPort;
    ScriptedRouteIo io = makeIo(target);
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_REQUIRE(result.ok);
    QNP_REQUIRE(io.controlRequestedRoutes.size() == 1);
    QNP_CHECK(io.controlRequestedRoutes[0]
              == qnet::SPECTATE_ROUTE_DIRECT);
    QNP_CHECK(io.controlReleaseCount == 0);
    QNP_CHECK(!io.relayReserveCalled);
    QNP_CHECK(result.grant.route == qnet::SPECTATE_ROUTE_DIRECT);
    QNP_CHECK(result.routeLabel
              != static_cast<uint16_t>(SpectatorRouteLabel::Relay));
}

QNP_TEST(preflight_auto_refuses_relay_until_direct_release_is_acknowledged) {
    SpectateTarget target = baseTarget();
    target.allowRelayFallback = true;
    target.relayHostText = "192.168.0.10";
    target.relayPort = kRelayPort;
    ScriptedRouteIo io = makeIo(target);
    io.controlReleaseOk = false;
    io.relay.ok = true;
    io.relay.allocated = relayEndpoint();
    io.relay.viewerId = target.viewerId;
    io.answers[endpointKey(relayEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::RelayUnavailable);
    QNP_CHECK(io.controlRequestedRoutes.size() == 1);
    QNP_CHECK(io.controlReleaseCount >= 1);
    QNP_CHECK(!io.relayReserveCalled);
    QNP_CHECK(!result.reservationsReleased);
}

QNP_TEST(preflight_rejects_a_grant_for_a_different_ingress_route) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    ControlAnswer confused = okControl(target);
    confused.grant.route = qnet::SPECTATE_ROUTE_RELAY;
    io.controlByRoute.emplace(qnet::SPECTATE_ROUTE_DIRECT, confused);

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::GrantInvalid);
    QNP_CHECK(io.controlReleaseCount == 1);
    QNP_CHECK(!io.relayReserveCalled);
    QNP_CHECK(result.reservationsReleased);
}

QNP_TEST(preflight_legacy_direct_detach_is_not_capacity_release_proof) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.control.hasGrant = false;
    io.control.grant = {};
    io.control.sourceConfirmedLive = false;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 400, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(io.helloCount > 0);
    QNP_CHECK(io.detachCount > 0);
    QNP_CHECK(io.controlReleaseCount == 0);
    QNP_CHECK(!result.reservationsReleased);
}

QNP_TEST(preflight_relay_unavailable_is_its_own_reason_and_releases_nothing) {
    SpectateTarget target = baseTarget();
    target.allowRelayFallback = true;
    target.relayHostText = "192.168.0.10";
    target.relayPort = kRelayPort;
    ScriptedRouteIo io = makeIo(target);
    io.control.sourceConfirmedLive = false;
    io.control.hasGrant = false;
    io.control.grant = {};
    io.relay.ok = false;
    io.relay.error = "the relay spectator lane is full";

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::RelayUnavailable);
    QNP_CHECK(result.failureReason.find("full") != std::string::npos);
    QNP_CHECK(!result.reservationsReleased);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_cancellation_sends_detach_and_releases_the_relay) {
    SpectateTarget target = baseTarget();
    target.allowRelayFallback = true;
    target.relayHostText = "192.168.0.10";
    target.relayPort = kRelayPort;
    target.directRaceMs = 0;
    ScriptedRouteIo io = makeIo(target);
    io.relay.ok = true;
    io.relay.allocated = relayEndpoint();
    io.relay.viewerId = target.viewerId;
    io.answers[endpointKey(relayEndpoint())] = Answer::Silence;

    bool cancel = false;
    const std::function<bool()> cancelled = [&] {
        if (io.helloCount >= 1) cancel = true;
        return cancel;
    };
    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, cancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::Cancelled);
    QNP_CHECK(result.detachSent);
    QNP_CHECK(io.detachCount > 0);
    QNP_CHECK(io.relayReleased);
    QNP_CHECK(result.reservationsReleased);
    QNP_CHECK(!io.socketOpen);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_cancellation_before_io_touches_nothing) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    const std::function<bool()> cancelled = [] { return true; };
    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, cancelled, io);
    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::Cancelled);
    QNP_CHECK(io.helloCount == 0);
    QNP_CHECK(!io.socketOpen);
    QNP_CHECK(!io.relayReserveCalled);
    QNP_CHECK(!result.processCreated);
}

QNP_TEST(preflight_socket_failure_is_an_internal_error_not_a_route_error) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.openFails = true;
    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);
    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::InternalError);
    QNP_CHECK(result.failureReason.find("scripted bind failure")
              != std::string::npos);
    QNP_CHECK(io.helloCount == 0);
}

QNP_TEST(spectator_relay_adapter_leaves_direct_datagrams_byte_identical) {
    qsp::RelayViewerAdapter adapter;
    const std::vector<uint8_t> direct{0x51, 0x53, 0x50, 0x33, 1, 2, 3, 4};
    std::vector<uint8_t> output;
    QNP_REQUIRE(adapter.encodeOutbound(
        direct.data(), direct.size(), output));
    QNP_CHECK(output == direct);

    output.clear();
    QNP_CHECK(adapter.decodeInbound(
                  false, direct.data(), direct.size(), output)
              == qsp::RelayViewerIngress::PassThrough);
    QNP_CHECK(output == direct);
}

QNP_TEST(spectator_relay_adapter_never_emits_raw_qsp_to_the_relay) {
    const SpectateTarget target = baseTarget();
    const qsp::RelayViewerBinding binding = relayBinding(target);
    qsp::RelayViewerAdapter adapter(binding);
    QNP_REQUIRE(adapter.enabled());
    const std::vector<uint8_t> hello = viewerHelloDatagram(target);
    QNP_REQUIRE(!hello.empty());

    std::vector<uint8_t> framed;
    QNP_REQUIRE(adapter.encodeOutbound(
        hello.data(), hello.size(), framed));
    QNP_CHECK(framed != hello);
    QNP_CHECK(framed.size()
              == qnet::relay::kSpectatorFrameHeaderSize + hello.size());
    QNP_CHECK(qnet::relay::hasSpectatorFrameKind(
        framed.data(), framed.size()));

    qnet::relay::SpectatorFrameHeader header{};
    QNP_REQUIRE(qnet::relay::decodeSpectatorFrameHeader(
                    framed.data(), framed.size(), header)
                == qnet::relay::DecodeError::None);
    QNP_CHECK(header.party == qnet::relay::SpectatorParty::Viewer);
    QNP_CHECK(header.viewerId == binding.viewerId);
    QNP_CHECK(header.sourceGeneration == binding.sourceGeneration);
    QNP_CHECK(std::memcmp(
                  framed.data() + qnet::relay::kSpectatorFrameHeaderSize,
                  hello.data(), hello.size())
              == 0);
}

QNP_TEST(spectator_relay_adapter_binds_every_viewer_payload_to_its_lease) {
    const SpectateTarget target = baseTarget();
    const qsp::RelayViewerBinding viewerBinding = relayBinding(target);
    qsp::RelayViewerAdapter viewerAdapter(viewerBinding);

    qsp::RelaySourceBinding sourceBinding{};
    sourceBinding.enabled = true;
    sourceBinding.room = viewerBinding.room;
    sourceBinding.token = viewerBinding.token;
    sourceBinding.sourceGeneration = viewerBinding.sourceGeneration;
    sourceBinding.identity = viewerBinding.identity;
    qsp::RelaySourceAdapter sourceAdapter;
    QNP_REQUIRE(sourceAdapter.configure(sourceBinding));

    const auto matching = viewerIdentityDatagrams(
        target.identity(), viewerBinding.viewerId);
    const auto foreign = viewerIdentityDatagrams(
        target.identity(), viewerBinding.viewerId + 1);
    QNP_REQUIRE(matching.size() == 6);
    QNP_REQUIRE(foreign.size() == matching.size());

    for (size_t i = 0; i < matching.size(); ++i) {
        QNP_REQUIRE(matching[i].first == foreign[i].first);

        std::vector<uint8_t> framed;
        QNP_CHECK(!viewerAdapter.encodeOutbound(
            foreign[i].second.data(), foreign[i].second.size(), framed));
        QNP_CHECK(framed.empty());

        const std::vector<uint8_t> forged = relayFrame(
            qnet::relay::SpectatorParty::Viewer, viewerBinding.viewerId,
            viewerBinding.sourceGeneration, foreign[i].second);
        std::vector<uint8_t> inner;
        qsp::RelayVirtualEndpoint from{};
        QNP_CHECK(sourceAdapter.decodeInbound(
                      true, forged.data(), forged.size(), inner, from)
                  == qsp::RelaySourceIngress::Rejected);
        QNP_CHECK(inner.empty());
        QNP_CHECK(!from.valid());

        QNP_REQUIRE(viewerAdapter.encodeOutbound(
            matching[i].second.data(), matching[i].second.size(), framed));
        const std::vector<uint8_t> admitted = relayFrame(
            qnet::relay::SpectatorParty::Viewer, viewerBinding.viewerId,
            viewerBinding.sourceGeneration, matching[i].second);
        QNP_CHECK(sourceAdapter.decodeInbound(
                      true, admitted.data(), admitted.size(), inner, from)
                  == qsp::RelaySourceIngress::Data);
        QNP_CHECK(inner == matching[i].second);
        QNP_CHECK(from.valid());
    }
    QNP_CHECK(sourceAdapter.viewerCount() == 1);

    const std::vector<uint8_t> heartbeat =
        sourceHeartbeatDatagram(target.identity());
    std::vector<uint8_t> framed;
    QNP_CHECK(!viewerAdapter.encodeOutbound(
        heartbeat.data(), heartbeat.size(), framed));
    const std::vector<uint8_t> forgedHeartbeat = relayFrame(
        qnet::relay::SpectatorParty::Viewer, viewerBinding.viewerId,
        viewerBinding.sourceGeneration, heartbeat);
    std::vector<uint8_t> inner;
    qsp::RelayVirtualEndpoint from{};
    QNP_CHECK(sourceAdapter.decodeInbound(
                  true, forgedHeartbeat.data(), forgedHeartbeat.size(),
                  inner, from)
              == qsp::RelaySourceIngress::Rejected);
    QNP_CHECK(inner.empty());
    QNP_CHECK(!from.valid());
}

QNP_TEST(spectator_relay_adapter_unwraps_only_the_bound_source_frame) {
    const SpectateTarget target = baseTarget();
    const qsp::RelayViewerBinding binding = relayBinding(target);
    qsp::RelayViewerAdapter adapter(binding);
    const std::vector<uint8_t> heartbeat =
        sourceHeartbeatDatagram(target.identity());
    QNP_REQUIRE(!heartbeat.empty());

    const std::vector<uint8_t> valid = relayFrame(
        qnet::relay::SpectatorParty::Source, binding.viewerId,
        binding.sourceGeneration, heartbeat);
    std::vector<uint8_t> output;
    QNP_CHECK(adapter.decodeInbound(
                  true, valid.data(), valid.size(), output)
              == qsp::RelayViewerIngress::Data);
    QNP_CHECK(output == heartbeat);
    QNP_CHECK(adapter.ready());

    const std::vector<uint8_t> wrongViewer = relayFrame(
        qnet::relay::SpectatorParty::Source, binding.viewerId + 1,
        binding.sourceGeneration, heartbeat);
    QNP_CHECK(adapter.decodeInbound(
                  true, wrongViewer.data(), wrongViewer.size(), output)
              == qsp::RelayViewerIngress::Rejected);
    QNP_CHECK(output.empty());

    const std::vector<uint8_t> wrongGeneration = relayFrame(
        qnet::relay::SpectatorParty::Source, binding.viewerId,
        binding.sourceGeneration + 1, heartbeat);
    QNP_CHECK(adapter.decodeInbound(
                  true, wrongGeneration.data(), wrongGeneration.size(), output)
              == qsp::RelayViewerIngress::Rejected);

    const std::vector<uint8_t> wrongParty = relayFrame(
        qnet::relay::SpectatorParty::Viewer, binding.viewerId,
        binding.sourceGeneration, heartbeat);
    QNP_CHECK(adapter.decodeInbound(
                  true, wrongParty.data(), wrongParty.size(), output)
              == qsp::RelayViewerIngress::Rejected);

    qsp::SessionIdentity foreign = target.identity();
    foreign.sessionId += 1;
    const std::vector<uint8_t> wrongSessionInner =
        sourceHeartbeatDatagram(foreign);
    const std::vector<uint8_t> wrongSession = relayFrame(
        qnet::relay::SpectatorParty::Source, binding.viewerId,
        binding.sourceGeneration, wrongSessionInner);
    QNP_CHECK(adapter.decodeInbound(
                  true, wrongSession.data(), wrongSession.size(), output)
              == qsp::RelayViewerIngress::Rejected);

    QNP_CHECK(adapter.decodeInbound(
                  true, heartbeat.data(), heartbeat.size(), output)
              == qsp::RelayViewerIngress::Rejected);
    QNP_CHECK(output.empty());
}

QNP_TEST(spectator_relay_registration_is_bound_and_recovers_after_long_loss) {
    const SpectateTarget target = baseTarget();
    const qsp::RelayViewerBinding binding = relayBinding(target);
    qsp::RelayViewerAdapter adapter(binding);
    QNP_CHECK(adapter.registrationDue(1000));

    std::vector<uint8_t> encoded;
    QNP_REQUIRE(adapter.encodeRegistration(1000, encoded));
    qnet::relay::SpectatorRegister registration{};
    QNP_REQUIRE(qnet::relay::decodeSpectatorRegister(
                    encoded.data(), encoded.size(), registration)
                == qnet::relay::DecodeError::None);
    QNP_CHECK(registration.party == qnet::relay::SpectatorParty::Viewer);
    QNP_CHECK(registration.room.low == binding.room.low);
    QNP_CHECK(registration.room.high == binding.room.high);
    QNP_CHECK(registration.token.low == binding.token.low);
    QNP_CHECK(registration.token.high == binding.token.high);
    QNP_CHECK(registration.viewerId == binding.viewerId);
    QNP_CHECK(registration.sourceGeneration == binding.sourceGeneration);
    QNP_CHECK(!adapter.registrationDue(
        1000 + qsp::kRelayViewerRegistrationRetryMs - 1));

    for (uint32_t attempt = 1;
         attempt < qsp::kRelayViewerFastRegistrationAttempts;
         ++attempt) {
        const uint64_t now =
            1000 + attempt * qsp::kRelayViewerRegistrationRetryMs;
        QNP_REQUIRE(adapter.registrationDue(now));
        QNP_REQUIRE(adapter.encodeRegistration(now, encoded));
    }
    QNP_CHECK(adapter.registrationAttempts()
              == qsp::kRelayViewerFastRegistrationAttempts);

    const uint64_t lastFastAttempt =
        1000
        + (qsp::kRelayViewerFastRegistrationAttempts - 1)
            * qsp::kRelayViewerRegistrationRetryMs;
    QNP_CHECK(!adapter.registrationDue(
        lastFastAttempt + qsp::kRelayViewerSlowRegistrationRetryMs - 1));

    const uint64_t recoveredAt = lastFastAttempt + 10'000;
    QNP_REQUIRE(adapter.registrationDue(recoveredAt));
    QNP_REQUIRE(adapter.encodeRegistration(recoveredAt, encoded));
    QNP_CHECK(adapter.registrationAttempts()
              == qsp::kRelayViewerFastRegistrationAttempts + 1);

    qnet::relay::SpectatorReady ready{};
    ready.room = binding.room;
    ready.party = qnet::relay::SpectatorParty::Viewer;
    ready.sourceGeneration = binding.sourceGeneration;
    ready.token = binding.token;
    ready.viewerId = binding.viewerId;
    ready.peer = {kHostIpv4BE, kSourcePort};
    QNP_REQUIRE(qnet::relay::encodeSpectatorReady(ready, encoded));
    std::vector<uint8_t> output;
    QNP_CHECK(adapter.decodeInbound(
                  true, encoded.data(), encoded.size(), output)
              == qsp::RelayViewerIngress::Ready);
    QNP_CHECK(adapter.ready());
    QNP_CHECK(!adapter.registrationDue(recoveredAt + 100'000));
    QNP_CHECK(!adapter.encodeRegistration(
        recoveredAt + 100'000, encoded, true));
}

QNP_TEST(spectator_relay_ready_and_detach_control_are_strictly_bound) {
    const SpectateTarget target = baseTarget();
    const qsp::RelayViewerBinding binding = relayBinding(target);
    qsp::RelayViewerAdapter adapter(binding);

    qnet::relay::SpectatorReady ready{};
    ready.room = binding.room;
    ready.party = qnet::relay::SpectatorParty::Viewer;
    ready.sourceGeneration = binding.sourceGeneration;
    ready.token = binding.token;
    ready.viewerId = binding.viewerId;
    ready.peer = {kHostIpv4BE, kSourcePort};
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(qnet::relay::encodeSpectatorReady(ready, encoded));
    std::vector<uint8_t> output;
    QNP_CHECK(adapter.decodeInbound(
                  true, encoded.data(), encoded.size(), output)
              == qsp::RelayViewerIngress::Ready);
    QNP_CHECK(adapter.ready());
    QNP_CHECK(output.empty());
    QNP_CHECK(!adapter.registrationDue(100000));

    qsp::RelayViewerAdapter wrongTokenAdapter(binding);
    qnet::relay::SpectatorReady wrong = ready;
    wrong.token.low ^= 1;
    QNP_REQUIRE(qnet::relay::encodeSpectatorReady(wrong, encoded));
    QNP_CHECK(wrongTokenAdapter.decodeInbound(
                  true, encoded.data(), encoded.size(), output)
              == qsp::RelayViewerIngress::Rejected);
    QNP_CHECK(!wrongTokenAdapter.ready());

    QNP_REQUIRE(adapter.encodeDetach(
        qnet::relay::SpectatorDetachReason::ViewerRequested, encoded));
    qnet::relay::SpectatorDetach detach{};
    QNP_REQUIRE(qnet::relay::decodeSpectatorDetach(
                    encoded.data(), encoded.size(), detach)
                == qnet::relay::DecodeError::None);
    QNP_CHECK(detach.party == qnet::relay::SpectatorParty::Viewer);
    QNP_CHECK(detach.sourceGeneration == binding.sourceGeneration);
    QNP_CHECK(detach.viewerId == binding.viewerId);
    QNP_CHECK(adapter.decodeInbound(
                  true, encoded.data(), encoded.size(), output)
              == qsp::RelayViewerIngress::Detached);
    QNP_CHECK(adapter.detached());
    QNP_CHECK(output.empty());

    qsp::RelayViewerAdapter wrongDetachAdapter(binding);
    detach.sourceGeneration += 1;
    QNP_REQUIRE(qnet::relay::encodeSpectatorDetach(detach, encoded));
    QNP_CHECK(wrongDetachAdapter.decodeInbound(
                  true, encoded.data(), encoded.size(), output)
              == qsp::RelayViewerIngress::Rejected);
    QNP_CHECK(!wrongDetachAdapter.detached());
}

QNP_TEST(spectator_relay_source_keeps_viewers_distinct_and_consumes_control) {
    const SpectateTarget target = baseTarget();
    qsp::RelaySourceBinding binding{};
    binding.enabled = true;
    binding.room = {0x1111222233334444ull, 0x5555666677778888ull};
    binding.token = {0xA1A2A3A4A5A6A7A8ull,
                     0xB1B2B3B4B5B6B7B8ull};
    binding.sourceGeneration = 9;
    binding.identity = target.identity();
    qsp::RelaySourceAdapter adapter;
    QNP_REQUIRE(adapter.configure(binding));

    const uint64_t viewerOne = 0x101;
    const uint64_t viewerTwo = 0x202;
    const auto readyFor = [&](uint64_t viewerId) {
        qnet::relay::SpectatorReady ready{};
        ready.room = binding.room;
        ready.party = qnet::relay::SpectatorParty::Source;
        ready.sourceGeneration = binding.sourceGeneration;
        ready.token = binding.token;
        ready.viewerId = viewerId;
        ready.peer = {kRelayIpv4BE, kRelayPort};
        std::vector<uint8_t> encoded;
        qnet::relay::encodeSpectatorReady(ready, encoded);
        return encoded;
    };

    std::vector<uint8_t> inner;
    qsp::RelayVirtualEndpoint endpointOne{};
    qsp::RelayVirtualEndpoint endpointTwo{};
    const std::vector<uint8_t> readyOne = readyFor(viewerOne);
    const std::vector<uint8_t> readyTwo = readyFor(viewerTwo);
    QNP_CHECK(adapter.decodeInbound(
                  true, readyOne.data(), readyOne.size(),
                  inner, endpointOne)
              == qsp::RelaySourceIngress::Ready);
    QNP_CHECK(adapter.decodeInbound(
                  true, readyTwo.data(), readyTwo.size(),
                  inner, endpointTwo)
              == qsp::RelaySourceIngress::Ready);
    QNP_CHECK(endpointOne.valid());
    QNP_CHECK(endpointTwo.valid());
    QNP_CHECK(!(endpointOne == endpointTwo));
    QNP_CHECK(adapter.viewerCount() == 2);

    SpectateTarget viewerOneTarget = target;
    viewerOneTarget.viewerId = viewerOne;
    SpectateTarget viewerTwoTarget = target;
    viewerTwoTarget.viewerId = viewerTwo;
    const std::vector<uint8_t> helloOne =
        viewerHelloDatagram(viewerOneTarget);
    const std::vector<uint8_t> helloTwo =
        viewerHelloDatagram(viewerTwoTarget);
    const std::vector<uint8_t> frameOne = relayFrame(
        qnet::relay::SpectatorParty::Viewer, viewerOne,
        binding.sourceGeneration, helloOne);
    const std::vector<uint8_t> frameTwo = relayFrame(
        qnet::relay::SpectatorParty::Viewer, viewerTwo,
        binding.sourceGeneration, helloTwo);
    qsp::RelayVirtualEndpoint from{};
    QNP_CHECK(adapter.decodeInbound(
                  true, frameOne.data(), frameOne.size(), inner, from)
              == qsp::RelaySourceIngress::Data);
    QNP_CHECK(from == endpointOne);
    QNP_CHECK(inner == helloOne);
    QNP_CHECK(adapter.decodeInbound(
                  true, frameTwo.data(), frameTwo.size(), inner, from)
              == qsp::RelaySourceIngress::Data);
    QNP_CHECK(from == endpointTwo);
    QNP_CHECK(inner == helloTwo);

    const std::vector<uint8_t> heartbeat =
        sourceHeartbeatDatagram(target.identity());
    std::vector<uint8_t> framed;
    QNP_REQUIRE(adapter.encodeOutbound(
        endpointOne, heartbeat.data(), heartbeat.size(), framed));
    qnet::relay::SpectatorFrameHeader header{};
    QNP_REQUIRE(qnet::relay::decodeSpectatorFrameHeader(
                    framed.data(), framed.size(), header)
                == qnet::relay::DecodeError::None);
    QNP_CHECK(header.party == qnet::relay::SpectatorParty::Source);
    QNP_CHECK(header.viewerId == viewerOne);
    QNP_REQUIRE(adapter.encodeOutbound(
        endpointTwo, heartbeat.data(), heartbeat.size(), framed));
    QNP_REQUIRE(qnet::relay::decodeSpectatorFrameHeader(
                    framed.data(), framed.size(), header)
                == qnet::relay::DecodeError::None);
    QNP_CHECK(header.viewerId == viewerTwo);

    qsp::RelayVirtualEndpoint unknown = endpointTwo;
    ++unknown.port;
    QNP_CHECK(!adapter.encodeOutbound(
        unknown, heartbeat.data(), heartbeat.size(), framed));

    QNP_CHECK(adapter.decodeInbound(
                  true, helloOne.data(), helloOne.size(), inner, from)
              == qsp::RelaySourceIngress::Rejected);
    QNP_CHECK(inner.empty());
    QNP_CHECK(!from.valid());

    qnet::relay::SpectatorDetach detach{};
    detach.room = binding.room;
    detach.party = qnet::relay::SpectatorParty::Source;
    detach.reason =
        qnet::relay::SpectatorDetachReason::ViewerRequested;
    detach.sourceGeneration = binding.sourceGeneration;
    detach.token = binding.token;
    detach.viewerId = viewerOne;
    std::vector<uint8_t> detached;
    QNP_REQUIRE(qnet::relay::encodeSpectatorDetach(detach, detached));
    uint64_t detachedViewerId = 0;
    QNP_CHECK(adapter.decodeInbound(
                  true, detached.data(), detached.size(), inner, from,
                  &detachedViewerId)
              == qsp::RelaySourceIngress::Detached);
    QNP_CHECK(from == endpointOne);
    QNP_CHECK(detachedViewerId == viewerOne);
    QNP_CHECK(adapter.viewerCount() == 1);
    uint64_t mapped = 0;
    QNP_CHECK(!adapter.viewerForEndpoint(endpointOne, mapped));
    QNP_CHECK(adapter.viewerForEndpoint(endpointTwo, mapped));
    QNP_CHECK(mapped == viewerTwo);

    std::vector<uint8_t> registrationBytes;
    QNP_REQUIRE(adapter.encodeRegistration(1000, registrationBytes));
    QNP_CHECK(!adapter.encodeRegistration(
        1000 + qsp::kRelaySourceRegistrationRefreshMs - 1,
        registrationBytes));
    QNP_REQUIRE(adapter.encodeRegistration(
        1000 + qsp::kRelaySourceRegistrationRefreshMs,
        registrationBytes));
    qnet::relay::SpectatorRegister registration{};
    QNP_REQUIRE(qnet::relay::decodeSpectatorRegister(
                    registrationBytes.data(), registrationBytes.size(),
                    registration)
                == qnet::relay::DecodeError::None);
    QNP_CHECK(registration.party
              == qnet::relay::SpectatorParty::Source);
    QNP_CHECK(registration.viewerId == 0);
    QNP_CHECK(registration.sourceGeneration
              == binding.sourceGeneration);
    QNP_CHECK(registration.room == binding.room);
    QNP_CHECK(registration.token == binding.token);
}

QNP_TEST(spectator_relay_ipc_handoff_fails_closed) {
    qipc::IpcConfig cfg{};
    cfg.mode = qipc::MODE_SPECTATOR;
    cfg.localPlayer = 0;
    cfg.isHost = 0;
    cfg.hasSocket = 1;
    cfg.peerAddrBE = kRelayIpv4BE;
    cfg.peerPortBE = qnet::swapPort16(kRelayPort);
    cfg.sessionId = 7;
    cfg.spectatorCookieLow = 1;
    cfg.spectatorViewerId = 2;
    cfg.spectatorRelayActive = 1;
    cfg.spectatorRelayRoomLow = 3;
    cfg.spectatorRelayTokenHigh = 4;
    cfg.spectatorRelaySourceGeneration = 5;
    QNP_CHECK(qipc::validateSpectatorRelayViewer(cfg) == nullptr);

    qipc::IpcConfig bad = cfg;
    bad.mode = qipc::MODE_NETPLAY;
    QNP_CHECK(qipc::validateSpectatorRelayViewer(bad) != nullptr);

    bad = cfg;
    bad.spectatorRelayTokenHigh = 0;
    QNP_CHECK(qipc::validateSpectatorRelayViewer(bad) != nullptr);

    bad = cfg;
    bad.spectatorRelaySourceGeneration = 0;
    QNP_CHECK(qipc::validateSpectatorRelayViewer(bad) != nullptr);

    bad = cfg;
    bad.reservedV15[1] = 1;
    QNP_CHECK(qipc::validateSpectatorRelayViewer(bad) != nullptr);

    bad = cfg;
    bad.spectatorRelayActive = 0;
    QNP_CHECK(qipc::validateSpectatorRelayViewer(bad) != nullptr);

    qipc::IpcConfig direct{};
    direct.mode = qipc::MODE_SPECTATOR;
    QNP_CHECK(qipc::validateSpectatorRelayViewer(direct) == nullptr);
}

QNP_TEST(route_labels_distinguish_lan_public_hairpin_punched_and_relay) {
    SpectateCandidate lan{};
    lan.kind = SpectateCandidateKind::Lan;
    lan.endpoint.ipv4BE = kHostIpv4BE;
    lan.endpoint.port = kSourcePort;
    QNP_CHECK(labelForCandidate(lan, 0, false) == SpectatorRouteLabel::LanDirect);

    SpectateCandidate relay{};
    relay.kind = SpectateCandidateKind::Relay;
    relay.endpoint = relayEndpoint();
    QNP_CHECK(labelForCandidate(relay, 0, false) == SpectatorRouteLabel::Relay);

    SpectateCandidate publicCandidate{};
    publicCandidate.kind = SpectateCandidateKind::Reflexive;
    publicCandidate.endpoint.ipv4BE = 0x0100007Fu ^ 0x11223344u;
    publicCandidate.endpoint.port = kSourcePort;
    QNP_CHECK(labelForCandidate(publicCandidate, 0, false)
              == SpectatorRouteLabel::PublicDirect);
    QNP_CHECK(labelForCandidate(publicCandidate, 0, true)
              == SpectatorRouteLabel::Punched);
    QNP_CHECK(labelForCandidate(publicCandidate,
                                publicCandidate.endpoint.ipv4BE, false)
              == SpectatorRouteLabel::PublicDirectHairpin);

    SpectateCandidate typed{};
    typed.kind = SpectateCandidateKind::Host;
    typed.endpoint = sourceEndpoint();
    QNP_CHECK(labelForCandidate(typed, 0, true)
              == SpectatorRouteLabel::LanDirect);
}

QNP_TEST(punch_control_uses_the_resolved_target_endpoint) {
    SpectateTarget target = baseTarget();
    target.punchRelayIpv4BE = qnet::kLoopbackIpv4BE;
    target.punchRelayPort = 58034;

    RouteEndpoint endpoint{};
    QNP_REQUIRE(punchRelayEndpointForTarget(target, endpoint));
    QNP_CHECK(endpoint.ipv4BE == qnet::kLoopbackIpv4BE);
    QNP_CHECK(endpoint.port == 58034);

    target.punchRelayPort = 0;
    endpoint = sourceEndpoint();
    QNP_CHECK(!punchRelayEndpointForTarget(target, endpoint));
    QNP_CHECK(!endpoint.valid());
}

QNP_TEST(migrated_target_preserves_received_history_and_bumps_generation) {
    SpectateTarget previous = baseTarget();
    previous.generation = 4;
    previous.resumeRecord = 10;
    previous.resumeDigest = 0xAAAA;

    const SpectateTarget next = migratedTarget(previous, 5000, 0xBEEF);
    QNP_CHECK(next.generation == 5);
    QNP_CHECK(next.resumeRecord == 5000);
    QNP_CHECK(next.resumeDigest == 0xBEEF);
    QNP_CHECK(next.sessionId == previous.sessionId);
    QNP_CHECK(next.keyLow == previous.keyLow);
    QNP_CHECK(next.keyHigh == previous.keyHigh);
    QNP_CHECK(next.viewerId == previous.viewerId);
    QNP_CHECK(next.simulationContract == previous.simulationContract);

    SpectateTarget wrapping = previous;
    wrapping.generation = 0xFFFFFFFFu;
    QNP_CHECK(migratedTarget(wrapping, 1, 2).generation == 1);
}

QNP_TEST(migrated_target_resumes_at_the_carried_cursor_on_the_wire) {
    SpectateTarget target = baseTarget();
    target.resumeRecord = 1234;
    target.resumeDigest = 0xFEEDBEEF;
    target.generation = 9;
    ScriptedRouteIo io = makeIo(target);
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_REQUIRE(result.ok);
    QNP_CHECK(result.generation == 9);
    QNP_CHECK(result.manifest.resumeRecord == 1234);
    QNP_CHECK(result.manifest.resumeDigest == 0xFEEDBEEF);
    QNP_CHECK(result.firstAvailableRecord == 1234);
}

QNP_TEST(redacted_tag_never_reveals_the_secret) {
    const uint64_t secret = 0x0123456789ABCDEFull;
    const std::string tag = redactedTag(secret);
    QNP_CHECK(!tag.empty());
    QNP_CHECK(tag[0] == '#');
    QNP_CHECK(tag.size() <= 8);
    QNP_CHECK(tag.find("0123456789abcdef") == std::string::npos);
    QNP_CHECK(tag.find("0123456789ABCDEF") == std::string::npos);
    QNP_CHECK(redactedTag(secret) == tag);
    QNP_CHECK(redactedTag(0) == "#none");
}

QNP_TEST(preflight_success_hands_over_the_next_qsp_sequence) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_REQUIRE(result.ok);
    QNP_CHECK(io.helloCount >= 1u);
    QNP_CHECK(result.nextSequence
              == static_cast<uint32_t>(io.helloCount) + 1u);
}

QNP_TEST(preflight_retries_a_transient_control_failure_within_budget) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.controlTransientFailures = 1;
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 30000, kNeverCancelled, io);

    QNP_REQUIRE(result.ok);
    QNP_CHECK(io.controlRequestedRoutes.size() == 2u);
}

QNP_TEST(preflight_does_not_retry_control_on_a_short_budget) {
    SpectateTarget target = baseTarget();
    ScriptedRouteIo io = makeIo(target);
    io.controlTransientFailures = 1;
    io.answers[endpointKey(sourceEndpoint())] = Answer::GameStart;

    const SpectatePreflightResult result =
        preflightSpectateWith(target, 5000, kNeverCancelled, io);

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.failure == PreflightFailure::Unreachable);
    QNP_CHECK(result.inconclusive);
    QNP_CHECK(io.controlRequestedRoutes.size() == 1u);
    QNP_CHECK(result.nextSequence == 0u);
}
