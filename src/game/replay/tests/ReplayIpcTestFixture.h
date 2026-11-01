#pragma once
#include "game/replay/ReplayIpcWorker.h"
#include "replay/ReplayIntegrity.h"
#include "replay/ReplaySimulationContractV2.h"
#include "replay/StageCatalog.h"
#include <cstring>

namespace replay_ipc_test {
namespace wire = qreplay::ipc::v3;
using qgame::replay::ReplayIpcOptions;
using qgame::replay::ReplayIpcWorker;

class MemoryChannel final : public qgame::ByteChannel {
public:
    bool connect(uint32_t) override { open = true; return true; }
    void close() override { open = false; }
    bool connected() const override { return open; }
    bool read(uint8_t* dst, size_t cap, size_t& got) override {
        if (failRead) return false;
        got = (std::min)(cap, incoming.size() - position);
        if (got) std::memcpy(dst, incoming.data() + position, got);
        position += got;
        return true;
    }
    bool write(const uint8_t* src, size_t size) override {
        if (failWrite) return false;
        outgoing.insert(outgoing.end(), src, src + size);
        return true;
    }
    bool open = false, failRead = false, failWrite = false;
    size_t position = 0u;
    std::vector<uint8_t> incoming, outgoing;
};

struct Game {
    qreplay::ConfirmedGameStartV2 start{};
    qreplay::ConfirmedGameFrameV2 frame{};
    std::array<qreplay::ConfirmedGameEventV2, 2> events{};
    wire::PlaybackSessionInfoPayload info{};

    explicit Game(uint32_t generation = 1u) {
        using namespace qreplay;
        auto& m = start.manifest;
        m.identity.gameGeneration = generation;
        m.identity.gameUuidLow = generation + 100u;
        auto& c = m.contract;
        std::array<uint8_t, 32> contentRoot;
        contentRoot.fill(7u);
        c = makeSeSimulationContractV2(qipc::kRuntimeCompatibilityId,
            supportedSeExecutableSha256V2(), contentRoot);
        m.lanes[0].active = m.lanes[1].active = true;
        for (size_t i = 0u; i < m.slots.size(); ++i)
            m.slots[i].side = static_cast<uint8_t>(i & 1u);
        m.slots[0].ownership = m.slots[1].ownership = SlotOwnership::RecordedHuman;
        m.slots[0].identity.nativeCharacterId = 1u;
        m.slots[1].identity.nativeCharacterId = 8u;
        StageCatalogV2::forSeBuild().makeIdentity(1u, m.stage);
        start.start.frameZeroPreStateHash = 100u;
        start.start.bootstrapFingerprint = 99u;
        start.start.initialRollingDigest = initialReplayDigestV2(start);
        uint64_t digest = start.start.initialRollingDigest;
        for (size_t i = 0u; i < events.size(); ++i) {
            auto& e = events[i];
            e.kind = i == 0u ? GameEventKindV2::GameStarted : GameEventKindV2::RoundStarted;
            e.phase = GameEventPhaseV2::PreFrame;
            e.eventSequence = static_cast<uint32_t>(i);
            e.stateProof = 100u;
            if (i != 0u) { e.transitionSiteId = 1u; e.roundOrdinal = 1u; }
            e.rollingDigest = advanceReplayDigestV2(digest, e);
            digest = e.rollingDigest;
        }
        frame.core.activeLaneMask = 3u;
        frame.core.before.scene = frame.core.before.requestedScene = 2u;
        frame.core.after = frame.core.before;
        frame.core.after.matchSub = 1;
        frame.core.dispatcherResult = 1;
        frame.core.preStateHash = 100u;
        frame.core.postStateHash = 200u;
        frame.rollingDigest = advanceReplayDigestV2(digest, frame);
        info.frameCount = 1u;
        info.eventCount = 2u;
        info.activeLaneMask = info.activeFighterMask = 3u;
        info.validatedSourceByteCount = 1000u;
        info.validatedSourceRangeDigest.fill(static_cast<uint8_t>(generation));
        info.recordedContract = c;
        info.endpoint.kind = wire::PlaybackEndpointKind::UnsealedValidatedPrefix;
        info.endpoint.nextRecord = 1u;
        info.endpoint.postTailStateHash = 200u;
        info.endpoint.finalRollingDigest = frame.rollingDigest;
        info.endpoint.validatedSourceByteCount = info.validatedSourceByteCount;
        info.endpoint.validatedSourceRangeDigest = info.validatedSourceRangeDigest;
    }

