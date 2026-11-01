#include "netplay/tests/TestHarness.h"

#include "game/replay/ReplayIpcWorker.h"
#include "game/replay/ReplayRuntime.h"
#include "game/replay/ReplayTimelineSource.h"
#include "game/spectator/SpectatorPublisher.h"
#include "replay/CharacterCatalog.h"
#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"
#include "replay/ReplayTimeline.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <vector>

namespace {
std::atomic<long> g_allocations{0};
std::atomic<bool> g_countAllocations{false};

inline void beginAllocationWatch() {
    g_allocations.store(0, std::memory_order_relaxed);
    g_countAllocations.store(true, std::memory_order_relaxed);
}
inline long endAllocationWatch() {
    g_countAllocations.store(false, std::memory_order_relaxed);
    return g_allocations.load(std::memory_order_relaxed);
}
}

void* operator new(size_t size) {
    if (g_countAllocations.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(size ? size : 1u);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

using namespace qgame;
using namespace qgame::replay;

namespace {

constexpr uint64_t kSnapshotSentinel = 0xC5A17E5B5EED0001ull;
constexpr uint32_t kTestGeneration = 0x51A2C3D4u;
constexpr uint64_t kTestNonce = 0x0F1E2D3C4B5A6978ull;
constexpr uint64_t kTestManifest = 0xABCDEF0123456789ull;
constexpr uint64_t kInitialDigest = 0x1122334455667788ull;

uint64_t stateFor(uint32_t frame) {
    uint64_t x = 0x9E3779B97F4A7C15ull * (static_cast<uint64_t>(frame) + 1ull);
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    return x ^ 0xA5A5A5A5A5A5A5A5ull;
}

class FakeChannel final : public ByteChannel {
public:
    bool connect(uint32_t) override {
        if (refuseConnect) return false;
        connected_ = true;
        return true;
    }
    void close() override { connected_ = false; }
    bool connected() const override { return connected_; }

    bool read(uint8_t* dst, size_t cap, size_t& got) override {
        got = 0;
        if (failRead) return false;
        const size_t available = inbound.size() - inPos;
        size_t n = (cap < available) ? cap : available;
        if (n > 0u) {
            std::memcpy(dst, inbound.data() + inPos, n);
            inPos += n;
            got = n;
        }
        return true;
    }

    bool write(const uint8_t* src, size_t len) override {
        if (failWrite) return false;
        outbound.insert(outbound.end(), src, src + len);
        return true;
    }

    void feed(const std::vector<uint8_t>& bytes) {
        inbound.insert(inbound.end(), bytes.begin(), bytes.end());
    }

    bool refuseConnect = false;
    bool failRead = false;
    bool failWrite = false;
    std::vector<uint8_t> inbound;
    size_t inPos = 0u;
    std::vector<uint8_t> outbound;

private:
    bool connected_ = false;
};

void appendMessage(std::vector<uint8_t>& out, IpcMessageKind kind,
                   uint32_t generation, uint64_t nonce, uint32_t& sequence,
                   uint32_t param0, uint32_t param1,
                   const std::vector<uint8_t>& payload) {
    IpcFrameHeader header{};
    header.magic = kReplayIpcMagic;
    header.wireVersion = kReplayIpcSchemaVersion;
    header.channel = static_cast<uint8_t>(
        qreplay::ipc::channelForMessageType(static_cast<uint8_t>(kind)));
    header.type = static_cast<uint8_t>(kind);
    header.generation = generation;
    header.nonce = nonce;
    header.sequence = ++sequence;
    header.param0 = param0;
    header.param1 = param1;
    header.payloadBytes = static_cast<uint32_t>(payload.size());
    header.payloadCrc32c = payload.empty()
        ? 0u
        : qreplay::crc32c(payload.data(), payload.size());
    encodeIpcHeader(header, out);
    out.insert(out.end(), payload.begin(), payload.end());
}

struct Stream {
    BrokerSessionInfo info{};
    qreplay::DeterministicStart start{};
    std::vector<qreplay::ReplayFrameV1> frames;
    uint64_t finalDigest = 0u;
    uint32_t nextRecord = 0u;
    uint64_t postTailStateHash = 0u;
};

Stream makeStream(uint32_t firstFrame, uint32_t count) {
    Stream s{};
    s.info.manifestDigest = kTestManifest;
    s.info.sessionUuidLow = 0x1111222233334444ull;
    s.info.sessionUuidHigh = 0x5555666677778888ull;
    s.info.simulationContractId = 0x00C0FFEE00C0FFEEull;
    s.info.contentDigest = 0x00DEC0DE00DEC0DEull;
    s.info.matchId = 7u;
    s.info.matchOrdinal = 1u;
    s.info.firstTimelineFrame = firstFrame;
    s.info.lastTimelineFrame = firstFrame + count;
    s.info.totalFrames = count;
    s.info.matchCount = 1u;
    s.info.bootstrapKind =
        static_cast<uint8_t>(qreplay::BootstrapKind::OfflineVsDirectBootV1);
    s.info.vsRow = 1u;
    s.info.cpuMask = 0u;
    s.info.activeLaneMask = 0x03u;
    s.info.activeSlotMask = 0x03u;
    s.info.slotToSourceRoute = {{0u, 1u, 2u, 3u}};
    s.info.bootstrapFingerprint = 0x0BADF00D0BADF00Dull;

    s.start.firstTimelineFrame = firstFrame;
    s.start.initialStateHash = stateFor(firstFrame);
    s.start.initialDigest = kInitialDigest;
    s.start.seedGameplay = 0x1234u;
    s.start.seedCosmetic = 0x5678u;

    uint64_t digest = kInitialDigest;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t f = firstFrame + i;
        qreplay::ReplayFrameV1 frame{};
        frame.timelineFrame = f;
        frame.inputDomain = qreplay::InputDomain::LogicalSourceV1;
        frame.activeLaneMask = 0x03u;
        frame.actions[0] = static_cast<uint16_t>((f * 0x0101u) & 0x0F0Fu);
        frame.actions[1] = static_cast<uint16_t>((f * 0x0202u) & 0x0F0Fu);
        frame.routeEpoch = 1u;
        frame.preStateHash = stateFor(f);
        frame.postStateHash = stateFor(f + 1u);
        frame.dispatcherResult = 0;
        digest = qreplay::sealFrameDigest(digest, frame);
        s.frames.push_back(frame);
    }
    s.finalDigest = digest;
    s.nextRecord = firstFrame + count;
    s.postTailStateHash = stateFor(firstFrame + count);
    return s;
}

std::vector<uint8_t> terminalPayload(const Stream& stream, uint16_t reason) {
    std::vector<uint8_t> payload;
    qreplay::putU16(payload, reason);
    qreplay::putU16(payload, 0u);
    qreplay::putU32(payload, stream.nextRecord);
    qreplay::putU64(payload, stream.postTailStateHash);
    qreplay::putU64(payload, stream.finalDigest);
    return payload;
}

std::vector<uint8_t> buildBrokerBytes(const Stream& stream, bool withTerminal,
                                      uint32_t generation = kTestGeneration,
                                      uint64_t nonce = kTestNonce) {
    std::vector<uint8_t> bytes;
    uint32_t sequence = 0u;

    std::vector<uint8_t> payload;
    encodeBrokerSessionInfo(stream.info, payload);
    appendMessage(bytes, IpcMessageKind::SessionInfo, generation, nonce,
                  sequence, 0u, 0u, payload);

    payload.clear();
    qreplay::encodeDeterministicStart(stream.start, payload);
    appendMessage(bytes, IpcMessageKind::SessionStart, generation, nonce,
                  sequence, 0u, 0u, payload);

    if (!stream.frames.empty()) {
        payload.clear();
        qreplay::encodeInputBlock(stream.frames.data(), stream.frames.size(),
                                  payload);
        appendMessage(bytes, IpcMessageKind::InputBlock, generation, nonce,
                      sequence, stream.frames.front().timelineFrame,
                      static_cast<uint32_t>(stream.frames.size()), payload);
    }

    if (withTerminal) {
        payload = terminalPayload(
            stream, static_cast<uint16_t>(qreplay::TerminalReason::Completed));
        appendMessage(bytes, IpcMessageKind::Terminal, generation, nonce,
                      sequence, stream.nextRecord, 0u, payload);
    }
    return bytes;
}

qipc::IpcConfig makeReplayConfig() {
    qipc::IpcConfig cfg{};
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.magic = qipc::kIpcMagic;
    cfg.version = qipc::kIpcVersion;
    cfg.mode = qipc::MODE_REPLAY;
    cfg.compatibilityId = qipc::kRuntimeCompatibilityId;
    cfg.replayChannelGeneration = kTestGeneration;
    cfg.replayBrokerNonce = kTestNonce;
    cfg.replayManifestDigest = kTestManifest;
    cfg.replayIpcSchemaVersion = kReplayIpcSchemaVersion;
    cfg.replayMatchOrdinal = 1u;
    cfg.replaySpeedPermille = 1000u;
    for (size_t i = 0u;
         i < sizeof(cfg.replayContentManifestRoot); ++i) {
        cfg.replayContentManifestRoot[i] =
            static_cast<uint8_t>(0x40u + i);
    }
    return cfg;
}

qipc::IpcConfig makeRecordingConfig() {
    qipc::IpcConfig cfg{};
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.magic = qipc::kIpcMagic;
    cfg.version = qipc::kIpcVersion;
    cfg.mode = qipc::MODE_NETPLAY;
    cfg.isHost = 1u;
    cfg.localPlayer = 1u;
    cfg.compatibilityId = qipc::kRuntimeCompatibilityId;
    cfg.recordEnabled = 1u;
    cfg.recordChannelGeneration = 0x2468ACE0u;
    cfg.recordChannelNonce = 0x7D8E9FA0B1C2D3E4ull;
    cfg.replayManifestDigest = kTestManifest;
    for (size_t i = 0u;
         i < sizeof(cfg.replayContentManifestRoot); ++i) {
        cfg.replayContentManifestRoot[i] =
            static_cast<uint8_t>(0x80u + i);
    }
    return cfg;
}

qipc::IpcConfig makeSidecarConfig() {
    qipc::IpcConfig cfg{};
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.magic = qipc::kIpcMagic;
    cfg.version = qipc::kIpcVersion;
    cfg.mode = qipc::MODE_NETPLAY;
    cfg.isHost = 1u;
    cfg.localPlayer = 1u;
    cfg.serveSpectators = 1u;
    cfg.spectatorServiceMode = qipc::SPECTATOR_SERVICE_SIDECAR;
    cfg.cookieLow = 0x13572468u;
    cfg.spectatorCookieLow = 0x24681357u;
    cfg.spectatorCookieHigh = 0x1122334455667788ull;
    return cfg;
}

struct FakeGame {
    uint64_t state = 0u;
    uint32_t frame = 0u;
};

bool fakeCapture(void* user, uint8_t* dst, size_t cap, size_t& written) {
    FakeGame* game = static_cast<FakeGame*>(user);
    if (cap < 24u) return false;
    std::memcpy(dst, &kSnapshotSentinel, 8u);
    std::memcpy(dst + 8, &game->state, 8u);
    std::memcpy(dst + 16, &game->frame, 4u);
    std::memset(dst + 20, 0, 4u);
    written = 24u;
    return true;
}

bool fakeRestore(void* user, const uint8_t* src, size_t len) {
    FakeGame* game = static_cast<FakeGame*>(user);
    if (len < 24u) return false;
    uint64_t sentinel = 0u;
    std::memcpy(&sentinel, src, 8u);
    if (sentinel != kSnapshotSentinel) return false;
    std::memcpy(&game->state, src + 8, 8u);
    std::memcpy(&game->frame, src + 16, 4u);
    return true;
}

uint64_t fakeStateHash(void* user) {
    return static_cast<FakeGame*>(user)->state;
}

SimulationHooks makeHooks(FakeGame& game) {
    SimulationHooks hooks{};
    hooks.user = &game;
    hooks.captureSnapshot = &fakeCapture;
    hooks.restoreSnapshot = &fakeRestore;
    hooks.currentStateHash = &fakeStateHash;
    return hooks;
}

uint64_t applyTick(FakeGame& game, const qreplay::ReplayFrameV1& frame) {
    game.frame = frame.timelineFrame + 1u;
    game.state = stateFor(game.frame);
    return game.state;
}

bool containsSentinel(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 8u) return false;
    uint8_t needle[8];
    std::memcpy(needle, &kSnapshotSentinel, 8u);
    for (size_t i = 0; i + 8u <= bytes.size(); ++i)
        if (std::memcmp(bytes.data() + i, needle, 8u) == 0) return true;
    return false;
}

void pumpWorker(int turns) {
    for (int i = 0; i < turns; ++i) pumpWorkerForTest();
}

struct PlaybackFixture {
    FakeChannel* channel = nullptr;
    FakeGame game{};

    bool start(const Stream& stream, bool withTerminal,
               uint32_t generation = kTestGeneration,
               uint64_t nonce = kTestNonce,
               uint32_t snapshotInterval = 600u) {
        channel = new FakeChannel();
        channel->feed(buildBrokerBytes(stream, withTerminal, generation, nonce));

        ReplayRuntimeTestConfig config{};
        config.ipc = makeReplayConfig();
        config.brokerChannel = channel;
        config.inlineWorker = true;
        config.source.timelineCapacityFrames = 8192u;
        config.source.snapshotSlotBytes = 4096u;
        config.source.snapshotSlots = snapshotInterval ? 4u : 0u;
        config.source.snapshotIntervalFrames =
            snapshotInterval ? snapshotInterval : 1u;
        if (!startReplayRuntimeForTest(config)) return false;

        game.frame = stream.start.firstTimelineFrame;
        game.state = stateFor(game.frame);
        replaySetSimulationHooks(makeHooks(game));
        return true;
    }

    void stop() {
        stopReplayRuntime();
        channel = nullptr;
    }
};

}

