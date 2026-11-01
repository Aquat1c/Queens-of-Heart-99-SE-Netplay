#include "../SpectatorSidecar.h"

#include "../../../game/spectator/SpectatorPublisher.h"
#include "../../../netplay/WireProtocol.h"
#include "../../../netplay/tests/TestHarness.h"
#include "../../../spectator/tests/SpectatorV4TestFixtures.h"

#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../../../ipc/IpcConfig.h"
#include "../../../net/Autopunch.h"
#include "../../../net/AutopunchSession.h"
#include "../../../net/ControlLink.h"
#include "../../../net/Sockets.h"
#endif

using namespace qclient::spectator;
namespace qsp = qnetplay::spectator;
namespace topology = qclient::spectator::topology;

namespace {

constexpr uint64_t kContract = 0xABCDEF0123456789ull;
constexpr uint64_t kGameId = 1;

qsp::SessionIdentity makeIdentity() {
    qsp::SessionIdentity identity{};
    identity.sessionId = 424242;
    identity.epoch = 1;
    identity.cookie.low = 0x1111222233334444ull;
    identity.cookie.high = 0x5555666677778888ull;
    return identity;
}

qnetplay::compat::CompatibilityIdentityWire makeCompatibilityIdentity() {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = qnetplay::wire::kVersion;
    (void)qnetplay::compat::setProfileId(identity, "qoh99-se1126");
    for (size_t i = 0u;
         i < qnetplay::compat::kCompatibilitySha256Bytes; ++i) {
        identity.runtimeBuild.executableSha256[i] =
            static_cast<uint8_t>(0x11u + i);
        identity.simulationContent.simulationRootSha256[i] =
            static_cast<uint8_t>(0x44u + i);
        identity.simulationContent.validationRootSha256[i] =
            static_cast<uint8_t>(0x88u + i);
    }
    identity.runtimeBuild.executableSize = 704512u;
    identity.runtimeBuild.peTimestamp = 0x3A19A8A9u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    identity.simulationContent.manifestFlags = 1u;
    identity.simulationContent.entryCount = 68u;
    identity.simulationContent.requiredEntryCount = 68u;
    identity.stateSchemaId = 0x51u;
    identity.modSimulationId = 0x52u;
    identity.timingProfileId = 0x53u;
    return identity;
}

uint64_t stateHashFor(uint32_t record) {
    return 0x9000000000000000ull + record * 0x1000003ull;
}

qsp::GameDescriptor makeDescriptor() {
    qsp::GameDescriptor descriptor{};
    descriptor.gameId = kGameId;
    descriptor.simulationContract = kContract;
    descriptor.compatibilityIdentity = makeCompatibilityIdentity();
    descriptor.initialStateHash = stateHashFor(0);
    descriptor.initialDigest = 0;
    descriptor.firstTimelineFrame = 0;
    descriptor.playerOneDelay = 2;
    descriptor.playerTwoDelay = 2;
    descriptor.rollbackWindow = 8;
    return descriptor;
}

qsp::ConfirmedRecord makeRecord(uint32_t index) {
    qsp::ConfirmedRecord record{};
    record.record = index;
    record.timelineFrame = index;
    record.playerOneInput = static_cast<uint16_t>(index & 0x0F0F);
    record.playerTwoInput = static_cast<uint16_t>((index >> 1) & 0x0F0F);
    record.stateHash = stateHashFor(index);
    record.rollingDigest = 0;
    return record;
}

uint64_t prefixDigestFor(uint32_t count) {
    uint64_t digest = qsp::expectedInitialDigest(makeDescriptor());
    for (uint32_t index = 0; index < count; ++index) {
        const qsp::ConfirmedRecord record = makeRecord(index);
        digest = qsp::advanceRollingDigest(digest, kGameId, record);
    }
    return digest;
}

struct FakeTransport final : SidecarTransport {
    struct Packet {
        RouteEndpoint peer{};
        std::vector<uint8_t> bytes;
    };

    bool opened = false;
    bool openFails = false;
    bool relayGrants = false;
    uint16_t port = 41000;
    std::deque<Packet> inbound;
    std::map<uint64_t, std::deque<Packet>> outbound;
    std::deque<SidecarTransportEvent> events;
    std::map<uint64_t, uint64_t> relayViewerIds;
    size_t sentPackets = 0;

    static uint64_t key(const RouteEndpoint& endpoint) {
        return (static_cast<uint64_t>(endpoint.ipv4BE) << 16)
             | static_cast<uint64_t>(endpoint.port);
    }

    bool open(uint16_t requested, std::string& error) override {
        if (openFails) {
            error = "scripted spectator bind failure";
            return false;
        }
        if (requested != 0) port = requested;
        opened = true;
        return true;
    }
    uint16_t boundPort() const override { return opened ? port : 0; }
    void close() override { opened = false; }

    int recvFrom(uint8_t* buffer, size_t capacity, RouteEndpoint& from) override {
        if (inbound.empty()) return 0;
        Packet next = inbound.front();
        inbound.pop_front();
        if (next.bytes.size() > capacity) return 0;
        std::memcpy(buffer, next.bytes.data(), next.bytes.size());
        from = next.peer;
        return static_cast<int>(next.bytes.size());
    }

    bool sendTo(const RouteEndpoint& to, const uint8_t* bytes,
                size_t length) override {
        Packet packet{};
        packet.peer = to;
        packet.bytes.assign(bytes, bytes + length);
        outbound[key(to)].push_back(packet);
        ++sentPackets;
        return true;
    }

    bool pollEvent(SidecarTransportEvent& event) override {
        if (events.empty()) return false;
        event = events.front();
        events.pop_front();
        return true;
    }

    uint32_t relaySourceGeneration() const override {
        return relayGrants ? 7u : 0u;
    }
    bool relayViewerIdentity(const RouteEndpoint& endpoint,
                             uint64_t& viewerId) const override {
        viewerId = 0;
        const auto found = relayViewerIds.find(key(endpoint));
        if (found == relayViewerIds.end()) return false;
        viewerId = found->second;
        return viewerId != 0;
    }
    bool signRelayGrant(qsp::SpectatorGrant& grant) const override {
        if (!relayGrants) return false;
        grant.macLow = 0xA5A5A5A5A5A5A5A5ull;
        grant.macHigh = 0x5A5A5A5A5A5A5A5Aull;
        return true;
    }
    bool verifyRelayGrant(const qsp::SpectatorGrant& grant,
                          uint64_t nowUnix) const override {
        return relayGrants
            && grant.sourceGeneration == 7u
            && grant.expiryUnix >= nowUnix
            && grant.macLow == 0xA5A5A5A5A5A5A5A5ull
            && grant.macHigh == 0x5A5A5A5A5A5A5A5Aull;
    }

    void deliver(const RouteEndpoint& from, const std::vector<uint8_t>& bytes) {
        Packet packet{};
        packet.peer = from;
        packet.bytes = bytes;
        inbound.push_back(packet);
    }

    void detachRelayViewer(uint64_t viewerId,
                           const RouteEndpoint& endpoint) {
        SidecarTransportEvent event{};
        event.kind = SidecarTransportEventKind::ViewerDetached;
        event.viewerId = viewerId;
        event.endpoint = endpoint;
        events.push_back(event);
    }

    void authenticateRelayViewer(const RouteEndpoint& endpoint,
                                 uint64_t viewerId) {
        relayViewerIds[key(endpoint)] = viewerId;
    }
};

struct ScriptedPublicationChannel final : PublicationChannel {
    std::deque<PublicationEvent> events;
    bool live = true;
    bool reconnectPending = false;
    bool failed = false;

    int drain(std::vector<PublicationEvent>& out,
              size_t maxEvents) override {
        if (failed) return -1;
        int count = 0;
        while (!events.empty()
               && static_cast<size_t>(count) < maxEvents) {
            out.push_back(std::move(events.front()));
            events.pop_front();
            ++count;
        }
        return count;
    }
    bool connected() const override { return live; }
    bool reconnecting() const override { return reconnectPending; }
    void close() override { live = false; }
};

struct CapturingPublisherChannel final : qgame::ByteChannel {
    bool isConnected = false;
    std::vector<uint8_t> bytes;

    bool connect(uint32_t) override {
        isConnected = true;
        return true;
    }
    void close() override { isConnected = false; }
    bool connected() const override { return isConnected; }
    bool read(uint8_t*, size_t, size_t& got) override {
        got = 0u;
        return true;
    }
    bool write(const uint8_t* src, size_t len) override {
        bytes.insert(bytes.end(), src, src + len);
        return true;
    }
};

struct HarnessViewer {
    RouteEndpoint endpoint{};
    std::unique_ptr<qsp::SpectatorViewer> viewer;
    bool playbackEnabled = true;
    size_t playedRecords = 0;

    HarnessViewer(uint32_t ipv4BE, uint16_t port, uint64_t viewerId,
                  const qsp::SessionIdentity& identity) {
        endpoint.ipv4BE = ipv4BE;
        endpoint.port = port;
        qsp::ViewerConfig config{};
        config.identity = identity;
        config.simulationContract = kContract;
        config.compatibilityIdentity = makeCompatibilityIdentity();
        config.secureRandom = viewerId;
        config.viewerNonce = viewerId;
        viewer.reset(new qsp::SpectatorViewer(config));
        viewer->begin(qsp::kLatestGameId);
    }

    void pump(FakeTransport& transport, uint64_t nowMs) {
        auto& queue = transport.outbound[FakeTransport::key(endpoint)];
        while (!queue.empty()) {
            const FakeTransport::Packet packet = queue.front();
            queue.pop_front();
            viewer->onDatagram(packet.bytes.data(), packet.bytes.size(), nowMs);
        }
        if (playbackEnabled) {
            qsp::ConfirmedRecord record{};
            while (viewer->peekPlaybackRecord(record)) {
                viewer->commitPlaybackRecord(record.record, record.stateHash,
                                             stateHashFor(record.record + 1));
                ++playedRecords;
            }
        }
        for (const qsp::ViewerDatagram& outgoing : viewer->pollOutbound(nowMs))
            transport.deliver(endpoint, outgoing.bytes);
    }

    bool complete() const {
        return viewer->phase() == qsp::ViewerPhase::Complete;
    }
};

struct Fixture {
    qsp::SessionIdentity identity = makeIdentity();
    SidecarConfig config{};
    FakeTransport* transport = nullptr;
    std::unique_ptr<SpectatorSidecar> sidecar;
    uint64_t nowMs = 10000;
    uint64_t publicationSequence = 1;
    uint32_t publishedRecords = 0;
    uint64_t publishedDigest = 0;
    std::vector<std::string> log;

    Fixture() {
        config.identity = identity;
        config.simulationContract = kContract;
        config.compatibilityIdentity = makeCompatibilityIdentity();
        config.maxViewers = 4;
        config.drainDeadlineMs = 5000;
        config.retentionMs = 20000;
        config.handshakeDeadlineMs = 4000;
        config.noProgressDeadlineMs = 8000;
        config.terminalAckDeadlineMs = 4000;
        config.heartbeatIntervalMs = 500;
        config.retainAllRecords = true;
        config.unixClock = [] { return uint64_t{1000000}; };
        config.diagnostics.sink = nullptr;
    }

    void start(AdmissionPolicy policy = AdmissionPolicy::Public) {
        config.policy = policy;
        config.diagnostics.sink = [this](const char* line) {
            log.push_back(line ? line : "");
        };
        std::unique_ptr<FakeTransport> owned(new FakeTransport());
        transport = owned.get();
        sidecar.reset(new SpectatorSidecar(config));
        std::string error;
        sidecar->start(std::move(owned), nullptr, nowMs, error);
    }

    void publishStart() {
        PublicationEvent event{};
        event.kind = PublicationKind::Start;
        event.sequence = publicationSequence++;
        event.descriptor = makeDescriptor();
        publishedDigest = qsp::expectedInitialDigest(event.descriptor);
        sidecar->publish(event, nowMs);
    }

    void publishRecords(uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            PublicationEvent event{};
            event.kind = PublicationKind::Record;
            event.sequence = publicationSequence++;
            event.record = makeRecord(publishedRecords++);
            publishedDigest = qsp::advanceRollingDigest(
                publishedDigest, kGameId, event.record);
            event.record.rollingDigest = publishedDigest;
            sidecar->publish(event, nowMs);
        }
    }

    void publishTailProof(uint32_t cursor, uint64_t hash) {
        PublicationEvent event{};
        event.kind = PublicationKind::TailProof;
        event.sequence = publicationSequence++;
        event.tailRecordCursor = cursor;
        event.postTailStateHash = hash;
        event.hasPostTailProof = hash != 0;
        sidecar->publish(event, nowMs);
    }

    void publishSeal(qsp::TerminalReason reason =
                         qsp::TerminalReason::MatchEnded) {
        PublicationEvent event{};
        event.kind = PublicationKind::Seal;
        event.sequence = publicationSequence++;
        event.reason = reason;
        event.terminalRecordCursor = publishedRecords;
        event.terminalPrefixDigest = publishedDigest;
        event.hasTerminalPrefixProof = publishedDigest != 0;
        event.hasPostTailProof = true;
        event.postTailStateHash = stateHashFor(publishedRecords);
        sidecar->publish(event, nowMs);
    }

    void advance(uint64_t deltaMs) { nowMs += deltaMs; }

    void step(std::vector<HarnessViewer*> viewers, uint64_t deltaMs = 20) {
        advance(deltaMs);
        sidecar->service(nowMs);
        for (HarnessViewer* viewer : viewers) viewer->pump(*transport, nowMs);
        sidecar->service(nowMs);
    }

    void run(std::vector<HarnessViewer*> viewers, int iterations,
             uint64_t deltaMs = 20) {
        for (int i = 0; i < iterations; ++i) step(viewers, deltaMs);
    }
};

RouteEndpoint peer(uint8_t host, uint16_t port) {
    RouteEndpoint endpoint{};
    endpoint.ipv4BE = 0x0000A8C0u | (0x00010000u) | (static_cast<uint32_t>(host) << 24);
    endpoint.port = port;
    return endpoint;
}

topology::TopologyActivationRequest topologyActivationFor(
    const uint16_t qspPort, const bool forceQsrBindFailure = false,
    const bool forceQsrServiceFailure = false) {
    topology::TopologyActivationRequest request{};
    request.requested = true;
    request.rootQsp.local = RouteEndpoint{0u, qspPort};
    request.rootQsp.advertised.kind =
        qnetplay::spectator::resilient::RouteKind::Direct;
    request.rootQsp.advertised.ipv4BE = 0x0100007Fu;
    request.rootQsp.advertised.port = qspPort;
    request.rootQsp.advertised.generation = 1u;
    request.rootQsp.advertised.routeProved = true;
    request.rootQsp.localProvenance = topology::EndpointProvenance::LocalBind;
    request.rootQsp.advertisedProvenance =
        topology::EndpointProvenance::PublishedDirectCandidate;
    request.rootQsp.generation = 1u;
    request.sourceGeneration = 1u;
    request.forceQsrBindFailure = forceQsrBindFailure;
    request.forceQsrServiceFailure = forceQsrServiceFailure;
    return request;
}

std::vector<uint8_t> encodeHello(const qsp::SessionIdentity& identity,
                                 uint64_t viewerId, uint32_t generation,
                                 uint32_t sequence) {
    qsp::HelloPayload hello{};
    hello.simulationContract = kContract;
    hello.compatibilityIdentity = makeCompatibilityIdentity();
    hello.requestedGameId = qsp::kLatestGameId;
    hello.viewerId = viewerId;
    hello.generation = generation;
    hello.viewerNonce = qsp::viewerGenerationNonce(viewerId, generation);
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    if (qsp::encodePayload(hello, payload))
        qsp::encodeDatagram(qsp::MessageType::Hello, identity, sequence,
                            payload, datagram);
    return datagram;
}

std::vector<uint8_t> encodeHelloAt(
    const qsp::SessionIdentity& identity,
    uint64_t viewerId,
    uint32_t generation,
    uint32_t sequence,
    uint32_t resumeRecord,
    uint64_t resumeDigest) {
    qsp::HelloPayload hello{};
    hello.simulationContract = kContract;
    hello.compatibilityIdentity = makeCompatibilityIdentity();
    hello.requestedGameId = qsp::kLatestGameId;
    hello.viewerId = viewerId;
    hello.generation = generation;
    hello.viewerNonce =
        qsp::viewerGenerationNonce(viewerId, generation);
    hello.resumeRecord = resumeRecord;
    hello.resumeDigest = resumeDigest;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    if (qsp::encodePayload(hello, payload)) {
        qsp::encodeDatagram(
            qsp::MessageType::Hello, identity, sequence,
            payload, datagram);
    }
    return datagram;
}

std::vector<uint8_t> encodeMigrate(
    const qsp::SessionIdentity& identity,
    uint64_t viewerId,
    uint32_t oldGeneration,
    uint32_t newGeneration,
    uint32_t resumeRecord,
    uint64_t resumeDigest,
    uint32_t sequence) {
    qsp::MigratePayload migrate{};
    migrate.viewerId = viewerId;
    migrate.oldGeneration = oldGeneration;
    migrate.newGeneration = newGeneration;
    migrate.resumeRecord = resumeRecord;
    migrate.resumeDigest = resumeDigest;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    if (qsp::encodePayload(migrate, payload)) {
        qsp::encodeDatagram(
            qsp::MessageType::Migrate, identity, sequence,
            payload, datagram);
    }
    return datagram;
}

std::vector<uint8_t> encodeDetach(
    const qsp::SessionIdentity& identity,
    uint64_t viewerId,
    uint32_t generation,
    uint32_t sequence,
    uint32_t nextRecord,
    uint64_t digest) {
    qsp::DetachPayload detach{};
    detach.viewerId = viewerId;
    detach.generation = generation;
    detach.nextRecord = nextRecord;
    detach.digest = digest;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    if (qsp::encodePayload(detach, payload)) {
        qsp::encodeDatagram(
            qsp::MessageType::Detach, identity, sequence,
            payload, datagram);
    }
    return datagram;
}

std::vector<uint8_t> encodeUnknownAck(
    const qsp::SessionIdentity& identity,
    uint64_t viewerId,
    uint32_t sequence) {
    qsp::AckPayload ack{};
    ack.gameId = kGameId;
    ack.digest = 1u;
    ack.viewerId = viewerId;
    ack.generation = 1u;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    if (qsp::encodePayload(ack, payload)) {
        qsp::encodeDatagram(qsp::MessageType::Ack, identity, sequence,
                            payload, datagram);
    }
    return datagram;
}

bool sawReject(FakeTransport& transport, const RouteEndpoint& endpoint,
               const qsp::SessionIdentity& identity,
               qsp::RejectReason& reason) {
    auto& queue = transport.outbound[FakeTransport::key(endpoint)];
    for (const FakeTransport::Packet& packet : queue) {
        qsp::Datagram datagram{};
        if (qsp::decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                identity, datagram)
            != qsp::DecodeError::None)
            continue;
        if (datagram.header.type != qsp::MessageType::Reject) continue;
        qsp::RejectPayload reject{};
        if (!qsp::decodePayload(datagram.payload, reject)) continue;
        reason = reject.reason;
        return true;
    }
    return false;
}

}

QNP_TEST(sidecar_topology_activation_is_complete_or_fail_soft) {
    Fixture ready;
    ready.start();
    QNP_REQUIRE(ready.sidecar != nullptr);
    QNP_REQUIRE(ready.transport != nullptr);
    std::string error;
    const topology::TopologyActivationRequest request = topologyActivationFor(
        ready.transport->boundPort());
    QNP_REQUIRE(ready.sidecar->configureTopologyAuthority(
        request, ready.nowMs, error));
    QNP_CHECK(ready.sidecar->status().serving);
    QNP_CHECK(!ready.sidecar->status().topologyAuthorityReady);

    ready.publishStart();
    const SidecarStatus active = ready.sidecar->status();
    QNP_CHECK(active.serving);
    QNP_CHECK(active.topologyAuthorityReady);
    QNP_CHECK(active.topologyAuthorityGeneration != 0u);
    QNP_CHECK(active.topologyRootNodeId != 0u);
    QNP_CHECK(active.topologyQsrPort != 0u);
    QNP_CHECK(active.topologyEnrolled == 0u);

    Fixture failed;
    failed.start();
    QNP_REQUIRE(failed.sidecar != nullptr);
    QNP_REQUIRE(failed.transport != nullptr);
    const topology::TopologyActivationRequest forceFailure =
        topologyActivationFor(failed.transport->boundPort(), true);
    QNP_CHECK(!failed.sidecar->configureTopologyAuthority(
        forceFailure, failed.nowMs, error));
    QNP_CHECK(error.find("forced to fail") != std::string::npos);
    const SidecarStatus beforeStart = failed.sidecar->status();
    QNP_CHECK(beforeStart.serving);
    QNP_CHECK(!beforeStart.topologyAuthorityReady);
    QNP_CHECK(beforeStart.topologyQsrPort == 0u);
    failed.publishStart();
    const SidecarStatus afterStart = failed.sidecar->status();
    QNP_CHECK(afterStart.serving);
    QNP_CHECK(!afterStart.topologyAuthorityReady);
    QNP_CHECK(afterStart.topologyQsrPort == 0u);

    Fixture qsrRuntimeFailure;
    qsrRuntimeFailure.start();
    QNP_REQUIRE(qsrRuntimeFailure.sidecar != nullptr);
    QNP_REQUIRE(qsrRuntimeFailure.transport != nullptr);
    const topology::TopologyActivationRequest forceServiceFailure =
        topologyActivationFor(qsrRuntimeFailure.transport->boundPort(), false,
                              true);
    QNP_REQUIRE(qsrRuntimeFailure.sidecar->configureTopologyAuthority(
        forceServiceFailure, qsrRuntimeFailure.nowMs, error));
    qsrRuntimeFailure.publishStart();
    QNP_CHECK(qsrRuntimeFailure.sidecar->status().topologyAuthorityReady);
    qsrRuntimeFailure.sidecar->service(++qsrRuntimeFailure.nowMs);
    const SidecarStatus afterQsrFailure = qsrRuntimeFailure.sidecar->status();
    QNP_CHECK(afterQsrFailure.serving);
    QNP_CHECK(!afterQsrFailure.topologyAuthorityReady);
    QNP_CHECK(afterQsrFailure.topologyQsrPort == 0u);
}

QNP_TEST(sidecar_relay_child_assignment_rejects_unavailable_topology_without_e0_mutation) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    QNP_REQUIRE(fixture.sidecar != nullptr);

    const SidecarStatus before = fixture.sidecar->status();
    std::string error;
    QNP_CHECK(!fixture.sidecar->issueTopologyRelayChildAssignment(
        0xC001u, 0xC002u, fixture.nowMs, error));
    QNP_CHECK(error.find("assignment authority is unavailable")
              != std::string::npos);

    const SidecarStatus after = fixture.sidecar->status();
    QNP_CHECK(after.serving == before.serving);
    QNP_CHECK(after.hasGame == before.hasGame);
    QNP_CHECK(after.gameId == before.gameId);
    QNP_CHECK(after.boundPort == before.boundPort);
    QNP_CHECK(after.viewerCount == before.viewerCount);
    QNP_CHECK(after.reservationCount == before.reservationCount);
    QNP_CHECK(after.topologyAuthorityReady == before.topologyAuthorityReady);
    QNP_CHECK(after.topologyQsrPort == before.topologyQsrPort);
    QNP_CHECK(after.topologyEnrolled == before.topologyEnrolled);
    QNP_CHECK(after.topologyAttached == before.topologyAttached);
}

QNP_TEST(sidecar_relay_child_assignment_forwards_only_the_explicit_unready_pair) {
    Fixture fixture;
    fixture.start();
    QNP_REQUIRE(fixture.sidecar != nullptr);
    QNP_REQUIRE(fixture.transport != nullptr);
    std::string error;
    QNP_REQUIRE(fixture.sidecar->configureTopologyAuthority(
        topologyActivationFor(fixture.transport->boundPort()), fixture.nowMs,
        error));
    fixture.publishStart();
    QNP_REQUIRE(fixture.sidecar->status().topologyAuthorityReady);

    const SidecarStatus before = fixture.sidecar->status();
    QNP_CHECK(!fixture.sidecar->issueTopologyRelayChildAssignment(
        0xC101u, 0xC102u, fixture.nowMs, error));
    QNP_CHECK(error.find("coordinator-selected relay child assignment was rejected")
              != std::string::npos);

    const SidecarStatus after = fixture.sidecar->status();
    QNP_CHECK(after.serving == before.serving);
    QNP_CHECK(after.boundPort == before.boundPort);
    QNP_CHECK(after.topologyAuthorityReady == before.topologyAuthorityReady);
    QNP_CHECK(after.topologyAuthorityGeneration
              == before.topologyAuthorityGeneration);
    QNP_CHECK(after.topologyRootNodeId == before.topologyRootNodeId);
    QNP_CHECK(after.topologyQsrPort == before.topologyQsrPort);
    QNP_CHECK(after.topologyEnrolled == before.topologyEnrolled);
    QNP_CHECK(after.topologyAttached == before.topologyAttached);
    QNP_CHECK(after.viewerCount == before.viewerCount);
    QNP_CHECK(after.reservationCount == before.reservationCount);
}

