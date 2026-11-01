#include "../SpectatorArchive.h"
#include "../SpectatorGrant.h"
#include "../SpectatorSession.h"
#include "../TopologyGrant.h"
#include "../TopologyRelayAdapter.h"
#include "../ResilientSpectatorProtocol.h"
#include "../../game/SpectatorPlaybackPolicy.h"
#include "../../net/RelayProtocol.h"
#include "../../netplay/WireProtocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

namespace {

using namespace qnetplay::spectator;

int g_failures = 0;

#define CHECK_TRUE(expr)                                                     \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::fprintf(stderr, "%s:%d CHECK failed: %s\n",                \
                         __FILE__, __LINE__, #expr);                          \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

#define CHECK_EQ(left, right)                                                \
    do {                                                                     \
        const auto checkLeft = (left);                                       \
        const auto checkRight = (right);                                     \
        if (!(checkLeft == checkRight)) {                                    \
            std::fprintf(stderr, "%s:%d CHECK_EQ failed: %s != %s\n",       \
                         __FILE__, __LINE__, #left, #right);                  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

constexpr uint64_t kSimulationContract = 0x514F48393953494Dull;
constexpr uint64_t kGameId = 0x5000u;
constexpr uint32_t kFirstFrame = 100u;
constexpr uint32_t kBacklogRecords = 8000u;

SessionIdentity spectatorIdentity() {
    return SessionIdentity{
        0x1020304050607080ull,
        7,
        SessionCookie{
            0x1122334455667788ull,
            0x8877665544332211ull,
        },
    };
}

qnetplay::compat::CompatibilityIdentityWire productionCompatibilityIdentity() {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = qnetplay::wire::kVersion;
    CHECK_TRUE(qnetplay::compat::setProfileId(identity, "qoh99-se1126"));
    for (size_t index = 0;
         index < qnetplay::compat::kCompatibilitySha256Bytes;
         ++index) {
        identity.runtimeBuild.executableSha256[index] =
            static_cast<uint8_t>(0x31u + index);
        identity.simulationContent.simulationRootSha256[index] =
            static_cast<uint8_t>(0xA5u + index);
        identity.simulationContent.validationRootSha256[index] =
            static_cast<uint8_t>(0x61u + index);
    }
    identity.runtimeBuild.executableSize = 0x123450u;
    identity.runtimeBuild.peTimestamp = 0x4A514F48u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    identity.simulationContent.entryCount = 24u;
    identity.simulationContent.requiredEntryCount = 21u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.stateSchemaId = 0x535441544531u;
    identity.modSimulationId = 0x4D4F44494431u;
    identity.timingProfileId = 0x54494D494E4731u;
    CHECK_TRUE(qnetplay::compat::isValidCompatibilityIdentity(identity));
    return identity;
}

ConfirmedRecord recordFor(uint32_t index) {
    ConfirmedRecord record{};
    record.record = index;
    record.timelineFrame = kFirstFrame + index;
    record.playerOneInput = static_cast<uint16_t>(
        (index & 0x0Fu) | ((index & 0xF0u) << 4));
    record.playerTwoInput = static_cast<uint16_t>(
        ((index * 3u + 1u) & 0x0Fu)
        | (((index * 3u + 1u) & 0xF0u) << 4));
    record.stateHash =
        0xD15EA5E000000000ull ^ (static_cast<uint64_t>(index) * 17u);
    return record;
}

GameDescriptor productionDescriptor() {
    GameDescriptor game{};
    game.gameId = kGameId;
    game.simulationContract = kSimulationContract;
    game.compatibilityIdentity = productionCompatibilityIdentity();
    game.initialStateHash = 0xA500000000000000ull ^ kGameId;
    game.firstTimelineFrame = kFirstFrame;
    game.playerOneDelay = 2;
    game.playerTwoDelay = 4;
    game.rollbackWindow = 12;
    game.metadata = {0x51, 0x4F, 0x48, 0x39, 0x39};
    return game;
}

struct WirePeer {
    ViewerId viewerId = 0;
    EndpointId endpoint = 0;
    uint32_t generation = 1;
    uint32_t sequence = 1;

    bool started = false;
    bool complete = false;
    uint64_t startFingerprint = 0;
    uint64_t initialDigest = 0;
    uint32_t nextRecord = 0;
    uint64_t digest = 0;
    uint32_t nextEvent = 0;

    uint64_t dataBytes = 0;
    uint32_t dataPackets = 0;
    uint64_t lastServicePoll = 0;
    uint64_t maxServiceGapPolls = 0;
    uint64_t helloPoll = 0;
    uint64_t gameStartPoll = 0;
    uint64_t terminalPoll = 0;

    std::vector<std::vector<uint8_t>> outbound;

    uint64_t nonce() const {
        return viewerGenerationNonce(viewerId, generation);
    }

    void emit(MessageType type, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> packet;
        if (!encodeDatagram(type, spectatorIdentity(), sequence,
                            payload, packet)) {
            ++g_failures;
            return;
        }
        ++sequence;
        outbound.push_back(std::move(packet));
    }

    void sendHello(uint64_t gameId) {
        HelloPayload hello{};
        hello.simulationContract = kSimulationContract;
        hello.compatibilityIdentity = productionCompatibilityIdentity();
        hello.requestedGameId = gameId;
        hello.viewerId = viewerId;
        hello.generation = generation;
        hello.viewerNonce = nonce();
        std::vector<uint8_t> payload;
        CHECK_TRUE(encodePayload(hello, payload));
        emit(MessageType::Hello, payload);
    }

    void sendAck(uint64_t gameId) {
        AckPayload ack{};
        ack.gameId = gameId;
        ack.viewerId = viewerId;
        ack.generation = generation;
        ack.nextRecord = nextRecord;
        ack.nextEvent = nextEvent;
        ack.digest = nextRecord == 0 ? initialDigest : digest;
        std::vector<uint8_t> payload;
        CHECK_TRUE(encodePayload(ack, payload));
        emit(MessageType::Ack, payload);
    }

    void receive(const SourceDatagram& packet, uint64_t poll) {
        Datagram datagram{};
        if (decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                           spectatorIdentity(), datagram)
            != DecodeError::None) {
            ++g_failures;
            return;
        }
        switch (datagram.header.type) {
            case MessageType::GameStart: {
                GameStartPayload start{};
                CHECK_TRUE(decodePayload(datagram.payload, start));
                if (!started) {
                    gameStartPoll = poll;
                    startFingerprint = gameStartFingerprint(start);
                    initialDigest = start.initialDigest;
                    digest = start.resumeDigest;
                    started = true;
                }
                InitAckPayload ack{};
                ack.gameId = start.gameId;
                ack.viewerId = viewerId;
                ack.generation = generation;
                ack.viewerNonce = nonce();
                ack.startFingerprint = startFingerprint;
                ack.resumeRecord = start.resumeRecord;
                ack.resumeEvent = start.resumeEvent;
                ack.resumeDigest = start.resumeDigest;
                std::vector<uint8_t> payload;
                CHECK_TRUE(encodePayload(ack, payload));
                emit(MessageType::InitAck, payload);
                sendAck(start.gameId);
                break;
            }
            case MessageType::ConfirmedInputBatch: {
                ConfirmedInputBatchPayload batch{};
                CHECK_TRUE(decodePayload(datagram.payload, batch));
                dataBytes += packet.bytes.size();
                ++dataPackets;
                if (lastServicePoll != 0) {
                    maxServiceGapPolls = (std::max)(
                        maxServiceGapPolls, poll - lastServicePoll);
                }
                lastServicePoll = poll;
                if (batch.startRecord == nextRecord) {
                    nextRecord = batch.records.back().record + 1u;
                    digest = batch.endDigest;
                }
                sendAck(batch.gameId);
                break;
            }
            case MessageType::Terminal: {
                TerminalPayload terminal{};
                CHECK_TRUE(decodePayload(datagram.payload, terminal));
                if (terminalPoll == 0) terminalPoll = poll;
                if (terminal.nextRecord != nextRecord
                    || terminal.nextEvent != nextEvent) {
                    break;
                }
                TerminalAckPayload ack{};
                ack.gameId = terminal.gameId;
                ack.viewerId = viewerId;
                ack.generation = generation;
                ack.finalDigest = terminal.finalDigest;
                ack.postTailStateHash = terminal.postTailStateHash;
                ack.nextRecord = terminal.nextRecord;
                ack.nextEvent = terminal.nextEvent;
                ack.reason = terminal.reason;
                std::vector<uint8_t> payload;
                CHECK_TRUE(encodePayload(ack, payload));
                emit(MessageType::TerminalAck, payload);
                complete = true;
                break;
            }
            case MessageType::Heartbeat:
            case MessageType::Reject:
            case MessageType::SetEvent:
            case MessageType::CheckpointOffer:
            case MessageType::CheckpointChunk:
                break;
            default:
                ++g_failures;
                break;
        }
    }
};

SourceConfig productionSourceConfig() {
    SourceConfig config{};
    config.identity = spectatorIdentity();
    config.simulationContract = kSimulationContract;
    config.compatibilityIdentity = productionCompatibilityIdentity();
    config.maxViewers = 32;
    config.recordsPerBatch = 24;
    config.sendWindowRecords = 96;
    config.perViewerPacketBudget = 1;
    config.globalPacketBudget = 8;
    return config;
}

enum class PlaybackNetworkProfile {
    Stable300MsRtt,
    Jittery200MsRtt,
};

struct ScheduledPlaybackDatagram {
    bool toSource = false;
    std::vector<uint8_t> bytes;
};

struct PlaybackRegressionStats {
    bool primed = false;
    bool droppedDataBatch = false;
    uint32_t droppedStartRecord = 0;
    uint64_t primedAtUs = 0;
    uint32_t primeTransitions = 0;
    uint32_t underruns = 0;
    uint32_t catchupEpisodes = 0;
    uint32_t playedRecords = 0;
    size_t initialPrimeBuffer = 0u;
    size_t minimumPostPrimeBuffer =
        (std::numeric_limits<size_t>::max)();
};

uint64_t playbackLinkDelayUs(PlaybackNetworkProfile profile,
                             bool toSource,
                             uint64_t ordinal) {
    if (profile == PlaybackNetworkProfile::Stable300MsRtt)
        return 150000u;

    constexpr int kJitterMs[] = {
        -65, 35, -20, 60, 5, -45, 45, -10,
        20, -55, 65, -30, 10, 50, -15, -40,
    };
    constexpr size_t kJitterCount =
        sizeof(kJitterMs) / sizeof(kJitterMs[0]);
    const size_t phase = static_cast<size_t>(
        (ordinal + (toSource ? 5u : 0u)) % kJitterCount);
    int64_t delayMs = 100 + kJitterMs[phase];
    if ((ordinal + (toSource ? 11u : 0u)) % 29u == 0u)
        delayMs += 120;
    return static_cast<uint64_t>(delayMs) * 1000u;
}

bool acceptableSourceIngress(IngressResult result) {
    return result == IngressResult::Accepted
        || result == IngressResult::StaleAck
        || result == IngressResult::UnexpectedMessage
        || result == IngressResult::ReplayedSequence;
}

bool acceptableViewerIngress(IngressResult result) {
    return result == IngressResult::Accepted
        || result == IngressResult::UnexpectedMessage
        || result == IngressResult::ReplayedSequence;
}

PlaybackRegressionStats runLivePlaybackRegression(
    PlaybackNetworkProfile profile) {
    constexpr EndpointId kEndpoint = 0x0A0000F1ull;
    constexpr uint64_t kServiceStepUs = 5000u;
    constexpr uint64_t kNativePeriodUs = 16500u;
    constexpr uint64_t kRunDurationUs = 36000000u;
    constexpr uint64_t kDropAfterPrimeUs = 2000000u;

    SpectatorArchive archive;
    CHECK_EQ(archive.beginGame(productionDescriptor()), ArchiveResult::Ok);

    SourceConfig sourceConfig = productionSourceConfig();
    CHECK_EQ(sourceConfig.maxViewers, size_t{32});
    CHECK_EQ(sourceConfig.recordsPerBatch, size_t{24});
    CHECK_EQ(sourceConfig.sendWindowRecords, 96u);
    CHECK_EQ(sourceConfig.perViewerPacketBudget, size_t{1});
    CHECK_EQ(sourceConfig.globalPacketBudget, size_t{8});
    CHECK_EQ(sourceConfig.retransmitMs, 350u);
    SpectatorSource source(sourceConfig, archive);

    ViewerConfig viewerConfig{};
    viewerConfig.identity = spectatorIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = productionCompatibilityIdentity();
    viewerConfig.viewerNonce = 0x7A11000000000001ull;
    viewerConfig.secureRandom = viewerConfig.viewerNonce;
    SpectatorViewer viewer(viewerConfig);
    CHECK_TRUE(source.isConfigured());
    CHECK_TRUE(viewer.isConfigured());
    CHECK_TRUE(viewer.begin(kGameId));

    using NetworkKey = std::pair<uint64_t, uint64_t>;
    std::map<NetworkKey, ScheduledPlaybackDatagram> inFlight;
    uint64_t networkOrder = 0u;
    uint64_t viewerLinkOrdinal = 0u;
    uint64_t sourceLinkOrdinal = 0u;
    uint64_t nextPublishUs = 0u;
    uint64_t nextPresentationUs = 0u;
    uint32_t publishedRecords = 0u;
    uint32_t elasticCredit = 0u;
    bool presentationPrimed = false;
    bool everPrimed = false;
    bool catchingUp = false;
    PlaybackRegressionStats stats{};

    for (uint64_t nowUs = 0u; nowUs <= kRunDurationUs;
         nowUs += kServiceStepUs) {
        const uint64_t nowMs = nowUs / 1000u;

        while (nextPublishUs <= nowUs) {
            CHECK_EQ(archive.appendConfirmed(
                         kGameId, recordFor(publishedRecords), nullptr),
                     ArchiveResult::Ok);
            ++publishedRecords;
            nextPublishUs += kNativePeriodUs;
        }

        while (!inFlight.empty()
               && inFlight.begin()->first.first <= nowUs) {
            ScheduledPlaybackDatagram packet =
                std::move(inFlight.begin()->second);
            inFlight.erase(inFlight.begin());
            if (packet.toSource) {
                CHECK_TRUE(acceptableSourceIngress(source.onDatagram(
                    kEndpoint, packet.bytes.data(), packet.bytes.size(),
                    nowMs)));
            } else {
                CHECK_TRUE(acceptableViewerIngress(viewer.onDatagram(
                    packet.bytes.data(), packet.bytes.size(), nowMs)));
            }
        }

        for (const ViewerDatagram& packet :
             viewer.pollOutbound(nowMs, 4u)) {
            const uint64_t ordinal = viewerLinkOrdinal++;
            const uint64_t dueUs = nowUs + playbackLinkDelayUs(
                profile, true, ordinal);
            inFlight.emplace(
                NetworkKey{dueUs, networkOrder++},
                ScheduledPlaybackDatagram{true, packet.bytes});
        }

        for (const SourceDatagram& packet : source.pollOutbound(nowMs)) {
            if (packet.destination != kEndpoint) continue;

            if (!stats.droppedDataBatch
                && presentationPrimed
                && nowUs >= stats.primedAtUs + kDropAfterPrimeUs
                && packet.type == MessageType::ConfirmedInputBatch) {
                Datagram datagram{};
                ConfirmedInputBatchPayload batch{};
                CHECK_EQ(decodeDatagram(packet.bytes.data(),
                                        packet.bytes.size(),
                                        spectatorIdentity(), datagram),
                         DecodeError::None);
                CHECK_TRUE(decodePayload(datagram.payload, batch));
                CHECK_TRUE(!batch.records.empty());
                if (!batch.records.empty())
                    stats.droppedStartRecord = batch.startRecord;
                stats.droppedDataBatch = true;
                ++sourceLinkOrdinal;
                continue;
            }

            const uint64_t ordinal = sourceLinkOrdinal++;
            const uint64_t dueUs = nowUs + playbackLinkDelayUs(
                profile, false, ordinal);
            inFlight.emplace(
                NetworkKey{dueUs, networkOrder++},
                ScheduledPlaybackDatagram{false, packet.bytes});
        }

        while (nextPresentationUs <= nowUs) {
            size_t buffered = viewer.bufferedPlaybackRecords();
            if (!presentationPrimed
                && qgame::spectatorPlaybackPrimed(
                    buffered, false, false, everPrimed)) {
                presentationPrimed = true;
                everPrimed = true;
                stats.primed = true;
                stats.primedAtUs = nowUs;
                ++stats.primeTransitions;
                if (stats.primeTransitions == 1u)
                    stats.initialPrimeBuffer = buffered;
            }

            if (presentationPrimed) {
                buffered = viewer.bufferedPlaybackRecords();
                if (buffered == 0u) {
                    presentationPrimed = false;
                    catchingUp = false;
                    elasticCredit = 0u;
                    ++stats.underruns;
                } else {
                    const bool wasCatchingUp = catchingUp;
                    const qgame::SpectatorCatchupPlan catchupPlan =
                        qgame::spectatorCatchupPlan(
                            buffered, wasCatchingUp);
                    catchingUp = catchupPlan.catchingUp;
                    if (!wasCatchingUp && catchingUp)
                        ++stats.catchupEpisodes;

                    size_t tickBudget = catchupPlan.tickBudget;
                    if (!catchingUp) {
                        const qgame::SpectatorElasticPlan elasticPlan =
                            qgame::spectatorElasticPlan(
                                buffered, elasticCredit);
                        elasticCredit = elasticPlan.nextCredit;
                        tickBudget = elasticPlan.advance ? 1u : 0u;
                    } else {
                        elasticCredit = 0u;
                    }

                    for (size_t tick = 0u; tick < tickBudget; ++tick) {
                        ConfirmedRecord record{};
                        const bool hasRecord =
                            viewer.peekPlaybackRecord(record);
                        CHECK_TRUE(hasRecord);
                        if (!hasRecord) break;
                        CHECK_EQ(viewer.commitPlaybackRecord(
                                     record.record, record.stateHash,
                                     record.stateHash
                                         ^ 0xA55AA55AA55AA55Aull),
                                 IngressResult::Accepted);
                        ++stats.playedRecords;
                    }
                    stats.minimumPostPrimeBuffer = (std::min)(
                        stats.minimumPostPrimeBuffer,
                        viewer.bufferedPlaybackRecords());
                }
            }
            nextPresentationUs += kNativePeriodUs;
        }
    }

    SourceViewerSnapshot snapshot{};
    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), snapshot));
    CHECK_EQ(snapshot.phase, SourceViewerPhase::Streaming);
    CHECK_TRUE(snapshot.recordRepairSends >= 1u);
    CHECK_TRUE(viewer.receivedNextRecord() > stats.droppedStartRecord);
    CHECK_TRUE(viewer.playbackNextRecord() > stats.droppedStartRecord);
    CHECK_EQ(viewer.phase(), ViewerPhase::Streaming);
    CHECK_TRUE(stats.primed);
    CHECK_EQ(qgame::kSpectatorStartupBufferRecords, size_t{120});
    CHECK_TRUE(stats.initialPrimeBuffer
               >= qgame::kSpectatorStartupBufferRecords);
    CHECK_EQ(stats.primeTransitions, 1u);
    CHECK_EQ(stats.underruns, 0u);
    CHECK_TRUE(stats.droppedDataBatch);
    CHECK_TRUE(stats.playedRecords >= 1200u);
    CHECK_TRUE(stats.minimumPostPrimeBuffer > 0u);
    CHECK_TRUE(stats.catchupEpisodes <= 1u);
    return stats;
}