QNP_TEST(spectator_publish_is_allocation_free) {
    using namespace qgame::spectator_publish;

    FakeChannel* channel = new FakeChannel();
    SidecarPublisherOptions options{};
    options.inlineWorker = true;
    options.checkpointSlots = 2u;
    options.checkpointSlotBytes = 4096u;
    options.drainPerTurn = 64u;

    QNP_REQUIRE(
        startSidecarPublisherForTest(makeSidecarConfig(), options, channel));

    SidecarStartInfo info{};
    info.gameId = 0xFEEDBEEFu;
    info.initialStateHash = stateFor(0);
    QNP_CHECK(publishStart(info, nullptr, 0u));

    qnetplay::spectator::ConfirmedRecord record{};
    record.record = 0u;
    record.timelineFrame = 0u;
    record.playerOneInput = 0x0101u;
    record.playerTwoInput = 0x0202u;
    record.stateHash = stateFor(0);
    record.rollingDigest = 0u;

    QNP_CHECK(publishConfirmed(record));
    std::vector<uint8_t> checkpoint(1024u, 0x5Au);

    beginAllocationWatch();
    for (uint32_t i = 0; i < 256u; ++i) {
        record.record = i + 1u;
        record.timelineFrame = i + 1u;
        record.stateHash = stateFor(i + 1u);
        publishConfirmed(record);
    }
    publishCheckpoint(64u, checkpoint.data(), checkpoint.size());
    publishSeal(1u, stateFor(257));
    const long allocations = endAllocationWatch();

    QNP_CHECK(allocations == 0);

    const SidecarPublisherStats stats = sidecarStats();
    QNP_CHECK(stats.published == 257u);
    QNP_CHECK(stats.dropped == 0u);
    QNP_CHECK(stats.disabled == 0u);
    QNP_CHECK(stats.checkpointsPublished == 1u);

    stopSidecarPublisher();
}

QNP_TEST(spectator_publish_overflow_disables_without_touching_match) {
    using namespace qgame::spectator_publish;

    FakeChannel* channel = new FakeChannel();
    SidecarPublisherOptions options{};
    options.inlineWorker = true;
    options.checkpointSlots = 1u;
    options.checkpointSlotBytes = 256u;
    QNP_REQUIRE(
        startSidecarPublisherForTest(makeSidecarConfig(), options, channel));

    SidecarStartInfo info{};
    info.gameId = 0xFEEDBEEFu;
    info.initialStateHash = stateFor(0);
    QNP_REQUIRE(publishStart(info, nullptr, 0u));

    qnetplay::spectator::ConfirmedRecord record{};
    uint64_t accepted = 0u;
    bool refused = false;
    for (uint32_t i = 0; i < kConfirmedRingCapacity + 64u; ++i) {
        record.record = i;
        record.timelineFrame = i;
        record.stateHash = stateFor(i);
        record.rollingDigest = 0u;
        if (publishConfirmed(record)) {
            ++accepted;
        } else {
            refused = true;
        }
    }

    QNP_CHECK(refused);
    QNP_CHECK(accepted == kConfirmedRingCapacity);

    const SidecarPublisherStats stats = sidecarStats();
    QNP_CHECK(stats.disabled == 1u);
    QNP_CHECK(stats.disableReason
              == static_cast<uint16_t>(PublishDisableReason::RingOverflow));
    QNP_CHECK(stats.dropped > 0u);
    std::vector<uint8_t> huge(1024u, 0x11u);
    QNP_CHECK(!publishCheckpoint(1u, huge.data(), huge.size()));

    publishSeal(1u, stateFor(
        static_cast<uint32_t>(kConfirmedRingCapacity)));
    for (int turn = 0; turn < 32; ++turn)
        pumpSidecarForTest();
    const SidecarPublisherStats terminal = sidecarStats();
    QNP_CHECK(terminal.written == kConfirmedRingCapacity);
    QNP_CHECK(terminal.faulted == 1u);
    QNP_CHECK(terminal.faultSent == 1u);
    QNP_CHECK(terminal.sealed == 0u);
    QNP_CHECK(terminal.terminalRecordCursor
              == static_cast<uint32_t>(kConfirmedRingCapacity));
    QNP_CHECK(terminal.terminalPrefixDigest != 0u);

    stopSidecarPublisher();
}

QNP_TEST(spectator_seal_waits_for_more_than_one_worker_batch) {
    using namespace qgame::spectator_publish;

    FakeChannel* channel = new FakeChannel();
    SidecarPublisherOptions options{};
    options.inlineWorker = true;
    options.drainPerTurn = 512u;
    QNP_REQUIRE(
        startSidecarPublisherForTest(makeSidecarConfig(), options, channel));

    SidecarStartInfo info{};
    info.gameId = 0xA11CE55u;
    info.initialStateHash = stateFor(0);
    QNP_REQUIRE(publishStart(info, nullptr, 0u));

    qnetplay::spectator::ConfirmedRecord record{};
    for (uint32_t index = 0; index < 600u; ++index) {
        record.record = index;
        record.timelineFrame = index;
        record.stateHash = stateFor(index);
        record.rollingDigest = 0u;
        QNP_REQUIRE(publishConfirmed(record));
    }
    publishSeal(1u, stateFor(600u));

    pumpSidecarForTest();
    SidecarPublisherStats stats = sidecarStats();
    QNP_CHECK(stats.written == 512u);
    QNP_CHECK(stats.sealed == 0u);
    QNP_CHECK(stats.terminalRecordCursor == 600u);

    pumpSidecarForTest();
    stats = sidecarStats();
    QNP_CHECK(stats.written == 600u);
    QNP_CHECK(stats.sealed == 1u);
    QNP_CHECK(stats.faulted == 0u);
    QNP_CHECK(stats.terminalPrefixDigest != 0u);

    stopSidecarPublisher();
}