QNP_TEST(sidecar_paired_activation_forwards_only_the_explicit_unready_pair) {
    Fixture fixture;
    fixture.start();
    QNP_REQUIRE(fixture.sidecar != nullptr);
    QNP_REQUIRE(fixture.transport != nullptr);
    std::string error;
    QNP_REQUIRE(fixture.sidecar->configureTopologyAuthority(
        topologyActivationFor(fixture.transport->boundPort()), fixture.nowMs,
        error));
    fixture.publishStart();
    QNP_REQUIRE(fixture.sidecar->status().topologyAuthorityReady);

    const SidecarStatus before = fixture.sidecar->status();
    QNP_CHECK(!fixture.sidecar->issueTopologyRelayChildActivation(
        0xC201u, 0xC202u, fixture.nowMs, error));
    QNP_CHECK(error.find("coordinator-selected relay child activation was rejected")
              != std::string::npos);

    const SidecarStatus after = fixture.sidecar->status();
    QNP_CHECK(after.serving == before.serving);
    QNP_CHECK(after.boundPort == before.boundPort);
    QNP_CHECK(after.topologyAuthorityReady == before.topologyAuthorityReady);
    QNP_CHECK(after.topologyAuthorityGeneration
              == before.topologyAuthorityGeneration);
    QNP_CHECK(after.topologyRootNodeId == before.topologyRootNodeId);
    QNP_CHECK(after.topologyQsrPort == before.topologyQsrPort);
    QNP_CHECK(after.topologyEnrolled == before.topologyEnrolled);
    QNP_CHECK(after.topologyAttached == before.topologyAttached);
    QNP_CHECK(after.viewerCount == before.viewerCount);
    QNP_CHECK(after.reservationCount == before.reservationCount);
}

QNP_TEST(sidecar_host_viewer_selection_rejects_invalid_pairs_without_e0_mutation) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    QNP_REQUIRE(fixture.sidecar != nullptr);

    const SidecarStatus before = fixture.sidecar->status();
    std::string error;
    const TopologyViewerSelection none{};
    const TopologyViewerSelection other{0xC301u, 1u};
    QNP_CHECK(!fixture.sidecar->issueTopologyRelayChildActivationForViewers(
        none, other, fixture.nowMs, error));
    QNP_CHECK(error.find("distinct current viewers") != std::string::npos);

    const TopologyViewerSelection same{0xC302u, 1u};
    QNP_CHECK(!fixture.sidecar->issueTopologyRelayChildActivationForViewers(
        same, same, fixture.nowMs, error));
    QNP_CHECK(error.find("distinct current viewers") != std::string::npos);

    const SidecarStatus after = fixture.sidecar->status();
    QNP_CHECK(after.serving == before.serving);
    QNP_CHECK(after.hasGame == before.hasGame);
    QNP_CHECK(after.gameId == before.gameId);
    QNP_CHECK(after.boundPort == before.boundPort);
    QNP_CHECK(after.viewerCount == before.viewerCount);
    QNP_CHECK(after.reservationCount == before.reservationCount);
    QNP_CHECK(after.topologyAuthorityReady == before.topologyAuthorityReady);
    QNP_CHECK(after.topologyEnrolled == before.topologyEnrolled);
    QNP_CHECK(after.topologyAttached == before.topologyAttached);
}

QNP_TEST(sidecar_host_viewer_selection_revalidates_generations_and_never_promotes) {
    Fixture fixture;
    fixture.start();
    QNP_REQUIRE(fixture.sidecar != nullptr);
    QNP_REQUIRE(fixture.transport != nullptr);
    std::string error;
    QNP_REQUIRE(fixture.sidecar->configureTopologyAuthority(
        topologyActivationFor(fixture.transport->boundPort()), fixture.nowMs,
        error));
    fixture.publishStart();
    QNP_REQUIRE(fixture.sidecar->status().topologyAuthorityReady);

    HarnessViewer relay(peer(50, 50150).ipv4BE, 50150, 0xC311u,
                        fixture.identity);
    HarnessViewer child(peer(51, 50151).ipv4BE, 50151, 0xC312u,
                        fixture.identity);
    fixture.run({&relay, &child}, 40);
    const SidecarStatus streaming = fixture.sidecar->status();
    QNP_REQUIRE(streaming.viewerCount == 2u);

    topology::TopologyEnrollment relayEnrollment{};
    topology::TopologyEnrollment childEnrollment{};
    QNP_REQUIRE(fixture.sidecar->enrollTopologyViewer(
        0xC311u, 1u, true, fixture.nowMs,
        relayEnrollment, error));
    QNP_REQUIRE(fixture.sidecar->enrollTopologyViewer(
        0xC312u, 1u, true, fixture.nowMs,
        childEnrollment, error));
    QNP_REQUIRE(relayEnrollment.granted);
    QNP_REQUIRE(childEnrollment.granted);
    QNP_REQUIRE(relayEnrollment.nodeId != childEnrollment.nodeId);

    const SidecarStatus before = fixture.sidecar->status();
    QNP_REQUIRE(before.topologyEnrolled == 2u);
    const TopologyViewerSelection relaySelection{0xC311u, 1u};
    const TopologyViewerSelection childSelection{0xC312u, 1u};

    const TopologyViewerSelection staleRelay{0xC311u, 2u};
    QNP_CHECK(!fixture.sidecar->issueTopologyRelayChildActivationForViewers(
        staleRelay, childSelection, fixture.nowMs, error));
    QNP_CHECK(error.find("current streaming QSP viewer") != std::string::npos);

    SidecarStatus afterStale = fixture.sidecar->status();
    QNP_CHECK(afterStale.serving == before.serving);
    QNP_CHECK(afterStale.viewerCount == before.viewerCount);
    QNP_CHECK(afterStale.reservationCount == before.reservationCount);
    QNP_CHECK(afterStale.topologyAuthorityReady == before.topologyAuthorityReady);
    QNP_CHECK(afterStale.topologyEnrolled == before.topologyEnrolled);
    QNP_CHECK(afterStale.topologyAttached == before.topologyAttached);

    QNP_CHECK(!fixture.sidecar->issueTopologyRelayChildActivationForViewers(
        relaySelection, childSelection, fixture.nowMs, error));
    QNP_CHECK(error.find("coordinator-selected relay child activation was rejected")
              != std::string::npos);

    const SidecarStatus after = fixture.sidecar->status();
    QNP_CHECK(after.serving == before.serving);
    QNP_CHECK(after.boundPort == before.boundPort);
    QNP_CHECK(after.viewerCount == before.viewerCount);
    QNP_CHECK(after.reservationCount == before.reservationCount);
    QNP_CHECK(after.topologyAuthorityReady == before.topologyAuthorityReady);
    QNP_CHECK(after.topologyAuthorityGeneration
              == before.topologyAuthorityGeneration);
    QNP_CHECK(after.topologyRootNodeId == before.topologyRootNodeId);
    QNP_CHECK(after.topologyEnrolled == before.topologyEnrolled);
    QNP_CHECK(after.topologyAttached == before.topologyAttached);
}

QNP_TEST(sidecar_drain_completes_a_lagging_viewer_after_the_game_would_have_exited) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(60);

    HarnessViewer live(peer(2, 50001).ipv4BE, 50001, 0xAAAA0001ull,
                       fixture.identity);
    HarnessViewer lagging(peer(3, 50002).ipv4BE, 50002, 0xAAAA0002ull,
                          fixture.identity);
    lagging.playbackEnabled = false;

    fixture.run({&live, &lagging}, 40);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 2);

    fixture.publishSeal();
    SidecarStatus sealed = fixture.sidecar->status();
    QNP_CHECK(sealed.sealed);
    QNP_CHECK(!sealed.sealedIsTruncation);
    QNP_CHECK(sealed.hasPostTailProof);
    QNP_CHECK(sealed.postTailStateHash == stateHashFor(60));
    QNP_CHECK(sealed.terminalReason == qsp::TerminalReason::MatchEnded);

    lagging.playbackEnabled = true;
    fixture.run({&live, &lagging}, 120);

    QNP_CHECK(live.complete());
    QNP_CHECK(lagging.complete());
    const SidecarStatus after = fixture.sidecar->status();
    QNP_CHECK(after.viewersCompleted == 2);
    QNP_CHECK(after.viewersDeadlineExpired == 0);
    QNP_CHECK(after.viewerCount == 0);
    QNP_CHECK(after.phase == SidecarPhase::Retained);
}

QNP_TEST(sidecar_reports_deadline_expiry_for_a_viewer_that_never_finishes) {
    Fixture fixture;
    fixture.config.drainDeadlineMs = 3000;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(40);

    HarnessViewer stuck(peer(4, 50003).ipv4BE, 50003, 0xAAAA0003ull,
                        fixture.identity);
    stuck.playbackEnabled = false;
    fixture.run({&stuck}, 30);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 1);

    fixture.publishSeal();
    fixture.run({&stuck}, 400, 20);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(!stuck.complete());
    QNP_CHECK(status.viewersDeadlineExpired == 1);
    QNP_CHECK(status.viewersCompleted == 0);
    QNP_CHECK(status.viewerCount == 0);
}

QNP_TEST(sidecar_postgame_retention_is_absolute_from_seal_not_after_drain) {
    Fixture fixture;
    fixture.config.drainDeadlineMs = 3000;
    fixture.config.retentionMs = 5000;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(20);

    HarnessViewer stuck(peer(47, 50047).ipv4BE, 50047,
                        0xAAAA0047ull, fixture.identity);
    stuck.playbackEnabled = false;
    fixture.run({&stuck}, 30);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    fixture.publishSeal();
    SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.retentionRemainingMs > 0);
    QNP_CHECK(status.retentionRemainingMs <= 5000);
    fixture.advance(3001);
    fixture.sidecar->service(fixture.nowMs);
    status = fixture.sidecar->status();
    QNP_REQUIRE(status.phase == SidecarPhase::Retained);
    QNP_CHECK(status.viewerCount == 0);
    QNP_CHECK(status.retentionRemainingMs <= 1999);

    fixture.advance(2000);
    fixture.sidecar->service(fixture.nowMs);
    status = fixture.sidecar->status();
    QNP_CHECK(status.phase == SidecarPhase::Expired);
    QNP_CHECK(status.retentionRemainingMs == 0);
}

QNP_TEST(sidecar_retains_then_expires_the_completed_set) {
    Fixture fixture;
    fixture.config.retentionMs = 5000;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(10);
    fixture.publishSeal();

    fixture.sidecar->service(fixture.nowMs);
    SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.phase == SidecarPhase::Retained);
    QNP_CHECK(status.code == SourceStatusCode::CompletedClean);
    QNP_CHECK(status.retentionRemainingMs > 0);
    QNP_CHECK(status.retentionRemainingMs <= 5000);

    fixture.transport->deliver(
        peer(9, 50100),
        encodeHello(fixture.identity, 0xBBBB0001ull, 1, 1));
    fixture.advance(10);
    fixture.sidecar->service(fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 1);

    fixture.advance(6000);
    fixture.sidecar->service(fixture.nowMs);
    status = fixture.sidecar->status();
    QNP_CHECK(status.phase == SidecarPhase::Expired);
    QNP_CHECK(status.code == SourceStatusCode::RetentionExpired);
    QNP_CHECK(status.viewerCount == 0);

    fixture.transport->deliver(
        peer(10, 50101),
        encodeHello(fixture.identity, 0xBBBB0002ull, 1, 1));
    fixture.advance(10);
    fixture.sidecar->service(fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
}

QNP_TEST(sidecar_retention_deadline_is_not_reset_by_late_viewers) {
    Fixture fixture;
    fixture.config.retentionMs = 5000;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(10);
    fixture.publishSeal();
    fixture.sidecar->service(fixture.nowMs);
    QNP_REQUIRE(fixture.sidecar->status().phase
                == SidecarPhase::Retained);
    const uint64_t originalRemaining =
        fixture.sidecar->status().retentionRemainingMs;

    fixture.advance(4000);
    fixture.transport->deliver(
        peer(11, 50102),
        encodeHello(fixture.identity, 0xBBBB0003ull, 1, 1));
    fixture.sidecar->service(fixture.nowMs);
    SidecarStatus status = fixture.sidecar->status();
    QNP_REQUIRE(status.viewerCount == 1);
    QNP_CHECK(status.retentionRemainingMs < originalRemaining);
    QNP_CHECK(status.retentionRemainingMs <= 1000);

    fixture.advance(1100);
    fixture.sidecar->service(fixture.nowMs);
    status = fixture.sidecar->status();
    QNP_CHECK(status.phase == SidecarPhase::Expired);
    QNP_CHECK(status.viewerCount == 0);
    QNP_CHECK(status.viewersDeadlineExpired == 1);
}

QNP_TEST(sidecar_seals_a_publication_gap_as_truncation_not_a_proof) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(20);

    PublicationEvent gap{};
    gap.kind = PublicationKind::Record;
    gap.sequence = fixture.publicationSequence + 5;
    gap.record = makeRecord(25);
    fixture.sidecar->publish(gap, fixture.nowMs);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.sealed);
    QNP_CHECK(status.sealedIsTruncation);
    QNP_CHECK(status.terminalReason == qsp::TerminalReason::ProtocolFailure);
    QNP_CHECK(status.publicationGaps == 1);
    QNP_CHECK(status.truncatedRecords >= 1);
    QNP_CHECK(status.confirmedRecords == 19);
    QNP_CHECK(status.hasPostTailProof);
    QNP_CHECK(status.postTailStateHash == stateHashFor(19));
    QNP_CHECK(status.postTailStateHash != stateHashFor(20));
    QNP_CHECK(status.postTailStateHash != stateHashFor(25));
    QNP_CHECK(!status.failureReason.empty());
}

QNP_TEST(sidecar_seal_without_a_proof_is_a_truncation_not_match_ended) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(8);

    fixture.sidecar->requestSeal(qsp::TerminalReason::MatchEnded, 0, false,
                                 fixture.nowMs);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.sealed);
    QNP_CHECK(status.sealedIsTruncation);
    QNP_CHECK(status.terminalReason == qsp::TerminalReason::ProtocolFailure);
    QNP_CHECK(status.confirmedRecords == 7);
    QNP_CHECK(status.postTailStateHash == stateHashFor(7));
    QNP_CHECK(status.truncatedRecords == 1);
}

QNP_TEST(sidecar_explicit_loss_report_truncates_too) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(5);

    PublicationEvent loss{};
    loss.kind = PublicationKind::LossReport;
    loss.sequence = fixture.publicationSequence++;
    fixture.sidecar->publish(loss, fixture.nowMs);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.sealed);
    QNP_CHECK(status.sealedIsTruncation);
    QNP_CHECK(status.postTailStateHash == stateHashFor(4));
}

QNP_TEST(sidecar_empty_archive_truncation_uses_the_descriptor_initial_state) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.sidecar->requestSeal(qsp::TerminalReason::SourceStopped, 0, false,
                                 fixture.nowMs);
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.sealed);
    QNP_CHECK(status.sealedIsTruncation);
    QNP_CHECK(status.confirmedRecords == 0);
    QNP_CHECK(status.hasPostTailProof);
    QNP_CHECK(status.postTailStateHash == stateHashFor(0));
}

QNP_TEST(sidecar_clean_seal_commits_the_held_record) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(12);
    QNP_CHECK(fixture.sidecar->status().confirmedRecords == 11);
    fixture.publishSeal();
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.confirmedRecords == 12);
    QNP_CHECK(!status.sealedIsTruncation);
    QNP_CHECK(status.truncatedRecords == 0);
    QNP_CHECK(status.postTailStateHash == stateHashFor(12));
    QNP_CHECK(status.terminalRecordCursor == 12);
    QNP_CHECK(status.terminalPrefixDigest == prefixDigestFor(12));
}

QNP_TEST(sidecar_first_terminal_is_immutable_and_exact_duplicates_are_idempotent) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(12);
    fixture.publishSeal();

    PublicationEvent duplicate{};
    duplicate.kind = PublicationKind::Seal;
    duplicate.sequence = fixture.publicationSequence++;
    duplicate.reason = qsp::TerminalReason::MatchEnded;
    duplicate.terminalRecordCursor = 12;
    duplicate.terminalPrefixDigest = prefixDigestFor(12);
    duplicate.hasTerminalPrefixProof = true;
    duplicate.postTailStateHash = stateHashFor(12);
    duplicate.hasPostTailProof = true;
    fixture.sidecar->publish(duplicate, fixture.nowMs);
    QNP_CHECK(
        fixture.sidecar->status().conflictingTerminalRequests == 0);

    PublicationEvent conflict = duplicate;
    conflict.sequence = fixture.publicationSequence++;
    conflict.reason = qsp::TerminalReason::SourceStopped;
    conflict.postTailStateHash ^= 0x55u;
    fixture.sidecar->publish(conflict, fixture.nowMs);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.conflictingTerminalRequests == 1);
    QNP_CHECK(status.terminalReason == qsp::TerminalReason::MatchEnded);
    QNP_CHECK(status.terminalRecordCursor == 12);
    QNP_CHECK(status.terminalPrefixDigest == prefixDigestFor(12));
    QNP_CHECK(status.postTailStateHash == stateHashFor(12));
    QNP_CHECK(!status.sealedIsTruncation);
}

QNP_TEST(sidecar_rejects_a_terminal_with_a_missing_cursor_or_bad_prefix) {
    for (int variant = 0; variant < 2; ++variant) {
        Fixture fixture;
        fixture.start();
        fixture.publishStart();
        fixture.publishRecords(12);

        PublicationEvent seal{};
        seal.kind = PublicationKind::Seal;
        seal.sequence = fixture.publicationSequence++;
        seal.reason = qsp::TerminalReason::MatchEnded;
        seal.terminalRecordCursor = variant == 0 ? 11u : 12u;
        seal.terminalPrefixDigest =
            prefixDigestFor(12) ^ (variant == 0 ? 0u : 1u);
        seal.hasTerminalPrefixProof = true;
        seal.postTailStateHash = stateHashFor(12);
        seal.hasPostTailProof = true;
        fixture.sidecar->publish(seal, fixture.nowMs);

        const SidecarStatus status = fixture.sidecar->status();
        QNP_CHECK(status.sealed);
        QNP_CHECK(status.sealedIsTruncation);
        QNP_CHECK(status.terminalReason
                  == qsp::TerminalReason::ProtocolFailure);
        QNP_CHECK(status.confirmedRecords == 11);
        QNP_CHECK(status.postTailStateHash == stateHashFor(11));
    }
}

QNP_TEST(sidecar_tail_proof_releases_the_held_record_without_sealing) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(12);
    QNP_CHECK(fixture.sidecar->status().confirmedRecords == 11);

    fixture.publishTailProof(12, stateHashFor(12));

    const SidecarStatus held = fixture.sidecar->status();
    QNP_CHECK(held.confirmedRecords == 12);
    QNP_CHECK(!held.sealed);
    QNP_CHECK(held.truncatedRecords == 0);
    QNP_CHECK(held.tailProofs == 1);
    QNP_CHECK(held.tailProofCommits == 1);
    QNP_CHECK(held.phase == SidecarPhase::Active);

    fixture.publishTailProof(12, stateHashFor(12));
    QNP_CHECK(fixture.sidecar->status().confirmedRecords == 12);
    QNP_CHECK(fixture.sidecar->status().tailProofCommits == 1);

    fixture.publishRecords(3);
    QNP_CHECK(fixture.sidecar->status().confirmedRecords == 14);
    fixture.publishSeal();
    const SidecarStatus sealed = fixture.sidecar->status();
    QNP_CHECK(sealed.confirmedRecords == 15);
    QNP_CHECK(!sealed.sealedIsTruncation);
    QNP_CHECK(sealed.postTailStateHash == stateHashFor(15));
}

QNP_TEST(sidecar_publisher_rejects_sidecar_socket_handoff) {
    using namespace qgame::spectator_publish;

    qipc::IpcConfig publisherConfig{};
    publisherConfig.magic = qipc::kIpcMagic;
    publisherConfig.version = qipc::kIpcVersion;
    publisherConfig.mode = qipc::MODE_NETPLAY;
    publisherConfig.isHost = 1u;
    publisherConfig.localPlayer = 1u;
    publisherConfig.serveSpectators = 1u;
    publisherConfig.spectatorServiceMode =
        qipc::SPECTATOR_SERVICE_SIDECAR;
    publisherConfig.sessionId = 0xE001u;
    publisherConfig.cookieLow = 0x12345678u;
    publisherConfig.spectatorCookieLow = 0xABCDEF01u;
    publisherConfig.spectatorCookieHigh = 0x1020304050607080ull;
    publisherConfig.hasSpectatorSocket = 1u;

    auto* channel = new CapturingPublisherChannel();
    SidecarPublisherOptions options{};
    options.inlineWorker = true;
    QNP_CHECK(!startSidecarPublisherForTest(
        publisherConfig, options, channel));
    QNP_CHECK(!sidecarPublisherActive());
}

QNP_TEST(sidecar_client_private_publisher_requires_its_separate_entry_point) {
    using namespace qgame::spectator_publish;

    qipc::IpcConfig publisherConfig{};
    publisherConfig.mode = qipc::MODE_NETPLAY;
    publisherConfig.isHost = 0u;
    publisherConfig.localPlayer = 2u;
    publisherConfig.sessionId = 0xE002u;
    publisherConfig.compatibilityId = qipc::kRuntimeCompatibilityId;
    publisherConfig.cookieLow = 0x12345678u;
    publisherConfig.spectatorCookieLow = 0xABCDEF01u;
    publisherConfig.spectatorCookieHigh = 0x1020304050607080ull;
    publisherConfig.hasSocket = 1u;
    publisherConfig.peerAddrBE = 0x0100007Fu;
    publisherConfig.peerPortBE = 0x4C1Du;
    publisherConfig.serveSpectators = 1u;
    publisherConfig.spectatorServiceMode =
        qipc::SPECTATOR_SERVICE_SIDECAR;
    publisherConfig.spectatorClientPublisherEnabled = 1u;
    publisherConfig.spectatorClientPublisherGeneration = 73u;
    publisherConfig.spectatorClientPublisherNonce =
        0x7B3A98D14560C2EFull;

    SidecarPublisherOptions options{};
    options.inlineWorker = true;
    QNP_CHECK(!startSidecarPublisherForTest(
        publisherConfig, options, new CapturingPublisherChannel()));
    QNP_CHECK(!sidecarPublisherActive());
    QNP_REQUIRE(startClientPrivateSidecarPublisherForTest(
        publisherConfig, options, new CapturingPublisherChannel()));
    QNP_CHECK(sidecarPublisherActive());
    stopSidecarPublisher();

    publisherConfig.spectatorClientPublisherEnabled = 0u;
    publisherConfig.spectatorClientPublisherGeneration = 0u;
    publisherConfig.spectatorClientPublisherNonce = 0u;
    publisherConfig.isHost = 1u;
    publisherConfig.localPlayer = 1u;
    QNP_CHECK(!startClientPrivateSidecarPublisherForTest(
        publisherConfig, options, new CapturingPublisherChannel()));
    QNP_CHECK(!sidecarPublisherActive());
}

QNP_TEST(sidecar_client_private_collector_never_becomes_a_public_qsp_root) {
    Fixture fixture;
    fixture.config.policy = AdmissionPolicy::CodeOnly;
    fixture.config.maxViewers = 1u;
    fixture.config.maxReservations = 1u;
    fixture.config.clientPrivateRoot.enabled = true;
    fixture.config.clientPrivateRoot.publisherGeneration = 73u;
    fixture.config.clientPrivateRoot.publisherNonce =
        0x7B3A98D14560C2EFull;
    fixture.start(AdmissionPolicy::CodeOnly);

    const SidecarStatus initial = fixture.sidecar->status();
    QNP_CHECK(fixture.transport->opened);
    QNP_CHECK(initial.clientPrivateRootDormant);
    QNP_CHECK(!initial.clientPrivateRuntimeApplied);
    QNP_CHECK(!initial.serving);
    QNP_CHECK(initial.boundPort == fixture.transport->port);
    QNP_CHECK(initial.policy == AdmissionPolicy::CodeOnly);

    fixture.sidecar->setAdmissionPolicy(AdmissionPolicy::Public);
    QNP_CHECK(fixture.sidecar->admissionPolicy()
              == AdmissionPolicy::CodeOnly);

    const uint8_t garbage[] = {0x51u, 0x53u, 0x50u};
    QNP_CHECK(fixture.sidecar->onDatagram(peer(42, 42042), garbage,
                                          sizeof(garbage), fixture.nowMs)
              == IngressOutcome::ServiceUnavailable);
    fixture.transport->deliver(peer(43, 42043),
                               std::vector<uint8_t>{0x01u, 0x02u});
    fixture.sidecar->service(fixture.nowMs + 1u);
    QNP_CHECK(fixture.transport->inbound.size() == 1u);

    GrantRequest grantRequest{};
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_CHECK(!fixture.sidecar->issueGrant(
        grantRequest, grant, deny, fixture.nowMs + 2u));
    QNP_CHECK(deny == AdmissionDenyReason::PolicyDisabled);

    std::string topologyError;
    const topology::TopologyActivationRequest topologyRequest =
        topologyActivationFor(fixture.transport->port);
    QNP_CHECK(!fixture.sidecar->configureTopologyAuthority(
        topologyRequest, fixture.nowMs + 3u, topologyError));

    fixture.publishStart();
    fixture.publishRecords(1u);
    fixture.publishTailProof(1u, stateHashFor(1u));
    const SidecarStatus collected = fixture.sidecar->status();
    QNP_CHECK(collected.hasGame);
    QNP_CHECK(collected.confirmedRecords == 1u);
    QNP_CHECK(collected.clientPrivateRootDormant);
    QNP_CHECK(!collected.serving);
}