void testProductionLivePlaybackSurvivesLatencyJitterAndOneLostBatch() {
    const PlaybackRegressionStats stable = runLivePlaybackRegression(
        PlaybackNetworkProfile::Stable300MsRtt);
    const PlaybackRegressionStats jittery = runLivePlaybackRegression(
        PlaybackNetworkProfile::Jittery200MsRtt);
    CHECK_EQ(stable.primeTransitions, 1u);
    CHECK_EQ(jittery.primeTransitions, 1u);
    CHECK_EQ(stable.underruns, 0u);
    CHECK_EQ(jittery.underruns, 0u);
}

void runBackloggedFanout(size_t packetOverride,
                         unsigned polls,
                         std::vector<WirePeer>& peers,
                         SpectatorArchive& archive,
                         SpectatorSource& source,
                         uint64_t& nowMs,
                         uint64_t& poll) {
    std::map<EndpointId, WirePeer*> byEndpoint;
    for (WirePeer& peer : peers) byEndpoint[peer.endpoint] = &peer;

    for (unsigned step = 0; step < polls; ++step) {
        ++poll;
        nowMs += 16;
        for (WirePeer& peer : peers) {
            for (const std::vector<uint8_t>& packet : peer.outbound) {
                source.onDatagram(peer.endpoint, packet.data(),
                                  packet.size(), nowMs);
            }
            peer.outbound.clear();
        }
        for (const SourceDatagram& packet :
             source.pollOutbound(nowMs, packetOverride)) {
            const auto found = byEndpoint.find(packet.destination);
            if (found == byEndpoint.end()) continue;
            found->second->receive(packet, poll);
        }
        (void)archive;
    }
}