QNP_TEST(injected_action_is_latched_for_the_whole_tick) {
    PlaybackFixture fixture;
    Stream stream = makeStream(0u, 8u);
    QNP_REQUIRE(fixture.start(stream, true));

    pumpWorker(8);

    qreplay::ReplayFrameV1 frame{};
    QNP_REQUIRE(replayNextFrame(frame));

    const uint16_t lane0 = replayInjectedAction(0);
    const uint16_t lane1 = replayInjectedAction(1);
    for (int i = 0; i < 16; ++i) {
        QNP_CHECK(replayInjectedAction(0) == lane0);
        QNP_CHECK(replayInjectedAction(1) == lane1);
    }
    QNP_CHECK(lane0 == stream.frames[0].actions[0]);
    QNP_CHECK(lane1 == stream.frames[0].actions[1]);
    QNP_CHECK(replayInjectedAction(2) == 0u);
    QNP_CHECK(replayInjectedAction(3) == 0u);
    QNP_CHECK(replayInjectedAction(-1) == 0u);
    QNP_CHECK(replayInjectedAction(4) == 0u);

    replayCommitFrame(frame, applyTick(fixture.game, frame));

    qreplay::ReplayFrameV1 second{};
    QNP_REQUIRE(replayNextFrame(second));
    QNP_CHECK(second.timelineFrame == frame.timelineFrame + 1u);
    QNP_CHECK(replayInjectedAction(0) == stream.frames[1].actions[0]);

    fixture.stop();
}

QNP_TEST(underrun_holds_the_frame_and_advances_nothing) {
    PlaybackFixture fixture;
    Stream stream = makeStream(0u, 0u);
    QNP_REQUIRE(fixture.start(stream, false));

    pumpWorker(8);

    ReplayStatusSnapshot before{};
    replayStatus(before);

    qreplay::ReplayFrameV1 frame{};
    for (int i = 0; i < 32; ++i) QNP_CHECK(!replayNextFrame(frame));

    ReplayStatusSnapshot after{};
    replayStatus(after);
    QNP_CHECK(after.currentFrame == before.currentFrame);
    QNP_CHECK(after.buffering == 1u);
    QNP_CHECK(after.halted == 0u);
    QNP_CHECK(after.underruns >= 32u);
    QNP_CHECK(replayInjectedAction(0) == 0u);

    fixture.stop();
}

QNP_TEST(frame_gap_halts_and_consumes_nothing) {
    Stream stream = makeStream(0u, 4u);
    for (auto& frame : stream.frames) frame.timelineFrame += 1u;
    uint64_t digest = kInitialDigest;
    for (auto& frame : stream.frames)
        digest = qreplay::sealFrameDigest(digest, frame);
    stream.finalDigest = digest;

    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, false));
    pumpWorker(8);

    qreplay::ReplayFrameV1 frame{};
    QNP_CHECK(!replayNextFrame(frame));

    ReplayStatusSnapshot status{};
    replayStatus(status);
    QNP_CHECK(status.halted == 1u);
    QNP_CHECK(status.currentFrame == 0u);

    const ReplayHaltDiagnostic diag = replayHaltDiagnostic();
    QNP_CHECK(diag.reason == HaltReason::ExpectedFrameMismatch);
    QNP_CHECK(diag.manifestDigest == kTestManifest);

    for (int i = 0; i < 8; ++i) QNP_CHECK(!replayNextFrame(frame));
    replayStatus(status);
    QNP_CHECK(status.currentFrame == 0u);

    fixture.stop();
}

QNP_TEST(lane_shape_mismatch_halts) {
    Stream stream = makeStream(0u, 4u);
    uint64_t digest = kInitialDigest;
    for (auto& frame : stream.frames) {
        frame.activeLaneMask = 0x0Fu;
        frame.actions[2] = 0x0101u;
        frame.actions[3] = 0x0202u;
        digest = qreplay::sealFrameDigest(digest, frame);
    }
    stream.finalDigest = digest;

    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, false));
    pumpWorker(8);

    qreplay::ReplayFrameV1 frame{};
    QNP_CHECK(!replayNextFrame(frame));
    QNP_CHECK(replayHalted());
    QNP_CHECK(replayHaltDiagnostic().reason == HaltReason::LaneShapeMismatch);

    ReplayStatusSnapshot status{};
    replayStatus(status);
    QNP_CHECK(status.currentFrame == 0u);

    fixture.stop();
}

QNP_TEST(invalid_action_mask_halts) {
    Stream stream = makeStream(0u, 1u);

    qreplay::ReplayFrameV1 a = stream.frames[0];
    qreplay::ReplayFrameV1 b = stream.frames[0];
    a.actions[0] = 0x0101u;
    b.actions[0] = 0x0202u;
    std::vector<uint8_t> encodedA;
    std::vector<uint8_t> encodedB;
    QNP_REQUIRE(qreplay::encodeInputBlock(&a, 1u, encodedA));
    QNP_REQUIRE(qreplay::encodeInputBlock(&b, 1u, encodedB));
    QNP_REQUIRE(encodedA.size() == encodedB.size());
    size_t actionOffset = encodedA.size();
    for (size_t i = 0; i < encodedA.size(); ++i) {
        if (encodedA[i] != encodedB[i]) { actionOffset = i; break; }
    }
    QNP_REQUIRE(actionOffset + 1u < encodedA.size());

    stream.frames[0] = a;
    uint64_t digest = kInitialDigest;
    digest = qreplay::sealFrameDigest(digest, stream.frames[0]);
    stream.finalDigest = digest;

    std::vector<uint8_t> payload;
    QNP_REQUIRE(qreplay::encodeInputBlock(stream.frames.data(), 1u, payload));
    payload[actionOffset + 1u] = static_cast<uint8_t>(
        payload[actionOffset + 1u] | 0x10u);

    std::vector<uint8_t> bytes;
    uint32_t sequence = 0u;
    std::vector<uint8_t> scratch;
    encodeBrokerSessionInfo(stream.info, scratch);
    appendMessage(bytes, IpcMessageKind::SessionInfo, kTestGeneration,
                  kTestNonce, sequence, 0u, 0u, scratch);
    scratch.clear();
    qreplay::encodeDeterministicStart(stream.start, scratch);
    appendMessage(bytes, IpcMessageKind::SessionStart, kTestGeneration,
                  kTestNonce, sequence, 0u, 0u, scratch);
    appendMessage(bytes, IpcMessageKind::InputBlock, kTestGeneration,
                  kTestNonce, sequence, 0u, 1u, payload);

    FakeChannel* channel = new FakeChannel();
    channel->feed(bytes);

    ReplayRuntimeTestConfig config{};
    config.ipc = makeReplayConfig();
    config.brokerChannel = channel;
    config.inlineWorker = true;
    QNP_REQUIRE(startReplayRuntimeForTest(config));

    pumpWorker(8);
    qreplay::ReplayFrameV1 frame{};
    QNP_CHECK(!replayNextFrame(frame));
    QNP_CHECK(replayHalted());
    const HaltReason reason = replayHaltDiagnostic().reason;
    QNP_CHECK(reason == HaltReason::InvalidActionMask
              || reason == HaltReason::ChannelFault);

    ReplayStatusSnapshot status{};
    replayStatus(status);
    QNP_CHECK(status.currentFrame == 0u);

    stopReplayRuntime();
}

QNP_TEST(digest_break_halts) {
    Stream stream = makeStream(0u, 4u);
    stream.frames[2].rollingDigest ^= 1ull;

    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, false));
    pumpWorker(8);

    qreplay::ReplayFrameV1 frame{};
    int played = 0;
    while (replayNextFrame(frame)) {
        replayCommitFrame(frame, applyTick(fixture.game, frame));
        ++played;
        if (played > 8) break;
    }
    QNP_CHECK(played <= 2);
    QNP_CHECK(replayHalted());
    QNP_CHECK(replayHaltDiagnostic().reason == HaltReason::DigestBreak);

    fixture.stop();
}

QNP_TEST(pre_state_mismatch_halts_before_consuming) {
    Stream stream = makeStream(0u, 4u);
    uint64_t digest = kInitialDigest;
    for (size_t i = 0; i < stream.frames.size(); ++i) {
        if (i == 2u) stream.frames[i].preStateHash ^= 0xDEADull;
        digest = qreplay::sealFrameDigest(digest, stream.frames[i]);
    }
    stream.finalDigest = digest;

    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, false));
    pumpWorker(8);

    qreplay::ReplayFrameV1 frame{};
    int played = 0;
    while (replayNextFrame(frame)) {
        replayCommitFrame(frame, applyTick(fixture.game, frame));
        ++played;
        if (played > 8) break;
    }

    QNP_CHECK(played == 2);
    QNP_CHECK(replayHalted());
    const ReplayHaltDiagnostic diag = replayHaltDiagnostic();
    QNP_CHECK(diag.reason == HaltReason::PreStateMismatch);
    QNP_CHECK(diag.replayFrame == 2u);
    QNP_CHECK(diag.expectedStateHash == stream.frames[2].preStateHash);
    QNP_CHECK(diag.actualStateHash == stateFor(2));
    QNP_CHECK(diag.activeLaneMask == 0x03u);
    QNP_CHECK(diag.inputMasks[0] == stream.frames[2].actions[0]);

    ReplayStatusSnapshot status{};
    replayStatus(status);
    QNP_CHECK(status.currentFrame == 2u);

    fixture.stop();
}

QNP_TEST(post_state_mismatch_halts) {
    Stream stream = makeStream(0u, 4u);
    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, false));
    pumpWorker(8);

    qreplay::ReplayFrameV1 frame{};
    QNP_REQUIRE(replayNextFrame(frame));
    replayCommitFrame(frame, stateFor(1) ^ 0x55ull);

    QNP_CHECK(replayHalted());
    const ReplayHaltDiagnostic diag = replayHaltDiagnostic();
    QNP_CHECK(diag.reason == HaltReason::PostStateMismatch);
    QNP_CHECK(diag.expectedStateHash == stateFor(1));

    QNP_CHECK(!replayNextFrame(frame));
    fixture.stop();
}