#ifdef _WIN32
QNP_TEST(sidecar_client_private_runtime_receipt_only_records_exact_local_launch) {
    qsp::SessionIdentity identity = makeIdentity();
    identity.sessionId = 0x13572468u;
    identity.cookie.low = 0x2468ACE013579BDFull;
    identity.cookie.high = 0x0F1E2D3C4B5A6978ull;

    SidecarConfig config{};
    config.identity = identity;
    config.simulationContract = qipc::kRuntimeCompatibilityId;
    config.compatibilityIdentity = makeCompatibilityIdentity();
    config.policy = AdmissionPolicy::CodeOnly;
    config.maxViewers = 1u;
    config.maxReservations = 1u;
    config.clientPrivateRoot.enabled = true;
    config.clientPrivateRoot.publisherGeneration = 73u;
    config.clientPrivateRoot.publisherNonce =
        0x7B3A98D14560C2EFull;

    auto transport = std::unique_ptr<FakeTransport>(new FakeTransport());
    std::unique_ptr<SpectatorSidecar> sidecar(new SpectatorSidecar(config));
    std::string startError;
    QNP_REQUIRE(sidecar->start(std::move(transport), nullptr, 1000u,
                               startError));

    qipc::IpcConfig launch{};
    launch.mode = qipc::MODE_NETPLAY;
    launch.isHost = 0u;
    launch.localPlayer = 2u;
    launch.delay = 3u;
    launch.rollback = 6u;
    launch.remoteDelay = 2u;
    launch.remoteRollback = 7u;
    launch.sessionId = static_cast<uint32_t>(identity.sessionId);
    launch.compatibilityId = qipc::kRuntimeCompatibilityId;
    launch.compatibilityIdentity = config.compatibilityIdentity;
    launch.cookieLow = 0xABCD1234u;
    launch.spectatorCookieLow = identity.cookie.low;
    launch.spectatorCookieHigh = identity.cookie.high;
    launch.hasSocket = 1u;
    launch.peerAddrBE = 0x0100007Fu;
    launch.peerPortBE = 0x4C1Du;
    launch.serveSpectators = 1u;
    launch.spectatorServiceMode = qipc::SPECTATOR_SERVICE_SIDECAR;
    launch.spectatorClientPublisherEnabled = 1u;
    launch.spectatorClientPublisherGeneration =
        config.clientPrivateRoot.publisherGeneration;
    launch.spectatorClientPublisherNonce =
        config.clientPrivateRoot.publisherNonce;
    launch.configurationProvenance =
        qnetplay::config_provenance::makeCommittedWire(
            qnetplay::config_provenance::Stage::LocallySelected,
            qnetplay::config_provenance::Stage::LocallySelected,
            qnetplay::config_provenance::Stage::CasterAdvertised,
            qnetplay::config_provenance::Stage::CasterAdvertised);
    QNP_REQUIRE(qipc::validateSpectatorClientPrivatePublisher(launch)
                == nullptr);
    QNP_REQUIRE(qipc::validateIpcCompatibilityIdentity(launch) == nullptr);
    QNP_REQUIRE(qipc::validateIpcConfigurationProvenance(launch) == nullptr);

    constexpr uint32_t kPid = 4242u;
    qipc::SpectatorClientPublisherRuntimeAppliedReceipt receipt{};
    QNP_REQUIRE(qipc::buildSpectatorClientPublisherRuntimeAppliedReceipt(
        launch, kPid, receipt));
    qipc::SpectatorClientPublisherRuntimeAppliedReceipt mismatched = receipt;
    ++mismatched.channelGeneration;
    std::string receiptError;
    QNP_CHECK(!sidecar->confirmClientPrivateRuntimeApplied(
        launch, mismatched, kPid, receiptError));
    QNP_CHECK(!sidecar->status().clientPrivateRuntimeApplied);

    receiptError.clear();
    QNP_CHECK(sidecar->confirmClientPrivateRuntimeApplied(
        launch, receipt, kPid, receiptError));
    const SidecarStatus status = sidecar->status();
    QNP_CHECK(status.clientPrivateRootDormant);
    QNP_CHECK(status.clientPrivateRuntimeApplied);
    QNP_CHECK(!status.serving);

    receiptError.clear();
    QNP_CHECK(!sidecar->confirmClientPrivateRuntimeApplied(
        launch, receipt, kPid, receiptError));
}

QNP_TEST(client_private_runtime_receipt_receiver_reads_one_exact_local_record) {
    constexpr uint32_t kGeneration = 0xCAFE73u;
    const uint32_t pid = static_cast<uint32_t>(GetCurrentProcessId());
    qipc::IpcConfig launch{};
    launch.mode = qipc::MODE_NETPLAY;
    launch.isHost = 0u;
    launch.localPlayer = 2u;
    launch.delay = 3u;
    launch.rollback = 6u;
    launch.remoteDelay = 2u;
    launch.remoteRollback = 7u;
    launch.sessionId = 0x13572468u;
    launch.compatibilityId = qipc::kRuntimeCompatibilityId;
    launch.cookieLow = 0xABCD1234u;
    launch.spectatorCookieLow = 0x13579BDFu;
    launch.hasSocket = 1u;
    launch.peerAddrBE = 0x0100007Fu;
    launch.peerPortBE = 0x4C1Du;
    launch.serveSpectators = 1u;
    launch.spectatorServiceMode = qipc::SPECTATOR_SERVICE_SIDECAR;
    launch.spectatorClientPublisherEnabled = 1u;
    launch.spectatorClientPublisherGeneration = kGeneration;
    launch.spectatorClientPublisherNonce = 0x7B3A98D14560C2EFull;
    launch.configurationProvenance =
        qnetplay::config_provenance::makeCommittedWire(
            qnetplay::config_provenance::Stage::LocallySelected,
            qnetplay::config_provenance::Stage::LocallySelected,
            qnetplay::config_provenance::Stage::CasterAdvertised,
            qnetplay::config_provenance::Stage::CasterAdvertised);
    qipc::SpectatorClientPublisherRuntimeAppliedReceipt expected{};
    QNP_REQUIRE(qipc::buildSpectatorClientPublisherRuntimeAppliedReceipt(
        launch, pid, expected));
    uint8_t bytes[qipc::kSpectatorClientPublisherRuntimeAppliedReceiptBytes]{};
    size_t byteCount = 0u;
    QNP_REQUIRE(qipc::encodeSpectatorClientPublisherRuntimeAppliedReceipt(
        expected, bytes, sizeof(bytes), byteCount));

    ClientPrivateRuntimeAppliedReceiver receiver;
    std::string receiverError;
    QNP_REQUIRE(receiver.open(pid, kGeneration, receiverError));

    wchar_t pipeName[128]{};
    qipc::ipcSpectatorClientPublisherPipeName(
        pipeName, sizeof(pipeName) / sizeof(pipeName[0]), pid, kGeneration);
    bool writerOpened = false;
    bool writerWrote = false;
    DWORD writerError = ERROR_SUCCESS;
    std::thread writer([&] {
        HANDLE pipe = CreateFileW(pipeName, GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            writerError = GetLastError();
            return;
        }
        writerOpened = true;
        DWORD written = 0u;
        writerWrote = WriteFile(pipe, bytes, static_cast<DWORD>(byteCount),
                                &written, nullptr) != FALSE
            && written == byteCount;
        if (!writerWrote) writerError = GetLastError();
        CloseHandle(pipe);
    });

    qipc::SpectatorClientPublisherRuntimeAppliedReceipt received{};
    const bool read = receiver.receive(3000u, received, receiverError);
    writer.join();
    QNP_CHECK(writerOpened);
    QNP_CHECK(writerWrote);
    QNP_CHECK(writerError == ERROR_SUCCESS);
    QNP_CHECK(read);
    QNP_CHECK(qipc::spectatorClientPublisherRuntimeAppliedReceiptMatches(
        received, launch, pid));
}
#endif

QNP_TEST(sidecar_accepts_publisher_digest_with_complete_compatibility_identity) {
    using namespace qgame::spectator_publish;

    Fixture fixture;
    fixture.start();

    qipc::IpcConfig publisherConfig{};
    publisherConfig.magic = qipc::kIpcMagic;
    publisherConfig.version = qipc::kIpcVersion;
    publisherConfig.mode = qipc::MODE_NETPLAY;
    publisherConfig.isHost = 1u;
    publisherConfig.localPlayer = 1u;
    publisherConfig.serveSpectators = 1u;
    publisherConfig.spectatorServiceMode =
        qipc::SPECTATOR_SERVICE_SIDECAR;
    publisherConfig.sessionId = fixture.identity.sessionId;
    publisherConfig.cookieLow = 0x12345678u;
    publisherConfig.spectatorCookieLow = fixture.identity.cookie.low;
    publisherConfig.spectatorCookieHigh = fixture.identity.cookie.high;

    auto* channel = new CapturingPublisherChannel();
    SidecarPublisherOptions options{};
    options.inlineWorker = true;
    options.drainPerTurn = 1u;
    options.tailProofIdleMs = 0u;
    QNP_REQUIRE(startSidecarPublisherForTest(
        publisherConfig, options, channel));
    struct StopPublisher {
        ~StopPublisher() { stopSidecarPublisher(); }
    } stopPublisher;

    SidecarStartInfo start{};
    start.gameId = kGameId;
    start.simulationContract = kContract;
    start.compatibilityIdentity = fixture.config.compatibilityIdentity;
    start.initialStateHash = stateHashFor(0);
    start.firstTimelineFrame = 0u;
    start.playerOneDelay = 2u;
    start.playerTwoDelay = 2u;
    start.rollbackWindow = 8u;
    QNP_REQUIRE(publishStart(start, nullptr, 0u));

    qsp::ConfirmedRecord record = makeRecord(0u);
    record.playerOneInput = 0x0101u;
    record.playerTwoInput = 0x0202u;
    QNP_REQUIRE(publishConfirmed(record));
    publishTailProof(1u, stateHashFor(1u));
    qgame::spectator_publish::pumpSidecarForTest();

    const SidecarPublisherStats publisherStats = sidecarStats();
    const std::vector<uint8_t> bytes = channel->bytes;
    QNP_CHECK(publisherStats.written == 1u);
    QNP_CHECK(publisherStats.tailProofsWritten == 1u);
    QNP_REQUIRE(!bytes.empty());

    size_t offset = 0u;
    while (offset < bytes.size()) {
        QNP_REQUIRE(bytes.size() - offset >= kPublicationHeaderSize);
        const size_t payloadOffset = offset + 16u;
        const uint32_t payloadBytes =
            static_cast<uint32_t>(bytes[payloadOffset])
            | (static_cast<uint32_t>(bytes[payloadOffset + 1u]) << 8u)
            | (static_cast<uint32_t>(bytes[payloadOffset + 2u]) << 16u)
            | (static_cast<uint32_t>(bytes[payloadOffset + 3u]) << 24u);
        const size_t frameBytes =
            kPublicationHeaderSize + static_cast<size_t>(payloadBytes);
        QNP_REQUIRE(frameBytes >= kPublicationHeaderSize);
        QNP_REQUIRE(frameBytes <= bytes.size() - offset);

        std::vector<PublicationEvent> events;
        QNP_REQUIRE(decodePublicationFrame(
            bytes.data() + offset, frameBytes, events));
        for (const PublicationEvent& event : events)
            fixture.sidecar->publish(event, fixture.nowMs);
        offset += frameBytes;
    }

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.hasGame);
    QNP_CHECK(status.phase == SidecarPhase::Active);
    QNP_CHECK(status.confirmedRecords == 1u);
    QNP_CHECK(status.tailProofs == 1u);
    QNP_CHECK(status.tailProofCommits == 1u);
    QNP_CHECK(status.spoolFailures == 0u);
    QNP_CHECK(status.failureReason.empty());
}

QNP_TEST(sidecar_chained_set_events_interleave_with_records_exactly) {
    using namespace qgame::spectator_publish;

    Fixture fixture;
    fixture.start();

    qipc::IpcConfig publisherConfig{};
    publisherConfig.magic = qipc::kIpcMagic;
    publisherConfig.version = qipc::kIpcVersion;
    publisherConfig.mode = qipc::MODE_NETPLAY;
    publisherConfig.isHost = 1u;
    publisherConfig.localPlayer = 1u;
    publisherConfig.serveSpectators = 1u;
    publisherConfig.spectatorServiceMode =
        qipc::SPECTATOR_SERVICE_SIDECAR;
    publisherConfig.sessionId = fixture.identity.sessionId;
    publisherConfig.cookieLow = 0x12345678u;
    publisherConfig.spectatorCookieLow = fixture.identity.cookie.low;
    publisherConfig.spectatorCookieHigh = fixture.identity.cookie.high;

    auto* channel = new CapturingPublisherChannel();
    SidecarPublisherOptions options{};
    options.inlineWorker = true;
    options.drainPerTurn = 64u;
    options.tailProofIdleMs = 0u;
    QNP_REQUIRE(startSidecarPublisherForTest(
        publisherConfig, options, channel));
    struct StopPublisher {
        ~StopPublisher() { stopSidecarPublisher(); }
    } stopPublisher;

    SidecarStartInfo start{};
    start.gameId = kGameId;
    start.simulationContract = kContract;
    start.compatibilityIdentity = fixture.config.compatibilityIdentity;
    start.initialStateHash = stateHashFor(0);
    start.firstTimelineFrame = 0u;
    start.playerOneDelay = 2u;
    start.playerTwoDelay = 2u;
    start.rollbackWindow = 8u;
    QNP_REQUIRE(publishStart(start, nullptr, 0u));

    for (uint32_t index = 0; index < 3; ++index)
        QNP_REQUIRE(publishConfirmed(makeRecord(index)));
    qsp::SetEvent started{};
    started.kind = qsp::SetEventKind::MatchStarted;
    started.matchId = 1u;
    started.valueC = 1u;
    QNP_REQUIRE(publishChainedSetEvent(started));
    for (uint32_t index = 3; index < 5; ++index)
        QNP_REQUIRE(publishConfirmed(makeRecord(index)));
    qsp::SetEvent palette{};
    palette.kind = qsp::SetEventKind::PaletteSelected;
    palette.matchId = 1u;
    palette.activeSlotMask = 0x02u;
    palette.valueC = 3u;
    for (size_t i = 4; i < qsp::kSetPaletteEntryBytes; ++i)
        palette.paletteEntries[i] = static_cast<uint8_t>(i * 3u);
    QNP_REQUIRE(publishChainedSetEvent(palette));
    for (uint32_t index = 5; index < 7; ++index)
        QNP_REQUIRE(publishConfirmed(makeRecord(index)));
    qsp::SetEvent ended{};
    ended.kind = qsp::SetEventKind::MatchResult;
    ended.matchId = 1u;
    QNP_REQUIRE(publishChainedSetEvent(ended));
    publishTailProof(7u, stateHashFor(7u));
    qgame::spectator_publish::pumpSidecarForTest();

    const std::vector<uint8_t> bytes = channel->bytes;
    QNP_REQUIRE(!bytes.empty());
    size_t offset = 0u;
    while (offset < bytes.size()) {
        QNP_REQUIRE(bytes.size() - offset >= kPublicationHeaderSize);
        const size_t payloadOffset = offset + 16u;
        const uint32_t payloadBytes =
            static_cast<uint32_t>(bytes[payloadOffset])
            | (static_cast<uint32_t>(bytes[payloadOffset + 1u]) << 8u)
            | (static_cast<uint32_t>(bytes[payloadOffset + 2u]) << 16u)
            | (static_cast<uint32_t>(bytes[payloadOffset + 3u]) << 24u);
        const size_t frameBytes =
            kPublicationHeaderSize + static_cast<size_t>(payloadBytes);
        QNP_REQUIRE(frameBytes <= bytes.size() - offset);
        std::vector<PublicationEvent> events;
        QNP_REQUIRE(decodePublicationFrame(
            bytes.data() + offset, frameBytes, events));
        for (const PublicationEvent& event : events)
            fixture.sidecar->publish(event, fixture.nowMs);
        offset += frameBytes;
    }

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.hasGame);
    QNP_CHECK(status.confirmedRecords == 7u);
    QNP_CHECK(status.spoolFailures == 0u);
    QNP_CHECK(status.failureReason.empty());

    uint32_t eventCount = 0u;
    QNP_REQUIRE(fixture.sidecar->archiveForTest()->setEventCount(kGameId, eventCount));
    QNP_CHECK(eventCount == 3u);
    qsp::SetEvent storedPalette{};
    QNP_REQUIRE(fixture.sidecar->archiveForTest()->setEventAt(kGameId, 1u, storedPalette));
    QNP_CHECK(storedPalette.kind == qsp::SetEventKind::PaletteSelected);
    QNP_CHECK(storedPalette.recordCursor == 5u);
    QNP_CHECK(storedPalette.matchId == 1u);
    QNP_CHECK(storedPalette.valueC == 3u);
    QNP_CHECK(std::memcmp(storedPalette.paletteEntries,
                          palette.paletteEntries,
                          qsp::kSetPaletteEntryBytes) == 0);
    std::vector<qsp::SetMatchIndexEntry> matches;
    QNP_REQUIRE(fixture.sidecar->archiveForTest()->matchIndex(kGameId, matches));
    QNP_REQUIRE(matches.size() == 1u);
    QNP_CHECK(matches[0].matchId == 1u);
    QNP_CHECK(matches[0].firstRecord == 3u);
    QNP_CHECK(matches[0].ended);
    uint32_t seek = 0u;
    QNP_REQUIRE(fixture.sidecar->archiveForTest()->seekRecordForMatch(kGameId, 1u, seek));
    QNP_CHECK(seek == 3u);
}

QNP_TEST(sidecar_tail_proof_for_another_prefix_or_without_a_hash_keeps_holding) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(9);
    QNP_CHECK(fixture.sidecar->status().confirmedRecords == 8);

    fixture.publishTailProof(7, stateHashFor(7));
    fixture.publishTailProof(12, stateHashFor(12));
    fixture.publishTailProof(9, 0);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.confirmedRecords == 8);
    QNP_CHECK(status.tailProofCommits == 0);
    QNP_CHECK(!status.sealed);

    fixture.sidecar->requestSeal(qsp::TerminalReason::MatchEnded, 0, false,
                                 fixture.nowMs);
    const SidecarStatus sealed = fixture.sidecar->status();
    QNP_CHECK(sealed.sealedIsTruncation);
    QNP_CHECK(sealed.confirmedRecords == 8);
    QNP_CHECK(sealed.postTailStateHash == stateHashFor(8));
}

QNP_TEST(sidecar_truncation_after_a_tail_proof_still_proves_its_prefix) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(6);
    fixture.publishTailProof(6, stateHashFor(6));
    QNP_CHECK(fixture.sidecar->status().confirmedRecords == 6);

    PublicationEvent loss{};
    loss.kind = PublicationKind::LossReport;
    loss.sequence = fixture.publicationSequence++;
    fixture.sidecar->publish(loss, fixture.nowMs);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.sealed);
    QNP_CHECK(status.sealedIsTruncation);
    QNP_CHECK(status.confirmedRecords == 6);
    QNP_CHECK(status.hasPostTailProof);
    QNP_CHECK(status.postTailStateHash == stateHashFor(6));
    QNP_CHECK(status.truncatedRecords == 0);
}

QNP_TEST(sidecar_refuses_new_admissions_once_sealed_so_the_drain_can_finish) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(30);

    HarnessViewer attached(peer(2, 50101).ipv4BE, 50101, 0xBBBB0001ull,
                           fixture.identity);
    fixture.run({&attached}, 30);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 1);

    fixture.publishSeal();
    QNP_CHECK(fixture.sidecar->status().phase == SidecarPhase::Sealed);

    const RouteEndpoint latecomer = peer(9, 50999);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0xCCCC0001ull, 1, 1);
    const IngressOutcome outcome = fixture.sidecar->onDatagram(
        latecomer, hello.data(), hello.size(), fixture.nowMs);
    QNP_CHECK(outcome == IngressOutcome::AdmissionDenied);
    fixture.sidecar->service(fixture.nowMs);
    qsp::RejectReason reason = qsp::RejectReason::InvalidRequest;
    QNP_CHECK(sawReject(*fixture.transport, latecomer, fixture.identity,
                        reason));

    fixture.run({&attached}, 120);
    QNP_CHECK(attached.complete());
    const SidecarStatus after = fixture.sidecar->status();
    QNP_CHECK(after.viewerCount == 0);
    QNP_CHECK(after.viewersCompleted == 1);
    QNP_CHECK(after.viewersDeadlineExpired == 0);
    QNP_CHECK(after.phase == SidecarPhase::Retained);
    QNP_CHECK(fixture.sidecar->stop(true, fixture.nowMs));

    const SpectatorSidecar::TerminalOutcome* completed = nullptr;
    for (const auto& entry : fixture.sidecar->terminalOutcomes())
        if (entry.state == ViewerServiceState::Completed) completed = &entry;
    QNP_CHECK(completed != nullptr);
}

QNP_TEST(sidecar_spool_exhaustion_disables_service_without_touching_players) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(10);

    HarnessViewer viewer(peer(5, 50004).ipv4BE, 50004, 0xCCCC0001ull,
                         fixture.identity);
    fixture.run({&viewer}, 20);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 1);

    fixture.sidecar->injectSpoolFailure("spool segment corrupt", fixture.nowMs);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.phase == SidecarPhase::Failed);
    QNP_CHECK(status.code == SourceStatusCode::ProtocolOrArchiveFailure);
    QNP_CHECK(status.spoolFailures >= 1);
    QNP_CHECK(!status.failureReason.empty());
    QNP_CHECK(status.sealed);
    QNP_CHECK(status.sealedIsTruncation);
    QNP_CHECK(status.playersUnaffected);
    QNP_CHECK(!status.serving);
    QNP_CHECK(status.viewerCount == 0);
    QNP_CHECK(!fixture.transport->opened);

    const uint64_t decodedBefore = status.ingressDecoded;
    fixture.transport->deliver(
        peer(6, 50005),
        encodeHello(fixture.identity, 0xCCCC0002ull, 1, 1));
    fixture.advance(20);
    fixture.sidecar->service(fixture.nowMs);
    const SidecarStatus after = fixture.sidecar->status();
    QNP_CHECK(after.viewerCount == 0);
    QNP_CHECK(after.ingressDecoded == decodedBefore);
}

QNP_TEST(sidecar_transport_bind_failure_is_soft_and_explicit) {
    Fixture fixture;
    fixture.config.policy = AdmissionPolicy::Public;
    fixture.config.diagnostics.sink = [&fixture](const char* line) {
        fixture.log.push_back(line ? line : "");
    };
    std::unique_ptr<FakeTransport> owned(new FakeTransport());
    owned->openFails = true;
    fixture.transport = owned.get();
    fixture.sidecar.reset(new SpectatorSidecar(fixture.config));
    std::string error;
    const bool started = fixture.sidecar->start(std::move(owned), nullptr,
                                                fixture.nowMs, error);
    QNP_CHECK(!started);
    QNP_CHECK(error.find("scripted spectator bind failure") != std::string::npos);
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.phase == SidecarPhase::Failed);
    QNP_CHECK(!status.serving);
    QNP_CHECK(status.playersUnaffected);
}

QNP_TEST(sidecar_source_autopunch_configuration_failure_is_advisory_only) {
    Fixture fixture;
    fixture.config.sourceAutopunch.enabled = true;
    fixture.config.sourceAutopunch.relay = peer(99, 14763);
    fixture.start();

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.serving);
    QNP_CHECK(status.phase == SidecarPhase::Waiting);
    QNP_CHECK(status.playersUnaffected);
    QNP_CHECK(fixture.transport->opened);

    bool sawAdvisory = false;
    for (const std::string& line : fixture.log) {
        if (line.find("AutoPunch") != std::string::npos) {
            sawAdvisory = true;
            break;
        }
    }
    QNP_CHECK(sawAdvisory);
}