void testBackloggedFanoutIsFairAtProductionBudgets() {
    const size_t overrides[] = {1u, 2u, 8u};
    for (const size_t packetOverride : overrides) {
        SpectatorArchive archive;
        CHECK_EQ(archive.beginGame(productionDescriptor()),
                 ArchiveResult::Ok);
        for (uint32_t index = 0; index < kBacklogRecords; ++index) {
            CHECK_EQ(archive.appendConfirmed(
                         kGameId, recordFor(index), nullptr),
                     ArchiveResult::Ok);
        }
        SourceConfig config = productionSourceConfig();
        SpectatorSource source(config, archive);
        CHECK_TRUE(source.isConfigured());

        std::vector<WirePeer> peers(32);
        for (size_t index = 0; index < peers.size(); ++index) {
            peers[index].viewerId =
                0x9000000000000000ull + static_cast<uint64_t>(index) + 1u;
            peers[index].endpoint =
                0x0A000000ull + static_cast<uint64_t>(index) + 1u;
            peers[index].sendHello(kGameId);
        }

        uint64_t nowMs = 1;
        uint64_t poll = 0;
        runBackloggedFanout(packetOverride, 400u, peers, archive, source,
                            nowMs, poll);
        for (const WirePeer& peer : peers) {
            CHECK_TRUE(peer.started);
            CHECK_TRUE(peer.gameStartPoll != 0);
            CHECK_TRUE(peer.gameStartPoll <= 400u);
        }
        CHECK_EQ(source.viewerCount(), size_t{32});

        for (WirePeer& peer : peers) {
            peer.dataBytes = 0;
            peer.dataPackets = 0;
            peer.lastServicePoll = 0;
            peer.maxServiceGapPolls = 0;
        }
        const uint64_t measureStart = poll;
        const unsigned measurePolls = 800u;
        runBackloggedFanout(packetOverride, measurePolls, peers, archive,
                            source, nowMs, poll);

        uint64_t minBytes = ~uint64_t{0};
        uint64_t maxBytes = 0;
        uint64_t maxGap = 0;
        uint32_t minPackets = 0xFFFFFFFFu;
        uint32_t maxPackets = 0;
        for (const WirePeer& peer : peers) {
            minBytes = (std::min)(minBytes, peer.dataBytes);
            maxBytes = (std::max)(maxBytes, peer.dataBytes);
            minPackets = (std::min)(minPackets, peer.dataPackets);
            maxPackets = (std::max)(maxPackets, peer.dataPackets);
            maxGap = (std::max)(maxGap, peer.maxServiceGapPolls);
        }
        CHECK_TRUE(minPackets > 0u);
        CHECK_TRUE(maxPackets - minPackets <= 1u);
        CHECK_TRUE(maxBytes - minBytes
                   <= (maxBytes / (std::max)(minPackets, 1u)) + 1u);
        const uint64_t expectedCycle =
            (32u + packetOverride - 1u) / packetOverride;
        CHECK_TRUE(maxGap <= expectedCycle + 2u);
        (void)measureStart;
        (void)measurePolls;
    }
}