    ReplayIpcOptions options() const {
        ReplayIpcOptions o{};
        o.gamePid = 123u;
        o.replayGeneration = 5u;
        o.brokerNonce = 6u;
        o.runtimeCompatibilityId = start.manifest.contract.runtimeSimulationId;
        o.playback = o.playbackV3 = true;
        o.schemaVersion = wire::kWireVersion;
        o.featureBits = wire::kPlaybackRequiredFeaturesV3;
        o.playbackGame = start.manifest.identity;
        o.playbackExpectedFrameCount = info.frameCount;
        o.playbackExpectedEventCount = info.eventCount;
        o.playbackValidatedSourceByteCount = info.validatedSourceByteCount;
        o.playbackValidatedSourceRangeDigest = info.validatedSourceRangeDigest;
        o.playbackEndpointKind = static_cast<uint8_t>(info.endpoint.kind);
        o.playbackRuntimeContract = start.manifest.contract;
        return o;
    }

    wire::PlaybackNextGamePayload next(uint64_t switchId) const {
        wire::PlaybackNextGamePayload n{};
        n.switchId = switchId;
        n.game = start.manifest.identity;
        n.frameCount = info.frameCount;
        n.eventCount = info.eventCount;
        n.validatedSourceByteCount = info.validatedSourceByteCount;
        n.validatedSourceRangeDigest = info.validatedSourceRangeDigest;
        n.endpointKind = info.endpoint.kind;
        n.presentationFlags = 1u;
        n.runtimeContract = start.manifest.contract;
        return n;
    }

    wire::PlaybackResultPayload result() const {
        wire::PlaybackResultPayload r{};
        r.reason = wire::PlaybackResultReason::VerifiedUnsealedPrefix;
        r.endpointKind = info.endpoint.kind;
        r.validatedSourceByteCount = info.validatedSourceByteCount;
        r.validatedSourceRangeDigest = info.validatedSourceRangeDigest;
        r.runtimeContract = start.manifest.contract;
        r.nextRecordConsumed = 1u;
        r.finalRollingDigest = frame.rollingDigest;
        r.postTailStateHash = 200u;
        return r;
    }
};

inline bool feed(MemoryChannel& channel, uint32_t& sequence,
                 const qreplay::GameIdentityV2& game, wire::MessageType type,
                 const std::vector<uint8_t>& payload, uint32_t p0 = 0u, uint32_t p1 = 0u) {
    wire::Frame f{};
    f.channel = wire::Channel::Playback;
    f.type = type;
    f.launchGeneration = 5u;
    f.launchNonce = 6u;
    f.sequence = ++sequence;
    f.game = game;
    f.param0 = p0;
    f.param1 = p1;
    f.payload = payload;
    return wire::encodeFrame(f, channel.incoming);
}

inline bool feedGame(MemoryChannel& channel, uint32_t& sequence, const Game& game,
                     const qreplay::ReplayPaletteV2* palette = nullptr) {
    std::vector<uint8_t> p;
    const auto& id = game.start.manifest.identity;
    if (!wire::encodePlaybackSessionInfo(game.info, p)
        || !feed(channel, sequence, id, wire::MessageType::PlaybackSessionInfo, p)) return false;
    p.clear();
    if (!wire::encodePlaybackGameStart(game.start, p)
        || !feed(channel, sequence, id, wire::MessageType::PlaybackGameStart, p)) return false;
    p.clear();
    if (palette) {
        if (!wire::encodePlaybackPalette(*palette, p)
            || !feed(channel, sequence, id, wire::MessageType::PlaybackPalette, p)) return false;
        p.clear();
    }
    if (!wire::encodePlaybackFrames(&game.frame, 1u, p)
        || !feed(channel, sequence, id, wire::MessageType::PlaybackFrames, p, 0u, 1u)) return false;
    p.clear();
    if (!wire::encodePlaybackEvents(game.events.data(), game.events.size(), p)
        || !feed(channel, sequence, id, wire::MessageType::PlaybackEvents, p, 0u, 2u)) return false;
    p.clear();
    return wire::encodePlaybackEndpoint(game.info.endpoint, p)
        && feed(channel, sequence, id, wire::MessageType::PlaybackEndpoint, p, 1u, 2u);
}

inline std::vector<wire::Frame> readOutput(const MemoryChannel& channel) {
    std::vector<wire::Frame> frames;
    size_t offset = 0u;
    while (offset < channel.outgoing.size()) {
        wire::Frame f;
        size_t consumed = 0u;
        if (wire::decodeFrame(channel.outgoing.data() + offset,
                             channel.outgoing.size() - offset, f, consumed)
            != wire::FrameStatus::Ok) return {};
        frames.push_back(std::move(f));
        offset += consumed;
    }
    return frames;
}
}