QNP_TEST(caller_reported_mismatch_halts_with_region_detail) {
    Stream stream = makeStream(0u, 4u);
    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, false));
    pumpWorker(8);

    replaySetRegionDetail("fighter_p1_state[0x40]");
    replayReportMismatch(3u, 0xAAAAull, 0xBBBBull);

    QNP_CHECK(replayHalted());
    const ReplayHaltDiagnostic diag = replayHaltDiagnostic();
    QNP_CHECK(diag.reason == HaltReason::CallerReportedMismatch);
    QNP_CHECK(diag.replayFrame == 3u);
    QNP_CHECK(diag.expectedStateHash == 0xAAAAull);
    QNP_CHECK(diag.actualStateHash == 0xBBBBull);
    QNP_CHECK(std::strcmp(diag.regionDetail, "fighter_p1_state[0x40]") == 0);

    qreplay::ReplayFrameV1 frame{};
    QNP_CHECK(!replayNextFrame(frame));
    fixture.stop();
}

QNP_TEST(stale_generation_is_rejected) {
    Stream stream = makeStream(0u, 4u);
    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, false, kTestGeneration - 1u, kTestNonce));
    pumpWorker(8);

    qreplay::ReplayFrameV1 frame{};
    QNP_CHECK(!replayNextFrame(frame));
    QNP_CHECK(replayHalted());
    QNP_CHECK(replayHaltDiagnostic().sourceFault
              == static_cast<uint16_t>(SourceFault::StaleGeneration));

    ReplayIpcWorker* worker = replayWorkerForTest();
    QNP_REQUIRE(worker != nullptr);
    QNP_CHECK(worker->stats().framesReceived == 0u);
    QNP_CHECK(worker->stats().messagesRejected > 0u);

    fixture.stop();
}

QNP_TEST(wrong_nonce_is_rejected) {
    Stream stream = makeStream(0u, 4u);
    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, false, kTestGeneration, kTestNonce ^ 1u));
    pumpWorker(8);

    qreplay::ReplayFrameV1 frame{};
    QNP_CHECK(!replayNextFrame(frame));
    QNP_CHECK(replayHalted());
    QNP_CHECK(replayHaltDiagnostic().sourceFault
              == static_cast<uint16_t>(SourceFault::WrongNonce));

    ReplayIpcWorker* worker = replayWorkerForTest();
    QNP_REQUIRE(worker != nullptr);
    QNP_CHECK(worker->stats().framesReceived == 0u);

    fixture.stop();
}

QNP_TEST(manifest_mismatch_is_rejected) {
    Stream stream = makeStream(0u, 4u);
    stream.info.manifestDigest = kTestManifest ^ 0xFFull;

    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, false));
    pumpWorker(8);

    qreplay::ReplayFrameV1 frame{};
    QNP_CHECK(!replayNextFrame(frame));
    QNP_CHECK(replayHalted());
    QNP_CHECK(replayHaltDiagnostic().reason == HaltReason::ManifestMismatch);

    fixture.stop();
}

QNP_TEST(replay_launch_contract_is_enforced) {
    qipc::IpcConfig cfg = makeReplayConfig();
    cfg.hasSocket = 1u;
    QNP_CHECK(qipc::validateReplayLaunch(cfg) != nullptr);

    ReplayRuntimeTestConfig config{};
    config.ipc = cfg;
    config.brokerChannel = new FakeChannel();
    config.inlineWorker = true;
    QNP_CHECK(!startReplayRuntimeForTest(config));
    QNP_CHECK(!replayRuntimeActive());

    cfg = makeReplayConfig();
    cfg.recordEnabled = 1u;
    QNP_CHECK(qipc::validateReplayLaunch(cfg) != nullptr);

    cfg = makeReplayConfig();
    cfg.localPlayer = 1u;
    QNP_CHECK(qipc::validateReplayLaunch(cfg) != nullptr);

    cfg = makeReplayConfig();
    cfg.offlineExtraPresent[0] = 1u;
    QNP_CHECK(qipc::validateReplayLaunch(cfg) != nullptr);

    cfg = makeReplayConfig();
    cfg.recordEnabled = 1u;
    cfg.recordChannelGeneration = 4u;
    QNP_CHECK(!startReplayRecorder(cfg));
}

QNP_TEST(recorder_overflow_latches_disabled_and_keeps_published_frames) {
    FakeChannel* channel = new FakeChannel();

    ReplayRuntimeTestConfig config{};
    config.ipc = makeRecordingConfig();
    config.recorderChannel = channel;
    config.inlineWorker = true;
    config.recorderQueueCapacity = 8u;
    QNP_REQUIRE(startReplayRecorderForTest(config));
    QNP_CHECK(replayRecorderActive());

    Stream stream = makeStream(0u, 64u);

    uint64_t accepted = 0u;
    bool refused = false;
    for (const qreplay::ReplayFrameV1& frame : stream.frames) {
        if (replayRecordConfirmed(frame)) {
            ++accepted;
        } else {
            refused = true;
        }
    }
    QNP_CHECK(refused);
    QNP_CHECK(accepted == 8u);

    ReplayRecorderStats stats = replayRecorderStats();
    QNP_CHECK(stats.disabled == 1u);
    QNP_CHECK(stats.dropped > 0u);
    QNP_CHECK(stats.published == accepted);

    for (int i = 0; i < 8; ++i) pumpWorkerForTest();

    stats = replayRecorderStats();
    QNP_CHECK(stats.written == accepted);

    const std::vector<uint8_t> sent = channel->outbound;
    bool sawTerminal = false;
    size_t offset = 0u;
    while (offset + kIpcFrameHeaderBytes <= sent.size()) {
        IpcFrameHeader header{};
        QNP_REQUIRE(
            decodeIpcHeader(sent.data() + offset, sent.size() - offset, header));
        const size_t total = kIpcFrameHeaderBytes + header.payloadBytes;
        if (offset + total > sent.size()) break;
        if (header.type
            == static_cast<uint8_t>(IpcMessageKind::RecorderTerminal)) {
            const uint8_t* payload = sent.data() + offset + kIpcFrameHeaderBytes;
            size_t cursor = 0u;
            uint16_t reason = 0u;
            QNP_REQUIRE(qreplay::getU16(payload, header.payloadBytes, cursor,
                                        reason));
            QNP_CHECK(reason == static_cast<uint16_t>(
                                    qreplay::TerminalReason::RecorderOverflow));
            sawTerminal = true;
        }
        offset += total;
    }
    QNP_CHECK(sawTerminal);

    stopReplayRecorder();
}

QNP_TEST(recorder_publish_is_bounded_and_never_blocks) {
    FakeChannel* channel = new FakeChannel();
    ReplayRuntimeTestConfig config{};
    config.ipc = makeRecordingConfig();
    config.recorderChannel = channel;
    config.inlineWorker = true;
    config.recorderQueueCapacity = 1024u;
    QNP_REQUIRE(startReplayRecorderForTest(config));

    Stream stream = makeStream(0u, 512u);
    for (const qreplay::ReplayFrameV1& frame : stream.frames)
        QNP_CHECK(replayRecordConfirmed(frame));

    for (int i = 0; i < 8; ++i) pumpWorkerForTest();

    const ReplayRecorderStats stats = replayRecorderStats();
    QNP_CHECK(stats.disabled == 0u);
    QNP_CHECK(stats.dropped == 0u);
    QNP_CHECK(stats.published == 512u);
    QNP_CHECK(stats.written == 512u);

    stopReplayRecorder();
}

QNP_TEST(recorder_publishes_one_manifest_before_any_frame) {
    FakeChannel* channel = new FakeChannel();
    ReplayRuntimeTestConfig config{};
    config.ipc = makeRecordingConfig();
    config.ipc.localPlayer = 1u;
    config.recorderChannel = channel;
    config.inlineWorker = true;
    config.recorderQueueCapacity = 64u;
    QNP_REQUIRE(startReplayRecorderForTest(config));
    QNP_CHECK(!replayRecorderManifestPublished());

    beginAllocationWatch();
    const bool published = replayRecordManifest();
    const bool again = replayRecordManifest();
    const long allocations = endAllocationWatch();
    QNP_CHECK(published);
    QNP_CHECK(again);
    QNP_CHECK(allocations == 0);
    QNP_CHECK(replayRecorderManifestPublished());

    Stream stream = makeStream(0u, 4u);
    for (const qreplay::ReplayFrameV1& frame : stream.frames)
        QNP_CHECK(replayRecordConfirmed(frame));

    for (int i = 0; i < 8; ++i) pumpWorkerForTest();

    const ReplayRecorderStats stats = replayRecorderStats();
    QNP_CHECK(stats.manifestPublished == 1u);
    QNP_CHECK(stats.manifestBytes != 0u);
    QNP_CHECK(stats.disabled == 0u);
    QNP_CHECK(stats.dropped == 0u);
    QNP_CHECK(stats.published == 4u);
    QNP_CHECK(stats.written == 4u);

    const std::vector<uint8_t>& sent = channel->outbound;
    uint32_t manifestFrames = 0u;
    bool sawFramesFirst = false;
    qreplay::ReplayPrefix prefix{};
    qreplay::SessionManifest manifest{};
    size_t offset = 0u;
    while (offset + kIpcFrameHeaderBytes <= sent.size()) {
        IpcFrameHeader header{};
        QNP_REQUIRE(
            decodeIpcHeader(sent.data() + offset, sent.size() - offset, header));
        const size_t total = kIpcFrameHeaderBytes + header.payloadBytes;
        if (offset + total > sent.size()) break;
        const uint8_t* payload = sent.data() + offset + kIpcFrameHeaderBytes;
        if (header.type
            == static_cast<uint8_t>(IpcMessageKind::RecorderFrames)) {
            if (manifestFrames == 0u) sawFramesFirst = true;
        }
        if (header.type
            == static_cast<uint8_t>(IpcMessageKind::RecorderManifest)) {
            ++manifestFrames;
            QNP_CHECK(qreplay::ipc::decodeRecorderManifest(
                payload, header.payloadBytes, prefix, manifest));
        }
        offset += total;
    }
    QNP_CHECK(manifestFrames == 1u);
    QNP_CHECK(!sawFramesFirst);

    QNP_CHECK(manifest.contract.runtimeSimulationId
              == qipc::kRuntimeCompatibilityId);
    QNP_CHECK(manifest.contract.characterCatalogId
              == qreplay::CharacterCatalog::forSeBuild().catalogId());
    QNP_CHECK(manifest.contract.fpuControlWord == 0x027Fu);
    QNP_CHECK(manifest.contract.mxcsr == 0x1F80u);
    QNP_CHECK(manifest.bootstrapKind
              == qreplay::BootstrapKind::NetworkVsDirectBootV1);
    QNP_CHECK(manifest.display.sourceKind
              == qreplay::ReplaySourceKind::NetplayPlayer);
    QNP_CHECK(manifest.vsRow == qipc::kVsRow1v1);
    QNP_CHECK(manifest.cpuMask == 0u);
    QNP_CHECK(manifest.cpuContract == qreplay::CpuContract::None);
    for (size_t slot = 0; slot < qreplay::kFighterSlots; ++slot)
        QNP_CHECK(manifest.slotToSourceRoute[slot]
                  == static_cast<uint8_t>(slot));
    QNP_CHECK(manifest.lanes[0].active);
    QNP_CHECK(manifest.lanes[1].active);
    QNP_CHECK(!manifest.lanes[2].active);
    QNP_CHECK(!manifest.lanes[3].active);
    QNP_CHECK(manifest.slots[0].ownership
              == qreplay::SlotOwnership::RecordedHuman);
    QNP_CHECK(manifest.slots[1].ownership
              == qreplay::SlotOwnership::RecordedHuman);
    QNP_CHECK(manifest.slots[2].ownership == qreplay::SlotOwnership::Inactive);
    QNP_CHECK(manifest.slots[3].ownership == qreplay::SlotOwnership::Inactive);
    QNP_CHECK((prefix.sessionUuidLow | prefix.sessionUuidHigh) != 0ull);
    QNP_CHECK(prefix.containerMajor == qreplay::kContainerVersionMajor);

    stopReplayRecorder();
    QNP_CHECK(!replayRecorderManifestPublished());
}