#ifdef _WIN32
namespace {

bool startSourceRelayListener(qnet::TcpListener& listener,
                              uint16_t& port) {
    static uint16_t nextPort = 26701;
    for (unsigned attempt = 0; attempt < 256; ++attempt) {
        const uint16_t candidate = nextPort++;
        if (nextPort > 27500) nextPort = 26701;
        if (listener.listen(candidate)) {
            port = candidate;
            return true;
        }
    }
    return false;
}

SourceRelayConfig makeSourceRelayConfig(uint16_t port) {
    SourceRelayConfig config{};
    config.enabled = true;
    in_addr loopback{};
    InetPtonA(AF_INET, "127.0.0.1", &loopback);
    config.relay.ipv4BE = loopback.s_addr;
    config.relay.port = port;
    config.room.low = 0x1020304050607080ull;
    config.room.high = 0x8877665544332211ull;
    config.sourceToken.low = 0xA1A2A3A4A5A6A7A8ull;
    config.sourceToken.high = 0xB1B2B3B4B5B6B7B8ull;
    config.identity = makeIdentity();
    return config;
}

bool acceptSourceRelayHello(
    SidecarTransport& transport,
    qnet::TcpListener& listener,
    uint64_t nowMs,
    qnet::TcpConn& server,
    qnet::relay::SpectatorHello& hello) {
    for (unsigned turn = 0; turn < 128; ++turn) {
        transport.serviceTraversal(nowMs);
        if (!server.isOpen()) {
            qnet::TcpConn accepted = listener.acceptOne(0);
            if (accepted.isOpen()) server = std::move(accepted);
        }
        if (server.isOpen()) {
            qnet::TcpConn::Received received;
            const qnet::TcpConn::Rs result =
                server.recvMsgEx(received, 0);
            if (result == qnet::TcpConn::Rs::Msg) {
                return received.type == qnet::relay::kTcpEnvelopeType
                    && qnet::relay::decodeSpectatorHello(
                           received.body.data(), received.body.size(), hello)
                           == qnet::relay::DecodeError::None;
            }
            if (result != qnet::TcpConn::Rs::Timeout) return false;
        }
        Sleep(1);
    }
    return false;
}

bool sendSourceRelayWelcome(
    qnet::TcpConn& server,
    const SourceRelayConfig& config,
    qnet::relay::SpectatorStatus status,
    uint32_t generation,
    bool wrongRoom = false) {
    qnet::relay::SpectatorWelcome welcome{};
    welcome.room = config.room;
    if (wrongRoom) welcome.room.low ^= 1u;
    welcome.party = qnet::relay::SpectatorParty::Source;
    welcome.status = status;
    welcome.sessionId = config.identity.sessionId;
    welcome.retryAfterMs = 250;
    if (status == qnet::relay::SpectatorStatus::Accepted) {
        welcome.registrationToken.low = 0xC1C2C3C4C5C6C7C8ull;
        welcome.registrationToken.high = 0xD1D2D3D4D5D6D7D8ull;
        welcome.sourceGeneration = generation;
        welcome.grantKey.assign(
            qnet::relay::kGrantKeyBytes, uint8_t{0});
        for (size_t i = 0; i < welcome.grantKey.size(); ++i)
            welcome.grantKey[i] = static_cast<uint8_t>(i + 1u);
    }
    std::vector<uint8_t> encoded;
    return qnet::relay::encodeSpectatorWelcome(welcome, encoded)
        && server.sendMsg(
            qnet::relay::kTcpEnvelopeType,
            encoded.data(), encoded.size());
}

bool pumpUntilRelayEvent(SidecarTransport& transport,
                         uint64_t nowMs,
                         SidecarTransportEventKind expected) {
    for (unsigned turn = 0; turn < 128; ++turn) {
        transport.serviceTraversal(nowMs);
        SidecarTransportEvent event{};
        while (transport.pollEvent(event)) {
            if (event.kind == expected) return true;
        }
        Sleep(1);
    }
    return false;
}

}

QNP_TEST(sidecar_source_relay_transient_close_recovers_after_bounded_backoff) {
    QNP_REQUIRE(qnet::netInit());
    qnet::TcpListener listener;
    uint16_t relayPort = 0;
    QNP_REQUIRE(startSourceRelayListener(listener, relayPort));

    std::unique_ptr<SidecarTransport> transport =
        makeUdpSidecarTransport();
    QNP_REQUIRE(transport != nullptr);
    std::string warning;
    QNP_REQUIRE(transport->open(0, warning));
    const SourceRelayConfig config =
        makeSourceRelayConfig(relayPort);
    QNP_REQUIRE(transport->configureSourceRelay(config, warning));
    QNP_CHECK(warning.empty());

    constexpr uint64_t startMs = 10000;
    qnet::TcpConn firstServer;
    qnet::relay::SpectatorHello firstHello{};
    QNP_REQUIRE(acceptSourceRelayHello(
        *transport, listener, startMs, firstServer, firstHello));
    QNP_CHECK(firstHello.party
              == qnet::relay::SpectatorParty::Source);
    QNP_CHECK(firstHello.admission
              == qnet::relay::SpectatorAdmission::None);
    QNP_CHECK(firstHello.room == config.room);
    QNP_CHECK(firstHello.sourceToken == config.sourceToken);
    QNP_CHECK(firstHello.sessionId == config.identity.sessionId);

    firstServer.close();
    QNP_REQUIRE(pumpUntilRelayEvent(
        *transport, startMs + 1,
        SidecarTransportEventKind::RelaySourceRetrying));
    QNP_CHECK(transport->relaySourceGeneration() == 0);

    for (uint64_t now = startMs + 2; now < startMs + 200; ++now)
        transport->serviceTraversal(now);
    qnet::TcpConn tooEarly = listener.acceptOne(0);
    QNP_CHECK(!tooEarly.isOpen());

    qnet::TcpConn recoveredServer;
    qnet::relay::SpectatorHello recoveredHello{};
    QNP_REQUIRE(acceptSourceRelayHello(
        *transport, listener, startMs + 500,
        recoveredServer, recoveredHello));
    QNP_CHECK(recoveredHello.room == firstHello.room);
    QNP_CHECK(recoveredHello.sourceToken == firstHello.sourceToken);
    QNP_CHECK(sendSourceRelayWelcome(
        recoveredServer, config,
        qnet::relay::SpectatorStatus::Accepted, 9));
    QNP_REQUIRE(pumpUntilRelayEvent(
        *transport, startMs + 501,
        SidecarTransportEventKind::RelaySourceReady));
    QNP_CHECK(transport->relaySourceGeneration() == 9);

    transport->close();
}

QNP_TEST(sidecar_source_relay_active_lease_close_reregisters) {
    QNP_REQUIRE(qnet::netInit());
    qnet::TcpListener listener;
    uint16_t relayPort = 0;
    QNP_REQUIRE(startSourceRelayListener(listener, relayPort));

    std::unique_ptr<SidecarTransport> transport =
        makeUdpSidecarTransport();
    QNP_REQUIRE(transport != nullptr);
    std::string warning;
    QNP_REQUIRE(transport->open(0, warning));
    const SourceRelayConfig config =
        makeSourceRelayConfig(relayPort);
    QNP_REQUIRE(transport->configureSourceRelay(config, warning));

    constexpr uint64_t startMs = 15000;
    qnet::TcpConn firstServer;
    qnet::relay::SpectatorHello firstHello{};
    QNP_REQUIRE(acceptSourceRelayHello(
        *transport, listener, startMs, firstServer, firstHello));
    QNP_REQUIRE(sendSourceRelayWelcome(
        firstServer, config,
        qnet::relay::SpectatorStatus::Accepted, 17));
    QNP_REQUIRE(pumpUntilRelayEvent(
        *transport, startMs + 1,
        SidecarTransportEventKind::RelaySourceReady));
    QNP_CHECK(transport->relaySourceGeneration() == 17);

    firstServer.close();
    QNP_REQUIRE(pumpUntilRelayEvent(
        *transport, startMs + 2,
        SidecarTransportEventKind::RelaySourceRetrying));
    QNP_CHECK(transport->relaySourceGeneration() == 0);

    for (uint64_t now = startMs + 3; now < startMs + 252; ++now)
        transport->serviceTraversal(now);
    qnet::TcpConn tooEarly = listener.acceptOne(0);
    QNP_CHECK(!tooEarly.isOpen());

    qnet::TcpConn recoveredServer;
    qnet::relay::SpectatorHello recoveredHello{};
    QNP_REQUIRE(acceptSourceRelayHello(
        *transport, listener, startMs + 502,
        recoveredServer, recoveredHello));
    QNP_CHECK(recoveredHello.room == firstHello.room);
    QNP_CHECK(recoveredHello.sourceToken == firstHello.sourceToken);
    QNP_REQUIRE(sendSourceRelayWelcome(
        recoveredServer, config,
        qnet::relay::SpectatorStatus::Accepted, 18));
    QNP_REQUIRE(pumpUntilRelayEvent(
        *transport, startMs + 503,
        SidecarTransportEventKind::RelaySourceReady));
    QNP_CHECK(transport->relaySourceGeneration() == 18);

    transport->close();
}

QNP_TEST(sidecar_source_relay_token_rejection_is_terminal_without_churn) {
    QNP_REQUIRE(qnet::netInit());
    qnet::TcpListener listener;
    uint16_t relayPort = 0;
    QNP_REQUIRE(startSourceRelayListener(listener, relayPort));

    std::unique_ptr<SidecarTransport> transport =
        makeUdpSidecarTransport();
    QNP_REQUIRE(transport != nullptr);
    std::string warning;
    QNP_REQUIRE(transport->open(0, warning));
    const SourceRelayConfig config =
        makeSourceRelayConfig(relayPort);
    QNP_REQUIRE(transport->configureSourceRelay(config, warning));

    constexpr uint64_t startMs = 20000;
    qnet::TcpConn server;
    qnet::relay::SpectatorHello hello{};
    QNP_REQUIRE(acceptSourceRelayHello(
        *transport, listener, startMs, server, hello));
    QNP_CHECK(sendSourceRelayWelcome(
        server, config,
        qnet::relay::SpectatorStatus::GrantRejected, 0));
    QNP_REQUIRE(pumpUntilRelayEvent(
        *transport, startMs + 1,
        SidecarTransportEventKind::RelaySourceUnavailable));
    QNP_CHECK(transport->relaySourceGeneration() == 0);

    for (unsigned turn = 0; turn < 32; ++turn)
        transport->serviceTraversal(startMs + 120000 + turn);
    qnet::TcpConn replay = listener.acceptOne(0);
    QNP_CHECK(!replay.isOpen());
    transport->close();
}

QNP_TEST(sidecar_source_relay_malformed_accept_is_terminal_without_churn) {
    QNP_REQUIRE(qnet::netInit());
    qnet::TcpListener listener;
    uint16_t relayPort = 0;
    QNP_REQUIRE(startSourceRelayListener(listener, relayPort));

    std::unique_ptr<SidecarTransport> transport =
        makeUdpSidecarTransport();
    QNP_REQUIRE(transport != nullptr);
    std::string warning;
    QNP_REQUIRE(transport->open(0, warning));
    const SourceRelayConfig config =
        makeSourceRelayConfig(relayPort);
    QNP_REQUIRE(transport->configureSourceRelay(config, warning));

    constexpr uint64_t startMs = 30000;
    qnet::TcpConn server;
    qnet::relay::SpectatorHello hello{};
    QNP_REQUIRE(acceptSourceRelayHello(
        *transport, listener, startMs, server, hello));
    QNP_CHECK(sendSourceRelayWelcome(
        server, config,
        qnet::relay::SpectatorStatus::Accepted, 11,
        true));
    QNP_REQUIRE(pumpUntilRelayEvent(
        *transport, startMs + 1,
        SidecarTransportEventKind::RelaySourceUnavailable));
    QNP_CHECK(transport->relaySourceGeneration() == 0);

    for (unsigned turn = 0; turn < 32; ++turn)
        transport->serviceTraversal(startMs + 120000 + turn);
    qnet::TcpConn replay = listener.acceptOne(0);
    QNP_CHECK(!replay.isOpen());
    transport->close();
}

QNP_TEST(sidecar_source_relay_transient_retries_have_a_hard_attempt_ceiling) {
    QNP_REQUIRE(qnet::netInit());
    qnet::TcpListener listener;
    uint16_t relayPort = 0;
    QNP_REQUIRE(startSourceRelayListener(listener, relayPort));

    std::unique_ptr<SidecarTransport> transport =
        makeUdpSidecarTransport();
    QNP_REQUIRE(transport != nullptr);
    std::string warning;
    QNP_REQUIRE(transport->open(0, warning));
    const SourceRelayConfig config =
        makeSourceRelayConfig(relayPort);
    QNP_REQUIRE(transport->configureSourceRelay(config, warning));

    uint64_t attemptMs = 40000;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        qnet::TcpConn server;
        qnet::relay::SpectatorHello hello{};
        QNP_REQUIRE(acceptSourceRelayHello(
            *transport, listener, attemptMs, server, hello));
        server.close();
        for (unsigned poll = 0; poll < 128; ++poll) {
            transport->serviceTraversal(attemptMs + 1);
            Sleep(1);
        }
        attemptMs += 4001;
    }

    QNP_REQUIRE(pumpUntilRelayEvent(
        *transport, attemptMs,
        SidecarTransportEventKind::RelaySourceUnavailable));
    QNP_CHECK(transport->relaySourceGeneration() == 0);
    for (unsigned turn = 0; turn < 32; ++turn)
        transport->serviceTraversal(attemptMs + 120000 + turn);
    qnet::TcpConn ninth = listener.acceptOne(0);
    QNP_CHECK(!ninth.isOpen());
    transport->close();
}

QNP_TEST(sidecar_udp_transport_never_sends_a_virtual_relay_address) {
    std::unique_ptr<SidecarTransport> transport =
        makeUdpSidecarTransport();
    QNP_REQUIRE(transport != nullptr);
    std::string error;
    QNP_REQUIRE(transport->open(0, error));

    RouteEndpoint virtualEndpoint{};
    virtualEndpoint.ipv4BE = qsp::kRelayVirtualIpv4BE;
    virtualEndpoint.port = 1;
    const uint8_t byte = 0x5A;
    QNP_CHECK(!transport->sendTo(
        virtualEndpoint, &byte, sizeof(byte)));
    transport->close();
}

QNP_TEST(sidecar_source_socket_demultiplexes_only_control_link_frames) {
    QNP_REQUIRE(qnet::netInit());
    struct DisableBridge {
        ~DisableBridge() { setSidecarControlIngressEnabled(false); }
    } disableBridge;
    setSidecarControlIngressEnabled(true);

    std::unique_ptr<SidecarTransport> transport =
        makeUdpSidecarTransport();
    QNP_REQUIRE(transport != nullptr);
    std::string error;
    QNP_REQUIRE(transport->open(0, error));

    qnet::UdpSock sender;
    QNP_REQUIRE(sender.open(0));
    qnet::Endpoint sourceEndpoint{};
    qnet::Endpoint senderEndpoint{};
    QNP_REQUIRE(qnet::Endpoint::parse(
        "127.0.0.1", transport->boundPort(), sourceEndpoint));
    QNP_REQUIRE(qnet::Endpoint::parse(
        "127.0.0.1", sender.boundPort(), senderEndpoint));

    qnet::LinkHello hello{ qnet::kProtocolVersion, 0x123456789ABCDEF0ull };
    uint8_t framed[qnet::kMaxMsgSize]{};
    const size_t framedLength = qnet::packMsg(
        qnet::MSG_LINK_HELLO, &hello, sizeof(hello), framed,
        sizeof(framed));
    QNP_REQUIRE(framedLength != 0);
    QNP_REQUIRE(sender.sendTo(sourceEndpoint, framed, framedLength));

    uint8_t qspBytes[qnetplay::spectator::kMaxSpectatorDatagramSize]{};
    RouteEndpoint from{};
    SidecarControlDatagram bridged{};
    const uint64_t deadline = GetTickCount64() + 1000ull;
    while (GetTickCount64() < deadline
           && !pollSidecarControlDatagram(bridged)) {
        (void)transport->recvFrom(qspBytes, sizeof(qspBytes), from);
        Sleep(1);
    }
    QNP_REQUIRE(bridged.length == framedLength);
    QNP_CHECK(std::memcmp(bridged.bytes.data(), framed, framedLength) == 0);
    QNP_CHECK(bridged.from == senderEndpoint);

    const uint8_t ordinary[] = { 0x51u, 0x53u, 0x50u, 0x34u, 0x01u };
    QNP_REQUIRE(sender.sendTo(sourceEndpoint, ordinary, sizeof(ordinary)));
    int delivered = 0;
    while (GetTickCount64() < deadline + 1000ull && delivered == 0) {
        delivered = transport->recvFrom(qspBytes, sizeof(qspBytes), from);
        if (delivered == 0) Sleep(1);
    }
    QNP_CHECK(delivered == static_cast<int>(sizeof(ordinary)));
    QNP_CHECK(!pollSidecarControlDatagram(bridged));
    transport->close();
}

QNP_TEST(sidecar_source_autopunch_uses_exact_socket_refreshes_and_filters_qsp) {
    QNP_REQUIRE(qnet::netInit());

    qnet::UdpSock relay;
    qnet::UdpSock viewer;
    QNP_REQUIRE(relay.open(0));
    QNP_REQUIRE(viewer.open(0));

    qnet::Endpoint loopbackRelay;
    QNP_REQUIRE(qnet::Endpoint::parse("127.0.0.1", relay.boundPort(),
                                      loopbackRelay));

    SidecarConfig config{};
    config.identity = makeIdentity();
    config.simulationContract = kContract;
    config.compatibilityIdentity = makeCompatibilityIdentity();
    config.policy = AdmissionPolicy::Public;
    config.retainAllRecords = true;
    config.sourceAutopunch.enabled = true;
    config.sourceAutopunch.relay.ipv4BE =
        loopbackRelay.sa.sin_addr.s_addr;
    config.sourceAutopunch.relay.port = relay.boundPort();

    SpectatorSidecar sidecar(config);
    std::string error;
    constexpr uint64_t startMs = 10000;
    QNP_REQUIRE(sidecar.start(makeUdpSidecarTransport(), nullptr, startMs,
                              error));
    QNP_REQUIRE(sidecar.boundPort() != 0);

    qnet::Endpoint sourceAtRelay;
    uint8_t packet[qsp::kMaxSpectatorDatagramSize] = {};

    sidecar.service(startMs);
    QNP_REQUIRE(relay.waitReadable(500));
    int received = relay.recvFrom(packet, sizeof(packet), sourceAtRelay);
    QNP_REQUIRE(received
                == static_cast<int>(qnet::autopunch::kRegisterBytes));
    const uint16_t registeredPort =
        static_cast<uint16_t>((static_cast<uint16_t>(packet[0]) << 8)
                              | static_cast<uint16_t>(packet[1]));
    QNP_CHECK(registeredPort == sidecar.boundPort());
    QNP_CHECK(ntohs(sourceAtRelay.sa.sin_port) == sidecar.boundPort());

    sidecar.service(startMs + qnet::autopunch::kRegisterIntervalMs - 1);
    QNP_CHECK(!relay.waitReadable(20));
    sidecar.service(startMs + qnet::autopunch::kRegisterIntervalMs);
    QNP_REQUIRE(relay.waitReadable(500));
    qnet::Endpoint refreshSource;
    received = relay.recvFrom(packet, sizeof(packet), refreshSource);
    QNP_REQUIRE(received
                == static_cast<int>(qnet::autopunch::kRegisterBytes));
    QNP_CHECK(refreshSource.sa.sin_port == sourceAtRelay.sa.sin_port);

    qnet::Endpoint viewerEndpoint;
    QNP_REQUIRE(qnet::Endpoint::parse("127.0.0.1", viewer.boundPort(),
                                      viewerEndpoint));
    uint8_t mapping[qnet::autopunch::kMappingBytes] = {};
    mapping[0] = static_cast<uint8_t>((viewer.boundPort() >> 8) & 0xFFu);
    mapping[1] = static_cast<uint8_t>(viewer.boundPort() & 0xFFu);
    mapping[2] = static_cast<uint8_t>((viewer.boundPort() >> 8) & 0xFFu);
    mapping[3] = static_cast<uint8_t>(viewer.boundPort() & 0xFFu);
    std::memcpy(mapping + 4, &viewerEndpoint.sa.sin_addr.s_addr,
                sizeof(viewerEndpoint.sa.sin_addr.s_addr));
    QNP_REQUIRE(relay.sendTo(sourceAtRelay, mapping, sizeof(mapping)));

    const SidecarStatus beforeMapping = sidecar.status();
    sidecar.service(startMs + qnet::autopunch::kRegisterIntervalMs + 1);
    const SidecarStatus afterMapping = sidecar.status();
    QNP_CHECK(afterMapping.ingressPackets == beforeMapping.ingressPackets);
    QNP_CHECK(afterMapping.ingressDecoded == beforeMapping.ingressDecoded);

    QNP_REQUIRE(viewer.waitReadable(500));
    bool sawPunch = false;
    qnet::Endpoint punchSource;
    for (;;) {
        received = viewer.recvFrom(packet, sizeof(packet), punchSource);
        if (received <= 0) break;
        if (received == static_cast<int>(qnet::autopunch::kPunchBytes)
            && packet[0] == qnet::autopunch::kPunchByte) {
            sawPunch = true;
            QNP_CHECK(ntohs(punchSource.sa.sin_port) == sidecar.boundPort());
        }
    }
    QNP_CHECK(sawPunch);

    const uint8_t punch = qnet::autopunch::kPunchByte;
    QNP_REQUIRE(viewer.sendTo(sourceAtRelay, &punch, sizeof(punch)));
    sidecar.service(startMs + qnet::autopunch::kRegisterIntervalMs + 2);
    const SidecarStatus afterPunch = sidecar.status();
    QNP_CHECK(afterPunch.ingressPackets == beforeMapping.ingressPackets);
    QNP_CHECK(afterPunch.ingressDecoded == beforeMapping.ingressDecoded);

    PublicationEvent start{};
    start.kind = PublicationKind::Start;
    start.sequence = 1;
    start.descriptor = makeDescriptor();
    sidecar.publish(start,
                    startMs + qnet::autopunch::kRegisterIntervalMs + 3);
    const std::vector<uint8_t> hello =
        encodeHello(config.identity, 0xACCE5501ull, 1, 1);
    QNP_REQUIRE(viewer.sendTo(sourceAtRelay, hello.data(), hello.size()));
    sidecar.service(startMs + qnet::autopunch::kRegisterIntervalMs + 4);
    const SidecarStatus afterQsp = sidecar.status();
    QNP_CHECK(afterQsp.ingressPackets
              == beforeMapping.ingressPackets + 1);
    QNP_CHECK(afterQsp.ingressDecoded
              == beforeMapping.ingressDecoded + 1);
    QNP_CHECK(afterQsp.viewerCount == 1);
}

QNP_TEST(sidecar_adopts_one_prebound_ephemeral_source_socket_without_rebind) {
    QNP_REQUIRE(qnet::netInit());
    qnet::UdpSock prebound;
    QNP_REQUIRE(prebound.open(0));
    const uint16_t assigned = prebound.boundPort();
    QNP_REQUIRE(assigned != 0);

    SidecarConfig config{};
    config.identity = makeIdentity();
    config.simulationContract = kContract;
    config.compatibilityIdentity = makeCompatibilityIdentity();
    config.policy = AdmissionPolicy::Public;
    config.listenPort = assigned;
    config.retainAllRecords = true;

    const SOCKET raw = prebound.release();
    QNP_REQUIRE(raw != INVALID_SOCKET);
    SpectatorSidecar sidecar(config);
    std::string error;
    QNP_REQUIRE(sidecar.start(
        makeUdpSidecarTransport(static_cast<uintptr_t>(raw)), nullptr,
        10000u, error));
    QNP_CHECK(sidecar.boundPort() == assigned);

    qnet::UdpSock collision;
    QNP_CHECK(!collision.open(assigned));
    QNP_CHECK(sidecar.stop(false, 10001u));
    QNP_CHECK(collision.open(assigned));
    collision.close();
}

QNP_TEST(sidecar_pipe_start_failure_releases_prebound_source_socket) {
    QNP_REQUIRE(qnet::netInit());
    struct StopOnExit {
        ~StopOnExit() { (void)stopSidecar(false); }
    } stopOnExit;
    qnet::UdpSock prebound;
    QNP_REQUIRE(prebound.open(0));
    const uint16_t assigned = prebound.boundPort();
    QNP_REQUIRE(assigned != 0);

    const uint32_t fakePid = GetCurrentProcessId() ^ 0x5E0A51C3u;
    wchar_t pipeName[128] = {};
    qipc::ipcSpectatorSidecarPipeName(pipeName, 128, fakePid);
    const HANDLE blocker = CreateNamedPipeW(
        pipeName, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 64 * 1024,
        0, nullptr);
    QNP_REQUIRE(blocker != INVALID_HANDLE_VALUE);

    SidecarConfig config{};
    config.identity = makeIdentity();
    config.simulationContract = kContract;
    config.compatibilityIdentity = makeCompatibilityIdentity();
    config.policy = AdmissionPolicy::Public;
    config.listenPort = assigned;
    config.retainAllRecords = true;
    config.preboundSocket = static_cast<uintptr_t>(prebound.release());
    QNP_REQUIRE(config.preboundSocket
                != kInvalidPreboundSpectatorSocket);

    std::string error;
    QNP_CHECK(!startSidecar(config, fakePid, error));
    QNP_CHECK(error.find("publication pipe") != std::string::npos);
    QNP_CHECK(!sidecarRunning());

    qnet::UdpSock collision;
    QNP_CHECK(collision.open(assigned));
    collision.close();
    CloseHandle(blocker);
}