void testThirtyThirdViewerIsRejectedPromptlyUnderFullBacklog() {
    SpectatorArchive archive;
    CHECK_EQ(archive.beginGame(productionDescriptor()), ArchiveResult::Ok);
    for (uint32_t index = 0; index < kBacklogRecords; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     kGameId, recordFor(index), nullptr),
                 ArchiveResult::Ok);
    }
    SourceConfig config = productionSourceConfig();
    SpectatorSource source(config, archive);

    std::vector<WirePeer> peers(32);
    for (size_t index = 0; index < peers.size(); ++index) {
        peers[index].viewerId =
            0x9100000000000000ull + static_cast<uint64_t>(index) + 1u;
        peers[index].endpoint =
            0x0B000000ull + static_cast<uint64_t>(index) + 1u;
        peers[index].sendHello(kGameId);
    }
    uint64_t nowMs = 1;
    uint64_t poll = 0;
    runBackloggedFanout(8u, 400u, peers, archive, source, nowMs, poll);
    CHECK_EQ(source.viewerCount(), size_t{32});

    WirePeer overflow;
    overflow.viewerId = 0x91FFFFFFFFFFFFFFull;
    overflow.endpoint = 0x0BFFFFFFull;
    overflow.sendHello(kGameId);
    nowMs += 16;
    IngressResult admission = IngressResult::Accepted;
    for (const std::vector<uint8_t>& packet : overflow.outbound) {
        admission = source.onDatagram(
            overflow.endpoint, packet.data(), packet.size(), nowMs);
    }
    overflow.outbound.clear();
    CHECK_EQ(admission, IngressResult::CapacityReached);
    CHECK_EQ(source.viewerCount(), size_t{32});

    bool rejected = false;
    unsigned pollsUntilReject = 0;
    for (unsigned step = 0; step < 8u && !rejected; ++step) {
        ++pollsUntilReject;
        nowMs += 16;
        for (const SourceDatagram& packet : source.pollOutbound(nowMs, 8u)) {
            if (packet.destination != overflow.endpoint) continue;
            Datagram datagram{};
            CHECK_EQ(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                    spectatorIdentity(), datagram),
                     DecodeError::None);
            CHECK_EQ(datagram.header.type, MessageType::Reject);
            RejectPayload reject{};
            CHECK_TRUE(decodePayload(datagram.payload, reject));
            CHECK_EQ(reject.reason, RejectReason::CapacityReached);
            CHECK_EQ(reject.viewerId, overflow.viewerId);
            rejected = true;
        }
    }
    CHECK_TRUE(rejected);
    CHECK_TRUE(pollsUntilReject <= 2u);

    WirePeer& leaving = peers[7];
    DetachPayload detach{};
    detach.viewerId = leaving.viewerId;
    detach.generation = leaving.generation;
    detach.nextRecord = leaving.nextRecord;
    detach.nextEvent = leaving.nextEvent;
    uint64_t proof = 0;
    CHECK_TRUE(archive.chainDigestAt(
        kGameId, leaving.nextRecord, leaving.nextEvent, proof));
    detach.digest = proof;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(detach, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Detach, spectatorIdentity(), leaving.sequence + 1u,
        payload, packet));
    nowMs += 16;
    CHECK_EQ(source.onDatagram(
                 leaving.endpoint, packet.data(), packet.size(), nowMs),
             IngressResult::Detached);
    CHECK_EQ(source.viewerCount(), size_t{31});

    overflow.generation = 2u;
    overflow.sequence += 10u;
    overflow.sendHello(kGameId);
    nowMs += 16;
    for (const std::vector<uint8_t>& admissionPacket : overflow.outbound) {
        CHECK_EQ(source.onDatagram(
                     overflow.endpoint, admissionPacket.data(),
                     admissionPacket.size(), nowMs),
                 IngressResult::Accepted);
    }
    CHECK_EQ(source.viewerCount(), size_t{32});
}

SpectatorGrantKey testGrantKey(uint8_t seed) {
    SpectatorGrantKey key{};
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(seed ^ (i * 37u + 11u));
    }
    return key;
}

void testGrantIssueVerifyAndSingleUse() {
    const SpectatorGrantKey key = testGrantKey(0x5Au);
    const SpectatorGrantKey wrongKey = testGrantKey(0x5Bu);
    constexpr uint64_t viewerId = 0xABCDEF0123456789ull;
    constexpr uint64_t sessionId = 0x1020304050607080ull;

    SpectatorGrant grant{};
    grant.grantId = 0x11u;
    grant.viewerId = viewerId;
    grant.sessionId = sessionId;
    grant.expiryUnix = 1000u;
    grant.sourceGeneration = 3u;
    grant.resumeRecord = 512u;
    grant.resumeDigest = 0xFEEDFACECAFEBEEFull;
    grant.route = 2u;
    CHECK_TRUE(issueGrant(key, grant));
    CHECK_TRUE(grant.macLow != 0 || grant.macHigh != 0);

    SpectatorGrant again = grant;
    again.macLow = 0;
    again.macHigh = 0;
    CHECK_TRUE(issueGrant(key, again));
    CHECK_EQ(again.macLow, grant.macLow);
    CHECK_EQ(again.macHigh, grant.macHigh);

    CHECK_EQ(verifyGrant(key, grant, viewerId, sessionId, 999u),
             GrantVerdict::Ok);
    CHECK_EQ(verifyGrant(wrongKey, grant, viewerId, sessionId, 999u),
             GrantVerdict::BadMac);
    CHECK_EQ(verifyGrant(key, grant, viewerId, sessionId, 1000u),
             GrantVerdict::Expired);
    CHECK_EQ(verifyGrant(key, grant, viewerId + 1u, sessionId, 999u),
             GrantVerdict::WrongViewer);
    CHECK_EQ(verifyGrant(key, grant, viewerId, sessionId + 1u, 999u),
             GrantVerdict::WrongSession);

    const struct Mutation {
        const char* name;
        SpectatorGrant grant;
    } mutations[] = {
        {"grantId", [&] { SpectatorGrant m = grant; m.grantId ^= 1u; return m; }()},
        {"viewerId", [&] { SpectatorGrant m = grant; m.viewerId ^= 1u; return m; }()},
        {"sessionId", [&] { SpectatorGrant m = grant; m.sessionId ^= 1u; return m; }()},
        {"expiry", [&] { SpectatorGrant m = grant; m.expiryUnix ^= 1u; return m; }()},
        {"generation", [&] { SpectatorGrant m = grant; m.sourceGeneration ^= 1u; return m; }()},
        {"resumeRecord", [&] { SpectatorGrant m = grant; m.resumeRecord ^= 1u; return m; }()},
        {"resumeDigest", [&] { SpectatorGrant m = grant; m.resumeDigest ^= 1u; return m; }()},
        {"route", [&] { SpectatorGrant m = grant; m.route ^= 1u; return m; }()},
    };
    for (const Mutation& mutation : mutations) {
        const GrantVerdict verdict = verifyGrant(
            key, mutation.grant, mutation.grant.viewerId,
            mutation.grant.sessionId, 999u);
        if (verdict != GrantVerdict::BadMac) {
            std::fprintf(stderr,
                         "grant mutation %s was not rejected\n",
                         mutation.name);
            ++g_failures;
        }
    }

    SpectatorGrant malformed = grant;
    malformed.expiryUnix = 0;
    CHECK_EQ(verifyGrant(key, malformed, viewerId, sessionId, 999u),
             GrantVerdict::Malformed);

    GrantRegistry registry(key, 1u);
    CHECK_EQ(registry.redeem(grant, viewerId, sessionId, 999u),
             GrantVerdict::Ok);
    CHECK_EQ(registry.reservedCount(), size_t{1});
    CHECK_EQ(registry.redeem(grant, viewerId, sessionId, 999u),
             GrantVerdict::Replayed);

    SpectatorGrant second{};
    second.grantId = 0x12u;
    second.viewerId = viewerId + 5u;
    second.sessionId = sessionId;
    second.expiryUnix = 1000u;
    second.sourceGeneration = 3u;
    CHECK_TRUE(issueGrant(key, second));
    CHECK_EQ(registry.redeem(second, second.viewerId, sessionId, 999u),
             GrantVerdict::CapacityReached);

    CHECK_TRUE(registry.release(grant.grantId));
    CHECK_EQ(registry.reservedCount(), size_t{0});
    CHECK_EQ(registry.redeem(grant, viewerId, sessionId, 999u),
             GrantVerdict::Replayed);
    CHECK_EQ(registry.redeem(second, second.viewerId, sessionId, 999u),
             GrantVerdict::Ok);
}