namespace {

void setDisplayName(std::array<char, 32>& field, const char* text) {
    field.fill('\0');
    for (size_t i = 0; text != nullptr && text[i] != '\0' && i < 20u; ++i)
        field[i] = text[i];
}

void fillOneVsOneSlots(
    std::array<qreplay::FighterSlotDescriptor, qreplay::kFighterSlots>& slots,
    uint8_t characterZero, uint8_t characterOne) {
    for (size_t slot = 0; slot < qreplay::kFighterSlots; ++slot) {
        qreplay::FighterSlotDescriptor& descriptor = slots[slot];
        descriptor = qreplay::FighterSlotDescriptor{};
        descriptor.ownership = slot < 2u
            ? qreplay::SlotOwnership::RecordedHuman
            : qreplay::SlotOwnership::Inactive;
        descriptor.side = static_cast<uint8_t>(slot & 1u);
        descriptor.identity.nativeCharacterId =
            slot == 0u ? characterZero
                       : (slot == 1u ? characterOne
                                     : qreplay::kInactiveCharacterId);
        descriptor.identity.paletteIndex = slot < 2u
            ? static_cast<uint8_t>(slot + 1u)
            : 0u;
        descriptor.selectorProofPresent = false;
    }
}

struct WireRecord {
    uint8_t type = 0u;
    uint32_t param0 = 0u;
    uint32_t param1 = 0u;
    size_t payloadOffset = 0u;
    uint32_t payloadBytes = 0u;
};

bool collectWireRecords(const std::vector<uint8_t>& sent,
                        std::vector<WireRecord>& out) {
    out.clear();
    size_t offset = 0u;
    while (offset + kIpcFrameHeaderBytes <= sent.size()) {
        IpcFrameHeader header{};
        if (!decodeIpcHeader(sent.data() + offset, sent.size() - offset, header))
            return false;
        const size_t total = kIpcFrameHeaderBytes + header.payloadBytes;
        if (offset + total > sent.size()) return false;
        WireRecord record{};
        record.type = header.type;
        record.param0 = header.param0;
        record.param1 = header.param1;
        record.payloadOffset = offset + kIpcFrameHeaderBytes;
        record.payloadBytes = header.payloadBytes;
        out.push_back(record);
        offset += total;
    }
    return offset == sent.size();
}

int indexOfSingleRecord(const std::vector<WireRecord>& records,
                        IpcMessageKind kind, uint32_t& occurrences) {
    occurrences = 0u;
    int found = -1;
    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i].type != static_cast<uint8_t>(kind)) continue;
        ++occurrences;
        if (found < 0) found = static_cast<int>(i);
    }
    return found;
}

}