QNP_TEST(sidecar_running_rejection_releases_prebound_source_socket) {
    QNP_REQUIRE(qnet::netInit());
    struct StopOnExit {
        ~StopOnExit() { (void)stopSidecar(false); }
    } stopOnExit;

    SidecarConfig active{};
    active.identity = makeIdentity();
    active.simulationContract = kContract;
    active.compatibilityIdentity = makeCompatibilityIdentity();
    active.policy = AdmissionPolicy::Public;
    active.retainAllRecords = true;
    std::string error;
    QNP_REQUIRE(startSidecar(active, 0u, error));
    QNP_REQUIRE(sidecarStatus().serving);

    qnet::UdpSock prebound;
    QNP_REQUIRE(prebound.open(0));
    const uint16_t assigned = prebound.boundPort();
    QNP_REQUIRE(assigned != 0);

    SidecarConfig contender = active;
    contender.listenPort = assigned;
    contender.preboundSocket = static_cast<uintptr_t>(prebound.release());
    error.clear();
    QNP_CHECK(!startSidecar(contender, 0u, error));
    QNP_CHECK(error.find("already running") != std::string::npos);

    qnet::UdpSock collision;
    QNP_CHECK(collision.open(assigned));
    collision.close();
}
#endif

QNP_TEST(sidecar_disabled_policy_binds_nothing_and_serves_nothing) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::Disabled);
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.phase == SidecarPhase::Idle);
    QNP_CHECK(status.code == SourceStatusCode::Disabled);
    QNP_CHECK(!status.serving);

    fixture.transport->deliver(
        peer(7, 50006),
        encodeHello(fixture.identity, 0xDDDD0001ull, 1, 1));
    fixture.advance(20);
    fixture.sidecar->service(fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
    QNP_CHECK(fixture.sidecar->status().ingressDecoded == 0);
}

QNP_TEST(sidecar_disabling_a_live_service_releases_every_owner_immediately) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::Public);
    fixture.publishStart();

    const RouteEndpoint endpoint = peer(8, 50007);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0xDDDD0002ull, 1, 1);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    endpoint, hello.data(), hello.size(), fixture.nowMs)
                == IngressOutcome::Accepted);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);
    QNP_REQUIRE(fixture.transport->opened);

    fixture.sidecar->setAdmissionPolicy(AdmissionPolicy::Disabled);
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.phase == SidecarPhase::Expired);
    QNP_CHECK(status.code == SourceStatusCode::Disabled);
    QNP_CHECK(!status.serving);
    QNP_CHECK(status.viewerCount == 0);
    QNP_CHECK(status.reservationCount == 0);
    QNP_CHECK(!fixture.transport->opened);
}

QNP_TEST(sidecar_rate_limits_before_any_expensive_decoding) {
    Fixture fixture;
    fixture.config.rateLimits.globalPacketsPerSecond = 10;
    fixture.config.rateLimits.globalBurstPackets = 4;
    fixture.start();
    fixture.publishStart();

    const RouteEndpoint attacker = peer(20, 50200);
    std::vector<uint8_t> junk(qsp::kMaxSpectatorDatagramSize, 0x5A);

    size_t limited = 0;
    for (int i = 0; i < 200; ++i) {
        const IngressOutcome outcome = fixture.sidecar->onDatagram(
            attacker, junk.data(), junk.size(), fixture.nowMs);
        if (outcome == IngressOutcome::RateLimitedGlobal
            || outcome == IngressOutcome::RateLimitedAddress
            || outcome == IngressOutcome::AddressCooldown)
            ++limited;
    }

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.ingressPackets == 200);
    QNP_CHECK(limited > 150);
    QNP_CHECK(status.ingressDecoded <= 8);
    QNP_CHECK(status.ingressRateLimited == limited);
}

QNP_TEST(sidecar_untrusted_address_table_has_a_hard_ceiling) {
    Fixture fixture;
    fixture.config.rateLimits.maxTrackedAddresses = 4;
    fixture.config.rateLimits.globalPacketsPerSecond = 1000000;
    fixture.config.rateLimits.globalBurstPackets = 1000000;
    fixture.config.rateLimits.globalBytesPerSecond = 1ull << 40;
    fixture.config.rateLimits.globalBurstBytes = 1ull << 40;
    fixture.config.rateLimits.perIpPacketsPerSecond = 0;
    fixture.config.rateLimits.perIpBurstPackets = 3;
    fixture.config.rateLimits.perIpBytesPerSecond = 1ull << 30;
    fixture.config.rateLimits.perIpBurstBytes = 1ull << 30;
    fixture.start();

    const uint8_t malformed = 0x7Fu;
    for (uint32_t index = 0; index < 100; ++index) {
        RouteEndpoint spoofed{};
        spoofed.ipv4BE = 0x01000000u + index;
        spoofed.port = static_cast<uint16_t>(20000u + index);
        (void)fixture.sidecar->onDatagram(
            spoofed, &malformed, sizeof(malformed), fixture.nowMs);
    }

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.ingressPackets == 100);
    QNP_CHECK(status.trackedAddressCount == 4);
    QNP_CHECK(status.trackedAddressCount
              <= fixture.config.rateLimits.maxTrackedAddresses);
    QNP_CHECK(status.overflowAddressPackets == 96);
    QNP_CHECK(status.ingressRejected == 7);
    QNP_CHECK(status.ingressRateLimited == 93);
    QNP_CHECK(status.ingressDecoded == 0);
}

QNP_TEST(sidecar_per_address_bucket_and_cooldown_park_a_hostile_source) {
    Fixture fixture;
    fixture.config.rateLimits.globalPacketsPerSecond = 100000;
    fixture.config.rateLimits.globalBurstPackets = 100000;
    fixture.config.rateLimits.globalBytesPerSecond = 1ull << 40;
    fixture.config.rateLimits.globalBurstBytes = 1ull << 40;
    fixture.config.rateLimits.perIpPacketsPerSecond = 10;
    fixture.config.rateLimits.perIpBurstPackets = 4;
    fixture.config.rateLimits.perIpViolationsBeforeCooldown = 8;
    fixture.config.rateLimits.perIpCooldownMs = 5000;
    fixture.start();

    const RouteEndpoint hostile = peer(21, 50201);
    const RouteEndpoint innocent = peer(22, 50202);
    std::vector<uint8_t> junk(600, 0x33);

    size_t cooldowns = 0;
    for (int i = 0; i < 100; ++i) {
        if (fixture.sidecar->onDatagram(hostile, junk.data(), junk.size(),
                                        fixture.nowMs)
            == IngressOutcome::AddressCooldown)
            ++cooldowns;
    }
    QNP_CHECK(cooldowns > 0);

    const IngressOutcome other = fixture.sidecar->onDatagram(
        innocent, junk.data(), junk.size(), fixture.nowMs);
    QNP_CHECK(other != IngressOutcome::AddressCooldown);
    QNP_CHECK(other != IngressOutcome::RateLimitedAddress);
}

QNP_TEST(sidecar_per_grant_bucket_limits_an_admitted_viewer) {
    Fixture fixture;
    fixture.config.rateLimits.globalPacketsPerSecond = 1000000;
    fixture.config.rateLimits.globalBurstPackets = 1000000;
    fixture.config.rateLimits.perIpPacketsPerSecond = 1000000;
    fixture.config.rateLimits.perIpBurstPackets = 1000000;
    fixture.config.rateLimits.perGrantPacketsPerSecond = 4;
    fixture.config.rateLimits.perGrantBurstPackets = 2;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(4);

    const RouteEndpoint endpoint = peer(23, 50203);
    const std::vector<uint8_t> admission =
        encodeHello(fixture.identity, 0xEEEE0001ull, 1, 1);
    fixture.sidecar->onDatagram(endpoint, admission.data(), admission.size(),
                                fixture.nowMs);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    size_t grantLimited = 0;
    for (uint32_t i = 2; i < 40; ++i) {
        const std::vector<uint8_t> hello =
            encodeHello(fixture.identity, 0xEEEE0001ull, 1, i);
        if (fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                        fixture.nowMs)
            == IngressOutcome::RateLimitedGrant)
            ++grantLimited;
    }
    QNP_CHECK(grantLimited > 30);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 1);
}

QNP_TEST(sidecar_endpoint_prune_keeps_the_active_endpoint_token) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(8);

    constexpr uint64_t viewerId = 0xA1010001ull;
    HarnessViewer viewer(peer(60, 50700).ipv4BE, 50700, viewerId,
                         fixture.identity);
    fixture.run({&viewer}, 30);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    for (uint32_t index = 0; index < 520u; ++index) {
        RouteEndpoint churn{};
        churn.ipv4BE = 0x0A000001u + index;
        churn.port = static_cast<uint16_t>(40000u + index);
        const std::vector<uint8_t> ack = encodeUnknownAck(
            fixture.identity, 0xB0000000ull + index, 1u);
        (void)fixture.sidecar->onDatagram(
            churn, ack.data(), ack.size(), fixture.nowMs + index + 1u);
    }

    const SidecarStatus status = fixture.sidecar->status();
    QNP_REQUIRE(status.viewerCount == 1);
    QNP_REQUIRE(status.viewers.size() == 1);
    QNP_CHECK(status.viewers[0].endpoint == viewer.endpoint);

    fixture.transport->outbound[FakeTransport::key(viewer.endpoint)].clear();
    fixture.advance(1000u);
    fixture.sidecar->service(fixture.nowMs);
    QNP_CHECK(!fixture.transport
                   ->outbound[FakeTransport::key(viewer.endpoint)]
                   .empty());
}

QNP_TEST(sidecar_outbound_accounting_resolves_endpoint_token_to_viewer_id) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(8);

    constexpr uint64_t viewerId = 0xA1011001ull;
    HarnessViewer viewer(peer(63, 50703).ipv4BE, 50703, viewerId,
                         fixture.identity);
    fixture.run({&viewer}, 30);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_REQUIRE(status.viewers.size() == 1);
    QNP_CHECK(status.viewers[0].viewerId == viewerId);
    QNP_CHECK(status.viewers[0].bytesSent > 0u);
}

QNP_TEST(sidecar_migration_updates_bootstrap_cursor_and_new_token_accounting) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(32);

    constexpr uint64_t viewerId = 0xA1012001ull;
    HarnessViewer original(peer(64, 50704).ipv4BE, 50704, viewerId,
                           fixture.identity);
    fixture.run({&original}, 30);

    SidecarStatus before = fixture.sidecar->status();
    QNP_REQUIRE(before.viewers.size() == 1u);
    QNP_CHECK(before.viewers[0].generation == 1u);
    QNP_CHECK(before.viewers[0].bootstrapRecord == 0u);
    QNP_REQUIRE(before.viewers[0].bytesSent > 0u);

    constexpr uint32_t resumeRecord = 16u;
    const RouteEndpoint migrated = peer(65, 50705);
    const std::vector<uint8_t> migrate = encodeMigrate(
        fixture.identity, viewerId, 1u, 2u, resumeRecord,
        prefixDigestFor(resumeRecord), 200u);
    QNP_REQUIRE(!migrate.empty());
    fixture.advance(20u);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    migrated, migrate.data(), migrate.size(), fixture.nowMs)
                == IngressOutcome::Accepted);

    SidecarStatus afterMigrate = fixture.sidecar->status();
    QNP_REQUIRE(afterMigrate.viewers.size() == 1u);
    QNP_CHECK(afterMigrate.viewers[0].viewerId == viewerId);
    QNP_CHECK(afterMigrate.viewers[0].endpoint == migrated);
    QNP_CHECK(afterMigrate.viewers[0].generation == 2u);
    QNP_CHECK(afterMigrate.viewers[0].bootstrapRecord == resumeRecord);

    fixture.transport->outbound[FakeTransport::key(original.endpoint)].clear();
    fixture.transport->outbound[FakeTransport::key(migrated)].clear();
    fixture.publishRecords(2);
    fixture.advance(20u);
    fixture.sidecar->service(fixture.nowMs);

    const SidecarStatus afterOutbound = fixture.sidecar->status();
    QNP_REQUIRE(afterOutbound.viewers.size() == 1u);
    QNP_CHECK(!fixture.transport
                   ->outbound[FakeTransport::key(migrated)]
                   .empty());
    QNP_CHECK(fixture.transport
                  ->outbound[FakeTransport::key(original.endpoint)]
                  .empty());
    QNP_CHECK(afterOutbound.viewers[0].bytesSent
              > before.viewers[0].bytesSent);
}

QNP_TEST(sidecar_bare_hello_cannot_mutate_a_known_viewers_endpoint) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(8);

    constexpr uint64_t viewerId = 0xA1020001ull;
    HarnessViewer viewer(peer(61, 50701).ipv4BE, 50701, viewerId,
                         fixture.identity);
    fixture.run({&viewer}, 30);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    const RouteEndpoint attacker = peer(62, 50702);
    const std::vector<uint8_t> bareMove =
        encodeHello(fixture.identity, viewerId, 2u, 100u);
    const IngressOutcome outcome = fixture.sidecar->onDatagram(
        attacker, bareMove.data(), bareMove.size(), fixture.nowMs + 1u);
    QNP_CHECK(outcome == IngressOutcome::ProtocolRejected);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_REQUIRE(status.viewers.size() == 1);
    QNP_CHECK(status.viewers[0].endpoint == viewer.endpoint);
    QNP_CHECK(status.viewers[0].generation == 1u);
}

QNP_TEST(sidecar_per_ip_viewer_quota_caps_one_capability_holder) {
    Fixture fixture;
    fixture.config.maxViewers = 8;
    fixture.config.rateLimits.maxViewersPerIp = 2;
    fixture.start();
    fixture.publishStart();

    for (uint16_t i = 0; i < 6; ++i) {
        RouteEndpoint endpoint = peer(24, static_cast<uint16_t>(50300 + i));
        const std::vector<uint8_t> hello =
            encodeHello(fixture.identity, 0xF0000000ull + i, 1, 1);
        fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                    fixture.nowMs);
    }
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewerCount == 2);
    QNP_CHECK(status.admissionRejections == 4);

    qsp::RejectReason reason = qsp::RejectReason::InvalidRequest;
    QNP_CHECK(sawReject(*fixture.transport, peer(24, 50305), fixture.identity,
                        reason));
    QNP_CHECK(reason == qsp::RejectReason::CapacityReached);
}

QNP_TEST(sidecar_log_budget_holds_under_sustained_hostile_traffic) {
    Fixture fixture;
    fixture.config.rateLimits.globalPacketsPerSecond = 1000000;
    fixture.config.rateLimits.globalBurstPackets = 1000000;
    fixture.config.rateLimits.globalBytesPerSecond = 1ull << 40;
    fixture.config.rateLimits.globalBurstBytes = 1ull << 40;
    fixture.config.rateLimits.perIpPacketsPerSecond = 1000000;
    fixture.config.rateLimits.perIpBurstPackets = 1000000;
    fixture.config.rateLimits.perIpBytesPerSecond = 1ull << 40;
    fixture.config.rateLimits.perIpBurstBytes = 1ull << 40;
    fixture.config.diagnostics.windowMs = 5000;
    fixture.config.diagnostics.maxEmitsPerWindow = 4;
    fixture.config.diagnostics.maxTotalBytes = 2048;
    fixture.start();
    fixture.publishStart();

    const RouteEndpoint attacker = peer(25, 50400);
    std::vector<uint8_t> junk(qsp::kMaxSpectatorDatagramSize, 0x77);
    for (int i = 0; i < 30000; ++i) {
        if ((i % 50) == 0) fixture.advance(1000);
        fixture.sidecar->onDatagram(attacker, junk.data(), junk.size(),
                                    fixture.nowMs);
    }

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.ingressPackets == 30000);
    QNP_CHECK(status.diagnosticsDropped > 0);
    QNP_CHECK(status.diagnosticBytes <= 2048 + 256);
    QNP_CHECK(fixture.log.size() < 40);
    QNP_CHECK(status.diagnosticsEmitted == fixture.log.size());
    bool reportedSuppression = false;
    for (const std::string& line : fixture.log)
        if (line.find("suppressed") != std::string::npos)
            reportedSuppression = true;
    QNP_CHECK(reportedSuppression);
}

QNP_TEST(coalesced_diagnostics_emit_once_per_window_per_event) {
    DiagnosticsConfig config{};
    config.windowMs = 1000;
    config.maxEmitsPerWindow = 8;
    config.maxTotalBytes = 1024 * 1024;
    std::vector<std::string> lines;
    config.sink = [&lines](const char* line) { lines.push_back(line); };
    CoalescedDiagnostics diagnostics(config);

    uint64_t now = 0;
    for (int i = 0; i < 100; ++i)
        diagnostics.note(DiagEvent::MalformedDatagram, now, "flood");
    QNP_CHECK(diagnostics.emitted() == 1);
    QNP_CHECK(diagnostics.dropped() == 99);

    diagnostics.note(DiagEvent::RateLimited, now, "flood");
    QNP_CHECK(diagnostics.emitted() == 2);

    now += 1500;
    diagnostics.note(DiagEvent::MalformedDatagram, now, "flood");
    QNP_CHECK(diagnostics.emitted() == 3);
    QNP_REQUIRE(lines.size() == 3);
    QNP_CHECK(lines[2].find("suppressed 99") != std::string::npos);
}

QNP_TEST(coalesced_diagnostics_stop_at_the_lifetime_byte_cap) {
    DiagnosticsConfig config{};
    config.windowMs = 1;
    config.maxEmitsPerWindow = 1000;
    config.maxTotalBytes = 200;
    size_t emitted = 0;
    config.sink = [&emitted](const char*) { ++emitted; };
    CoalescedDiagnostics diagnostics(config);
    for (uint64_t i = 0; i < 5000; ++i)
        diagnostics.note(DiagEvent::MalformedDatagram, i * 2, "x");
    QNP_CHECK(diagnostics.bytes() <= 200 + 256);
    QNP_CHECK(emitted < 10);
    QNP_CHECK(diagnostics.dropped() > 4900);
}

QNP_TEST(sidecar_answers_a_pre_game_hello_with_a_waiting_heartbeat) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::Public);

    const RouteEndpoint endpoint = peer(40, 50600);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0x51DECA0ull, 1, 1);
    QNP_CHECK(fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                          fixture.nowMs)
              == IngressOutcome::Accepted);

    auto& queue = fixture.transport->outbound[FakeTransport::key(endpoint)];
    bool sawWaitingHeartbeat = false;
    for (const FakeTransport::Packet& packet : queue) {
        qsp::Datagram datagram{};
        if (qsp::decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                fixture.identity, datagram)
            != qsp::DecodeError::None)
            continue;
        QNP_CHECK(datagram.header.type != qsp::MessageType::Reject);
        if (datagram.header.type != qsp::MessageType::Heartbeat) continue;
        qsp::HeartbeatPayload beat{};
        if (!qsp::decodePayload(datagram.payload, beat)) continue;
        if (beat.sourcePhase == qsp::SourcePhase::Waiting)
            sawWaitingHeartbeat = true;
    }
    QNP_CHECK(sawWaitingHeartbeat);
}

QNP_TEST(sidecar_pre_game_hello_is_still_refused_under_code_only) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::CodeOnly);

    const RouteEndpoint endpoint = peer(41, 50601);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0x51DECA1ull, 1, 1);
    QNP_CHECK(fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                          fixture.nowMs)
              == IngressOutcome::AdmissionDenied);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
    qsp::RejectReason reason = qsp::RejectReason::InvalidRequest;
    QNP_CHECK(sawReject(*fixture.transport, endpoint, fixture.identity, reason));
    QNP_CHECK(reason == qsp::RejectReason::GrantInvalid);
}

QNP_TEST(sidecar_code_only_policy_refuses_an_ungranted_viewer) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();

    const RouteEndpoint endpoint = peer(30, 50500);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0x1234500ull, 1, 1);
    const IngressOutcome outcome = fixture.sidecar->onDatagram(
        endpoint, hello.data(), hello.size(), fixture.nowMs);

    QNP_CHECK(outcome == IngressOutcome::AdmissionDenied);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
    qsp::RejectReason reason = qsp::RejectReason::InvalidRequest;
    QNP_CHECK(sawReject(*fixture.transport, endpoint, fixture.identity, reason));
    QNP_CHECK(reason == qsp::RejectReason::GrantInvalid);
}

QNP_TEST(sidecar_public_relay_lease_needs_no_host_control_reservation) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::Public);
    fixture.publishStart();

    RouteEndpoint relayVirtual{};
    relayVirtual.ipv4BE = qsp::kRelayVirtualIpv4BE;
    relayVirtual.port = 50791;
    const uint64_t relayViewerId = 0x50554252454C4159ull;
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, relayViewerId, 1, 1);

    QNP_CHECK(fixture.sidecar->status().reservationCount == 0);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  relayVirtual, hello.data(), hello.size(), fixture.nowMs)
              == IngressOutcome::Accepted);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 1);
    QNP_CHECK(fixture.sidecar->status().reservationCount == 0);
}

QNP_TEST(sidecar_authenticated_relay_viewers_have_isolated_quotas) {
    Fixture fixture;
    fixture.config.maxViewers = 8;
    fixture.config.rateLimits.maxViewersPerIp = 1;
    fixture.config.rateLimits.globalPacketsPerSecond = 1000000;
    fixture.config.rateLimits.globalBurstPackets = 1000000;
    fixture.config.rateLimits.globalBytesPerSecond = 1ull << 40;
    fixture.config.rateLimits.globalBurstBytes = 1ull << 40;
    fixture.config.rateLimits.perIpPacketsPerSecond = 0;
    fixture.config.rateLimits.perIpBurstPackets = 1;
    fixture.config.rateLimits.perIpBytesPerSecond = 1ull << 30;
    fixture.config.rateLimits.perIpBurstBytes = 1ull << 30;
    fixture.start(AdmissionPolicy::Public);
    fixture.publishStart();

    RouteEndpoint first{};
    first.ipv4BE = qsp::kRelayVirtualIpv4BE;
    first.port = 50793;
    RouteEndpoint second = first;
    second.port = 50794;
    constexpr uint64_t firstViewer = 0x52454C4159303031ull;
    constexpr uint64_t secondViewer = 0x52454C4159303032ull;
    fixture.transport->authenticateRelayViewer(first, firstViewer);
    fixture.transport->authenticateRelayViewer(second, secondViewer);

    const std::vector<uint8_t> firstHello =
        encodeHello(fixture.identity, firstViewer, 1, 1);
    const std::vector<uint8_t> secondHello =
        encodeHello(fixture.identity, secondViewer, 1, 1);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  first, firstHello.data(), firstHello.size(), fixture.nowMs)
              == IngressOutcome::Accepted);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  second, secondHello.data(), secondHello.size(), fixture.nowMs)
              == IngressOutcome::Accepted);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewerCount == 2);
    QNP_CHECK(status.trackedAddressCount == 2);
    QNP_CHECK(status.ingressRateLimited == 0);
    QNP_CHECK(status.admissionRejections == 0);
}

QNP_TEST(sidecar_source_relay_loss_retires_only_old_relay_routes) {
    Fixture fixture;
    fixture.config.maxViewers = 4;
    fixture.start(AdmissionPolicy::Public);
    fixture.publishStart();

    RouteEndpoint relayVirtual{};
    relayVirtual.ipv4BE = qsp::kRelayVirtualIpv4BE;
    relayVirtual.port = 50795;
    constexpr uint64_t relayViewer = 0x52454C41594C4F53ull;
    fixture.transport->authenticateRelayViewer(
        relayVirtual, relayViewer);
    const std::vector<uint8_t> relayHello =
        encodeHello(fixture.identity, relayViewer, 1, 1);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    relayVirtual, relayHello.data(), relayHello.size(),
                    fixture.nowMs)
                == IngressOutcome::Accepted);

    const RouteEndpoint direct = peer(42, 50796);
    constexpr uint64_t directViewer = 0x4449524543544F4Bull;
    const std::vector<uint8_t> directHello =
        encodeHello(fixture.identity, directViewer, 1, 1);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    direct, directHello.data(), directHello.size(),
                    fixture.nowMs)
                == IngressOutcome::Accepted);

    fixture.transport->relayGrants = true;
    GrantRequest pendingRelay{};
    pendingRelay.viewerId = 0x52454C415947524Eull;
    pendingRelay.route = static_cast<uint8_t>(
        qnet::relay::SpectatorRouteKind::Relay);
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        pendingRelay, grant, deny, fixture.nowMs));
    QNP_CHECK(fixture.sidecar->status().capacityReservations == 1);

    SidecarTransportEvent lost{};
    lost.kind = SidecarTransportEventKind::RelaySourceRetrying;
    fixture.transport->events.push_back(lost);
    fixture.advance(1);
    fixture.sidecar->service(fixture.nowMs);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewerCount == 1);
    QNP_REQUIRE(status.viewers.size() == 1);
    QNP_CHECK(status.viewers[0].viewerId == directViewer);
    QNP_CHECK(status.viewersDetached == 1);
    QNP_CHECK(status.capacityReservations == 0);
    QNP_CHECK(status.consumedGrantTombstones == 1);
}