void testMigrationGrantGatesEndpointChange() {
    static const SpectatorGrantKey key = testGrantKey(0x77u);
    static uint64_t expectedViewerId = 0;
    struct Verifier {
        static bool check(void* context,
                          const MigratePayload& migrate,
                          uint64_t sessionId) {
            (void)context;
            SpectatorGrant grant{};
            grant.grantId = migrate.grantId;
            grant.viewerId = migrate.viewerId;
            grant.sessionId = sessionId;
            grant.expiryUnix = 1000u;
            grant.sourceGeneration = migrate.newGeneration;
            grant.resumeRecord = migrate.resumeRecord;
            grant.resumeDigest = migrate.resumeDigest;
            grant.macLow = migrate.grantMacLow;
            grant.macHigh = migrate.grantMacHigh;
            return verifyGrant(key, grant, expectedViewerId, sessionId,
                               500u) == GrantVerdict::Ok;
        }
    };

    SpectatorArchive archive;
    CHECK_EQ(archive.beginGame(productionDescriptor()), ArchiveResult::Ok);
    for (uint32_t index = 0; index < 48u; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     kGameId, recordFor(index), nullptr),
                 ArchiveResult::Ok);
    }
    SourceConfig config = productionSourceConfig();
    config.perViewerPacketBudget = 4;
    config.globalPacketBudget = 32;
    config.requireMigrationGrant = true;
    config.migrationGrantVerifier = &Verifier::check;
    SpectatorSource source(config, archive);

    WirePeer peer;
    peer.viewerId = 0xC0FFEE0000001234ull;
    peer.endpoint = 0x0C000001ull;
    expectedViewerId = peer.viewerId;
    peer.sendHello(kGameId);

    uint64_t nowMs = 1;
    uint64_t poll = 0;
    std::vector<WirePeer> single;
    single.push_back(peer);
    runBackloggedFanout(32u, 60u, single, archive, source, nowMs, poll);
    CHECK_TRUE(single[0].started);
    CHECK_EQ(single[0].nextRecord, 48u);

    uint64_t resumeDigest = 0;
    CHECK_TRUE(archive.chainDigestAt(kGameId, 48u, 0u, resumeDigest));

    MigratePayload migrate{};
    migrate.viewerId = single[0].viewerId;
    migrate.oldGeneration = 1u;
    migrate.newGeneration = 2u;
    migrate.resumeRecord = 48u;
    migrate.resumeEvent = 0u;
    migrate.resumeDigest = resumeDigest;
    migrate.grantId = 0x4242u;

    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(migrate, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Migrate, spectatorIdentity(),
        single[0].sequence + 1u, payload, packet));
    nowMs += 16;
    CHECK_EQ(source.onDatagram(
                 0x0C000002ull, packet.data(), packet.size(), nowMs),
             IngressResult::MigrationRejected);
    SourceViewerSnapshot pinned{};
    CHECK_TRUE(source.viewerSnapshot(single[0].viewerId, pinned));
    CHECK_EQ(pinned.endpoint, single[0].endpoint);

    SpectatorGrant grant{};
    grant.grantId = migrate.grantId;
    grant.viewerId = migrate.viewerId;
    grant.sessionId = spectatorIdentity().sessionId;
    grant.expiryUnix = 1000u;
    grant.sourceGeneration = migrate.newGeneration;
    grant.resumeRecord = migrate.resumeRecord;
    grant.resumeDigest = migrate.resumeDigest;
    CHECK_TRUE(issueGrant(key, grant));
    migrate.grantMacLow = grant.macLow;
    migrate.grantMacHigh = grant.macHigh;
    CHECK_TRUE(encodePayload(migrate, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Migrate, spectatorIdentity(),
        single[0].sequence + 2u, payload, packet));
    nowMs += 16;
    CHECK_EQ(source.onDatagram(
                 0x0C000002ull, packet.data(), packet.size(), nowMs),
             IngressResult::Accepted);
    CHECK_TRUE(source.viewerSnapshot(single[0].viewerId, pinned));
    CHECK_EQ(pinned.endpoint, 0x0C000002ull);
    CHECK_EQ(pinned.generation, 2u);
}

}