QNP_TEST(recorder_publishes_the_full_ordered_container_stream) {
    FakeChannel* channel = new FakeChannel();
    ReplayRuntimeTestConfig config{};
    config.ipc = makeRecordingConfig();
    config.recorderChannel = channel;
    config.inlineWorker = true;
    config.recorderQueueCapacity = 64u;
    QNP_REQUIRE(startReplayRecorderForTest(config));
    QNP_CHECK(!replayRecorderManifestPublished());
    QNP_CHECK(!replayRecorderSessionStartPublished());

    Stream stream = makeStream(0u, 6u);

    qreplay::DeterministicStart start = stream.start;
    start.initialPhase = 0x00020000u;
    start.bootstrapHash = 0x0BADF00D0BADF00Dull;
    start.timerMode = 1u;
    start.aiDifficulty = 2u;
    start.lifeMultiplier = 3u;
    start.roundsToWinMinusOne = 1u;
    start.playerOneNameCodePage = 932u;
    start.playerTwoNameCodePage = 932u;
    setDisplayName(start.displayNames[0], "HOST");
    setDisplayName(start.displayNames[1], "GUEST");
    start.initialDigest = kInitialDigest;

    qreplay::MatchStartRecord matchStart{};
    matchStart.matchId = 1u;
    matchStart.ordinal = 0u;
    matchStart.firstTimelineFrame = 0u;
    matchStart.dependsOnMatchId = 0u;
    matchStart.independentBootstrap = true;
    matchStart.start = start;
    fillOneVsOneSlots(matchStart.slots, 8u, 12u);
    matchStart.slotToSourceRoute = {{0u, 1u, 2u, 3u}};
    matchStart.vsRow = qipc::kVsRow1v1;
    matchStart.cpuMask = 0u;
    matchStart.rosterEpoch = 0u;

    qreplay::MatchEndRecord matchEnd{};
    matchEnd.matchId = 1u;
    matchEnd.lastTimelineFrame = 5u;
    matchEnd.winningSideMask = 0x1u;
    matchEnd.roundsSideZero = 2u;
    matchEnd.roundsSideOne = 1u;
    matchEnd.reason = qreplay::TerminalReason::Completed;
    matchEnd.postTailStateHash = stream.postTailStateHash;
    matchEnd.rollingDigest = stream.finalDigest;

    beginAllocationWatch();
    const bool manifestOk = replayRecordManifest();
    const bool sessionOk = replayRecordSessionStart(start);
    const bool sessionAgain = replayRecordSessionStart(start);
    const bool matchOk = replayRecordMatchStart(matchStart);
    bool framesOk = true;
    for (const qreplay::ReplayFrameV1& frame : stream.frames)
        framesOk = replayRecordConfirmed(frame) && framesOk;
    const bool endOk = replayRecordMatchEnd(matchEnd);
    replayRecordTerminal(
        static_cast<uint16_t>(qreplay::TerminalReason::Completed),
        stream.nextRecord, stream.postTailStateHash, stream.finalDigest);
    const long allocations = endAllocationWatch();

    QNP_CHECK(manifestOk);
    QNP_CHECK(sessionOk);
    QNP_CHECK(sessionAgain);
    QNP_CHECK(matchOk);
    QNP_CHECK(framesOk);
    QNP_CHECK(endOk);
    QNP_CHECK(allocations == 0);
    QNP_CHECK(replayRecorderSessionStartPublished());

    for (int i = 0; i < 8; ++i) pumpWorkerForTest();

    const ReplayRecorderStats stats = replayRecorderStats();
    QNP_CHECK(stats.manifestPublished == 1u);
    QNP_CHECK(stats.sessionStartPublished == 1u);
    QNP_CHECK(stats.matchesStarted == 1u);
    QNP_CHECK(stats.matchesEnded == 1u);
    QNP_CHECK(stats.disabled == 0u);
    QNP_CHECK(stats.dropped == 0u);
    QNP_CHECK(stats.published == 6u);
    QNP_CHECK(stats.written == 6u);

    std::vector<WireRecord> records;
    QNP_REQUIRE(collectWireRecords(channel->outbound, records));

    uint32_t manifestCount = 0u;
    uint32_t sessionCount = 0u;
    uint32_t matchStartCount = 0u;
    uint32_t matchEndCount = 0u;
    uint32_t terminalCount = 0u;
    const int manifestAt = indexOfSingleRecord(
        records, IpcMessageKind::RecorderManifest, manifestCount);
    const int sessionAt = indexOfSingleRecord(
        records, IpcMessageKind::RecorderSessionStart, sessionCount);
    const int matchStartAt = indexOfSingleRecord(
        records, IpcMessageKind::RecorderMatchStart, matchStartCount);
    const int matchEndAt = indexOfSingleRecord(
        records, IpcMessageKind::RecorderMatchEnd, matchEndCount);
    const int terminalAt = indexOfSingleRecord(
        records, IpcMessageKind::RecorderTerminal, terminalCount);

    QNP_CHECK(manifestCount == 1u);
    QNP_CHECK(sessionCount == 1u);
    QNP_CHECK(matchStartCount == 1u);
    QNP_CHECK(matchEndCount == 1u);
    QNP_CHECK(terminalCount == 1u);
    QNP_REQUIRE(manifestAt >= 0 && sessionAt >= 0 && matchStartAt >= 0
                && matchEndAt >= 0 && terminalAt >= 0);

    QNP_CHECK(manifestAt < sessionAt);
    QNP_CHECK(sessionAt < matchStartAt);
    QNP_CHECK(matchStartAt < matchEndAt);
    QNP_CHECK(matchEndAt < terminalAt);

    uint32_t frameRecords = 0u;
    uint32_t framedFrames = 0u;
    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i].type
            != static_cast<uint8_t>(IpcMessageKind::RecorderFrames))
            continue;
        ++frameRecords;
        framedFrames += records[i].param1;
        QNP_CHECK(static_cast<int>(i) > matchStartAt);
        QNP_CHECK(static_cast<int>(i) < matchEndAt);
    }
    QNP_CHECK(frameRecords >= 1u);
    QNP_CHECK(framedFrames == 6u);

    const uint8_t* sent = channel->outbound.data();
    qreplay::DeterministicStart decodedStart{};
    QNP_CHECK(qreplay::decodeDeterministicStart(
                  sent + records[sessionAt].payloadOffset,
                  records[sessionAt].payloadBytes, decodedStart)
              == qreplay::ParseStatus::Ok);
    QNP_CHECK(decodedStart.seedGameplay == start.seedGameplay);
    QNP_CHECK(decodedStart.seedCosmetic == start.seedCosmetic);
    QNP_CHECK(decodedStart.selector == start.selector);
    QNP_CHECK(decodedStart.initialPhase == start.initialPhase);
    QNP_CHECK(decodedStart.bootstrapHash == start.bootstrapHash);
    QNP_CHECK(decodedStart.timerMode == start.timerMode);
    QNP_CHECK(decodedStart.aiDifficulty == start.aiDifficulty);
    QNP_CHECK(decodedStart.lifeMultiplier == start.lifeMultiplier);
    QNP_CHECK(decodedStart.roundsToWinMinusOne == start.roundsToWinMinusOne);
    QNP_CHECK(decodedStart.playerOneNameCodePage == 932u);
    QNP_CHECK(decodedStart.playerTwoNameCodePage == 932u);
    QNP_CHECK(std::strcmp(decodedStart.displayNames[0].data(), "HOST") == 0);
    QNP_CHECK(std::strcmp(decodedStart.displayNames[1].data(), "GUEST") == 0);
    QNP_CHECK(decodedStart.displayNames[2][0] == '\0');
    QNP_CHECK(decodedStart.displayNames[3][0] == '\0');
    QNP_CHECK(decodedStart.firstTimelineFrame == 0u);
    QNP_CHECK(decodedStart.initialStateHash == start.initialStateHash);
    QNP_CHECK(decodedStart.initialDigest == kInitialDigest);
    QNP_CHECK(records[sessionAt].param0 == 0u);

    qreplay::MatchStartRecord decodedMatchStart{};
    QNP_CHECK(qreplay::decodeMatchStart(
                  sent + records[matchStartAt].payloadOffset,
                  records[matchStartAt].payloadBytes, decodedMatchStart)
              == qreplay::ParseStatus::Ok);
    QNP_CHECK(records[matchStartAt].param0 == 1u);
    QNP_CHECK(decodedMatchStart.matchId == 1u);
    QNP_CHECK(decodedMatchStart.ordinal == 0u);
    QNP_CHECK(decodedMatchStart.firstTimelineFrame == 0u);
    QNP_CHECK(decodedMatchStart.independentBootstrap);
    QNP_CHECK(decodedMatchStart.dependsOnMatchId == 0u);
    QNP_CHECK(decodedMatchStart.vsRow == qipc::kVsRow1v1);
    QNP_CHECK(decodedMatchStart.cpuMask == 0u);
    QNP_CHECK(decodedMatchStart.slots[0].identity.nativeCharacterId == 8u);
    QNP_CHECK(decodedMatchStart.slots[0].identity.paletteIndex == 1u);
    QNP_CHECK(decodedMatchStart.slots[1].identity.nativeCharacterId == 12u);
    QNP_CHECK(decodedMatchStart.slots[1].identity.paletteIndex == 2u);
    QNP_CHECK(decodedMatchStart.slots[2].ownership
              == qreplay::SlotOwnership::Inactive);
    QNP_CHECK(decodedMatchStart.slots[3].ownership
              == qreplay::SlotOwnership::Inactive);
    QNP_CHECK(decodedMatchStart.slots[2].identity.nativeCharacterId
              == qreplay::kInactiveCharacterId);
    QNP_CHECK(decodedMatchStart.slots[3].identity.nativeCharacterId
              == qreplay::kInactiveCharacterId);
    for (size_t slot = 0; slot < qreplay::kFighterSlots; ++slot) {
        QNP_CHECK(decodedMatchStart.slotToSourceRoute[slot]
                  == static_cast<uint8_t>(slot));
    }

    qreplay::MatchEndRecord decodedMatchEnd{};
    QNP_CHECK(qreplay::decodeMatchEnd(
                  sent + records[matchEndAt].payloadOffset,
                  records[matchEndAt].payloadBytes, decodedMatchEnd)
              == qreplay::ParseStatus::Ok);
    QNP_CHECK(records[matchEndAt].param0 == 1u);
    QNP_CHECK(decodedMatchEnd.matchId == 1u);
    QNP_CHECK(decodedMatchEnd.lastTimelineFrame == 5u);
    QNP_CHECK(decodedMatchEnd.winningSideMask == 0x1u);
    QNP_CHECK(decodedMatchEnd.roundsSideZero == 2u);
    QNP_CHECK(decodedMatchEnd.roundsSideOne == 1u);
    QNP_CHECK(decodedMatchEnd.reason == qreplay::TerminalReason::Completed);
    QNP_CHECK(decodedMatchEnd.postTailStateHash == stream.postTailStateHash);
    QNP_CHECK(decodedMatchEnd.rollingDigest == stream.finalDigest);

    qreplay::ipc::TerminalPayload decodedTerminal{};
    QNP_CHECK(qreplay::ipc::decodeTerminal(
        sent + records[terminalAt].payloadOffset,
        records[terminalAt].payloadBytes, decodedTerminal));
    QNP_CHECK(decodedTerminal.reason
              == static_cast<uint16_t>(qreplay::TerminalReason::Completed));
    QNP_CHECK(decodedTerminal.lastTimelineFrame == stream.nextRecord);
    QNP_CHECK(decodedTerminal.rollingDigest == stream.finalDigest);

    stopReplayRecorder();
    QNP_CHECK(!replayRecorderManifestPublished());
    QNP_CHECK(!replayRecorderSessionStartPublished());

    FakeChannel* second = new FakeChannel();
    ReplayRuntimeTestConfig nextConfig{};
    nextConfig.ipc = makeRecordingConfig();
    nextConfig.ipc.recordChannelGeneration = 0x13579BDFu;
    nextConfig.ipc.recordChannelNonce = 0x4E3D2C1B0AF9E8D7ull;
    nextConfig.recorderChannel = second;
    nextConfig.inlineWorker = true;
    nextConfig.recorderQueueCapacity = 64u;
    QNP_REQUIRE(startReplayRecorderForTest(nextConfig));
    QNP_CHECK(!replayRecorderManifestPublished());
    QNP_CHECK(!replayRecorderSessionStartPublished());

    const ReplayRecorderStats fresh = replayRecorderStats();
    QNP_CHECK(fresh.generation == 0x13579BDFu);
    QNP_CHECK(fresh.sessionStartPublished == 0u);
    QNP_CHECK(fresh.matchesStarted == 0u);
    QNP_CHECK(fresh.matchesEnded == 0u);
    QNP_CHECK(fresh.published == 0u);

    QNP_CHECK(replayRecordManifest());
    QNP_CHECK(replayRecordSessionStart(start));
    for (int i = 0; i < 8; ++i) pumpWorkerForTest();

    std::vector<WireRecord> secondRecords;
    QNP_REQUIRE(collectWireRecords(second->outbound, secondRecords));
    uint32_t secondManifests = 0u;
    uint32_t secondSessions = 0u;
    uint32_t secondMatches = 0u;
    (void)indexOfSingleRecord(secondRecords, IpcMessageKind::RecorderManifest,
                              secondManifests);
    (void)indexOfSingleRecord(secondRecords,
                              IpcMessageKind::RecorderSessionStart,
                              secondSessions);
    (void)indexOfSingleRecord(secondRecords, IpcMessageKind::RecorderMatchStart,
                              secondMatches);
    QNP_CHECK(secondManifests == 1u);
    QNP_CHECK(secondSessions == 1u);
    QNP_CHECK(secondMatches == 0u);
    QNP_CHECK(replayRecorderStats().matchesStarted == 0u);

    stopReplayRecorder();
}

QNP_TEST(recorder_refuses_a_match_record_before_the_session_start) {
    FakeChannel* channel = new FakeChannel();
    ReplayRuntimeTestConfig config{};
    config.ipc = makeRecordingConfig();
    config.recorderChannel = channel;
    config.inlineWorker = true;
    config.recorderQueueCapacity = 64u;
    QNP_REQUIRE(startReplayRecorderForTest(config));
    QNP_CHECK(replayRecordManifest());

    qreplay::MatchStartRecord matchStart{};
    matchStart.matchId = 1u;
    matchStart.ordinal = 0u;
    matchStart.firstTimelineFrame = 0u;
    matchStart.independentBootstrap = true;
    fillOneVsOneSlots(matchStart.slots, 8u, 12u);
    matchStart.vsRow = qipc::kVsRow1v1;

    QNP_CHECK(!replayRecordMatchStart(matchStart));
    QNP_CHECK(replayRecorderStats().disabled == 1u);
    QNP_CHECK(replayRecorderStats().matchesStarted == 0u);
    QNP_CHECK(!replayRecordSessionStart(qreplay::DeterministicStart{}));
    QNP_CHECK(!replayRecorderSessionStartPublished());

    for (int i = 0; i < 8; ++i) pumpWorkerForTest();
    std::vector<WireRecord> records;
    QNP_REQUIRE(collectWireRecords(channel->outbound, records));
    uint32_t matchStarts = 0u;
    (void)indexOfSingleRecord(records, IpcMessageKind::RecorderMatchStart,
                              matchStarts);
    QNP_CHECK(matchStarts == 0u);

    stopReplayRecorder();
}