QNP_TEST(sidecar_restrictive_policy_still_rejects_ungranted_relay_lease) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();

    RouteEndpoint relayVirtual{};
    relayVirtual.ipv4BE = qsp::kRelayVirtualIpv4BE;
    relayVirtual.port = 50792;
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0x434F444552454C41ull, 1, 1);

    QNP_CHECK(fixture.sidecar->onDatagram(
                  relayVirtual, hello.data(), hello.size(), fixture.nowMs)
              == IngressOutcome::AdmissionDenied);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
}

QNP_TEST(sidecar_code_only_policy_admits_a_granted_viewer) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();

    GrantRequest request{};
    request.viewerId = 0x777000ull;
    request.lifetimeSeconds = 600;
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(request, grant, deny,
                                            fixture.nowMs));
    QNP_CHECK(grant.viewerId == 0x777000ull);
    QNP_CHECK(grant.grantId != 0);
    QNP_CHECK(grant.sessionId == fixture.identity.sessionId);
    QNP_CHECK(grant.expiryUnix > 1000000);

    const RouteEndpoint endpoint = peer(31, 50501);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0x777000ull, 1, 1);
    QNP_CHECK(fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                          fixture.nowMs)
              == IngressOutcome::Accepted);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 1);
}

QNP_TEST(sidecar_expired_grant_is_refused_with_its_own_reason) {
    Fixture fixture;
    uint64_t unixNow = 1000000;
    fixture.config.unixClock = [&unixNow] { return unixNow; };
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();

    GrantRequest request{};
    request.viewerId = 0x888000ull;
    request.lifetimeSeconds = 10;
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(request, grant, deny,
                                            fixture.nowMs));
    unixNow += 60;

    const RouteEndpoint endpoint = peer(32, 50502);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0x888000ull, 1, 1);
    QNP_CHECK(fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                          fixture.nowMs)
              == IngressOutcome::AdmissionDenied);
    qsp::RejectReason reason = qsp::RejectReason::InvalidRequest;
    QNP_CHECK(sawReject(*fixture.transport, endpoint, fixture.identity, reason));
    QNP_CHECK(reason == qsp::RejectReason::GrantExpired);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
}

QNP_TEST(sidecar_ask_host_requires_control_reservation_then_admits_or_declines) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::AskHost);
    fixture.publishStart();

    const RouteEndpoint endpoint = peer(33, 50503);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0x999000ull, 1, 1);
    QNP_CHECK(fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                          fixture.nowMs)
              == IngressOutcome::AdmissionDenied);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
    QNP_CHECK(fixture.sidecar->status().pendingApprovals == 0);
    QNP_CHECK(fixture.sidecar->status().reservationCount == 0);
    fixture.transport->outbound[
        FakeTransport::key(endpoint)].clear();

    GrantRequest request{};
    request.viewerId = 0x999000ull;
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        request, grant, deny, fixture.nowMs));
    QNP_CHECK(fixture.sidecar->status().pendingApprovals == 1);
    QNP_CHECK(fixture.sidecar->pendingApprovals().size() == 1);
    QNP_CHECK(fixture.sidecar->pendingApprovals()[0] == 0x999000ull);

    const std::vector<uint8_t> pendingHello =
        encodeHello(fixture.identity, 0x999000ull, 1, 2);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  endpoint, pendingHello.data(), pendingHello.size(),
                  fixture.nowMs)
              == IngressOutcome::Accepted);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
    qsp::RejectReason unused = qsp::RejectReason::InvalidRequest;
    QNP_CHECK(!sawReject(*fixture.transport, endpoint, fixture.identity,
                         unused));

    QNP_CHECK(fixture.sidecar->approveViewer(0x999000ull, fixture.nowMs));
    fixture.advance(10);
    const std::vector<uint8_t> retry =
        encodeHello(fixture.identity, 0x999000ull, 1, 3);
    QNP_CHECK(fixture.sidecar->onDatagram(endpoint, retry.data(), retry.size(),
                                          fixture.nowMs)
              == IngressOutcome::Accepted);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 1);

    QNP_CHECK(fixture.sidecar->declineViewer(0x999000ull, fixture.nowMs));
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
    QNP_CHECK(fixture.sidecar->status().viewersKicked == 1);
}

QNP_TEST(sidecar_raw_ask_host_hello_flood_never_allocates_reservations) {
    Fixture fixture;
    fixture.config.maxReservations = 3;
    fixture.config.rateLimits.globalBurstPackets = 512;
    fixture.config.rateLimits.perIpBurstPackets = 512;
    fixture.start(AdmissionPolicy::AskHost);
    fixture.publishStart();

    for (uint64_t index = 0; index < 128; ++index) {
        const RouteEndpoint endpoint = peer(
            static_cast<uint8_t>(34 + (index % 32)),
            static_cast<uint16_t>(51000 + index));
        const std::vector<uint8_t> hello = encodeHello(
            fixture.identity, 0x9A0000ull + index, 1, 1);
        QNP_CHECK(fixture.sidecar->onDatagram(
                      endpoint, hello.data(), hello.size(),
                      fixture.nowMs)
                  == IngressOutcome::AdmissionDenied);
    }
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewerCount == 0);
    QNP_CHECK(status.pendingApprovals == 0);
    QNP_CHECK(status.reservationCount == 0);
}

QNP_TEST(sidecar_kick_and_revoke_release_the_slot_and_refuse_a_return) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();

    GrantRequest request{};
    request.viewerId = 0xAB0000ull;
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(request, grant, deny,
                                            fixture.nowMs));

    const RouteEndpoint endpoint = peer(34, 50504);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0xAB0000ull, 1, 1);
    fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                fixture.nowMs);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    QNP_CHECK(fixture.sidecar->kickViewer(0xAB0000ull, fixture.nowMs));
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
    QNP_CHECK(fixture.sidecar->status().viewersKicked == 1);

    fixture.advance(50);
    const std::vector<uint8_t> retry =
        encodeHello(fixture.identity, 0xAB0000ull, 2, 5);
    QNP_CHECK(fixture.sidecar->onDatagram(endpoint, retry.data(), retry.size(),
                                          fixture.nowMs)
              == IngressOutcome::AdmissionDenied);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
}

QNP_TEST(sidecar_revoke_by_grant_id_retires_the_holder) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();

    GrantRequest request{};
    request.viewerId = 0xCD0000ull;
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(request, grant, deny,
                                            fixture.nowMs));
    const RouteEndpoint endpoint = peer(35, 50505);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0xCD0000ull, 1, 1);
    fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                fixture.nowMs);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    QNP_CHECK(fixture.sidecar->revokeGrant(grant.grantId, fixture.nowMs));
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
    QNP_CHECK(!fixture.sidecar->revokeGrant(grant.grantId + 12345,
                                            fixture.nowMs));
}

QNP_TEST(sidecar_capacity_is_reserved_by_grants_before_the_viewer_arrives) {
    Fixture fixture;
    fixture.config.maxViewers = 2;
    fixture.config.rateLimits.maxViewersPerIp = 8;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();

    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    GrantRequest first{};
    first.viewerId = 1001;
    QNP_CHECK(fixture.sidecar->issueGrant(first, grant, deny, fixture.nowMs));
    GrantRequest second{};
    second.viewerId = 1002;
    QNP_CHECK(fixture.sidecar->issueGrant(second, grant, deny, fixture.nowMs));
    GrantRequest third{};
    third.viewerId = 1003;
    QNP_CHECK(!fixture.sidecar->issueGrant(third, grant, deny, fixture.nowMs));
    QNP_CHECK(deny == AdmissionDenyReason::CapacityFull);

    QNP_CHECK(fixture.sidecar->releaseReservation(1002));
    QNP_CHECK(fixture.sidecar->issueGrant(third, grant, deny, fixture.nowMs));
}

QNP_TEST(sidecar_never_grants_during_the_sealed_tail_drain) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();
    fixture.publishRecords(2);

    GrantRequest attached{};
    attached.viewerId = 0xD000u;
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        attached, grant, deny, fixture.nowMs));
    const RouteEndpoint endpoint = peer(70, 50800);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, attached.viewerId, 1, 1);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    endpoint, hello.data(), hello.size(), fixture.nowMs)
                == IngressOutcome::Accepted);

    fixture.publishSeal();
    fixture.sidecar->service(fixture.nowMs);
    QNP_REQUIRE(
        fixture.sidecar->status().phase == SidecarPhase::Draining);

    GrantRequest late{};
    late.viewerId = 0xD001u;
    QNP_CHECK(!fixture.sidecar->issueGrant(
        late, grant, deny, fixture.nowMs));
    QNP_CHECK(deny == AdmissionDenyReason::ServiceExpired);
    QNP_CHECK(fixture.sidecar->status().reservationCount == 1);
}

QNP_TEST(sidecar_bound_detach_releases_capacity_but_keeps_one_use_tombstone) {
    Fixture fixture;
    fixture.config.maxViewers = 1;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();
    fixture.publishRecords(4);

    GrantRequest first{};
    first.viewerId = 0xD001u;
    first.lifetimeSeconds = 60;
    qsp::SpectatorGrant firstGrant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        first, firstGrant, deny, fixture.nowMs));

    const RouteEndpoint endpoint = peer(71, 50801);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, first.viewerId, 1, 1);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    endpoint, hello.data(), hello.size(), fixture.nowMs)
                == IngressOutcome::Accepted);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    const MirrorPrefix prefix = fixture.sidecar->localPrefix();
    const std::vector<uint8_t> detach = encodeDetach(
        fixture.identity, first.viewerId, 1, 2,
        prefix.nextRecord, prefix.digest);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    endpoint, detach.data(), detach.size(), fixture.nowMs + 1)
                == IngressOutcome::Accepted);
    SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewerCount == 0);
    QNP_CHECK(status.capacityReservations == 0);
    QNP_CHECK(status.consumedGrantTombstones == 1);

    QNP_CHECK(fixture.sidecar->releaseReservation(
        first.viewerId, fixture.nowMs + 2));
    QNP_CHECK(fixture.sidecar->status().reservationCount == 1);

    GrantRequest second{};
    second.viewerId = 0xD002u;
    QNP_CHECK(fixture.sidecar->issueGrant(
        second, firstGrant, deny, fixture.nowMs + 2));

    const std::vector<uint8_t> replay =
        encodeHello(fixture.identity, first.viewerId, 2, 3);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  endpoint, replay.data(), replay.size(), fixture.nowMs + 3)
              == IngressOutcome::AdmissionDenied);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
}

QNP_TEST(sidecar_authenticated_relay_detach_releases_capacity_same_turn) {
    Fixture fixture;
    fixture.config.maxViewers = 1;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.transport->relayGrants = true;
    fixture.publishStart();
    fixture.publishRecords(4);

    GrantRequest first{};
    first.viewerId = 0xD021u;
    first.peerIpv4BE = peer(71, 50821).ipv4BE;
    first.route = static_cast<uint8_t>(
        qnet::relay::SpectatorRouteKind::Relay);
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        first, grant, deny, fixture.nowMs));

    RouteEndpoint relayVirtual{};
    relayVirtual.ipv4BE = qsp::kRelayVirtualIpv4BE;
    relayVirtual.port = 50822;
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, first.viewerId, 1, 1);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    relayVirtual, hello.data(), hello.size(), fixture.nowMs)
                == IngressOutcome::Accepted);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    fixture.transport->detachRelayViewer(first.viewerId, relayVirtual);
    fixture.advance(1);
    fixture.sidecar->service(fixture.nowMs);
    SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewerCount == 0);
    QNP_CHECK(status.capacityReservations == 0);
    QNP_CHECK(status.consumedGrantTombstones == 1);
    QNP_CHECK(status.viewersDetached == 1);

    GrantRequest second = first;
    second.viewerId = 0xD022u;
    QNP_CHECK(fixture.sidecar->issueGrant(
        second, grant, deny, fixture.nowMs));
    QNP_CHECK(fixture.nowMs < 11000);
}

QNP_TEST(sidecar_deadline_retirement_releases_grant_capacity_not_replay) {
    Fixture fixture;
    fixture.config.maxViewers = 1;
    fixture.config.handshakeDeadlineMs = 1000;
    fixture.config.noProgressDeadlineMs = 60000;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();

    GrantRequest first{};
    first.viewerId = 0xD101u;
    first.lifetimeSeconds = 60;
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        first, grant, deny, fixture.nowMs));
    const RouteEndpoint endpoint = peer(72, 50802);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, first.viewerId, 1, 1);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    endpoint, hello.data(), hello.size(), fixture.nowMs)
                == IngressOutcome::Accepted);

    fixture.advance(1100);
    fixture.sidecar->service(fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
    QNP_CHECK(fixture.sidecar->status().capacityReservations == 0);

    GrantRequest second{};
    second.viewerId = 0xD102u;
    QNP_CHECK(fixture.sidecar->issueGrant(
        second, grant, deny, fixture.nowMs));
    const std::vector<uint8_t> replay =
        encodeHello(fixture.identity, first.viewerId, 2, 2);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  endpoint, replay.data(), replay.size(), fixture.nowMs)
              == IngressOutcome::AdmissionDenied);
}

QNP_TEST(sidecar_pre_game_hello_and_control_cancel_leave_no_ghost_viewer) {
    Fixture fixture;
    fixture.config.maxViewers = 1;
    fixture.start(AdmissionPolicy::CodeOnly);

    GrantRequest first{};
    first.viewerId = 0xD201u;
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        first, grant, deny, fixture.nowMs));
    const RouteEndpoint endpoint = peer(73, 50803);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, first.viewerId, 1, 1);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    endpoint, hello.data(), hello.size(), fixture.nowMs)
                == IngressOutcome::Accepted);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
    QNP_CHECK(fixture.sidecar->status().capacityReservations == 1);

    QNP_CHECK(fixture.sidecar->releaseReservation(
        first.viewerId, fixture.nowMs + 1));
    QNP_CHECK(fixture.sidecar->status().reservationCount == 0);
    GrantRequest second{};
    second.viewerId = 0xD202u;
    QNP_CHECK(fixture.sidecar->issueGrant(
        second, grant, deny, fixture.nowMs + 1));
}

QNP_TEST(sidecar_unknown_authenticated_detach_reclaims_lost_preflight_reply) {
    Fixture fixture;
    fixture.config.maxViewers = 1;
    fixture.start(AdmissionPolicy::CodeOnly);

    GrantRequest first{};
    first.viewerId = 0xD301u;
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        first, grant, deny, fixture.nowMs));
    const RouteEndpoint endpoint = peer(74, 50804);
    const std::vector<uint8_t> cancel = encodeDetach(
        fixture.identity, first.viewerId, 1, 1, 0, 0);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    endpoint, cancel.data(), cancel.size(), fixture.nowMs)
                == IngressOutcome::Accepted);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
    QNP_CHECK(fixture.sidecar->status().capacityReservations == 0);
    QNP_CHECK(fixture.sidecar->status().consumedGrantTombstones == 1);

    GrantRequest second{};
    second.viewerId = 0xD302u;
    QNP_CHECK(fixture.sidecar->issueGrant(
        second, grant, deny, fixture.nowMs));
}

QNP_TEST(sidecar_unknown_relay_detach_reclaims_the_real_peer_reservation) {
    Fixture fixture;
    fixture.config.maxViewers = 1;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.transport->relayGrants = true;

    GrantRequest first{};
    first.viewerId = 0xD311u;
    first.peerIpv4BE = peer(74, 50804).ipv4BE;
    first.route = static_cast<uint8_t>(
        qnet::relay::SpectatorRouteKind::Relay);
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        first, grant, deny, fixture.nowMs));

    RouteEndpoint relayEndpoint{};
    relayEndpoint.ipv4BE = 0xFEFFFFEFu;
    relayEndpoint.port = 50805;
    const std::vector<uint8_t> cancel = encodeDetach(
        fixture.identity, first.viewerId, 1, 1, 0, 0);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    relayEndpoint, cancel.data(), cancel.size(),
                    fixture.nowMs)
                == IngressOutcome::Accepted);
    QNP_CHECK(fixture.sidecar->status().capacityReservations == 0);
    QNP_CHECK(fixture.sidecar->status().consumedGrantTombstones == 1);
}

QNP_TEST(sidecar_cancelled_ask_host_grant_is_not_pending_but_stays_one_use) {
    Fixture fixture;
    fixture.config.maxViewers = 1;
    fixture.config.handshakeDeadlineMs = 100;
    fixture.start(AdmissionPolicy::AskHost);

    GrantRequest request{};
    request.viewerId = 0xD321u;
    request.lifetimeSeconds = 2;
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        request, grant, deny, fixture.nowMs));
    QNP_REQUIRE(fixture.sidecar->status().pendingApprovals == 1);

    const RouteEndpoint endpoint = peer(75, 50806);
    const std::vector<uint8_t> cancel = encodeDetach(
        fixture.identity, request.viewerId, 1, 1, 0, 0);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    endpoint, cancel.data(), cancel.size(),
                    fixture.nowMs)
                == IngressOutcome::Accepted);
    SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.pendingApprovals == 0);
    QNP_CHECK(status.capacityReservations == 0);
    QNP_CHECK(status.reservationCount == 1);
    QNP_CHECK(status.consumedGrantTombstones == 1);

    fixture.advance(200);
    fixture.sidecar->service(fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().reservationCount == 1);
    fixture.advance(2000);
    fixture.sidecar->service(fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().reservationCount == 0);
}

QNP_TEST(sidecar_sweeps_unused_expired_and_stale_ask_host_reservations) {
    Fixture fixture;
    fixture.config.maxViewers = 1;
    fixture.config.handshakeDeadlineMs = 1000;
    fixture.start(AdmissionPolicy::CodeOnly);

    GrantRequest expiring{};
    expiring.viewerId = 0xD401u;
    expiring.lifetimeSeconds = 1;
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        expiring, grant, deny, fixture.nowMs));
    fixture.advance(1100);
    fixture.sidecar->service(fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().reservationCount == 0);

    GrantRequest replacement{};
    replacement.viewerId = 0xD402u;
    QNP_CHECK(fixture.sidecar->issueGrant(
        replacement, grant, deny, fixture.nowMs));

    Fixture ask;
    ask.config.handshakeDeadlineMs = 1000;
    ask.start(AdmissionPolicy::AskHost);
    GrantRequest pending{};
    pending.viewerId = 0xD403u;
    QNP_REQUIRE(ask.sidecar->issueGrant(
        pending, grant, deny, ask.nowMs));
    QNP_CHECK(ask.sidecar->status().pendingApprovals == 1);
    ask.advance(1100);
    ask.sidecar->service(ask.nowMs);
    QNP_CHECK(ask.sidecar->status().pendingApprovals == 0);
    QNP_CHECK(ask.sidecar->status().reservationCount == 0);
}

QNP_TEST(sidecar_declined_and_revoked_tombstones_survive_until_expiry_only) {
    Fixture fixture;
    fixture.config.maxViewers = 4;
    fixture.config.maxReservations = 2;
    fixture.start(AdmissionPolicy::CodeOnly);

    AdmissionDenyReason deny = AdmissionDenyReason::None;
    qsp::SpectatorGrant declinedGrant{};
    GrantRequest declined{};
    declined.viewerId = 0xD410u;
    declined.lifetimeSeconds = 1;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        declined, declinedGrant, deny, fixture.nowMs));
    QNP_REQUIRE(fixture.sidecar->declineViewer(
        declined.viewerId, fixture.nowMs));

    qsp::SpectatorGrant revokedGrant{};
    GrantRequest revoked{};
    revoked.viewerId = 0xD411u;
    revoked.lifetimeSeconds = 1;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        revoked, revokedGrant, deny, fixture.nowMs));
    QNP_REQUIRE(fixture.sidecar->revokeGrant(
        revokedGrant.grantId, fixture.nowMs));

    SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.reservationCount == 2);
    QNP_CHECK(status.capacityReservations == 0);

    GrantRequest blocked{};
    blocked.viewerId = 0xD412u;
    QNP_CHECK(!fixture.sidecar->issueGrant(
        blocked, revokedGrant, deny, fixture.nowMs));
    QNP_CHECK(deny == AdmissionDenyReason::CapacityFull);

    fixture.advance(1100);
    fixture.sidecar->service(fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().reservationCount == 0);
    QNP_CHECK(fixture.sidecar->issueGrant(
        blocked, revokedGrant, deny, fixture.nowMs));
}

QNP_TEST(sidecar_reservation_map_is_hard_bounded_and_recovers_after_expiry) {
    Fixture fixture;
    fixture.config.maxViewers = 8;
    fixture.config.maxReservations = 3;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();

    AdmissionDenyReason deny = AdmissionDenyReason::None;
    qsp::SpectatorGrant grant{};
    for (uint64_t index = 0; index < 3; ++index) {
        GrantRequest request{};
        request.viewerId = 0xD500u + index;
        request.lifetimeSeconds = 1;
        QNP_REQUIRE(fixture.sidecar->issueGrant(
            request, grant, deny, fixture.nowMs));
        const RouteEndpoint endpoint =
            peer(static_cast<uint8_t>(75 + index),
                 static_cast<uint16_t>(50805 + index));
        const std::vector<uint8_t> hello =
            encodeHello(fixture.identity, request.viewerId, 1, 1);
        QNP_REQUIRE(fixture.sidecar->onDatagram(
                        endpoint, hello.data(), hello.size(), fixture.nowMs)
                    == IngressOutcome::Accepted);
        const MirrorPrefix prefix = fixture.sidecar->localPrefix();
        const std::vector<uint8_t> detach = encodeDetach(
            fixture.identity, request.viewerId, 1, 2,
            prefix.nextRecord, prefix.digest);
        QNP_REQUIRE(fixture.sidecar->onDatagram(
                        endpoint, detach.data(), detach.size(),
                        fixture.nowMs)
                    == IngressOutcome::Accepted);
    }
    const SidecarStatus full = fixture.sidecar->status();
    QNP_CHECK(full.reservationCount == 3);
    QNP_CHECK(full.consumedGrantTombstones == 3);
    QNP_CHECK(full.capacityReservations == 0);

    GrantRequest blocked{};
    blocked.viewerId = 0xD599u;
    QNP_CHECK(!fixture.sidecar->issueGrant(
        blocked, grant, deny, fixture.nowMs));
    QNP_CHECK(deny == AdmissionDenyReason::CapacityFull);
    QNP_CHECK(fixture.sidecar->status().reservationCount == 3);

    fixture.advance(1100);
    fixture.sidecar->service(fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().reservationCount == 0);
    QNP_CHECK(fixture.sidecar->issueGrant(
        blocked, grant, deny, fixture.nowMs));
}

QNP_TEST(sidecar_same_ip_quota_counts_each_bound_reservation_once) {
    Fixture fixture;
    fixture.config.maxViewers = 4;
    fixture.config.rateLimits.maxViewersPerIp = 2;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();
    const RouteEndpoint endpoint = peer(80, 50810);

    AdmissionDenyReason deny = AdmissionDenyReason::None;
    qsp::SpectatorGrant grant{};
    GrantRequest first{};
    first.viewerId = 0xD601u;
    first.peerIpv4BE = endpoint.ipv4BE;
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        first, grant, deny, fixture.nowMs));
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, first.viewerId, 1, 1);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    endpoint, hello.data(), hello.size(), fixture.nowMs)
                == IngressOutcome::Accepted);

    GrantRequest second{};
    second.viewerId = 0xD602u;
    second.peerIpv4BE = endpoint.ipv4BE;
    QNP_CHECK(fixture.sidecar->issueGrant(
        second, grant, deny, fixture.nowMs));
    GrantRequest third{};
    third.viewerId = 0xD603u;
    third.peerIpv4BE = endpoint.ipv4BE;
    QNP_CHECK(!fixture.sidecar->issueGrant(
        third, grant, deny, fixture.nowMs));
    QNP_CHECK(deny == AdmissionDenyReason::PerIpQuota);
}