void testTopologyGrantIssueAndVerify() {
    const TopologyGrantKey key = testGrantKey(0x6Au);
    const TopologyGrantKey wrongKey = testGrantKey(0x6Bu);
    constexpr uint64_t nodeId = 0x0123456789ABCDEFull;
    constexpr uint64_t sessionId = 0x8070605040302010ull;

    TopologyGrant grant{};
    grant.grantId = 0x21u;
    grant.nodeId = nodeId;
    grant.sessionId = sessionId;
    grant.expiryUnix = 1000u;
    grant.authorityGeneration = 3u;
    CHECK_TRUE(issueTopologyGrant(key, grant));
    CHECK_TRUE(grant.macLow != 0 || grant.macHigh != 0);

    TopologyGrant again = grant;
    again.macLow = 0;
    again.macHigh = 0;
    CHECK_TRUE(issueTopologyGrant(key, again));
    CHECK_EQ(again.macLow, grant.macLow);
    CHECK_EQ(again.macHigh, grant.macHigh);

    CHECK_EQ(verifyTopologyGrant(key, grant, nodeId, sessionId, 999u),
             GrantVerdict::Ok);
    CHECK_EQ(verifyTopologyGrant(wrongKey, grant, nodeId, sessionId, 999u),
             GrantVerdict::BadMac);
    CHECK_EQ(verifyTopologyGrant(key, grant, nodeId, sessionId, 1000u),
             GrantVerdict::Expired);
    CHECK_EQ(verifyTopologyGrant(key, grant, nodeId + 1u, sessionId, 999u),
             GrantVerdict::WrongViewer);
    CHECK_EQ(verifyTopologyGrant(key, grant, nodeId, sessionId + 1u, 999u),
             GrantVerdict::WrongSession);

    const struct Mutation {
        const char* name;
        TopologyGrant grant;
    } mutations[] = {
        {"grantId",
         [&] { TopologyGrant m = grant; m.grantId ^= 1u; return m; }()},
        {"nodeId",
         [&] { TopologyGrant m = grant; m.nodeId ^= 1u; return m; }()},
        {"sessionId",
         [&] { TopologyGrant m = grant; m.sessionId ^= 1u; return m; }()},
        {"expiry",
         [&] { TopologyGrant m = grant; m.expiryUnix ^= 1u; return m; }()},
        {"generation",
         [&] {
             TopologyGrant m = grant;
             m.authorityGeneration ^= 1u;
             return m;
         }()},
    };
    for (const Mutation& mutation : mutations) {
        const GrantVerdict verdict = verifyTopologyGrant(
            key, mutation.grant, mutation.grant.nodeId,
            mutation.grant.sessionId, 999u);
        if (verdict != GrantVerdict::BadMac) {
            std::fprintf(stderr,
                         "topology grant mutation %s was not rejected\n",
                         mutation.name);
            ++g_failures;
        }
    }

    TopologyGrant malformed = grant;
    malformed.authorityGeneration = 0;
    CHECK_EQ(verifyTopologyGrant(key, malformed, nodeId, sessionId, 999u),
             GrantVerdict::Malformed);

    SpectatorGrant crossed{};
    crossed.grantId = grant.grantId;
    crossed.viewerId = grant.nodeId;
    crossed.sessionId = grant.sessionId;
    crossed.expiryUnix = grant.expiryUnix;
    crossed.sourceGeneration = grant.authorityGeneration;
    CHECK_TRUE(issueGrant(key, crossed));
    CHECK_TRUE(crossed.macLow != grant.macLow
               || crossed.macHigh != grant.macHigh);
    crossed.macLow = grant.macLow;
    crossed.macHigh = grant.macHigh;
    CHECK_EQ(verifyGrant(key, crossed, crossed.viewerId, crossed.sessionId,
                         999u),
             GrantVerdict::BadMac);
}

std::vector<uint8_t> topologyQtaEnvelope() {
    namespace rwire = qnetplay::spectator::resilient::wire;
    rwire::AuthorityState state{};
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = qnetplay::wire::kVersion;
    (void)qnetplay::compat::setProfileId(identity, "topo-relay-fixture");
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
    if (!qnetplay::spectator::resilient::makeStreamCompatibilityIdentity(
            identity, 0x5151515151515151ull, 3u, 0x4646464646464646ull,
            state.stream)) {
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
    std::vector<uint8_t> inner;
    if (!rwire::encodeControlDatagram(datagram, inner)) return {};

    std::vector<uint8_t> out(
        rwire::kTopologyQtaEnvelopePrefixBytes + inner.size(), 0u);
    const auto writeLe = [&out](std::size_t offset, uint64_t value,
                                unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i)
            out[offset + i] = static_cast<uint8_t>(value >> (i * 8u));
    };
    writeLe(0u, rwire::kTopologyQtaEnvelopeMagic, 4u);
    writeLe(4u, rwire::kTopologyQtaEnvelopeVersion, 2u);
    out[6] = rwire::kTopologyQtaEnvelopeControlType;
    writeLe(8u, inner.size(), 2u);
    writeLe(12u, 0x0101010101010101ull, 8u);
    writeLe(20u, 0x0202020202020202ull, 8u);
    writeLe(28u, 0x0303030303030303ull, 8u);
    for (std::size_t index = 0; index < rwire::kTopologyQtaEnvelopeMacBytes;
         ++index) {
        out[36u + index] = static_cast<uint8_t>(0x90u + index);
    }
    std::copy(inner.begin(), inner.end(),
              out.begin() + rwire::kTopologyQtaEnvelopePrefixBytes);
    return out;
}

void testTopologyRelayNodeAdapter() {
    namespace qr = qnet::relay;
    const qr::RoomId room{0x1111222233334444ull, 0x5555666677778888ull};
    const qr::RegistrationToken token{0xAAAABBBBCCCCDDDDull,
                                      0xEEEEFFFF00001111ull};
    const uint64_t nodeId = 0x77u;
    const uint32_t generation = 2u;

    TopologyRelayBinding binding{};
    binding.enabled = true;
    binding.room = room;
    binding.token = token;
    binding.nodeId = nodeId;
    binding.authorityGeneration = generation;
    CHECK_TRUE(validTopologyRelayBinding(binding));

    TopologyRelayNodeAdapter adapter;
    CHECK_TRUE(adapter.configure(binding));
    CHECK_TRUE(adapter.enabled());

    const std::vector<uint8_t> envelope = topologyQtaEnvelope();
    CHECK_TRUE(!envelope.empty());

    std::vector<uint8_t> framed;
    CHECK_TRUE(adapter.encodeOutbound(envelope.data(), envelope.size(),
                                      framed));
    CHECK_TRUE(qr::hasTopologyFrameKind(framed.data(), framed.size()));
    qr::TopologyFrameHeader outHeader{};
    CHECK_EQ(qr::decodeTopologyFrameHeader(framed.data(), framed.size(),
                                           outHeader),
             qr::DecodeError::None);
    CHECK_TRUE(outHeader.party == qr::TopologyParty::Node);
    CHECK_EQ(outHeader.nodeId, nodeId);
    CHECK_EQ(outHeader.authorityGeneration, generation);
    CHECK_EQ(framed.size(),
             qr::kTopologyFrameHeaderSize + envelope.size());

    std::vector<uint8_t> bogus(64u, 0xABu);
    std::vector<uint8_t> rejected;
    CHECK_TRUE(!adapter.encodeOutbound(bogus.data(), bogus.size(), rejected));

    std::vector<uint8_t> authFrame;
    {
        qr::TopologyFrameHeader header{};
        header.party = qr::TopologyParty::Authority;
        header.nodeId = nodeId;
        header.authorityGeneration = generation;
        CHECK_TRUE(qr::encodeTopologyFrameHeader(header, authFrame));
        authFrame.insert(authFrame.end(), envelope.begin(), envelope.end());
    }
    std::vector<uint8_t> unwrapped;
    CHECK_EQ(adapter.decodeInbound(true, authFrame.data(), authFrame.size(),
                                   unwrapped),
             TopologyRelayIngress::Data);
    CHECK_TRUE(unwrapped == envelope);
    CHECK_TRUE(adapter.ready());

    std::vector<uint8_t> ignored;
    CHECK_EQ(adapter.decodeInbound(true, framed.data(), framed.size(),
                                   ignored),
             TopologyRelayIngress::Rejected);
    {
        qr::TopologyFrameHeader header{};
        header.party = qr::TopologyParty::Authority;
        header.nodeId = nodeId + 1u;
        header.authorityGeneration = generation;
        std::vector<uint8_t> wrongNode;
        CHECK_TRUE(qr::encodeTopologyFrameHeader(header, wrongNode));
        wrongNode.insert(wrongNode.end(), envelope.begin(), envelope.end());
        CHECK_EQ(adapter.decodeInbound(true, wrongNode.data(),
                                       wrongNode.size(), ignored),
                 TopologyRelayIngress::Rejected);
    }
    {
        qr::TopologyFrameHeader header{};
        header.party = qr::TopologyParty::Authority;
        header.nodeId = nodeId;
        header.authorityGeneration = generation + 1u;
        std::vector<uint8_t> stale;
        CHECK_TRUE(qr::encodeTopologyFrameHeader(header, stale));
        stale.insert(stale.end(), envelope.begin(), envelope.end());
        CHECK_EQ(adapter.decodeInbound(true, stale.data(), stale.size(),
                                       ignored),
                 TopologyRelayIngress::Rejected);
    }
    CHECK_EQ(adapter.decodeInbound(true, envelope.data(), envelope.size(),
                                   ignored),
             TopologyRelayIngress::Rejected);

    {
        TopologyRelayNodeAdapter readyAdapter;
        CHECK_TRUE(readyAdapter.configure(binding));
        qr::TopologyReady ready{};
        ready.room = room;
        ready.party = qr::TopologyParty::Node;
        ready.authorityGeneration = generation;
        ready.token = token;
        ready.nodeId = nodeId;
        ready.peer = qr::Ipv4Endpoint{0x0100007Fu, 41000u};
        std::vector<uint8_t> readyBytes;
        CHECK_TRUE(qr::encodeTopologyReady(ready, readyBytes));
        std::vector<uint8_t> discard;
        CHECK_EQ(readyAdapter.decodeInbound(true, readyBytes.data(),
                                            readyBytes.size(), discard),
                 TopologyRelayIngress::Ready);
        CHECK_TRUE(readyAdapter.ready());

        qr::TopologyDetach detach{};
        detach.room = room;
        detach.party = qr::TopologyParty::Node;
        detach.reason = qr::TopologyDetachReason::ServiceStopping;
        detach.authorityGeneration = generation;
        detach.token = token;
        detach.nodeId = nodeId;
        std::vector<uint8_t> detachBytes;
        CHECK_TRUE(qr::encodeTopologyDetach(detach, detachBytes));
        CHECK_EQ(readyAdapter.decodeInbound(true, detachBytes.data(),
                                            detachBytes.size(), discard),
                 TopologyRelayIngress::Detached);
        CHECK_TRUE(readyAdapter.detached());
    }

    {
        TopologyRelayNodeAdapter reg;
        CHECK_TRUE(reg.configure(binding));
        CHECK_TRUE(reg.registrationDue(0u));
        std::vector<uint8_t> record;
        CHECK_TRUE(reg.encodeRegistration(0u, record, false));
        qr::TopologyRegister decoded{};
        CHECK_EQ(qr::decodeTopologyRegister(record.data(), record.size(),
                                            decoded),
                 qr::DecodeError::None);
        CHECK_EQ(decoded.nodeId, nodeId);
        CHECK_TRUE(decoded.party == qr::TopologyParty::Node);
        CHECK_TRUE(!reg.registrationDue(100u));
        CHECK_TRUE(reg.registrationDue(600u));
    }

    {
        TopologyRelayNodeAdapter passthrough;
        std::vector<uint8_t> copy;
        CHECK_TRUE(passthrough.encodeOutbound(envelope.data(),
                                              envelope.size(), copy));
        CHECK_TRUE(copy == envelope);
        std::vector<uint8_t> inCopy;
        CHECK_EQ(passthrough.decodeInbound(false, envelope.data(),
                                           envelope.size(), inCopy),
                 TopologyRelayIngress::PassThrough);
        CHECK_TRUE(inCopy == envelope);
    }
}