QNP_TEST(recorder_refuses_a_session_start_before_the_manifest) {
    FakeChannel* channel = new FakeChannel();
    ReplayRuntimeTestConfig config{};
    config.ipc = makeRecordingConfig();
    config.recorderChannel = channel;
    config.inlineWorker = true;
    config.recorderQueueCapacity = 64u;
    QNP_REQUIRE(startReplayRecorderForTest(config));
    QNP_CHECK(!replayRecorderManifestPublished());

    QNP_CHECK(!replayRecordSessionStart(qreplay::DeterministicStart{}));
    QNP_CHECK(!replayRecorderSessionStartPublished());
    QNP_CHECK(replayRecorderStats().disabled == 1u);

    for (int i = 0; i < 8; ++i) pumpWorkerForTest();
    std::vector<WireRecord> records;
    QNP_REQUIRE(collectWireRecords(channel->outbound, records));
    uint32_t sessions = 0u;
    (void)indexOfSingleRecord(records, IpcMessageKind::RecorderSessionStart,
                              sessions);
    QNP_CHECK(sessions == 0u);

    stopReplayRecorder();
}

QNP_TEST(full_playback_verifies_terminal_with_no_device_calls) {
    Stream stream = makeStream(0u, 240u);
    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, true));
    replayResetDeviceCallCounter();

    uint32_t played = 0u;
    for (int guard = 0; guard < 4096 && !replayComplete(); ++guard) {
        pumpWorkerForTest();
        const ReplayTickPlan plan = replayPlanOuterSlot();
        (void)plan;
        qreplay::ReplayFrameV1 frame{};
        if (!replayNextFrame(frame)) continue;
        QNP_CHECK(replayInjectedAction(0) == frame.actions[0]);
        QNP_CHECK(replayInjectedAction(0) == frame.actions[0]);
        replayCommitFrame(frame, applyTick(fixture.game, frame));
        ++played;
    }

    QNP_CHECK(played == 240u);
    QNP_CHECK(replayComplete());
    QNP_CHECK(!replayHalted());
    QNP_CHECK(replayDeviceCallCount() == 0u);

    ReplayStatusSnapshot status{};
    replayStatus(status);
    QNP_CHECK(status.currentFrame == 240u);
    QNP_CHECK(status.rollingDigest == stream.finalDigest);
    QNP_CHECK(status.deviceCalls == 0u);
    QNP_CHECK(status.sealed == 1u);

    fixture.stop();
}

QNP_TEST(device_call_instrument_reports_a_violation) {
    replayResetDeviceCallCounter();
    QNP_CHECK(replayDeviceCallCount() == 0u);
    replayNoteDeviceCall("test.dinput.poll");
    QNP_CHECK(replayDeviceCallCount() == 1u);
    QNP_CHECK(std::strcmp(replayFirstDeviceCallSite(), "test.dinput.poll") == 0);
    replayResetDeviceCallCounter();
    QNP_CHECK(replayDeviceCallCount() == 0u);
}

QNP_TEST(ephemeral_seek_matches_linear_playback_and_never_serializes) {
    Stream stream = makeStream(0u, 200u);
    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, true, kTestGeneration, kTestNonce,
                              20u));
    QNP_REQUIRE(replayAddMatchStart(1u, 0u, 199u));

    std::map<uint32_t, uint64_t> linearState;
    linearState[0u] = fixture.game.state;

    uint32_t played = 0u;
    for (int guard = 0; guard < 4096 && played < 150u; ++guard) {
        pumpWorkerForTest();
        qreplay::ReplayFrameV1 frame{};
        if (!replayNextFrame(frame)) continue;
        replayCommitFrame(frame, applyTick(fixture.game, frame));
        linearState[fixture.game.frame] = fixture.game.state;
        ++played;
    }
    QNP_REQUIRE(played == 150u);
    QNP_CHECK(!replayHalted());

    ReplayStatusSnapshot status{};
    replayStatus(status);
    QNP_CHECK(status.snapshotCaptures > 0u);

    const uint64_t linearAt100 = linearState[100u];

    const uint32_t generation = replayPresentationGeneration();
    QNP_REQUIRE(replaySubmitPresentationCommand(
        generation, static_cast<uint32_t>(PresentationCommand::SeekFrame),
        100u));

    for (int guard = 0; guard < 4096; ++guard) {
        const ReplayTickPlan plan = replayPlanOuterSlot();
        pumpWorkerForTest();
        ReplayStatusSnapshot now{};
        replayStatus(now);
        if (now.currentFrame >= 100u && now.fastForwardRemaining == 0u
            && now.seeking == 0u)
            break;
        const uint32_t ticks =
            plan.hiddenTicks + ((plan.present && !plan.paused) ? 1u : 0u);
        for (uint32_t i = 0; i < ticks; ++i) {
            qreplay::ReplayFrameV1 frame{};
            if (!replayNextFrame(frame)) break;
            replayCommitFrame(frame, applyTick(fixture.game, frame));
        }
        if (replayHalted()) break;
    }

    QNP_CHECK(!replayHalted());
    replayStatus(status);
    QNP_CHECK(status.currentFrame == 100u);
    QNP_CHECK(status.snapshotRestores > 0u);
    QNP_CHECK(fixture.game.frame == 100u);
    QNP_CHECK(fixture.game.state == linearAt100);

    QNP_REQUIRE(fixture.channel != nullptr);
    QNP_CHECK(!containsSentinel(fixture.channel->outbound));
    QNP_CHECK(fixture.channel->outbound.size() > 0u);

    fixture.stop();
}

QNP_TEST(seek_without_a_snapshot_falls_back_to_a_match_start) {
    Stream stream = makeStream(0u, 64u);
    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, true, kTestGeneration, kTestNonce,
                              0u));
    QNP_REQUIRE(replayAddMatchStart(1u, 0u, 63u));

    uint32_t played = 0u;
    for (int guard = 0; guard < 1024 && played < 32u; ++guard) {
        pumpWorkerForTest();
        qreplay::ReplayFrameV1 frame{};
        if (!replayNextFrame(frame)) continue;
        replayCommitFrame(frame, applyTick(fixture.game, frame));
        ++played;
    }
    QNP_REQUIRE(played == 32u);

    const uint32_t generation = replayPresentationGeneration();
    QNP_CHECK(replaySubmitPresentationCommand(
        generation, static_cast<uint32_t>(PresentationCommand::SeekFrame), 8u));
    replayPlanOuterSlot();

    ReplayStatusSnapshot status{};
    replayStatus(status);
    QNP_CHECK(status.currentFrame == 0u);
    QNP_CHECK(status.snapshotRestores == 0u);
    QNP_CHECK(status.fastForwardRemaining == 8u);

    fixture.stop();
}

QNP_TEST(presentation_commands_are_bounded_and_generation_checked) {
    Stream stream = makeStream(0u, 8u);
    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, true));

    const uint32_t generation = replayPresentationGeneration();
    QNP_CHECK(replaySubmitPresentationCommand(
        generation, static_cast<uint32_t>(PresentationCommand::Pause), 0u));
    QNP_CHECK(!replaySubmitPresentationCommand(
        generation + 1u, static_cast<uint32_t>(PresentationCommand::Pause),
        0u));
    QNP_CHECK(!replaySubmitPresentationCommand(generation, 0u, 0u));
    QNP_CHECK(!replaySubmitPresentationCommand(generation, 9999u, 0u));

    uint32_t accepted = 0u;
    for (uint32_t i = 0; i < kPresentationQueueCapacity * 2u; ++i) {
        if (replaySubmitPresentationCommand(
                generation, static_cast<uint32_t>(PresentationCommand::Step),
                0u))
            ++accepted;
    }
    QNP_CHECK(accepted <= kPresentationQueueCapacity);
    QNP_CHECK(replayDroppedPresentationCommands() > 0u);

    replayPlanOuterSlot();
    ReplayStatusSnapshot status{};
    replayStatus(status);
    QNP_CHECK(status.active == 1u);

    fixture.stop();
}

QNP_TEST(collision_display_command_is_known_bounded_and_session_scoped) {
    Stream stream = makeStream(0u, 8u);
    PlaybackFixture fixture;
    QNP_REQUIRE(fixture.start(stream, true));

    const uint32_t generation = replayPresentationGeneration();
    QNP_CHECK(!replayCollisionDisplayRequested());
    QNP_CHECK(isKnownPresentationCommand(static_cast<uint32_t>(
        PresentationCommand::ToggleCollisionDisplay)));
    QNP_CHECK(isKnownPresentationCommand(static_cast<uint32_t>(
        PresentationCommand::StepBack)));
    QNP_CHECK(!isKnownPresentationCommand(
        static_cast<uint32_t>(
            PresentationCommand::StepBack) + 1u));
    QNP_REQUIRE(replaySubmitPresentationCommand(
        generation,
        static_cast<uint32_t>(
            PresentationCommand::ToggleCollisionDisplay),
        0u));
    QNP_CHECK(!replaySubmitPresentationCommand(
        generation + 1u,
        static_cast<uint32_t>(
            PresentationCommand::ToggleCollisionDisplay),
        0u));

    fixture.stop();
    QNP_CHECK(!replayCollisionDisplayRequested());
}