QNP_TEST(sidecar_grant_binds_exact_resume_and_relay_transport_class) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();
    fixture.publishRecords(2);
    fixture.transport->relayGrants = true;
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    qsp::SpectatorGrant grant{};

    GrantRequest resume{};
    resume.viewerId = 0xD701u;
    resume.resumeRecord = 1;
    resume.resumeDigest = prefixDigestFor(1);
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        resume, grant, deny, fixture.nowMs));
    const RouteEndpoint direct = peer(81, 50811);
    const std::vector<uint8_t> wrongCursor =
        encodeHello(fixture.identity, resume.viewerId, 1, 1);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  direct, wrongCursor.data(), wrongCursor.size(),
                  fixture.nowMs)
              == IngressOutcome::AdmissionDenied);
    const std::vector<uint8_t> wrongDigest = encodeHelloAt(
        fixture.identity, resume.viewerId, 1, 2,
        resume.resumeRecord, resume.resumeDigest ^ 0x1u);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  direct, wrongDigest.data(), wrongDigest.size(),
                  fixture.nowMs)
              == IngressOutcome::AdmissionDenied);
    const std::vector<uint8_t> exactResume = encodeHelloAt(
        fixture.identity, resume.viewerId, 1, 3,
        resume.resumeRecord, resume.resumeDigest);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  direct, exactResume.data(), exactResume.size(),
                  fixture.nowMs)
              == IngressOutcome::Accepted);

    GrantRequest relay{};
    relay.viewerId = 0xD702u;
    relay.route = static_cast<uint8_t>(
        qnet::relay::SpectatorRouteKind::Relay);
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        relay, grant, deny, fixture.nowMs));
    const std::vector<uint8_t> relayOnDirect =
        encodeHello(fixture.identity, relay.viewerId, 1, 1);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  direct, relayOnDirect.data(), relayOnDirect.size(),
                  fixture.nowMs)
              == IngressOutcome::AdmissionDenied);

    GrantRequest directOnly{};
    directOnly.viewerId = 0xD703u;
    directOnly.route = static_cast<uint8_t>(
        qnet::relay::SpectatorRouteKind::Direct);
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        directOnly, grant, deny, fixture.nowMs));
    RouteEndpoint relayVirtual{};
    relayVirtual.ipv4BE = 0xFEFFFFEFu;
    relayVirtual.port = 50812;
    const std::vector<uint8_t> directOnRelay =
        encodeHello(fixture.identity, directOnly.viewerId, 1, 1);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  relayVirtual, directOnRelay.data(), directOnRelay.size(),
                  fixture.nowMs)
              == IngressOutcome::AdmissionDenied);
}

QNP_TEST(sidecar_same_viewer_direct_to_relay_reissue_replaces_one_slot) {
    Fixture fixture;
    fixture.config.maxViewers = 1;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();
    fixture.transport->relayGrants = true;

    AdmissionDenyReason deny = AdmissionDenyReason::None;
    GrantRequest direct{};
    direct.viewerId = 0xD801u;
    direct.route = static_cast<uint8_t>(
        qnet::relay::SpectatorRouteKind::Direct);
    qsp::SpectatorGrant directGrant{};
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        direct, directGrant, deny, fixture.nowMs));
    const RouteEndpoint directEndpoint = peer(82, 50813);
    const std::vector<uint8_t> firstHello =
        encodeHello(fixture.identity, direct.viewerId, 1, 1);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    directEndpoint, firstHello.data(), firstHello.size(),
                    fixture.nowMs)
                == IngressOutcome::Accepted);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    GrantRequest relay = direct;
    relay.route = static_cast<uint8_t>(
        qnet::relay::SpectatorRouteKind::Relay);
    qsp::SpectatorGrant relayGrant{};
    QNP_REQUIRE(fixture.sidecar->issueGrant(
        relay, relayGrant, deny, fixture.nowMs + 1));
    QNP_CHECK(relayGrant.grantId != directGrant.grantId);
    SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewerCount == 0);
    QNP_CHECK(status.reservationCount == 1);
    QNP_CHECK(status.capacityReservations == 1);

    const std::vector<uint8_t> oldRouteReplay =
        encodeHello(fixture.identity, direct.viewerId, 2, 2);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  directEndpoint, oldRouteReplay.data(),
                  oldRouteReplay.size(), fixture.nowMs + 2)
              == IngressOutcome::AdmissionDenied);

    RouteEndpoint relayEndpoint{};
    relayEndpoint.ipv4BE = 0xFEFFFFEFu;
    relayEndpoint.port = 50814;
    const std::vector<uint8_t> relayHello =
        encodeHello(fixture.identity, relay.viewerId, 2, 3);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  relayEndpoint, relayHello.data(), relayHello.size(),
                  fixture.nowMs + 3)
              == IngressOutcome::Accepted);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 1);
    QNP_CHECK(fixture.sidecar->status().capacityReservations == 0);
}

QNP_TEST(sidecar_status_redacts_grant_material) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();
    GrantRequest request{};
    request.viewerId = 0xEF0000ull;
    qsp::SpectatorGrant grant{};
    AdmissionDenyReason deny = AdmissionDenyReason::None;
    QNP_REQUIRE(fixture.sidecar->issueGrant(request, grant, deny,
                                            fixture.nowMs));
    const RouteEndpoint endpoint = peer(36, 50506);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0xEF0000ull, 1, 1);
    fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                fixture.nowMs);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_REQUIRE(status.viewers.size() == 1);
    QNP_CHECK(!status.viewers[0].grantTag.empty());
    QNP_CHECK(status.viewers[0].grantTag[0] == '#');
    QNP_CHECK(status.viewers[0].grantTag.size() <= 8);
    QNP_CHECK(status.viewers[0].grantTag != std::to_string(grant.grantId));
    for (const std::string& line : fixture.log)
        QNP_CHECK(line.find(std::to_string(grant.grantId)) == std::string::npos);
}

QNP_TEST(sidecar_status_covers_every_lifecycle_state) {
    Fixture fixture;
    fixture.start();
    QNP_CHECK(fixture.sidecar->status().phase == SidecarPhase::Waiting);
    QNP_CHECK(fixture.sidecar->status().code
              == SourceStatusCode::WaitingForStart);

    fixture.publishStart();
    fixture.publishRecords(3);
    QNP_CHECK(fixture.sidecar->status().phase == SidecarPhase::Active);
    QNP_CHECK(fixture.sidecar->status().code == SourceStatusCode::Streaming);

    PublicationEvent pause{};
    pause.kind = PublicationKind::Pause;
    pause.sequence = fixture.publicationSequence++;
    fixture.sidecar->publish(pause, fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().phase == SidecarPhase::Paused);
    QNP_CHECK(fixture.sidecar->status().code == SourceStatusCode::PausedHeld);

    PublicationEvent resume{};
    resume.kind = PublicationKind::Resume;
    resume.sequence = fixture.publicationSequence++;
    fixture.sidecar->publish(resume, fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().phase == SidecarPhase::Active);

    PublicationEvent checkpoint{};
    checkpoint.kind = PublicationKind::Checkpoint;
    checkpoint.sequence = fixture.publicationSequence++;
    checkpoint.checkpointId = 77;
    checkpoint.checkpointRecord = 2;
    checkpoint.checkpointDigest = 0x1234;
    checkpoint.checkpointBytes.assign(64, 0x11);
    fixture.sidecar->publish(checkpoint, fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().phase == SidecarPhase::Checkpointing);
    QNP_CHECK(fixture.sidecar->status().code
              == SourceStatusCode::Checkpointing);
    QNP_CHECK(fixture.sidecar->status().checkpointCount == 1);
    QNP_CHECK(fixture.sidecar->status().newestCheckpointRecord == 2);

    fixture.publishSeal(qsp::TerminalReason::SourceStopped);
    QNP_CHECK(fixture.sidecar->status().phase == SidecarPhase::Sealed);
    QNP_CHECK(fixture.sidecar->status().code
              == SourceStatusCode::PeerQuitSourceStopped);

    const SourceStatusCode codes[] = {
        SourceStatusCode::Disabled, SourceStatusCode::WaitingForStart,
        SourceStatusCode::Streaming, SourceStatusCode::PausedHeld,
        SourceStatusCode::Checkpointing, SourceStatusCode::SealedDraining,
        SourceStatusCode::CompletedClean,
        SourceStatusCode::PeerQuitSourceStopped,
        SourceStatusCode::ProtocolOrArchiveFailure,
        SourceStatusCode::CapacityRejection,
        SourceStatusCode::RouteLostResuming,
        SourceStatusCode::TerminalProofMismatch,
        SourceStatusCode::RetainedPostGame,
        SourceStatusCode::RetentionExpired,
    };
    for (SourceStatusCode code : codes)
        QNP_CHECK(std::string(sourceStatusText(code)).size() > 3);
}

QNP_TEST(sidecar_checkpoint_ring_is_bounded) {
    Fixture fixture;
    fixture.config.maxCheckpoints = 2;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(20);
    for (uint32_t i = 1; i <= 5; ++i) {
        PublicationEvent checkpoint{};
        checkpoint.kind = PublicationKind::Checkpoint;
        checkpoint.sequence = fixture.publicationSequence++;
        checkpoint.checkpointId = i;
        checkpoint.checkpointRecord = i * 2;
        checkpoint.checkpointDigest = i;
        checkpoint.checkpointBytes.assign(128, static_cast<uint8_t>(i));
        fixture.sidecar->publish(checkpoint, fixture.nowMs);
    }
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.checkpointCount == 2);
    QNP_CHECK(status.newestCheckpointRecord == 10);
}

QNP_TEST(sidecar_phase_maps_onto_the_wire_source_phase) {
    QNP_CHECK(toWireSourcePhase(SidecarPhase::Waiting)
              == qsp::SourcePhase::Waiting);
    QNP_CHECK(toWireSourcePhase(SidecarPhase::Active)
              == qsp::SourcePhase::Active);
    QNP_CHECK(toWireSourcePhase(SidecarPhase::Paused)
              == qsp::SourcePhase::Paused);
    QNP_CHECK(toWireSourcePhase(SidecarPhase::Checkpointing)
              == qsp::SourcePhase::Checkpointing);
    QNP_CHECK(toWireSourcePhase(SidecarPhase::Sealed)
              == qsp::SourcePhase::Sealed);
    QNP_CHECK(toWireSourcePhase(SidecarPhase::Draining)
              == qsp::SourcePhase::Draining);
    QNP_CHECK(toWireSourcePhase(SidecarPhase::Retained)
              == qsp::SourcePhase::Retained);
    QNP_CHECK(toWireSourcePhase(SidecarPhase::Expired)
              == qsp::SourcePhase::Expired);
    QNP_CHECK(toWireSourcePhase(SidecarPhase::Failed)
              == qsp::SourcePhase::Failed);
    QNP_CHECK(toWireSourcePhase(SidecarPhase::Idle)
              == qsp::SourcePhase::Waiting);
}

QNP_TEST(sidecar_accepts_a_matching_peer_prefix_and_becomes_an_alternate) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(30);

    const MirrorPrefix local = fixture.sidecar->localPrefix();
    QNP_CHECK(local.gameId == kGameId);
    QNP_CHECK(local.nextRecord == 29);
    QNP_CHECK(local.startFingerprint != 0);

    QNP_CHECK(fixture.sidecar->verifyPeerPrefix(local) == MirrorResult::Accepted);
    QNP_CHECK(!fixture.sidecar->isAlternateSource());
    QNP_CHECK(fixture.sidecar->acceptPeerPrefix(local, fixture.nowMs)
              == MirrorResult::Accepted);
    QNP_CHECK(fixture.sidecar->isAlternateSource());

    MirrorPrefix shorter = local;
    shorter.nextRecord = 10;
    uint64_t digestAtTen = 0;
    QNP_REQUIRE(fixture.sidecar->verifyPeerPrefix(shorter)
                != MirrorResult::Accepted);
    (void)digestAtTen;
}

QNP_TEST(sidecar_rejects_a_divergent_peer_prefix) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(30);
    const MirrorPrefix local = fixture.sidecar->localPrefix();

    MirrorPrefix divergent = local;
    divergent.digest ^= 0xDEADBEEFull;
    QNP_CHECK(fixture.sidecar->verifyPeerPrefix(divergent)
              == MirrorResult::DigestDivergence);
    QNP_CHECK(fixture.sidecar->acceptPeerPrefix(divergent, fixture.nowMs)
              == MirrorResult::DigestDivergence);
    QNP_CHECK(!fixture.sidecar->isAlternateSource());

    MirrorPrefix wrongGame = local;
    wrongGame.gameId = 99;
    QNP_CHECK(fixture.sidecar->verifyPeerPrefix(wrongGame)
              == MirrorResult::GameMismatch);

    MirrorPrefix wrongFingerprint = local;
    wrongFingerprint.startFingerprint ^= 0x1ull;
    QNP_CHECK(fixture.sidecar->verifyPeerPrefix(wrongFingerprint)
              == MirrorResult::FingerprintMismatch);

    MirrorPrefix ahead = local;
    ahead.nextRecord = local.nextRecord + 500;
    QNP_CHECK(fixture.sidecar->verifyPeerPrefix(ahead)
              == MirrorResult::CursorAhead);

    QNP_CHECK(!fixture.sidecar->isAlternateSource());
}

QNP_TEST(sidecar_without_an_archive_cannot_be_an_alternate_source) {
    Fixture fixture;
    fixture.start();
    MirrorPrefix prefix{};
    prefix.gameId = kGameId;
    QNP_CHECK(fixture.sidecar->verifyPeerPrefix(prefix)
              == MirrorResult::NoLocalArchive);
    QNP_CHECK(fixture.sidecar->acceptPeerPrefix(prefix, fixture.nowMs)
              == MirrorResult::NoLocalArchive);
    QNP_CHECK(!fixture.sidecar->isAlternateSource());

    const MirrorResult results[] = {
        MirrorResult::Accepted,   MirrorResult::NoLocalArchive,
        MirrorResult::GameMismatch, MirrorResult::FingerprintMismatch,
        MirrorResult::DigestDivergence, MirrorResult::CursorAhead,
        MirrorResult::ServiceFailed,
    };
    for (MirrorResult result : results)
        QNP_CHECK(std::string(mirrorResultText(result)).size() > 3);
}

QNP_TEST(sidecar_detach_releases_the_slot_immediately) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(5);

    const RouteEndpoint endpoint = peer(40, 50600);
    const uint64_t viewerId = 0x5150000ull;
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, viewerId, 1, 1);
    fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                fixture.nowMs);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    const MirrorPrefix prefix = fixture.sidecar->localPrefix();
    qsp::DetachPayload detach{};
    detach.viewerId = viewerId;
    detach.generation = 1;
    detach.nextRecord = prefix.nextRecord;
    detach.digest = prefix.digest;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    QNP_REQUIRE(qsp::encodePayload(detach, payload));
    QNP_REQUIRE(qsp::encodeDatagram(qsp::MessageType::Detach, fixture.identity,
                                    2, payload, datagram));
    fixture.advance(10);
    fixture.sidecar->onDatagram(endpoint, datagram.data(), datagram.size(),
                                fixture.nowMs);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewerCount == 0);
    QNP_CHECK(status.viewersDetached == 1);
    QNP_REQUIRE(!status.terminalOutcomes.empty());
    const ViewerTerminalOutcome& outcome = status.terminalOutcomes.back();
    QNP_CHECK(outcome.viewerId == viewerId);
    QNP_CHECK(outcome.generation == 1);
    QNP_CHECK(outcome.state == ViewerServiceState::Detached);
    QNP_CHECK(outcome.acknowledgedRecord == 0);
}

QNP_TEST(sidecar_seedless_direct_detach_after_lost_gamestart_retires_outer_slot) {
    Fixture fixture;
    fixture.config.maxViewers = 1;
    fixture.start(AdmissionPolicy::Public);
    fixture.publishStart();

    const RouteEndpoint endpoint = peer(42, 50602);
    const uint64_t viewerId = 0x6160001ull;
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, viewerId, 1, 1);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    endpoint, hello.data(), hello.size(), fixture.nowMs)
                == IngressOutcome::Accepted);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);
    QNP_CHECK(fixture.sidecar->status().reservationCount == 0);
    QNP_CHECK(fixture.sidecar->status().capacityReservations == 0);

    fixture.sidecar->service(fixture.nowMs);
    auto& outbound =
        fixture.transport->outbound[FakeTransport::key(endpoint)];
    bool emittedGameStart = false;
    for (const FakeTransport::Packet& packet : outbound) {
        qsp::Datagram datagram{};
        if (qsp::decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                fixture.identity, datagram)
                == qsp::DecodeError::None
            && datagram.header.type == qsp::MessageType::GameStart) {
            emittedGameStart = true;
        }
    }
    QNP_REQUIRE(emittedGameStart);
    outbound.clear();

    const std::vector<uint8_t> detach =
        encodeDetach(fixture.identity, viewerId, 1, 2, 0, 0);
    fixture.advance(1);
    QNP_CHECK(fixture.sidecar->onDatagram(
                  endpoint, detach.data(), detach.size(), fixture.nowMs)
              == IngressOutcome::Accepted);

    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewerCount == 0);
    QNP_CHECK(status.viewersDetached == 1);
    QNP_CHECK(status.reservationCount == 0);
    QNP_CHECK(status.capacityReservations == 0);
    QNP_REQUIRE(!status.terminalOutcomes.empty());
    const ViewerTerminalOutcome& outcome = status.terminalOutcomes.back();
    QNP_CHECK(outcome.viewerId == viewerId);
    QNP_CHECK(outcome.generation == 1);
    QNP_CHECK(outcome.state == ViewerServiceState::Detached);
}

QNP_TEST(sidecar_handshake_deadline_retires_a_silent_viewer) {
    Fixture fixture;
    fixture.config.handshakeDeadlineMs = 2000;
    fixture.config.noProgressDeadlineMs = 60000;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(5);

    const RouteEndpoint endpoint = peer(41, 50601);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0x6160000ull, 1, 1);
    fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                fixture.nowMs);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    fixture.advance(3000);
    fixture.sidecar->service(fixture.nowMs);
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewerCount == 0);
    QNP_CHECK(status.viewersDeadlineExpired == 1);
}

QNP_TEST(sidecar_caught_up_heartbeat_viewer_survives_a_long_source_pause) {
    Fixture fixture;
    fixture.config.noProgressDeadlineMs = 1000;
    fixture.config.heartbeatIntervalMs = 100;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(12);

    HarnessViewer viewer(peer(44, 50604).ipv4BE, 50604,
                         0x9190000ull, fixture.identity);
    fixture.run({&viewer}, 80, 20);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);
    QNP_REQUIRE(fixture.sidecar->status().viewers[0].acknowledgedRecord
                == fixture.sidecar->status().confirmedRecords);

    PublicationEvent pause{};
    pause.kind = PublicationKind::Pause;
    pause.sequence = fixture.publicationSequence++;
    fixture.sidecar->publish(pause, fixture.nowMs);

    fixture.run({&viewer}, 500, 20);
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.phase == SidecarPhase::Paused);
    QNP_CHECK(status.viewerCount == 1);
    QNP_CHECK(status.viewersDeadlineExpired == 0);
}

QNP_TEST(sidecar_archive_cap_retires_only_the_stale_viewer) {
    Fixture fixture;
    fixture.config.retainAllRecords = false;
    fixture.config.maxRetainedRecords = 8;
    fixture.config.maxRetainedBytes =
        8u * sizeof(qsp::ConfirmedRecord);
    fixture.config.maxViewerUnackedRecords = 1000;
    fixture.config.maxViewerUnackedBytes = 1024u * 1024u;
    fixture.config.handshakeDeadlineMs = 60000;
    fixture.start();
    fixture.publishStart();

    HarnessViewer fast(peer(45, 50605).ipv4BE, 50605,
                       0xA190000ull, fixture.identity);
    fixture.run({&fast}, 30, 20);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    const RouteEndpoint staleEndpoint = peer(46, 50606);
    const uint64_t staleId = 0xA190001ull;
    const std::vector<uint8_t> staleHello =
        encodeHello(fixture.identity, staleId, 1, 1);
    fixture.sidecar->onDatagram(
        staleEndpoint, staleHello.data(), staleHello.size(),
        fixture.nowMs);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 2);

    for (uint32_t record = 0; record < 10; ++record) {
        fixture.publishRecords(1);
        fixture.run({&fast}, 12, 20);
    }
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewerCount == 1);
    QNP_REQUIRE(status.viewers.size() == 1);
    QNP_CHECK(status.viewers[0].viewerId == fast.viewer->viewerId());
    QNP_CHECK(status.viewersDeadlineExpired == 1);
    QNP_CHECK(status.firstAvailableRecord > 0);
    QNP_CHECK(status.retainedRecordCount <= 8);
    QNP_CHECK(status.allocatedRecordBytes
              <= fixture.config.maxRetainedBytes);
}

QNP_TEST(sidecar_stop_without_drain_reports_the_cut_off_viewers) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(20);

    HarnessViewer viewer(peer(42, 50602).ipv4BE, 50602, 0x7170000ull,
                         fixture.identity);
    viewer.playbackEnabled = false;
    fixture.run({&viewer}, 20);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    QNP_CHECK(fixture.sidecar->stop(false, fixture.nowMs));
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewersDeadlineExpired == 1);
    QNP_CHECK(status.viewersCompleted == 0);
    QNP_CHECK(status.sealed);
    QNP_CHECK(status.sealedIsTruncation);
    QNP_CHECK(status.phase == SidecarPhase::Expired);
    QNP_CHECK(fixture.sidecar->stopped());
}

QNP_TEST(sidecar_stop_with_drain_completes_a_healthy_viewer) {
    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(24);

    HarnessViewer viewer(peer(43, 50603).ipv4BE, 50603, 0x8180000ull,
                         fixture.identity);
    fixture.run({&viewer}, 30);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    fixture.publishSeal();
    bool complete = false;
    for (int i = 0; i < 300 && !complete; ++i) {
        fixture.advance(20);
        complete = fixture.sidecar->stop(true, fixture.nowMs);
        viewer.pump(*fixture.transport, fixture.nowMs);
    }
    QNP_CHECK(complete);
    QNP_CHECK(viewer.complete());
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.viewersCompleted == 1);
    QNP_CHECK(status.viewersDeadlineExpired == 0);
    QNP_CHECK(!status.sealedIsTruncation);
}

QNP_TEST(sidecar_disconnect_without_a_terminal_seals_a_truncation) {
    SidecarConfig config{};
    config.identity = makeIdentity();
    config.simulationContract = kContract;
    config.compatibilityIdentity = makeCompatibilityIdentity();
    config.retainAllRecords = true;

    auto* channel = new ScriptedPublicationChannel();
    channel->live = false;
    PublicationEvent start{};
    start.kind = PublicationKind::Start;
    start.descriptor = makeDescriptor();
    channel->events.push_back(start);
    uint64_t digest = qsp::expectedInitialDigest(start.descriptor);
    for (uint32_t index = 0; index < 5u; ++index) {
        PublicationEvent record{};
        record.kind = PublicationKind::Record;
        record.record = makeRecord(index);
        digest = qsp::advanceRollingDigest(
            digest, kGameId, record.record);
        record.record.rollingDigest = digest;
        channel->events.push_back(record);
    }

    std::unique_ptr<FakeTransport> transport(new FakeTransport());
    SpectatorSidecar sidecar(config);
    std::string error;
    QNP_REQUIRE(sidecar.start(
        std::move(transport),
        std::unique_ptr<PublicationChannel>(channel), 1000u, error));
    sidecar.service(1020u);

    const SidecarStatus status = sidecar.status();
    QNP_CHECK(status.sealed);
    QNP_CHECK(status.sealedIsTruncation);
    QNP_CHECK(status.confirmedRecords == 4u);
    QNP_CHECK(status.postTailStateHash == stateHashFor(4u));
}

QNP_TEST(sidecar_waits_for_a_bounded_publication_reconnect) {
    SidecarConfig config{};
    config.identity = makeIdentity();
    config.simulationContract = kContract;
    config.compatibilityIdentity = makeCompatibilityIdentity();
    config.retainAllRecords = true;

    auto* channel = new ScriptedPublicationChannel();
    PublicationEvent start{};
    start.kind = PublicationKind::Start;
    start.descriptor = makeDescriptor();
    channel->events.push_back(start);
    channel->live = false;
    channel->reconnectPending = true;

    std::unique_ptr<FakeTransport> transport(new FakeTransport());
    SpectatorSidecar sidecar(config);
    std::string error;
    QNP_REQUIRE(sidecar.start(
        std::move(transport),
        std::unique_ptr<PublicationChannel>(channel), 1000u, error));
    sidecar.service(1020u);
    QNP_CHECK(sidecar.status().hasGame);
    QNP_CHECK(!sidecar.status().sealed);

    channel->reconnectPending = false;
    channel->live = true;
    PublicationEvent loss{};
    loss.kind = PublicationKind::LossReport;
    channel->events.push_back(loss);
    sidecar.service(1040u);
    QNP_CHECK(sidecar.status().sealed);
    QNP_CHECK(sidecar.status().sealedIsTruncation);
    QNP_CHECK(sidecar.status().publicationGaps == 1);
}