void testTopologyRelayAuthorityAdapter() {
    namespace qr = qnet::relay;
    const qr::RoomId room{0x2222333344445555ull, 0x6666777788889999ull};
    const qr::RegistrationToken token{0x1234123412341234ull,
                                      0x5678567856785678ull};
    const uint32_t generation = 5u;

    TopologyRelayAuthorityBinding binding{};
    binding.enabled = true;
    binding.room = room;
    binding.token = token;
    binding.authorityGeneration = generation;
    CHECK_TRUE(validTopologyRelayAuthorityBinding(binding));

    TopologyRelayAuthorityAdapter adapter;
    CHECK_TRUE(adapter.configure(binding));
    CHECK_TRUE(adapter.enabled());

    const std::vector<uint8_t> envelope = topologyQtaEnvelope();
    CHECK_TRUE(!envelope.empty());

    const uint64_t nodeId = 0x99u;
    std::vector<uint8_t> framed;
    CHECK_TRUE(adapter.encodeOutbound(nodeId, envelope.data(),
                                      envelope.size(), framed));
    qr::TopologyFrameHeader outHeader{};
    CHECK_EQ(qr::decodeTopologyFrameHeader(framed.data(), framed.size(),
                                           outHeader),
             qr::DecodeError::None);
    CHECK_TRUE(outHeader.party == qr::TopologyParty::Authority);
    CHECK_EQ(outHeader.nodeId, nodeId);
    CHECK_EQ(outHeader.authorityGeneration, generation);
    std::vector<uint8_t> refused;
    CHECK_TRUE(!adapter.encodeOutbound(0u, envelope.data(), envelope.size(),
                                       refused));
    std::vector<uint8_t> bogus(64u, 0xCDu);
    CHECK_TRUE(!adapter.encodeOutbound(nodeId, bogus.data(), bogus.size(),
                                       refused));
    TopologyRelayAuthorityAdapter unconfigured;
    CHECK_TRUE(!unconfigured.encodeOutbound(nodeId, envelope.data(),
                                            envelope.size(), refused));

    std::vector<uint8_t> nodeFrame;
    {
        qr::TopologyFrameHeader header{};
        header.party = qr::TopologyParty::Node;
        header.nodeId = nodeId;
        header.authorityGeneration = generation;
        CHECK_TRUE(qr::encodeTopologyFrameHeader(header, nodeFrame));
        nodeFrame.insert(nodeFrame.end(), envelope.begin(), envelope.end());
    }
    std::vector<uint8_t> unwrapped;
    uint64_t fromNode = 0u;
    CHECK_EQ(adapter.decodeInbound(true, nodeFrame.data(), nodeFrame.size(),
                                   unwrapped, fromNode),
             TopologyRelayIngress::Data);
    CHECK_TRUE(unwrapped == envelope);
    CHECK_EQ(fromNode, nodeId);
    std::vector<uint8_t> ignored;
    CHECK_EQ(adapter.decodeInbound(true, framed.data(), framed.size(),
                                   ignored, fromNode),
             TopologyRelayIngress::Rejected);
    {
        qr::TopologyFrameHeader header{};
        header.party = qr::TopologyParty::Node;
        header.nodeId = nodeId;
        header.authorityGeneration = generation + 1u;
        std::vector<uint8_t> stale;
        CHECK_TRUE(qr::encodeTopologyFrameHeader(header, stale));
        stale.insert(stale.end(), envelope.begin(), envelope.end());
        CHECK_EQ(adapter.decodeInbound(true, stale.data(), stale.size(),
                                       ignored, fromNode),
                 TopologyRelayIngress::Rejected);
    }
    CHECK_EQ(adapter.decodeInbound(true, envelope.data(), envelope.size(),
                                   ignored, fromNode),
             TopologyRelayIngress::Rejected);

    {
        qr::TopologyReady ready{};
        ready.room = room;
        ready.party = qr::TopologyParty::Authority;
        ready.authorityGeneration = generation;
        ready.token = token;
        ready.nodeId = nodeId;
        ready.peer = qr::Ipv4Endpoint{0x0100007Fu, 42000u};
        std::vector<uint8_t> readyBytes;
        CHECK_TRUE(qr::encodeTopologyReady(ready, readyBytes));
        std::vector<uint8_t> discard;
        uint64_t readyNode = 0u;
        CHECK_EQ(adapter.decodeInbound(true, readyBytes.data(),
                                       readyBytes.size(), discard, readyNode),
                 TopologyRelayIngress::Ready);
        CHECK_EQ(readyNode, nodeId);
        ready.party = qr::TopologyParty::Node;
        readyBytes.clear();
        CHECK_TRUE(qr::encodeTopologyReady(ready, readyBytes));
        CHECK_EQ(adapter.decodeInbound(true, readyBytes.data(),
                                       readyBytes.size(), discard, readyNode),
                 TopologyRelayIngress::Rejected);
    }
    {
        qr::TopologyDetach detach{};
        detach.room = room;
        detach.party = qr::TopologyParty::Authority;
        detach.reason = qr::TopologyDetachReason::AuthorityLost;
        detach.authorityGeneration = generation;
        detach.token = token;
        detach.nodeId = 0u;
        std::vector<uint8_t> detachBytes;
        CHECK_TRUE(qr::encodeTopologyDetach(detach, detachBytes));
        std::vector<uint8_t> discard;
        uint64_t detachedNode = 0xFFu;
        CHECK_EQ(adapter.decodeInbound(true, detachBytes.data(),
                                       detachBytes.size(), discard,
                                       detachedNode),
                 TopologyRelayIngress::Detached);
        CHECK_EQ(detachedNode, 0u);
    }

    {
        TopologyRelayAuthorityAdapter reg;
        CHECK_TRUE(reg.configure(binding));
        CHECK_TRUE(reg.registrationDue(0u));
        std::vector<uint8_t> record;
        CHECK_TRUE(reg.encodeRegistration(0u, record, false));
        qr::TopologyRegister decoded{};
        CHECK_EQ(qr::decodeTopologyRegister(record.data(), record.size(),
                                            decoded),
                 qr::DecodeError::None);
        CHECK_TRUE(decoded.party == qr::TopologyParty::Authority);
        CHECK_TRUE(!reg.registrationDue(100u));
        CHECK_TRUE(reg.registrationDue(600u));
        for (unsigned attempt = 1u;
             attempt < kTopologyRelayFastRegistrationAttempts; ++attempt) {
            CHECK_TRUE(reg.encodeRegistration(attempt * 600u, record, false));
        }
        const uint64_t base =
            kTopologyRelayFastRegistrationAttempts * 600u;
        CHECK_TRUE(!reg.registrationDue(base + 5'000u));
        CHECK_TRUE(reg.registrationDue(
            base + qnetplay::spectator::kTopologyRelayAuthorityRefreshMs));
    }
}

void testRelayQsrOfferCodec() {
    namespace qr = qnet::relay;
    qnet::SpectateTopologyInfoV2 info{};
    info.childNodeId = 0x77u;
    info.sessionId = 0x5151515151515151ull;
    info.authorityGeneration = 3u;
    info.rootQsr.advertisedIpv4BE = 0x0100007Fu;
    info.rootQsr.advertisedPort = 40000u;

    CHECK_TRUE(relayQsrOfferAbsent(info));
    RelayQsrOffer decoded{};
    CHECK_TRUE(relayQsrOfferFromInfo(info, decoded));
    CHECK_TRUE(!decoded.present);

    RelayQsrOffer offer{};
    offer.present = true;
    offer.relayIpv4BE = 0x0100007Fu;
    offer.relayPort = 41000u;
    offer.room = qr::RoomId{0x1111222233334444ull, 0x5555666677778888ull};
    offer.grant.grantId = 0x9001u;
    offer.grant.nodeId = info.childNodeId;
    offer.grant.sessionId = info.sessionId;
    offer.grant.expiryUnix = 1700000060ull;
    offer.grant.authorityGeneration = 3u;
    offer.grant.macLow = 0xABCDu;
    offer.grant.macHigh = 0xEF01u;

    CHECK_TRUE(writeRelayQsrOffer(offer, info));
    CHECK_TRUE(!relayQsrOfferAbsent(info));
    CHECK_TRUE(relayQsrOfferFromInfo(info, decoded));
    CHECK_TRUE(decoded.present);
    CHECK_EQ(decoded.relayIpv4BE, offer.relayIpv4BE);
    CHECK_EQ(decoded.relayPort, offer.relayPort);
    CHECK_TRUE(decoded.room == offer.room);
    CHECK_TRUE(decoded.grant == offer.grant);

    TopologyRelayBinding binding{};
    const qr::RegistrationToken token{0xAAAAu, 0xBBBBu};
    CHECK_TRUE(relayBindingFromOffer(decoded, token, binding));
    CHECK_EQ(binding.nodeId, info.childNodeId);
    CHECK_EQ(binding.authorityGeneration, 3u);
    CHECK_TRUE(validTopologyRelayBinding(binding));
    TopologyRelayBinding rejected{};
    CHECK_TRUE(!relayBindingFromOffer(decoded, qr::RegistrationToken{}, rejected));

    const struct Mutation {
        const char* name;
        qnet::SpectateTopologyInfoV2 info;
    } mutations[] = {
        {"endpoint-cleared",
         [&] { auto m = info; m.relayIpv4BE = 0u; return m; }()},
        {"port-cleared",
         [&] { auto m = info; m.relayPort = 0u; return m; }()},
        {"room-cleared",
         [&] {
             auto m = info;
             for (uint8_t& b : m.relayRoomId) b = 0u;
             return m;
         }()},
        {"grant-cleared",
         [&] { auto m = info; m.relayGrantId = 0u; return m; }()},
        {"expiry-cleared",
         [&] { auto m = info; m.relayGrantExpiryUnix = 0u; return m; }()},
        {"generation-cleared",
         [&] { auto m = info; m.relayGrantAuthorityGeneration = 0u; return m; }()},
        {"mac-cleared",
         [&] {
             auto m = info;
             m.relayGrantMacLow = 0u;
             m.relayGrantMacHigh = 0u;
             return m;
         }()},
        {"reserved-relay-nonzero",
         [&] { auto m = info; m.reservedRelay = 1u; return m; }()},
        {"reserved-grant-nonzero",
         [&] { auto m = info; m.reservedRelayGrant = 1u; return m; }()},
        {"relay-masquerades-as-host-qsr",
         [&] {
             auto m = info;
             m.relayIpv4BE = m.rootQsr.advertisedIpv4BE;
             m.relayPort = m.rootQsr.advertisedPort;
             return m;
         }()},
    };
    for (const Mutation& mutation : mutations) {
        RelayQsrOffer refusedOffer{};
        if (relayQsrOfferFromInfo(mutation.info, refusedOffer)) {
            std::fprintf(stderr,
                         "relay offer mutation %s was not rejected\n",
                         mutation.name);
            ++g_failures;
        }
    }

    qnet::SpectateTopologyInfoV2 target = info;
    RelayQsrOffer wrongNode = offer;
    wrongNode.grant.nodeId = info.childNodeId + 1u;
    CHECK_TRUE(!writeRelayQsrOffer(wrongNode, target));
    RelayQsrOffer wrongSession = offer;
    wrongSession.grant.sessionId = info.sessionId + 1u;
    CHECK_TRUE(!writeRelayQsrOffer(wrongSession, target));
    RelayQsrOffer laneGeneration = offer;
    laneGeneration.grant.authorityGeneration = 4u;
    CHECK_TRUE(writeRelayQsrOffer(laneGeneration, target));
    RelayQsrOffer decodedLane{};
    CHECK_TRUE(relayQsrOfferFromInfo(target, decodedLane));
    CHECK_EQ(decodedLane.grant.authorityGeneration, 4u);
    RelayQsrOffer zeroGeneration = offer;
    zeroGeneration.grant.authorityGeneration = 0u;
    CHECK_TRUE(!writeRelayQsrOffer(zeroGeneration, target));

    RelayQsrOffer none{};
    CHECK_TRUE(writeRelayQsrOffer(none, target));
    CHECK_TRUE(relayQsrOfferAbsent(target));
}

int main() {
    testProductionLivePlaybackSurvivesLatencyJitterAndOneLostBatch();
    testBackloggedFanoutIsFairAtProductionBudgets();
    testThirtyThirdViewerIsRejectedPromptlyUnderFullBacklog();
    testGrantIssueVerifyAndSingleUse();
    testTopologyGrantIssueAndVerify();
    testTopologyRelayNodeAdapter();
    testTopologyRelayAuthorityAdapter();
    testRelayQsrOfferCodec();
    testMigrationGrantGatesEndpointChange();

    if (g_failures != 0) {
        std::fprintf(stderr,
                     "spectator production tests FAILED: %d\n", g_failures);
        return 1;
    }
    std::puts("spectator production tests passed");
    return 0;
}