QNP_TEST(channel_loss_produces_a_typed_terminal_not_a_stall) {
    Stream stream = makeStream(0u, 8u);
    FakeChannel* channel = new FakeChannel();
    channel->refuseConnect = true;

    ReplayRuntimeTestConfig config{};
    config.ipc = makeReplayConfig();
    config.brokerChannel = channel;
    config.inlineWorker = true;
    QNP_REQUIRE(startReplayRuntimeForTest(config));

    for (int i = 0; i < 32; ++i) pumpWorkerForTest();

    qreplay::ReplayFrameV1 frame{};
    QNP_CHECK(!replayNextFrame(frame));
    QNP_CHECK(replayHalted());
    QNP_CHECK(replayHaltDiagnostic().sourceFault
              == static_cast<uint16_t>(SourceFault::ReconnectDeadline));

    stopReplayRuntime();
}

namespace {

uint32_t runSpeedSlots(uint32_t speedPermille, uint32_t slots,
                       uint32_t& credit, uint32_t& longestGap) {
    uint32_t ticks = 0u;
    uint32_t gap = 0u;
    longestGap = 0u;
    for (uint32_t i = 0u; i < slots; ++i) {
        const uint32_t n = replaySpeedTicksForSlotV3(speedPermille, credit);
        ticks += n;
        if (n == 0u) {
            ++gap;
            if (gap > longestGap) longestGap = gap;
        } else {
            gap = 0u;
        }
    }
    return ticks;
}

}

QNP_TEST(playback_speed_rungs_advance_the_expected_frame_counts) {
    constexpr uint32_t kSlots = 240u;
    uint32_t gap = 0u;

    uint32_t credit = replaySpeedInitialCreditV3(qreplay::kSpeedQuarter);
    QNP_CHECK(runSpeedSlots(qreplay::kSpeedQuarter, kSlots, credit, gap)
              == 60u);
    QNP_CHECK(gap == 3u);

    credit = replaySpeedInitialCreditV3(qreplay::kSpeedHalf);
    QNP_CHECK(runSpeedSlots(qreplay::kSpeedHalf, kSlots, credit, gap) == 120u);
    QNP_CHECK(gap == 1u);

    credit = replaySpeedInitialCreditV3(qreplay::kSpeedNormal);
    QNP_CHECK(runSpeedSlots(qreplay::kSpeedNormal, kSlots, credit, gap)
              == 240u);
    QNP_CHECK(gap == 0u);

    credit = replaySpeedInitialCreditV3(qreplay::kSpeedDouble);
    QNP_CHECK(runSpeedSlots(qreplay::kSpeedDouble, kSlots, credit, gap)
              == 480u);
    QNP_CHECK(gap == 0u);
    credit = replaySpeedInitialCreditV3(qreplay::kSpeedQuadruple);
    QNP_CHECK(runSpeedSlots(qreplay::kSpeedQuadruple, kSlots, credit, gap)
              == 960u);
    credit = replaySpeedInitialCreditV3(qreplay::kSpeedOctuple);
    QNP_CHECK(runSpeedSlots(qreplay::kSpeedOctuple, kSlots, credit, gap)
              == 1920u);
}

QNP_TEST(playback_speed_credit_is_periodic_and_never_drifts) {
    for (const uint32_t rung : {qreplay::kSpeedQuarter, qreplay::kSpeedHalf}) {
        const uint32_t period = qreplay::kSpeedNormal / rung;
        uint32_t credit = replaySpeedInitialCreditV3(rung);
        const uint32_t startCredit = credit;
        uint32_t gap = 0u;
        for (uint32_t cycle = 0u; cycle < 500u; ++cycle) {
            QNP_CHECK(runSpeedSlots(rung, period, credit, gap) == 1u);
            QNP_CHECK(credit == startCredit);
        }
        credit = replaySpeedInitialCreditV3(rung);
        QNP_CHECK(runSpeedSlots(rung, period * 500u, credit, gap) == 500u);
    }

    uint32_t credit = replaySpeedInitialCreditV3(333u);
    uint32_t gap = 0u;
    QNP_CHECK(runSpeedSlots(333u, 3000u, credit, gap) == 999u);

    credit = replaySpeedInitialCreditV3(qreplay::kSpeedQuarter);
    QNP_CHECK(replaySpeedTicksForSlotV3(qreplay::kSpeedQuarter, credit) == 1u);
    credit = 0u;
    QNP_CHECK(runSpeedSlots(qreplay::kSpeedQuarter, 240u, credit, gap) == 60u);

    credit = 0u;
    QNP_CHECK(runSpeedSlots(0u, 16u, credit, gap) == 16u);
}

QNP_TEST(a_slow_motion_slot_is_distinguishable_from_a_viewer_pause) {
    ReplayTickPlan plan{};
    QNP_CHECK(!plan.slowMotionHold);
    QNP_CHECK(!plan.takeoverCountdownHold);
    QNP_CHECK(!plan.paused);
    QNP_CHECK(!plan.present);
    QNP_CHECK(plan.hiddenTicks == 0u);
}

namespace {

struct SteppingViewer {
    bool paused = false;
    uint32_t pendingSteps = 0u;
    uint32_t speedPermille = qreplay::kSpeedNormal;
    uint32_t credit = 0u;
    uint32_t nextRecord = 0u;
    uint32_t endpointRecord = 0u;

    void pressStep() {
        paused = true;
        if (pendingSteps != 0xFFFFFFFFu) ++pendingSteps;
    }
    void pressTogglePause() {
        if (paused) {
            paused = false;
            pendingSteps = 0u;
        } else {
            paused = true;
        }
    }

    uint32_t runSlot() {
        const uint32_t ticks = replayTransportTicksForSlotV3(
            paused, pendingSteps, speedPermille, credit,
            endpointRecord - nextRecord);
        nextRecord += ticks;
        return ticks;
    }
};

}

QNP_TEST(a_step_while_already_paused_advances_one_frame_and_stays_paused) {
    SteppingViewer viewer{};
    viewer.endpointRecord = 8u;

    viewer.pressTogglePause();
    QNP_CHECK(viewer.paused);
    for (int held = 0; held < 32; ++held) {
        QNP_CHECK(viewer.runSlot() == 0u);
        QNP_CHECK(viewer.nextRecord == 0u);
        QNP_CHECK(viewer.paused);
    }

    viewer.pressStep();
    QNP_CHECK(viewer.pendingSteps == 1u);
    QNP_CHECK(viewer.runSlot() == 1u);
    QNP_CHECK(viewer.nextRecord == 1u);
    QNP_CHECK(viewer.paused);
    QNP_CHECK(viewer.pendingSteps == 0u);

    for (int held = 0; held < 16; ++held) {
        QNP_CHECK(viewer.runSlot() == 0u);
        QNP_CHECK(viewer.nextRecord == 1u);
        QNP_CHECK(viewer.paused);
    }

    SteppingViewer playing{};
    playing.endpointRecord = 8u;
    QNP_CHECK(!playing.paused);
    playing.pressStep();
    QNP_CHECK(playing.paused);
    QNP_CHECK(playing.runSlot() == 1u);
    QNP_CHECK(playing.nextRecord == 1u);
    QNP_CHECK(playing.paused);
    QNP_CHECK(playing.runSlot() == 0u);
    QNP_CHECK(playing.nextRecord == 1u);
}

QNP_TEST(queued_steps_spend_one_slot_each_and_never_burst) {
    SteppingViewer viewer{};
    viewer.endpointRecord = 8u;
    viewer.pressTogglePause();

    for (int press = 0; press < 4; ++press) viewer.pressStep();
    QNP_CHECK(viewer.pendingSteps == 4u);
    for (uint32_t expected = 1u; expected <= 4u; ++expected) {
        QNP_CHECK(viewer.runSlot() == 1u);
        QNP_CHECK(viewer.nextRecord == expected);
        QNP_CHECK(viewer.paused);
    }
    QNP_CHECK(viewer.pendingSteps == 0u);
    QNP_CHECK(viewer.runSlot() == 0u);
    QNP_CHECK(viewer.nextRecord == 4u);

    viewer.pressStep();
    viewer.pressStep();
    viewer.pressTogglePause();
    QNP_CHECK(!viewer.paused);
    QNP_CHECK(viewer.pendingSteps == 0u);

    SteppingViewer walk{};
    walk.endpointRecord = 6u;
    walk.pressTogglePause();
    uint32_t frames = 0u;
    for (int slot = 0; slot < 64 && walk.nextRecord < walk.endpointRecord;
         ++slot) {
        walk.pressStep();
        frames += walk.runSlot();
        QNP_CHECK(walk.paused);
    }
    QNP_CHECK(frames == 6u);
    QNP_CHECK(walk.nextRecord == walk.endpointRecord);
}

QNP_TEST(a_step_with_no_frame_left_to_spend_is_not_consumed) {
    SteppingViewer viewer{};
    viewer.endpointRecord = 2u;
    viewer.pressTogglePause();
    QNP_CHECK(viewer.runSlot() == 0u);

    viewer.pressStep();
    QNP_CHECK(viewer.runSlot() == 1u);
    viewer.pressStep();
    QNP_CHECK(viewer.runSlot() == 1u);
    QNP_CHECK(viewer.nextRecord == viewer.endpointRecord);

    viewer.pressStep();
    QNP_CHECK(viewer.pendingSteps == 1u);
    for (int slot = 0; slot < 8; ++slot) {
        QNP_CHECK(viewer.runSlot() == 0u);
        QNP_CHECK(viewer.pendingSteps == 1u);
        QNP_CHECK(viewer.nextRecord == viewer.endpointRecord);
        QNP_CHECK(viewer.paused);
    }

    uint32_t pending = 0u;
    uint32_t credit = 0u;
    QNP_CHECK(replayTransportTicksForSlotV3(
                  false, pending, qreplay::kSpeedDouble, credit, 1u) == 1u);
    credit = 0u;
    QNP_CHECK(replayTransportTicksForSlotV3(
                  false, pending, qreplay::kSpeedDouble, credit, 8u) == 2u);
    credit = 0u;
    QNP_CHECK(replayTransportTicksForSlotV3(
                  false, pending, qreplay::kSpeedDouble, credit, 0u) == 0u);
    QNP_CHECK(pending == 0u);
}