QNP_TEST(sidecar_drains_a_queued_exact_terminal_before_treating_eof_as_loss) {
    SidecarConfig config{};
    config.identity = makeIdentity();
    config.simulationContract = kContract;
    config.compatibilityIdentity = makeCompatibilityIdentity();
    config.retainAllRecords = true;

    auto* channel = new ScriptedPublicationChannel();
    channel->live = false;
    PublicationEvent start{};
    start.kind = PublicationKind::Start;
    start.descriptor = makeDescriptor();
    channel->events.push_back(start);
    uint64_t digest = qsp::expectedInitialDigest(start.descriptor);
    constexpr uint32_t kRecords = 1100u;
    for (uint32_t index = 0; index < kRecords; ++index) {
        PublicationEvent record{};
        record.kind = PublicationKind::Record;
        record.record = makeRecord(index);
        digest = qsp::advanceRollingDigest(
            digest, kGameId, record.record);
        record.record.rollingDigest = digest;
        channel->events.push_back(record);
    }
    PublicationEvent seal{};
    seal.kind = PublicationKind::Seal;
    seal.reason = qsp::TerminalReason::MatchEnded;
    seal.terminalRecordCursor = kRecords;
    seal.terminalPrefixDigest = digest;
    seal.hasTerminalPrefixProof = true;
    seal.postTailStateHash = stateHashFor(kRecords);
    seal.hasPostTailProof = true;
    channel->events.push_back(seal);

    std::unique_ptr<FakeTransport> transport(new FakeTransport());
    SpectatorSidecar sidecar(config);
    std::string error;
    QNP_REQUIRE(sidecar.start(
        std::move(transport),
        std::unique_ptr<PublicationChannel>(channel), 1000u, error));

    sidecar.service(1020u);
    QNP_CHECK(!sidecar.status().sealed);
    sidecar.service(1040u);
    const SidecarStatus status = sidecar.status();
    QNP_CHECK(status.sealed);
    QNP_CHECK(!status.sealedIsTruncation);
    QNP_CHECK(status.confirmedRecords == kRecords);
    QNP_CHECK(status.terminalRecordCursor == kRecords);
    QNP_CHECK(status.terminalPrefixDigest == digest);
}

namespace {

void putU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>(value >> 8));
}
void putU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
}
void putU64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
}

uint32_t testCrc32c(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> frameOf(PublicationWireKind kind, uint32_t sequence,
                             uint32_t param0, uint64_t gameId,
                             const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    putU32(frame, kPublicationMagic);
    putU16(frame, kPublicationVersion);
    putU16(frame, static_cast<uint16_t>(kind));
    putU32(frame, sequence);
    putU32(frame, param0);
    putU32(frame, static_cast<uint32_t>(payload.size()));
    putU32(frame, payload.empty()
                      ? 0u
                      : testCrc32c(payload.data(), payload.size()));
    putU64(frame, gameId);
    putU32(frame, testCrc32c(frame.data(), 32));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

}

#ifdef _WIN32
QNP_TEST(publication_pipe_accepts_one_bounded_reconnect_and_reports_loss) {
    const uint32_t pid = GetCurrentProcessId();
    std::string error;
    std::unique_ptr<PublicationChannel> channel =
        makePipePublicationChannel(pid, error);
    QNP_REQUIRE(channel != nullptr);

    wchar_t pipeName[128] = {};
    qipc::ipcSpectatorSidecarPipeName(pipeName, 128, pid);
    auto connectWriter = [&]() -> HANDLE {
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            HANDLE pipe = CreateFileW(
                pipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                nullptr);
            if (pipe != INVALID_HANDLE_VALUE) return pipe;
            if (GetLastError() == ERROR_PIPE_BUSY)
                WaitNamedPipeW(pipeName, 20);
            else
                Sleep(5);
        }
        return INVALID_HANDLE_VALUE;
    };
    auto writeAll = [](HANDLE pipe,
                       const std::vector<uint8_t>& bytes) {
        size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD wrote = 0;
            if (!WriteFile(
                    pipe, bytes.data() + offset,
                    static_cast<DWORD>(bytes.size() - offset),
                    &wrote, nullptr)
                || wrote == 0) {
                return false;
            }
            offset += wrote;
        }
        return true;
    };

    std::vector<uint8_t> hello;
    putU64(hello, kGameId);
    putU32(hello, kPublicationVersion);
    putU32(hello, 0u);
    const std::vector<uint8_t> first = frameOf(
        PublicationWireKind::Hello, 1u, 0u, kGameId, hello);

    HANDLE writer = connectWriter();
    QNP_REQUIRE(writer != INVALID_HANDLE_VALUE);
    QNP_REQUIRE(writeAll(writer, first));
    FlushFileBuffers(writer);
    CloseHandle(writer);

    for (unsigned attempt = 0;
         attempt < 100 && !channel->reconnecting(); ++attempt) {
        Sleep(5);
    }
    QNP_REQUIRE(channel->reconnecting());

    writer = connectWriter();
    QNP_REQUIRE(writer != INVALID_HANDLE_VALUE);
    QNP_REQUIRE(writeAll(writer, first));

    std::vector<PublicationEvent> events;
    for (unsigned attempt = 0; attempt < 100 && events.empty(); ++attempt) {
        const int drained = channel->drain(events, 8);
        QNP_CHECK(drained >= 0);
        if (events.empty()) Sleep(5);
    }
    QNP_REQUIRE(!events.empty());
    QNP_CHECK(events.front().kind == PublicationKind::LossReport);
    CloseHandle(writer);
    channel->close();
}
#endif

QNP_TEST(publication_frame_decodes_start_confirmed_checkpoint_and_seal) {
    std::vector<PublicationEvent> events;

    std::vector<uint8_t> start;
    putU64(start, kGameId);
    putU64(start, kContract);
    putU64(start, stateHashFor(0));
    putU64(start, 0);
    putU32(start, 0);
    putU16(start, 2);
    putU16(start, 3);
    putU16(start, 8);
    putU16(start, 0);
    putU32(start, 4);
    start.insert(start.end(), {0xDE, 0xAD, 0xBE, 0xEF});
    QNP_REQUIRE(decodePublicationFrame(
        frameOf(PublicationWireKind::Start, 2, 0, kGameId, start).data(),
        kPublicationHeaderSize + start.size(), events));
    QNP_REQUIRE(events.size() == 1);
    QNP_CHECK(events[0].kind == PublicationKind::Start);
    QNP_CHECK(events[0].descriptor.gameId == kGameId);
    QNP_CHECK(events[0].descriptor.simulationContract == kContract);
    QNP_CHECK(events[0].descriptor.playerTwoDelay == 3);
    QNP_CHECK(events[0].descriptor.metadata.size() == 4);

    events.clear();
    std::vector<uint8_t> batch;
    for (uint32_t i = 0; i < 3; ++i) {
        putU32(batch, i);
        putU32(batch, i);
        putU16(batch, static_cast<uint16_t>(i));
        putU16(batch, static_cast<uint16_t>(i + 1));
        putU32(batch, 0);
        putU64(batch, stateHashFor(i));
        putU64(batch, 0x100 + i);
    }
    QNP_REQUIRE(decodePublicationFrame(
        frameOf(PublicationWireKind::Confirmed, 3, 3, kGameId, batch).data(),
        kPublicationHeaderSize + batch.size(), events));
    QNP_REQUIRE(events.size() == 3);
    for (uint32_t i = 0; i < 3; ++i) {
        QNP_CHECK(events[i].kind == PublicationKind::Record);
        QNP_CHECK(events[i].record.record == i);
        QNP_CHECK(events[i].record.stateHash == stateHashFor(i));
    }

    events.clear();
    std::vector<uint8_t> checkpoint;
    putU32(checkpoint, 120);
    putU32(checkpoint, 5);
    checkpoint.insert(checkpoint.end(), {1, 2, 3, 4, 5});
    QNP_REQUIRE(decodePublicationFrame(
        frameOf(PublicationWireKind::Checkpoint, 4, 120, kGameId, checkpoint)
            .data(),
        kPublicationHeaderSize + checkpoint.size(), events));
    QNP_REQUIRE(events.size() == 1);
    QNP_CHECK(events[0].kind == PublicationKind::Checkpoint);
    QNP_CHECK(events[0].checkpointRecord == 120);
    QNP_CHECK(events[0].checkpointBytes.size() == 5);

    events.clear();
    std::vector<uint8_t> seal;
    putU16(seal, static_cast<uint16_t>(qsp::TerminalReason::MatchEnded));
    putU16(seal, 0);
    putU32(seal, 999);
    putU64(seal, prefixDigestFor(999));
    putU64(seal, stateHashFor(999));
    QNP_REQUIRE(decodePublicationFrame(
        frameOf(PublicationWireKind::Seal, 5, 1, kGameId, seal).data(),
        kPublicationHeaderSize + seal.size(), events));
    QNP_REQUIRE(events.size() == 1);
    QNP_CHECK(events[0].kind == PublicationKind::Seal);
    QNP_CHECK(events[0].reason == qsp::TerminalReason::MatchEnded);
    QNP_CHECK(events[0].terminalRecordCursor == 999);
    QNP_CHECK(events[0].terminalPrefixDigest == prefixDigestFor(999));
    QNP_CHECK(events[0].hasTerminalPrefixProof);
    QNP_CHECK(events[0].hasPostTailProof);
    QNP_CHECK(events[0].postTailStateHash == stateHashFor(999));
}

QNP_TEST(publication_frame_decodes_a_tail_proof_and_rejects_a_hollow_one) {
    std::vector<PublicationEvent> events;
    std::vector<uint8_t> proof;
    putU32(proof, 12);
    putU32(proof, 0);
    putU64(proof, stateHashFor(12));
    QNP_REQUIRE(decodePublicationFrame(
        frameOf(PublicationWireKind::TailProof, 6, 12, kGameId, proof).data(),
        kPublicationHeaderSize + proof.size(), events));
    QNP_REQUIRE(events.size() == 1);
    QNP_CHECK(events[0].kind == PublicationKind::TailProof);
    QNP_CHECK(events[0].tailRecordCursor == 12);
    QNP_CHECK(events[0].hasPostTailProof);
    QNP_CHECK(events[0].postTailStateHash == stateHashFor(12));

    events.clear();
    std::vector<uint8_t> noHash;
    putU32(noHash, 12);
    putU32(noHash, 0);
    putU64(noHash, 0);
    QNP_CHECK(!decodePublicationFrame(
        frameOf(PublicationWireKind::TailProof, 6, 12, kGameId, noHash).data(),
        kPublicationHeaderSize + noHash.size(), events));
    std::vector<uint8_t> noCursor;
    putU32(noCursor, 0);
    putU32(noCursor, 0);
    putU64(noCursor, stateHashFor(3));
    QNP_CHECK(!decodePublicationFrame(
        frameOf(PublicationWireKind::TailProof, 6, 0, kGameId, noCursor).data(),
        kPublicationHeaderSize + noCursor.size(), events));
    QNP_CHECK(!decodePublicationFrame(
        frameOf(PublicationWireKind::TailProof, 6, 11, kGameId, proof).data(),
        kPublicationHeaderSize + proof.size(), events));
    QNP_CHECK(events.empty());
}

QNP_TEST(publication_seal_with_a_zero_hash_is_not_a_proof) {
    std::vector<PublicationEvent> events;
    std::vector<uint8_t> seal;
    putU16(seal, static_cast<uint16_t>(qsp::TerminalReason::MatchEnded));
    putU16(seal, 0);
    putU32(seal, 6);
    putU64(seal, prefixDigestFor(6));
    putU64(seal, 0);
    QNP_REQUIRE(decodePublicationFrame(
        frameOf(PublicationWireKind::Seal, 5, 1, kGameId, seal).data(),
        kPublicationHeaderSize + seal.size(), events));
    QNP_REQUIRE(events.size() == 1);
    QNP_CHECK(!events[0].hasPostTailProof);

    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(6);
    fixture.sidecar->publish(events[0], fixture.nowMs);
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.sealedIsTruncation);
    QNP_CHECK(status.terminalReason == qsp::TerminalReason::ProtocolFailure);
}

QNP_TEST(sidecar_presence_uses_only_committed_connected_roster_and_scores) {
    namespace v4 = qnetplay::spectator::testv4;
    Fixture fixture;
    fixture.start();

    qsp::ConnectedSetDescriptorV4 descriptor = v4::descriptor();
    descriptor.sourceIdentity = fixture.identity;
    descriptor.simulationContract = kContract;
    descriptor.compatibilityIdentity = makeCompatibilityIdentity();
    qsp::ConnectedChainBuilderV4 builder;
    QNP_REQUIRE(builder.reset(descriptor));

    PublicationEvent descriptorEvent{};
    descriptorEvent.kind = PublicationKind::ConnectedDescriptorV4;
    descriptorEvent.sequence = fixture.publicationSequence++;
    descriptorEvent.connectedDescriptor = descriptor;
    fixture.sidecar->publish(descriptorEvent, fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().connectedV4Bound);

    std::vector<qsp::ConnectedPublicationV4> publications;
    QNP_REQUIRE(v4::appendCompletedGame(builder, 1u, publications));
    QNP_REQUIRE(publications.size() == 3u);

    PublicationEvent event{};
    event.kind = PublicationKind::ConnectedPublicationV4;
    event.sequence = fixture.publicationSequence++;
    event.connectedPublication = publications[0];
    fixture.sidecar->publish(event, fixture.nowMs);
    SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.presenceHasCurrentGame);
    QNP_CHECK(!status.presenceCurrentGameEnded);
    QNP_CHECK(status.presenceActiveFighterMask == 0x03u);
    QNP_CHECK(status.presenceCharacterIds[0] == 0u);
    QNP_CHECK(status.presenceCharacterIds[1] == 8u);
    QNP_CHECK(status.presencePaletteIndices[0] == 1u);
    QNP_CHECK(status.presencePaletteIndices[1] == 2u);
    QNP_CHECK(status.presencePlayerNames[0] == "Viewer P1");
    QNP_CHECK(status.presencePlayerNames[1] == "Viewer P2");
    QNP_CHECK(status.presenceRoundWins[0] == 0u);
    QNP_CHECK(status.presenceMatchWins[0] == 0u);

    event.sequence = fixture.publicationSequence++;
    event.connectedPublication = publications[2];
    PublicationEvent middle = event;
    middle.sequence = event.sequence;
    middle.connectedPublication = publications[1];
    fixture.sidecar->publish(middle, fixture.nowMs);
    event.sequence = fixture.publicationSequence++;
    fixture.sidecar->publish(event, fixture.nowMs);
    status = fixture.sidecar->status();
    QNP_CHECK(status.presenceCurrentGameEnded);
    QNP_CHECK(status.presenceRoundWins[0] == 2u);
    QNP_CHECK(status.presenceRoundWins[1] == 0u);
    QNP_CHECK(status.presenceMatchWins[0] == 1u);
    QNP_CHECK(status.presenceMatchWins[1] == 0u);
    QNP_CHECK(status.connectedCompleteGames == 1u);

    PublicationEvent duplicate{};
    duplicate.kind = PublicationKind::ConnectedPublicationV4;
    duplicate.connectedPublication = publications[2];
    fixture.sidecar->publish(duplicate, fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().presenceMatchWins[0] == 1u);

    qsp::ConnectedPublicationV4 returned{};
    returned.kind = qsp::ConnectedPublicationKindV4::SetEvent;
    returned.setEvent.event.kind =
        qsp::SetEventKind::ReturnedToCharacterSelect;
    returned.setEvent.event.matchId = 1u;
    QNP_REQUIRE(builder.stamp(returned));
    PublicationEvent returnedEvent{};
    returnedEvent.kind = PublicationKind::ConnectedPublicationV4;
    returnedEvent.sequence = fixture.publicationSequence++;
    returnedEvent.connectedPublication = returned;
    fixture.sidecar->publish(returnedEvent, fixture.nowMs);
    status = fixture.sidecar->status();
    QNP_CHECK(!status.presenceHasCurrentGame);
    QNP_CHECK(!status.presenceCurrentGameEnded);
    QNP_CHECK(status.presenceActiveFighterMask == 0u);
    QNP_CHECK(status.presenceCharacterIds[0] == 0xFFu);
    QNP_CHECK(status.presenceCharacterIds[1] == 0xFFu);
    QNP_CHECK(status.presenceRoundWins[0] == 0u);
    QNP_CHECK(status.presenceRoundWins[1] == 0u);
    QNP_CHECK(status.presenceMatchWins[0] == 1u);
    QNP_CHECK(status.presenceMatchWins[1] == 0u);
}

QNP_TEST(publication_fault_carries_the_exact_accepted_prefix_and_is_never_clean) {
    std::vector<uint8_t> fault;
    putU16(fault, 1u);
    putU16(fault, 0u);
    putU32(fault, 6u);
    putU64(fault, prefixDigestFor(6));
    putU64(fault, stateHashFor(6));

    std::vector<PublicationEvent> events;
    const std::vector<uint8_t> frame = frameOf(
        PublicationWireKind::PublicationFault, 6, 1u, kGameId, fault);
    QNP_REQUIRE(decodePublicationFrame(
        frame.data(), frame.size(), events));
    QNP_REQUIRE(events.size() == 1u);
    QNP_CHECK(events[0].kind == PublicationKind::LossReport);
    QNP_CHECK(events[0].publicationFaultReason == 1u);
    QNP_CHECK(events[0].terminalRecordCursor == 6u);
    QNP_CHECK(events[0].terminalPrefixDigest == prefixDigestFor(6));
    QNP_CHECK(events[0].hasTerminalPrefixProof);
    QNP_CHECK(events[0].hasPostTailProof);

    Fixture fixture;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(6);
    events[0].sequence = fixture.publicationSequence++;
    fixture.sidecar->publish(events[0], fixture.nowMs);
    const SidecarStatus status = fixture.sidecar->status();
    QNP_CHECK(status.sealed);
    QNP_CHECK(status.sealedIsTruncation);
    QNP_CHECK(status.confirmedRecords == 6u);
    QNP_CHECK(status.postTailStateHash == stateHashFor(6));
    QNP_CHECK(status.terminalReason
              == qsp::TerminalReason::ProtocolFailure);
}

QNP_TEST(publication_frame_rejects_every_corruption_without_partial_apply) {
    std::vector<uint8_t> batch;
    putU32(batch, 0);
    putU32(batch, 0);
    putU16(batch, 1);
    putU16(batch, 2);
    putU32(batch, 0);
    putU64(batch, stateHashFor(0));
    putU64(batch, 7);
    const std::vector<uint8_t> good =
        frameOf(PublicationWireKind::Confirmed, 1, 1, kGameId, batch);

    std::vector<PublicationEvent> events;
    QNP_REQUIRE(decodePublicationFrame(good.data(), good.size(), events));
    QNP_CHECK(events.size() == 1);

    struct Case {
        const char* name;
        size_t offset;
        uint8_t xorMask;
    };
    const Case cases[] = {
        {"magic", 0, 0x01},        {"version", 4, 0x01},
        {"kind", 6, 0x01},         {"param0", 12, 0x01},
        {"payloadBytes", 16, 0x01}, {"payloadCrc", 20, 0x01},
        {"gameId", 24, 0x01},      {"headerCrc", 32, 0x01},
        {"payload", 36, 0x01},
    };
    for (const Case& item : cases) {
        std::vector<uint8_t> corrupt = good;
        corrupt[item.offset] ^= item.xorMask;
        std::vector<PublicationEvent> out;
        QNP_CHECK(!decodePublicationFrame(corrupt.data(), corrupt.size(), out));
        QNP_CHECK(out.empty());
    }

    std::vector<PublicationEvent> out;
    QNP_CHECK(!decodePublicationFrame(good.data(), good.size() - 1, out));
    QNP_CHECK(!decodePublicationFrame(good.data(), kPublicationHeaderSize - 1,
                                      out));
    QNP_CHECK(!decodePublicationFrame(nullptr, 100, out));
    QNP_CHECK(out.empty());

    std::vector<uint8_t> reservedSet = batch;
    reservedSet[12] = 1;
    const std::vector<uint8_t> bad =
        frameOf(PublicationWireKind::Confirmed, 1, 1, kGameId, reservedSet);
    QNP_CHECK(!decodePublicationFrame(bad.data(), bad.size(), out));
    QNP_CHECK(out.empty());

    const std::vector<uint8_t> miscounted =
        frameOf(PublicationWireKind::Confirmed, 1, 2, kGameId, batch);
    QNP_CHECK(!decodePublicationFrame(miscounted.data(), miscounted.size(),
                                      out));
    QNP_CHECK(out.empty());
}

QNP_TEST(publication_hello_carries_no_event_but_validates_the_schema) {
    std::vector<uint8_t> hello;
    putU64(hello, kGameId);
    putU32(hello, kPublicationVersion);
    putU32(hello, 0);
    std::vector<PublicationEvent> events;
    const std::vector<uint8_t> frame =
        frameOf(PublicationWireKind::Hello, 1, 0, kGameId, hello);
    QNP_CHECK(decodePublicationFrame(frame.data(), frame.size(), events));
    QNP_CHECK(events.empty());

    std::vector<uint8_t> wrongSchema;
    putU64(wrongSchema, kGameId);
    putU32(wrongSchema, kPublicationVersion + 1);
    putU32(wrongSchema, 0);
    const std::vector<uint8_t> badFrame =
        frameOf(PublicationWireKind::Hello, 1, 0, kGameId, wrongSchema);
    QNP_CHECK(!decodePublicationFrame(badFrame.data(), badFrame.size(),
                                      events));
    QNP_CHECK(events.empty());
}

QNP_TEST(sidecar_upload_budget_throttles_data_but_never_control) {
    Fixture fixture;
    fixture.config.uploadBudgetBytesPerSecond = 1;
    fixture.config.uploadBurstBytes = 1;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();
    fixture.publishRecords(200);

    const RouteEndpoint endpoint = peer(44, 50604);
    const std::vector<uint8_t> hello =
        encodeHello(fixture.identity, 0x9190000ull, 1, 1);
    fixture.sidecar->onDatagram(endpoint, hello.data(), hello.size(),
                                fixture.nowMs);
    qsp::RejectReason reason = qsp::RejectReason::InvalidRequest;
    QNP_CHECK(sawReject(*fixture.transport, endpoint, fixture.identity, reason));
    QNP_CHECK(fixture.sidecar->status().egressPackets >= 1);
}

QNP_TEST(sidecar_reject_echoes_the_game_the_viewer_actually_asked_for) {
    Fixture fixture;
    fixture.start(AdmissionPolicy::CodeOnly);
    fixture.publishStart();

    constexpr uint64_t kSpecificGame = 1ull;
    constexpr uint64_t viewerId = 0x5350454349464943ull;

    qsp::HelloPayload hello{};
    hello.simulationContract = kContract;
    hello.compatibilityIdentity = makeCompatibilityIdentity();
    hello.requestedGameId = kSpecificGame;
    hello.viewerId = viewerId;
    hello.generation = 1;
    hello.viewerNonce = qsp::viewerGenerationNonce(viewerId, 1);
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    QNP_REQUIRE(qsp::encodePayload(hello, payload));
    QNP_REQUIRE(qsp::encodeDatagram(qsp::MessageType::Hello, fixture.identity,
                                    1, payload, datagram));

    const RouteEndpoint endpoint = peer(61, 50961);
    QNP_CHECK(fixture.sidecar->onDatagram(endpoint, datagram.data(),
                                          datagram.size(), fixture.nowMs)
              == IngressOutcome::AdmissionDenied);

    auto& queue = fixture.transport->outbound[FakeTransport::key(endpoint)];
    bool sawIt = false;
    for (const FakeTransport::Packet& packet : queue) {
        qsp::Datagram decoded{};
        if (qsp::decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                fixture.identity, decoded)
            != qsp::DecodeError::None) {
            continue;
        }
        if (decoded.header.type != qsp::MessageType::Reject) continue;
        qsp::RejectPayload reject{};
        QNP_REQUIRE(qsp::decodePayload(decoded.payload, reject));
        QNP_CHECK(reject.requestedGameId == kSpecificGame);
        QNP_CHECK(reject.viewerId == viewerId);
        QNP_CHECK(reject.viewerNonce
                  == qsp::viewerGenerationNonce(viewerId, 1));
        sawIt = true;
    }
    QNP_CHECK(sawIt);
}

QNP_TEST(sidecar_accepted_hello_retransmit_restarts_the_handshake_deadline) {
    Fixture fixture;
    fixture.config.handshakeDeadlineMs = 2000;
    fixture.config.noProgressDeadlineMs = 60000;
    fixture.start();
    fixture.publishStart();
    fixture.publishRecords(5);

    const RouteEndpoint endpoint = peer(47, 50607);
    const uint64_t viewerId = 0x6170000ull;
    const std::vector<uint8_t> first =
        encodeHello(fixture.identity, viewerId, 1, 1);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    endpoint, first.data(), first.size(), fixture.nowMs)
                == IngressOutcome::Accepted);
    QNP_REQUIRE(fixture.sidecar->status().viewerCount == 1);

    fixture.advance(1500);
    fixture.sidecar->service(fixture.nowMs);
    const std::vector<uint8_t> again =
        encodeHello(fixture.identity, viewerId, 1, 2);
    QNP_REQUIRE(fixture.sidecar->onDatagram(
                    endpoint, again.data(), again.size(), fixture.nowMs)
                == IngressOutcome::Accepted);

    fixture.advance(1000);
    fixture.sidecar->service(fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 1);
    QNP_CHECK(fixture.sidecar->status().viewersDeadlineExpired == 0);

    fixture.advance(1500);
    fixture.sidecar->service(fixture.nowMs);
    QNP_CHECK(fixture.sidecar->status().viewerCount == 0);
    QNP_CHECK(fixture.sidecar->status().viewersDeadlineExpired == 1);
}
