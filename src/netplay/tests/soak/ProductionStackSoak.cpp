#include "ProductionStackSoak.h"

#include "ToySimulation.h"
#include "../../NetplayCore.h"
#include "../../../spectator/SpectatorArchive.h"
#include "../../../spectator/SpectatorSession.h"
#include "../../../game/SpectatorPlaybackPolicy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace qnetplay::soak {

namespace {

namespace spec = qnetplay::spectator;

constexpr uint64_t kFrameDurationUs = 16500u;
constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001B3ull;
constexpr uint64_t kCompatibilityId = 0x514F483939505234ull;
constexpr uint64_t kSimulationContract = 0x514F48393953494Dull;
constexpr size_t kMaximumViewers = 32u;

uint64_t splitMix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31u);
}

void digestByte(uint64_t& digest, const uint8_t value) {
    digest ^= value;
    digest *= kFnvPrime;
}

void digestU16(uint64_t& digest, const uint16_t value) {
    digestByte(digest, static_cast<uint8_t>(value));
    digestByte(digest, static_cast<uint8_t>(value >> 8u));
}

void digestU32(uint64_t& digest, const uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        digestByte(digest, static_cast<uint8_t>(value >> shift));
}

void digestU64(uint64_t& digest, const uint64_t value) {
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        digestByte(digest, static_cast<uint8_t>(value >> shift));
}

void digestBytes(uint64_t& digest, const std::vector<uint8_t>& bytes) {
    digestU64(digest, bytes.size());
    for (const uint8_t value : bytes) digestByte(digest, value);
}

bool networkStatsEqual(const NetworkStats& left,
                       const NetworkStats& right) {
    return left.logicalPackets == right.logicalPackets
        && left.scheduledPackets == right.scheduledPackets
        && left.deliveredPackets == right.deliveredPackets
        && left.droppedPackets == right.droppedPackets
        && left.duplicatedPackets == right.duplicatedPackets
        && left.reorderedPackets == right.reorderedPackets
        && left.forcedDropPackets == right.forcedDropPackets
        && left.blackoutDropPackets == right.blackoutDropPackets
        && left.maxPendingPackets == right.maxPendingPackets;
}

bool rollbackStatsEqual(const RollbackStats& left,
                        const RollbackStats& right) {
    return left.uniqueCaptures == right.uniqueCaptures
        && left.repeatedCaptureCalls == right.repeatedCaptureCalls
        && left.localSubmissions == right.localSubmissions
        && left.visibleAdvances == right.visibleAdvances
        && left.predictedAdvances == right.predictedAdvances
        && left.replayedFrames == right.replayedFrames
        && left.rollbackCount == right.rollbackCount
        && left.rollbackFrames == right.rollbackFrames
        && left.mismatchingRemoteInputs
            == right.mismatchingRemoteInputs
        && left.confirmedFrames == right.confirmedFrames
        && left.stallPolls == right.stallPolls
        && left.stallEpisodes == right.stallEpisodes
        && left.stallPollsByReason == right.stallPollsByReason
        && left.stallEpisodesByReason == right.stallEpisodesByReason
        && left.remoteInputResults == right.remoteInputResults
        && left.remoteOutOfOrderInputs
            == right.remoteOutOfOrderInputs
        && left.remoteHolePrefixAdvances
            == right.remoteHolePrefixAdvances
        && left.maxRollbackDepth == right.maxRollbackDepth
        && left.maxSpeculativeFrames == right.maxSpeculativeFrames
        && left.maxConsecutiveStallPolls
            == right.maxConsecutiveStallPolls
        && left.maxRemoteHoleSpan == right.maxRemoteHoleSpan;
}

bool rollbackStatsInternallyConsistent(const RollbackStats& stats) {
    uint64_t stallPollSum = 0u;
    for (const uint64_t value : stats.stallPollsByReason)
        stallPollSum += value;
    uint64_t stallEpisodeSum = 0u;
    for (const uint64_t value : stats.stallEpisodesByReason)
        stallEpisodeSum += value;
    const size_t noStall = static_cast<size_t>(StallReason::None);
    const size_t inserted =
        static_cast<size_t>(RemoteInputResult::Inserted);
    for (size_t index = 0u;
         index < stats.remoteInputResults.size(); ++index) {
        if (index != inserted && stats.remoteInputResults[index] != 0u)
            return false;
    }
    return stallPollSum == stats.stallPolls
        && stallEpisodeSum == stats.stallEpisodes
        && stats.stallPollsByReason[noStall] == 0u
        && stats.stallEpisodesByReason[noStall] == 0u
        && stats.predictedAdvances <= stats.visibleAdvances
        && stats.confirmedFrames <= stats.visibleAdvances
        && stats.replayedFrames <= stats.rollbackFrames
        && stats.maxConsecutiveStallPolls <= stats.stallPolls;
}

bool sameHash(const wire::SyncHashPayload& left,
              const wire::SyncHashPayload& right) {
    return left.tag == right.tag
        && left.rngGameplay == right.rngGameplay
        && left.rngCosmetic == right.rngCosmetic
        && left.rngSelector == right.rngSelector
        && left.hp0 == right.hp0
        && left.hp1 == right.hp1
        && left.gameplayHash == right.gameplayHash;
}

uint16_t expectedInput(const uint64_t inputSeed,
                       const uint8_t player,
                       const uint32_t frame,
                       const uint8_t delay) {
    return delayedScriptedInput(inputSeed, player, frame, delay);
}

uint16_t capturedSourceInput(const uint64_t inputSeed,
                             const uint8_t player,
                             const uint32_t sourceFrame,
                             const uint8_t delay) {
    return delayedScriptedInput(
        inputSeed, player,
        sourceFrame + static_cast<uint32_t>(delay), delay);
}

qnetplay::compat::CompatibilityIdentityWire soakCompatibilityIdentity() {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = wire::kVersion;
    (void)qnetplay::compat::setProfileId(identity, "qoh99-se1126");
    for (size_t index = 0u;
         index < qnetplay::compat::kCompatibilitySha256Bytes; ++index) {
        identity.runtimeBuild.executableSha256[index] =
            static_cast<uint8_t>(0x21u + index);
        identity.simulationContent.simulationRootSha256[index] =
            static_cast<uint8_t>(0x61u + index);
        identity.simulationContent.validationRootSha256[index] =
            static_cast<uint8_t>(0xA1u + index);
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

CoreConfig playerConfig(const bool host,
                         const Endpoint& expectedPeer,
                         const uint64_t sessionId,
                        const wire::SessionCookie& cookie) {
    CoreConfig config{};
    config.localRole = host
        ? wire::PeerRole::Host : wire::PeerRole::Client;
    config.localPlayer = host ? 0u : 1u;
    config.compatibilityId = kCompatibilityId;
    config.compatibilityIdentity = soakCompatibilityIdentity();
    config.sessionId = sessionId;
    config.cookie = cookie;
    config.expectedPeer = expectedPeer;
    config.learnPeerPort = false;
    config.helloResendMs = 50u;
    config.inputResendMs = 33u;
    config.readyResendMs = 50u;
    config.hashResendMs = 50u;
    config.pingIntervalMs = 500u;
    config.disconnectResendMs = 50u;
    config.livenessTimeoutMs = 15000u;
    config.maxPastFrames = 1023u;
    config.maxFutureFrames = 120u;
    return config;
}

struct ViewerSlot {
    spec::ViewerId id = 0u;
    bool overflowProbe = false;
    bool resumeCandidate = false;
    bool resumeRequested = false;
    bool catchingUp = false;
    uint32_t resumeCursor = 0u;
    std::unique_ptr<spec::SpectatorViewer> viewer;
    std::unique_ptr<DeterministicNetwork> link;
    ToySimulation playback;

    ViewerSlot(const spec::ViewerId viewerId,
               const bool overflow,
               const bool canResume,
               const spec::ViewerConfig& config,
               const uint64_t linkSeed,
               const LinkFaults& viewerToSource,
               const LinkFaults& sourceToViewer,
               const uint32_t simulationSeed)
        : id(viewerId),
          overflowProbe(overflow),
          resumeCandidate(canResume),
          viewer(std::make_unique<spec::SpectatorViewer>(config)),
          link(std::make_unique<DeterministicNetwork>(
              linkSeed, viewerToSource, sourceToViewer)),
          playback(simulationSeed) {
        link->setDecisionRetention(false);
    }
};

struct ProductionSnapshot {
    uint32_t frame = 0u;
    ToyState state{};
    bool valid = false;
};

class ProductionRollbackPeer {
public:
    ProductionRollbackPeer(const uint8_t player,
                           const uint8_t delay,
                           const uint8_t rollback,
                           const uint64_t inputSeed,
                           const uint32_t simulationSeed)
        : _player(player),
          _delay(delay),
          _rollback(rollback),
          _inputSeed(inputSeed),
          _session(makeConfig(player, delay, rollback)),
          _simulation(simulationSeed),
          _snapshots(4096u),
          _executionDigest(kFnvOffset) {
    }

    bool valid() const { return _session.valid(); }
    uint32_t frontier() const { return _session.currentFrame(); }
    bool pendingRollback() const {
        return _session.hasPendingRollback();
    }

    bool prepare(NetplayCore& core,
                 const uint32_t targetFrames,
                 std::string& error) {
        if (!core.setSimulationFrame(frontier())) {
            error = "production core rejected rollback frontier";
            return false;
        }
        if (frontier() < targetFrames) {
            const uint16_t captured = capturedSourceInput(
                _inputSeed, _player, frontier(), _delay);
            if (!_session.captureLocalInput(frontier(), captured)) {
                error = "production rollback local capture rejected";
                return false;
            }
        }
        TaggedInput submission{};
        while (_session.popLocalSubmission(submission)) {
            const uint16_t expected = expectedInput(
                _inputSeed, _player, submission.frame, _delay);
            if (submission.value != expected
                || !core.submitLocalInput(
                    submission.frame, submission.value)) {
                error =
                    "RollbackSession -> NetplayCore submission diverged";
                return false;
            }
        }
        return true;
    }

    bool ingest(NetplayCore& core, std::string& error) {
        uint16_t value = 0u;
        while (core.getRemoteInput(_nextRemoteIngest, value)) {
            const RemoteInputResult received =
                _session.receiveRemoteInput(_nextRemoteIngest, value);
            if (received != RemoteInputResult::Inserted) {
                std::ostringstream stream;
                stream << "NetplayCore -> RollbackSession input rejected "
                       << _nextRemoteIngest << " result "
                       << static_cast<unsigned>(received);
                error = stream.str();
                return false;
            }
            ++_nextRemoteIngest;
        }
        return true;
    }

    bool step(const uint32_t targetFrames, std::string& error) {
        if (frontier() >= targetFrames && !pendingRollback())
            return true;

        const FrameAction action = _session.nextAction();
        digestByte(_executionDigest,
                   static_cast<uint8_t>(action.kind));
        digestByte(_executionDigest,
                   static_cast<uint8_t>(action.stallReason));
        digestU32(_executionDigest, action.frame);
        if (action.kind == ActionKind::Stall) {
            ++_consecutiveStalls;
            _maxConsecutiveStalls =
                std::max(_maxConsecutiveStalls, _consecutiveStalls);
            if (_maxConsecutiveStalls > 1024u) {
                error = "production rollback stall bound exceeded";
                return false;
            }
            return true;
        }
        _consecutiveStalls = 0u;

        if (action.kind == ActionKind::Rollback)
            return rollback(action, error);
        if (action.kind != ActionKind::Advance
            || action.inputs.frame != frontier()
            || _simulation.state().frame != frontier()) {
            error = "production rollback advance action invalid";
            return false;
        }

        ProductionSnapshot& snapshot =
            _snapshots[action.inputs.frame % _snapshots.size()];
        snapshot.frame = action.inputs.frame;
        snapshot.state = _simulation.state();
        snapshot.valid = true;
        const uint64_t preHash = _simulation.hash();
        digestU16(_executionDigest, action.inputs.p1);
        digestU16(_executionDigest, action.inputs.p2);
        digestByte(_executionDigest,
                   action.inputs.remotePredicted ? 1u : 0u);
        digestU64(_executionDigest, preHash);
        _simulation.step(action.inputs.p1, action.inputs.p2);
        digestU64(_executionDigest, _simulation.hash());
        if (!_session.commitAdvance(action.inputs, preHash)) {
            error = "production rollback advance commit rejected";
            return false;
        }
        return _simulation.state().frame == frontier();
    }

    bool popConfirmed(ConfirmedFrame& out) {
        return _session.popConfirmedFrame(out);
    }
    const RollbackStats& stats() const { return _session.stats(); }
    const ToyState& state() const { return _simulation.state(); }
    uint64_t hash() const { return _simulation.hash(); }
    uint64_t executionDigest() const { return _executionDigest; }
    uint32_t nextRemoteIngest() const { return _nextRemoteIngest; }
    uint32_t maxConsecutiveStalls() const {
        return _maxConsecutiveStalls;
    }

private:
    static RollbackConfig makeConfig(const uint8_t player,
                                     const uint8_t delay,
                                     const uint8_t rollback) {
        RollbackConfig config{};
        config.localPlayer = player;
        config.inputDelayFrames = delay;
        config.maxRollbackFrames = rollback;
        config.neutralInput = 0u;
        config.firstFrame = 0u;
        config.maxRemoteFutureFrames = 120u;
        config.historyCapacity = 4096u;
        return config;
    }

    bool rollback(const FrameAction& action, std::string& error) {
        if (action.rollbackFrom >= action.replayUntilExclusive
            || action.replayUntilExclusive != frontier()
            || action.replayUntilExclusive - action.rollbackFrom
                > _rollback) {
            error = "production rollback range exceeded bound";
            return false;
        }
        ProductionSnapshot& restore =
            _snapshots[action.rollbackFrom % _snapshots.size()];
        if (!restore.valid || restore.frame != action.rollbackFrom
            || !_session.beginRollback(
                action.rollbackFrom,
                action.replayUntilExclusive)) {
            error = "production rollback restore transaction invalid";
            return false;
        }
        _simulation.restore(restore.state);
        for (uint32_t frame = action.rollbackFrom;
             frame < action.replayUntilExclusive;
             ++frame) {
            CanonicalInputs inputs{};
            if (!_session.inputsForFrame(frame, inputs)
                || _simulation.state().frame != frame) {
                error = "production rollback replay input unavailable";
                return false;
            }
            ProductionSnapshot& snapshot =
                _snapshots[frame % _snapshots.size()];
            snapshot.frame = frame;
            snapshot.state = _simulation.state();
            snapshot.valid = true;
            const uint64_t preHash = _simulation.hash();
            digestU32(_executionDigest, frame);
            digestU16(_executionDigest, inputs.p1);
            digestU16(_executionDigest, inputs.p2);
            digestByte(_executionDigest,
                       inputs.remotePredicted ? 1u : 0u);
            digestU64(_executionDigest, preHash);
            _simulation.step(inputs.p1, inputs.p2);
            digestU64(_executionDigest, _simulation.hash());
            if (!_session.commitReplayFrame(inputs, preHash)) {
                error = "production rollback replay commit rejected";
                return false;
            }
        }
        if (!_session.finishRollback()) {
            error = "production rollback transaction did not finish";
            return false;
        }
        return _simulation.state().frame == frontier();
    }

    uint8_t _player = 0u;
    uint8_t _delay = 0u;
    uint8_t _rollback = 0u;
    uint64_t _inputSeed = 0u;
    RollbackSession _session;
    ToySimulation _simulation;
    std::vector<ProductionSnapshot> _snapshots;
    uint32_t _nextRemoteIngest = 0u;
    uint32_t _consecutiveStalls = 0u;
    uint32_t _maxConsecutiveStalls = 0u;
    uint64_t _executionDigest = kFnvOffset;
};

bool acceptablePlayerIngress(const IngressResult result,
                             const ScheduledPacket& packet,
                             const NetplayCore& destination,
                             const bool terminalStarted) {
    if (result == IngressResult::Accepted) return true;
    if (result == IngressResult::UnexpectedMessage
        && !destination.isEstablished()) {
        return true;
    }
    if (result == IngressResult::InvalidControl
        && packet.type == wire::MessageType::Pong) {
        return true;
    }
    return terminalStarted && result == IngressResult::Terminal;
}

bool acceptableSourceIngress(const spec::IngressResult result,
                             const ViewerSlot& slot) {
    if (result == spec::IngressResult::Accepted
        || result == spec::IngressResult::StaleAck
        || result == spec::IngressResult::UnknownViewer
        || result == spec::IngressResult::InitMismatch
        || result == spec::IngressResult::UnexpectedMessage) {
        return true;
    }
    if (result == spec::IngressResult::ReplayedSequence
        || result == spec::IngressResult::StaleGeneration) {
        return true;
    }
    if (slot.overflowProbe
        && result == spec::IngressResult::CapacityReached) {
        return true;
    }
    return false;
}

bool acceptableViewerIngress(const spec::IngressResult result,
                             const ViewerSlot& slot) {
    if (result == spec::IngressResult::Accepted
        || result == spec::IngressResult::InitMismatch) {
        return true;
    }
    if (result == spec::IngressResult::ReplayedSequence
        || result == spec::IngressResult::StaleGeneration) {
        return true;
    }
    if (slot.resumeRequested
        && result == spec::IngressResult::UnexpectedMessage) {
        return true;
    }
    return slot.overflowProbe
        && slot.viewer->phase() == spec::ViewerPhase::Rejected
        && result == spec::IngressResult::UnexpectedMessage;
}

wire::SyncHashPayload makeSyncHash(const ToyState& state,
                                   const uint64_t stateHash,
                                   const uint32_t frame) {
    wire::SyncHashPayload hash{};
    hash.tag.layoutGeneration = 1u;
    hash.tag.epoch = 1u;
    hash.tag.frame = frame;
    hash.rngGameplay = state.rng;
    hash.rngCosmetic = state.arena[0];
    hash.rngSelector = state.arena[1];
    hash.hp0 = static_cast<int32_t>(state.health[0]);
    hash.hp1 = static_cast<int32_t>(state.health[1]);
    hash.gameplayHash = stateHash;
    return hash;
}

void digestHash(uint64_t& digest, const wire::SyncHashPayload& hash) {
    digestU32(digest, hash.tag.layoutGeneration);
    digestU32(digest, hash.tag.epoch);
    digestU32(digest, hash.tag.frame);
    digestU32(digest, hash.rngGameplay);
    digestU32(digest, hash.rngCosmetic);
    digestU32(digest, hash.rngSelector);
    digestU32(digest, static_cast<uint32_t>(hash.hp0));
    digestU32(digest, static_cast<uint32_t>(hash.hp1));
    digestU64(digest, hash.gameplayHash);
}

bool decodeHashAckTag(const ScheduledPacket& packet,
                      const uint64_t sessionId,
                      const wire::SessionCookie& cookie,
                      wire::SyncHashTag& tag) {
    wire::Datagram datagram{};
    wire::SyncHashAckPayload ack{};
    if (wire::decodeDatagram(
            packet.bytes.data(), packet.bytes.size(),
            sessionId, cookie, datagram) != wire::DecodeError::None
        || datagram.header.type != wire::MessageType::SyncHashAck
        || !wire::decodePayload(datagram.payload, ack)) {
        return false;
    }
    tag = ack.tag;
    return true;
}

}

bool productionFingerprintsEqual(
    const ProductionStackFingerprint& left,
    const ProductionStackFingerprint& right,
    std::string& differingField) {
#define CHECK_PRODUCTION_FIELD(field) \
    do { \
        if (left.field != right.field) { \
            differingField = #field; \
            return false; \
        } \
    } while (false)
    CHECK_PRODUCTION_FIELD(playerNetworkDigest);
    CHECK_PRODUCTION_FIELD(playerExecutionDigest);
    CHECK_PRODUCTION_FIELD(remoteInputDigestA);
    CHECK_PRODUCTION_FIELD(remoteInputDigestB);
    CHECK_PRODUCTION_FIELD(remoteHashDigestA);
    CHECK_PRODUCTION_FIELD(remoteHashDigestB);
    CHECK_PRODUCTION_FIELD(archiveDigest);
    CHECK_PRODUCTION_FIELD(finalStateHash);
    CHECK_PRODUCTION_FIELD(layoutBarrierDoneTick);
    CHECK_PRODUCTION_FIELD(firstGameplayTick);
    CHECK_PRODUCTION_FIELD(playerDoneTick);
    CHECK_PRODUCTION_FIELD(peerAckA);
    CHECK_PRODUCTION_FIELD(peerAckB);
    CHECK_PRODUCTION_FIELD(remoteAckA);
    CHECK_PRODUCTION_FIELD(remoteAckB);
    CHECK_PRODUCTION_FIELD(rttA);
    CHECK_PRODUCTION_FIELD(rttB);
#undef CHECK_PRODUCTION_FIELD
    if (!networkStatsEqual(left.aToB, right.aToB)) {
        differingField = "aToB";
        return false;
    }
    if (!networkStatsEqual(left.bToA, right.bToA)) {
        differingField = "bToA";
        return false;
    }
    if (!rollbackStatsEqual(left.rollbackA, right.rollbackA)) {
        differingField = "rollbackA";
        return false;
    }
    if (!rollbackStatsEqual(left.rollbackB, right.rollbackB)) {
        differingField = "rollbackB";
        return false;
    }
    return true;
}

ProductionStackRun runProductionStack(
    const ProductionStackProfile& profile,
    const uint64_t scenarioSeed,
    const uint32_t targetFrames,
    const bool enableSpectators) {
    ProductionStackRun result{};
    if (targetFrames == 0u) {
        result.error = "production stack requires at least one frame";
        return result;
    }

    const uint64_t inputSeed =
        splitMix64(scenarioSeed ^ 0x50524F44494E5055ull);
    const uint64_t networkSeed =
        splitMix64(scenarioSeed ^ 0x50524F444E455431ull);
    const uint32_t simulationSeed = static_cast<uint32_t>(
        splitMix64(scenarioSeed ^ 0x50524F4453494D31ull));
    const uint64_t sessionId =
        splitMix64(scenarioSeed ^ 0x53455353494F4E31ull) | 1ull;
    const wire::SessionCookie playerCookie{
        splitMix64(scenarioSeed ^ 0x504C415945524C4Full),
        splitMix64(scenarioSeed ^ 0x504C415945524849ull),
    };
    const Endpoint endpointA{0x0A000001u, 41001u};
    const Endpoint endpointB{0x0A000002u, 41002u};

    NetplayCore coreA(playerConfig(
        true, endpointB, sessionId, playerCookie));
    NetplayCore coreB(playerConfig(
        false, endpointA, sessionId, playerCookie));
    if (coreA.hasTerminalError() || coreB.hasTerminalError()) {
        result.error = "production NetplayCore configuration rejected";
        return result;
    }
    ProductionRollbackPeer rollbackA(
        0u, profile.delayA, profile.rollbackA,
        inputSeed, simulationSeed);
    ProductionRollbackPeer rollbackB(
        1u, profile.delayB, profile.rollbackB,
        inputSeed, simulationSeed);
    if (!rollbackA.valid() || !rollbackB.valid()) {
        result.error = "production RollbackSession configuration rejected";
        return result;
    }

    wire::ReadyPayload ready{};
    ready.seedGameplay = simulationSeed;
    ready.seedCosmetic = simulationSeed ^ 0xA5A5A5A5u;
    ready.selector = static_cast<uint32_t>(scenarioSeed & 1ull);
    ready.bootstrapHash = ToySimulation(simulationSeed).hash();
    if (!coreA.submitLocalReady(ready)
        || !coreB.submitLocalReady(ready)) {
        result.error = "production ready submission rejected";
        return result;
    }
    wire::SyncHashPayload layoutFingerprint{};
    layoutFingerprint.tag = wire::SyncHashTag{
        1u, 1u, wire::kLayoutFingerprintFrame,
    };
    layoutFingerprint.gameplayHash = splitMix64(
        ready.bootstrapHash
        ^ kSimulationContract
        ^ (static_cast<uint64_t>(profile.delayA) << 8u)
        ^ (static_cast<uint64_t>(profile.delayB) << 16u)
        ^ (static_cast<uint64_t>(profile.rollbackA) << 24u)
        ^ (static_cast<uint64_t>(profile.rollbackB) << 32u)) | 1ull;
    if (!wire::isValidSyncHashPayload(layoutFingerprint)) {
        result.error = "production layout fingerprint construction failed";
        return result;
    }

    DeterministicNetwork playerNetwork(
        networkSeed, profile.aToB, profile.bToA);
    playerNetwork.setDecisionRetention(false);

    const spec::SessionIdentity spectatorIdentity{
        splitMix64(sessionId ^ 0x5350454353455353ull) | 1ull,
        1ull,
        spec::SessionCookie{
            splitMix64(scenarioSeed ^ 0x53504543434F4F31ull),
            splitMix64(scenarioSeed ^ 0x53504543434F4F32ull),
        },
    };
    const uint64_t gameId =
        splitMix64(scenarioSeed ^ 0x47414D4549443031ull) | 1ull;
    const spec::ArchiveConfig archiveConfig{
        1u, targetFrames + 16u,
    };
    spec::SpectatorArchive archive(archiveConfig);
    ToySimulation archiveSimulation(simulationSeed);
    spec::GameDescriptor descriptor{};
    descriptor.gameId = gameId;
    descriptor.simulationContract = kSimulationContract;
    descriptor.compatibilityIdentity = soakCompatibilityIdentity();
    descriptor.initialStateHash = archiveSimulation.hash();
    descriptor.firstTimelineFrame = 0u;
    descriptor.playerOneDelay = profile.delayA;
    descriptor.playerTwoDelay = profile.delayB;
    descriptor.rollbackWindow =
        std::max(profile.rollbackA, profile.rollbackB);
    descriptor.metadata = {
        0x51u, 0x4Fu, 0x48u, 0x39u, 0x39u,
        static_cast<uint8_t>(wire::kVersion),
        static_cast<uint8_t>(spec::kSpectatorVersion),
    };
    if (archive.beginGame(descriptor) != spec::ArchiveResult::Ok) {
        result.error = "production spectator archive begin failed";
        return result;
    }

    spec::SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity;
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = soakCompatibilityIdentity();
    sourceConfig.maxViewers = kMaximumViewers;
    sourceConfig.maxPendingRejects = 64u;
    sourceConfig.recordsPerBatch = 24u;
    sourceConfig.sendWindowRecords = 96u;
    sourceConfig.perViewerPacketBudget = 1u;
    sourceConfig.globalPacketBudget = 8u;
    sourceConfig.handshakeResendMs = 250u;
    sourceConfig.retransmitMs = 350u;
    sourceConfig.terminalResendMs = 250u;
    sourceConfig.idleTimeoutMs = 15000u;
    spec::SpectatorSource source(sourceConfig, archive);
    if (!source.isConfigured()) {
        result.error = "production spectator source configuration rejected";
        return result;
    }

    std::vector<std::unique_ptr<ViewerSlot>> viewers;
    viewers.reserve(kMaximumViewers + 1u);
    const auto addViewer = [&](const spec::ViewerId id,
                               const bool overflow,
                               const bool resumeCandidate) -> bool {
        spec::ViewerConfig config{};
        config.identity = spectatorIdentity;
        config.simulationContract = kSimulationContract;
        config.compatibilityIdentity = soakCompatibilityIdentity();
        config.viewerNonce =
            splitMix64(scenarioSeed ^ id ^ 0x564945574E4F4E43ull) | 1ull;
        config.helloResendMs = 50u;
        config.ackResendMs = 50u;
        config.terminalAckResendMs = 50u;
        config.maxReorderRecords = 512u;
        config.maxRecords = targetFrames + 16u;
        auto slot = std::make_unique<ViewerSlot>(
            id, overflow, resumeCandidate, config,
            splitMix64(networkSeed
                ^ (id * 0x9E3779B97F4A7C15ull)),
            profile.aToB, profile.bToA, simulationSeed);
        if (!slot->viewer->isConfigured()
            || !slot->viewer->begin(gameId)) {
            result.error = "production spectator viewer begin failed";
            return false;
        }
        viewers.push_back(std::move(slot));
        return true;
    };
    if (enableSpectators && !addViewer(1u, false, true)) {
        return result;
    }

    uint32_t verifiedRemoteA = 0u;
    uint32_t verifiedRemoteB = 0u;
    uint32_t archivedFrames = 0u;
    uint32_t sequenceHashesA = 0u;
    uint32_t sequenceHashesB = 0u;
    uint32_t lastHashAckA = wire::kNoFrame;
    uint32_t lastHashAckB = wire::kNoFrame;
    uint32_t lastSubmittedHashFrame = wire::kNoFrame;
    std::vector<wire::SyncHashPayload> submittedHashes;
    std::deque<ConfirmedFrame> confirmedA;
    std::deque<ConfirmedFrame> confirmedB;
    uint64_t inputDigestA = kFnvOffset;
    uint64_t inputDigestB = kFnvOffset;
    uint64_t hashDigestA = kFnvOffset;
    uint64_t hashDigestB = kFnvOffset;
    uint64_t executionDigest = kFnvOffset;
    uint64_t playerNowUs = 0u;
    uint64_t wallTick = 0u;
    bool archiveSealed = false;
    uint64_t archiveLastPreStateHash = descriptor.initialStateHash;
    uint64_t archivePostTailStateHash = descriptor.initialStateHash;
    bool playerTerminalStarted = false;
    bool playerDisconnectAcked = false;
    bool playerDone = false;
    bool layoutSubmitted = false;
    bool layoutReceivedA = false;
    bool layoutReceivedB = false;
    bool layoutAcknowledgedA = false;
    bool layoutAcknowledgedB = false;
    bool layoutBarrierComplete = false;
    uint64_t layoutBarrierDoneTick = 0u;
    uint64_t firstGameplayTick = 0u;
    bool pressureStarted = false;
    bool lateStarted = false;
    bool overflowStarted = false;
    bool maximumPressureObserved = false;
    bool spectatorPacketInjected = false;
    bool playerPacketInjected = false;
    constexpr uint32_t kFreshJoinHeadroomRecords = 512u;
    const size_t sourceRecordLiabilityCap = (std::min)(
        static_cast<size_t>(sourceConfig.maxUnackedRecords),
        sourceConfig.maxUnackedBytes / sizeof(spec::ConfirmedRecord));
    const uint32_t latestFreshJoinFrame = static_cast<uint32_t>((std::min)(
        static_cast<size_t>(targetFrames),
        sourceRecordLiabilityCap > kFreshJoinHeadroomRecords
            ? sourceRecordLiabilityCap - kFreshJoinHeadroomRecords
            : static_cast<size_t>(1u)));
    const uint32_t pressureFrame = std::max<uint32_t>(
        1u, latestFreshJoinFrame * 2u / 3u);
    const uint32_t lateFrame = std::max<uint32_t>(
        pressureFrame, latestFreshJoinFrame);
    const uint32_t resumeFrame = std::max<uint32_t>(
        8u, targetFrames / 3u);
    const uint64_t maximumTicks =
        static_cast<uint64_t>(targetFrames) * 5ull + 250000ull;

    auto fail = [&](const std::string& message) {
        result.error = message;
        return false;
    };

    while (wallTick < maximumTicks) {
        const uint64_t nowMs = playerNowUs / 1000u;

        if (!playerDone) {
            const bool playersReady =
                coreA.readyComplete() && coreB.readyComplete();
            if (playersReady && !layoutSubmitted) {
                if (!coreA.submitLocalSyncHash(layoutFingerprint)
                    || !coreB.submitLocalSyncHash(layoutFingerprint)) {
                    fail("production layout fingerprint submission rejected");
                    return result;
                }
                layoutSubmitted = true;
            }
            if (playersReady && layoutBarrierComplete) {
                if (firstGameplayTick == 0u)
                    firstGameplayTick = wallTick + 1u;
                if (!rollbackA.prepare(
                        coreA, targetFrames, result.error)
                    || !rollbackB.prepare(
                        coreB, targetFrames, result.error)) {
                    return result;
                }
            }

            const std::vector<OutboundDatagram> outboundA =
                coreA.pollOutbound(nowMs);
            const std::vector<OutboundDatagram> outboundB =
                coreB.pollOutbound(nowMs);
            for (const OutboundDatagram& packet : outboundA) {
                if (packet.destination != endpointB) {
                    fail("production core A emitted to wrong endpoint");
                    return result;
                }
                digestByte(executionDigest,
                           static_cast<uint8_t>(Direction::AtoB));
                digestByte(executionDigest,
                           static_cast<uint8_t>(packet.type));
                digestU32(executionDigest, packet.sequence);
                digestBytes(executionDigest, packet.bytes);
                playerNetwork.send(Direction::AtoB, packet.type,
                                   packet.sequence, packet.bytes,
                                   playerNowUs);
                if (!playerPacketInjected) {
                    const size_t before = source.viewerCount();
                    const spec::IngressResult injection = source.onDatagram(
                        0xDEADu, packet.bytes.data(), packet.bytes.size(),
                        nowMs);
                    if ((injection != spec::IngressResult::BadMagic
                         && injection != spec::IngressResult::TooShort)
                        || source.viewerCount() != before) {
                        fail("player datagram mutated spectator source");
                        return result;
                    }
                    playerPacketInjected = true;
                }
            }
            for (const OutboundDatagram& packet : outboundB) {
                if (packet.destination != endpointA) {
                    fail("production core B emitted to wrong endpoint");
                    return result;
                }
                digestByte(executionDigest,
                           static_cast<uint8_t>(Direction::BtoA));
                digestByte(executionDigest,
                           static_cast<uint8_t>(packet.type));
                digestU32(executionDigest, packet.sequence);
                digestBytes(executionDigest, packet.bytes);
                playerNetwork.send(Direction::BtoA, packet.type,
                                   packet.sequence, packet.bytes,
                                   playerNowUs);
                if (!playerPacketInjected) {
                    const size_t before = source.viewerCount();
                    const spec::IngressResult injection = source.onDatagram(
                        0xDEADu, packet.bytes.data(), packet.bytes.size(),
                        nowMs);
                    if ((injection != spec::IngressResult::BadMagic
                         && injection != spec::IngressResult::TooShort)
                        || source.viewerCount() != before) {
                        fail("player datagram mutated spectator source");
                        return result;
                    }
                    playerPacketInjected = true;
                }
            }

            std::vector<ScheduledPacket> delivered;
            playerNetwork.deliverDue(playerNowUs, delivered);
            for (const ScheduledPacket& packet : delivered) {
                NetplayCore& destination =
                    packet.direction == Direction::AtoB ? coreB : coreA;
                const Endpoint& sender =
                    packet.direction == Direction::AtoB
                    ? endpointA : endpointB;
                const IngressResult ingress = destination.onDatagram(
                    sender, packet.bytes.data(), packet.bytes.size(), nowMs);
                digestByte(executionDigest,
                           static_cast<uint8_t>(ingress));
                if (!acceptablePlayerIngress(
                        ingress, packet, destination,
                        playerTerminalStarted)) {
                    std::ostringstream stream;
                    stream << "production player ingress rejected type "
                           << static_cast<unsigned>(packet.type)
                           << " result " << static_cast<unsigned>(ingress);
                    fail(stream.str());
                    return result;
                }
                if (ingress == IngressResult::Accepted
                    && packet.type == wire::MessageType::SyncHashAck) {
                    wire::SyncHashTag acknowledgedTag{};
                    if (!decodeHashAckTag(
                            packet, sessionId, playerCookie,
                            acknowledgedTag)) {
                        fail("production SyncHashAck decode failed");
                        return result;
                    }
                    if (acknowledgedTag.frame
                            == wire::kLayoutFingerprintFrame) {
                        if (!layoutSubmitted
                            || acknowledgedTag
                                != layoutFingerprint.tag) {
                            fail("production layout fingerprint ACK "
                                 "tag diverged");
                            return result;
                        }
                    } else if (packet.direction == Direction::AtoB) {
                        if (lastHashAckB == wire::kNoFrame
                            || acknowledgedTag.frame > lastHashAckB) {
                            lastHashAckB = acknowledgedTag.frame;
                        }
                    } else {
                        if (lastHashAckA == wire::kNoFrame
                            || acknowledgedTag.frame > lastHashAckA) {
                            lastHashAckA = acknowledgedTag.frame;
                        }
                    }
                }
                if (ingress == IngressResult::Accepted
                    && packet.type == wire::MessageType::DisconnectAck
                    && packet.direction == Direction::BtoA) {
                    playerDisconnectAcked = true;
                }
            }

            if (coreA.readyComplete() && coreB.readyComplete()
                && layoutBarrierComplete) {
                if (!rollbackA.ingest(coreA, result.error)
                    || !rollbackB.ingest(coreB, result.error)
                    || !rollbackA.step(targetFrames, result.error)
                    || !rollbackB.step(targetFrames, result.error)) {
                    return result;
                }
                ConfirmedFrame confirmed{};
                while (rollbackA.popConfirmed(confirmed))
                    confirmedA.push_back(confirmed);
                while (rollbackB.popConfirmed(confirmed))
                    confirmedB.push_back(confirmed);
            }

            const auto verifyRemote =
                [&](NetplayCore& core,
                    uint32_t& next,
                    const uint8_t remotePlayer,
                    const uint8_t delay,
                    uint64_t& digest) -> bool {
                const uint32_t acknowledged = core.remoteAckThrough();
                while (next < targetFrames
                       && acknowledged != wire::kNoFrame
                       && next <= acknowledged) {
                    uint16_t actual = 0u;
                    const uint16_t expected = expectedInput(
                        inputSeed, remotePlayer, next, delay);
                    if (!core.getRemoteInput(next, actual)
                        || actual != expected) {
                        return false;
                    }
                    digestU32(digest, next);
                    digestU16(digest, actual);
                    ++next;
                }
                return true;
            };
            if (!verifyRemote(
                    coreA, verifiedRemoteA, 1u,
                    profile.delayB, inputDigestA)
                || !verifyRemote(
                    coreB, verifiedRemoteB, 0u,
                    profile.delayA, inputDigestB)) {
                fail("production core remote input disagrees with oracle");
                return result;
            }

            wire::SyncHashPayload receivedHash{};
            while (coreA.takeRemoteSyncHash(receivedHash)) {
                if (receivedHash.tag.frame
                        == wire::kLayoutFingerprintFrame) {
                    if (layoutReceivedA
                        || !sameHash(
                            receivedHash, layoutFingerprint)) {
                        fail("production core A received divergent "
                             "layout fingerprint");
                        return result;
                    }
                    layoutReceivedA = true;
                    continue;
                }
                if (!layoutBarrierComplete) {
                    fail("production core A received gameplay hash "
                         "before layout barrier");
                    return result;
                }
                if (sequenceHashesA >= submittedHashes.size()
                    || !sameHash(
                        receivedHash,
                        submittedHashes[sequenceHashesA])) {
                    fail("production core A received divergent sync hash");
                    return result;
                }
                digestHash(hashDigestA, receivedHash);
                ++sequenceHashesA;
            }
            while (coreB.takeRemoteSyncHash(receivedHash)) {
                if (receivedHash.tag.frame
                        == wire::kLayoutFingerprintFrame) {
                    if (layoutReceivedB
                        || !sameHash(
                            receivedHash, layoutFingerprint)) {
                        fail("production core B received divergent "
                             "layout fingerprint");
                        return result;
                    }
                    layoutReceivedB = true;
                    continue;
                }
                if (!layoutBarrierComplete) {
                    fail("production core B received gameplay hash "
                         "before layout barrier");
                    return result;
                }
                if (sequenceHashesB >= submittedHashes.size()
                    || !sameHash(
                        receivedHash,
                        submittedHashes[sequenceHashesB])) {
                    fail("production core B received divergent sync hash");
                    return result;
                }
                digestHash(hashDigestB, receivedHash);
                ++sequenceHashesB;
            }
            if (layoutSubmitted) {
                layoutAcknowledgedA =
                    layoutAcknowledgedA
                    || coreA.localSyncHashAcknowledged(
                        layoutFingerprint.tag);
                layoutAcknowledgedB =
                    layoutAcknowledgedB
                    || coreB.localSyncHashAcknowledged(
                        layoutFingerprint.tag);
            }
            if (!layoutBarrierComplete
                && layoutSubmitted
                && layoutReceivedA
                && layoutReceivedB
                && layoutAcknowledgedA
                && layoutAcknowledgedB) {
                layoutBarrierComplete = true;
                layoutBarrierDoneTick = wallTick + 1u;
                digestHash(executionDigest, layoutFingerprint);
                digestU64(executionDigest, layoutBarrierDoneTick);
            }
        }

        while (!confirmedA.empty() && !confirmedB.empty()
               && archivedFrames < targetFrames) {
            const ConfirmedFrame fromA = confirmedA.front();
            const ConfirmedFrame fromB = confirmedB.front();
            const uint16_t expectedP1 = expectedInput(
                inputSeed, 0u, archivedFrames, profile.delayA);
            const uint16_t expectedP2 = expectedInput(
                inputSeed, 1u, archivedFrames, profile.delayB);
            const uint64_t expectedPreHash = archiveSimulation.hash();
            if (fromA.frame != archivedFrames
                || fromB.frame != archivedFrames
                || fromA.p1 != fromB.p1
                || fromA.p2 != fromB.p2
                || fromA.preStateHash != fromB.preStateHash
                || fromA.p1 != expectedP1
                || fromA.p2 != expectedP2
                || fromA.preStateHash != expectedPreHash
                || archiveSimulation.state().frame != archivedFrames) {
                fail("production corrected confirmed frame "
                     "disagrees with peer/oracle");
                return result;
            }
            spec::ConfirmedRecord record{};
            record.record = archivedFrames;
            record.timelineFrame = archivedFrames;
            record.playerOneInput = fromA.p1;
            record.playerTwoInput = fromA.p2;
            record.stateHash = fromA.preStateHash;
            spec::ConfirmedRecord stored{};
            if (archive.appendConfirmed(gameId, record, &stored)
                != spec::ArchiveResult::Ok
                || stored.playerOneInput != expectedP1
                || stored.playerTwoInput != expectedP2
                || stored.stateHash != record.stateHash
                || stored.rollingDigest == 0u) {
                fail("production spectator archive append failed");
                return result;
            }
            archiveLastPreStateHash = stored.stateHash;

            const bool publishHash = archivedFrames % 600u == 0u
                || archivedFrames + 1u == targetFrames;
            if (publishHash) {
                const wire::SyncHashPayload hash = makeSyncHash(
                    archiveSimulation.state(),
                    expectedPreHash, archivedFrames);
                if (!coreA.submitLocalSyncHash(hash)
                    || !coreB.submitLocalSyncHash(hash)) {
                    fail("production confirmed SyncHash submission rejected");
                    return result;
                }
                submittedHashes.push_back(hash);
                lastSubmittedHashFrame = archivedFrames;
            }
            archiveSimulation.step(expectedP1, expectedP2);
            archivePostTailStateHash = archiveSimulation.hash();
            confirmedA.pop_front();
            confirmedB.pop_front();
            ++archivedFrames;
        }

        if (enableSpectators) {
            if (!pressureStarted && archivedFrames >= pressureFrame) {
                pressureStarted = true;
                for (spec::ViewerId id = 3u;
                     id <= static_cast<spec::ViewerId>(kMaximumViewers);
                     ++id) {
                    if (!addViewer(id, false, false)) return result;
                }
            }
            if (!lateStarted && archivedFrames >= lateFrame) {
                lateStarted = true;
                if (!addViewer(2u, false, false)) return result;
            }

            for (const auto& holder : viewers) {
                ViewerSlot& slot = *holder;
                const std::vector<spec::ViewerDatagram> packets =
                    slot.viewer->pollOutbound(nowMs, 4u);
                for (const spec::ViewerDatagram& packet : packets) {
                    slot.link->send(
                        Direction::AtoB, wire::MessageType::Inputs,
                        packet.sequence, packet.bytes, playerNowUs);
                    if (!spectatorPacketInjected) {
                        const bool established = coreA.isEstablished();
                        const uint64_t lastReceive =
                            coreA.lastValidReceiveMs();
                        const uint32_t remoteAck =
                            coreA.remoteAckThrough();
                        const uint32_t peerAck =
                            coreA.peerAckThrough();
                        const TerminalState terminal =
                            coreA.terminalState();
                        const IngressResult injection = coreA.onDatagram(
                            endpointB, packet.bytes.data(),
                            packet.bytes.size(), nowMs);
                        const TerminalState after =
                            coreA.terminalState();
                        if (injection != IngressResult::BadMagic
                            || coreA.isEstablished() != established
                            || coreA.lastValidReceiveMs() != lastReceive
                            || coreA.remoteAckThrough() != remoteAck
                            || coreA.peerAckThrough() != peerAck
                            || after.active != terminal.active
                            || after.fromPeer != terminal.fromPeer
                            || after.code != terminal.code
                            || after.detail != terminal.detail
                            || after.reasonId != terminal.reasonId
                            || after.reason != terminal.reason) {
                            fail("spectator datagram mutated player core");
                            return result;
                        }
                        spectatorPacketInjected = true;
                    }
                }
            }

            const std::vector<spec::SourceDatagram> sourcePackets =
                source.pollOutbound(nowMs, 128u);
            for (const spec::SourceDatagram& packet : sourcePackets) {
                const auto found = std::find_if(
                    viewers.begin(), viewers.end(),
                    [&](const std::unique_ptr<ViewerSlot>& slot) {
                        return slot->id == packet.destination;
                    });
                if (found == viewers.end()) {
                    fail("spectator source emitted to unknown viewer");
                    return result;
                }
                (*found)->link->send(
                    Direction::BtoA, wire::MessageType::Inputs,
                    packet.sequence, packet.bytes, playerNowUs);
            }

            for (const auto& holder : viewers) {
                ViewerSlot& slot = *holder;
                std::vector<ScheduledPacket> delivered;
                slot.link->deliverDue(playerNowUs, delivered);
                for (const ScheduledPacket& packet : delivered) {
                    if (packet.direction == Direction::AtoB) {
                        const spec::IngressResult ingress = source.onDatagram(
                            slot.id, packet.bytes.data(),
                            packet.bytes.size(), nowMs);
                        if (ingress == spec::IngressResult::CapacityReached)
                            result.capacityRejectObserved = true;
                        if (!acceptableSourceIngress(ingress, slot)) {
                            std::ostringstream stream;
                            stream << "spectator source ingress rejected "
                                   << static_cast<unsigned>(ingress);
                            fail(stream.str());
                            return result;
                        }
                    } else {
                        const spec::IngressResult ingress =
                            slot.viewer->onDatagram(
                                packet.bytes.data(),
                                packet.bytes.size(), nowMs);
                        if (!acceptableViewerIngress(ingress, slot)) {
                            std::ostringstream stream;
                            stream << "spectator viewer ingress rejected "
                                   << static_cast<unsigned>(ingress);
                            fail(stream.str());
                            return result;
                        }
                    }
                }

                if (slot.resumeCandidate
                    && !slot.resumeRequested
                    && archivedFrames >= resumeFrame
                    && slot.viewer->phase()
                        == spec::ViewerPhase::Streaming
                    && slot.viewer->receivedNextRecord() != 0u) {
                    slot.resumeCursor =
                        slot.viewer->receivedNextRecord();
                    if (!slot.viewer->resume()) {
                        fail("production spectator resume rejected");
                        return result;
                    }
                    slot.resumeRequested = true;
                    ++result.spectatorResumes;
                }

                if (!slot.overflowProbe) {
                    const qgame::SpectatorCatchupPlan plan =
                        qgame::spectatorCatchupPlan(
                            slot.viewer->bufferedPlaybackRecords(),
                            slot.catchingUp);
                    slot.catchingUp = plan.catchingUp;
                    size_t playbackBudget = plan.tickBudget;
                    spec::ConfirmedRecord record{};
                    while (playbackBudget-- != 0u
                           && slot.viewer->peekPlaybackRecord(record)) {
                        if (record.record
                                != slot.viewer->playbackNextRecord()
                            || record.timelineFrame != record.record
                            || record.playerOneInput != expectedInput(
                                inputSeed, 0u, record.record,
                                profile.delayA)
                            || record.playerTwoInput != expectedInput(
                                inputSeed, 1u, record.record,
                                profile.delayB)
                            || slot.playback.state().frame
                                != record.record) {
                            fail("production spectator playback record "
                                 "disagrees with oracle");
                            return result;
                        }
                        const uint64_t observedPreHash =
                            slot.playback.hash();
                        if (record.stateHash != observedPreHash) {
                            fail("production spectator pre-state "
                                 "hash disagrees with archive");
                            return result;
                        }
                        slot.playback.step(
                            record.playerOneInput,
                            record.playerTwoInput);
                        const uint64_t observedPostHash =
                            slot.playback.hash();
                        if (slot.viewer->commitPlaybackRecord(
                                record.record, observedPreHash,
                                observedPostHash)
                                != spec::IngressResult::Accepted) {
                            fail("production spectator playback hash failed");
                            return result;
                        }
                    }
                }
            }

            if (source.viewerCount() == kMaximumViewers)
                maximumPressureObserved = true;
            if (maximumPressureObserved && !overflowStarted) {
                overflowStarted = true;
                if (!addViewer(
                        static_cast<spec::ViewerId>(kMaximumViewers) + 1u,
                        true, false))
                    return result;
            }
        }

        if (enableSpectators) {
            uint64_t worstThisSlot = 0u;
            for (const auto& holder : viewers) {
                if (holder->overflowProbe) continue;
                if (holder->viewer->phase() != spec::ViewerPhase::Streaming)
                    continue;
                worstThisSlot = (std::max)(
                    worstThisSlot,
                    static_cast<uint64_t>(
                        holder->viewer->bufferedPlaybackRecords()));
            }
            if (!archiveSealed) {
                result.maximumLiveBacklog =
                    (std::max)(result.maximumLiveBacklog, worstThisSlot);
                if (!result.convergedToLiveTarget
                    && archivedFrames > qgame::kSpectatorStartupBufferRecords
                    && worstThisSlot != 0u
                    && worstThisSlot <= qgame::kSpectatorLiveTargetRecords) {
                    result.convergedToLiveTarget = true;
                    result.convergenceSlots = wallTick;
                }
            } else if (result.backlogAtSeal == 0u) {
                result.backlogAtSeal = worstThisSlot;
            }
        }

        if (!archiveSealed
            && archivedFrames == targetFrames) {
            if (archive.sealGame(
                    gameId, spec::TerminalReason::MatchEnded,
                    archivePostTailStateHash)
                != spec::ArchiveResult::Ok) {
                fail("production spectator archive seal failed");
                return result;
            }
            archiveSealed = true;
        }

        const bool allHashesReceived =
            sequenceHashesA == submittedHashes.size()
            && sequenceHashesB == submittedHashes.size();
        const bool allHashesAcknowledged =
            lastSubmittedHashFrame != wire::kNoFrame
            && lastHashAckA == lastSubmittedHashFrame
            && lastHashAckB == lastSubmittedHashFrame;
        const uint32_t lastLocalA =
            targetFrames - 1u + profile.delayA;
        const uint32_t lastLocalB =
            targetFrames - 1u + profile.delayB;
        const bool allInputsAcknowledged =
            coreA.peerAckThrough() == lastLocalA
            && coreB.peerAckThrough() == lastLocalB
            && coreA.remoteAckThrough() == lastLocalB
            && coreB.remoteAckThrough() == lastLocalA;
        const bool correctedSimulationComplete =
            rollbackA.frontier() == targetFrames
            && rollbackB.frontier() == targetFrames
            && !rollbackA.pendingRollback()
            && !rollbackB.pendingRollback()
            && rollbackA.state() == archiveSimulation.state()
            && rollbackB.state() == archiveSimulation.state()
            && confirmedA.empty() && confirmedB.empty();
        if (!playerTerminalStarted && archiveSealed
            && coreA.readyComplete() && coreB.readyComplete()
            && layoutBarrierComplete
            && allHashesReceived && allHashesAcknowledged
            && allInputsAcknowledged
            && correctedSimulationComplete) {
            coreA.setLocalTerminalError(
                wire::TerminalErrorCode::PeerRequested,
                0x514F4839u,
                "production soak completed");
            playerTerminalStarted = true;
        }

        if (!playerDone && playerTerminalStarted
            && playerDisconnectAcked) {
            const TerminalState a = coreA.terminalState();
            const TerminalState b = coreB.terminalState();
            if (!a.active || a.fromPeer
                || a.code != wire::TerminalErrorCode::PeerRequested
                || a.detail != 0x514F4839u
                || a.reasonId == 0
                || !b.active || !b.fromPeer
                || b.code != wire::TerminalErrorCode::PeerRequested
                || b.detail != 0x514F4839u
                || b.reasonId != a.reasonId
                || b.reason != a.reason) {
                fail("production player terminal handshake diverged");
                return result;
            }
            playerDone = true;
            ProductionStackFingerprint& fingerprint =
                result.fingerprint;
            fingerprint.playerNetworkDigest =
                playerNetwork.traceDigest();
            digestU64(executionDigest, rollbackA.executionDigest());
            digestU64(executionDigest, rollbackB.executionDigest());
            fingerprint.playerExecutionDigest = executionDigest;
            fingerprint.remoteInputDigestA = inputDigestA;
            fingerprint.remoteInputDigestB = inputDigestB;
            fingerprint.remoteHashDigestA = hashDigestA;
            fingerprint.remoteHashDigestB = hashDigestB;
            spec::ArchiveExtent extent{};
            if (!archive.extent(gameId, extent)
                || !extent.sealed
                || extent.nextRecord != targetFrames
                || extent.stateHash != archiveLastPreStateHash
                || extent.terminal.postTailStateHash
                    != archivePostTailStateHash) {
                fail("production archive terminal extent invalid");
                return result;
            }
            fingerprint.archiveDigest = extent.digest;
            fingerprint.finalStateHash = archiveSimulation.hash();
            fingerprint.layoutBarrierDoneTick =
                layoutBarrierDoneTick;
            fingerprint.firstGameplayTick = firstGameplayTick;
            fingerprint.playerDoneTick = wallTick + 1u;
            fingerprint.peerAckA = coreA.peerAckThrough();
            fingerprint.peerAckB = coreB.peerAckThrough();
            fingerprint.remoteAckA = coreA.remoteAckThrough();
            fingerprint.remoteAckB = coreB.remoteAckThrough();
            fingerprint.rttA = coreA.hasRtt() ? coreA.rttMs() : 0u;
            fingerprint.rttB = coreB.hasRtt() ? coreB.rttMs() : 0u;
            fingerprint.rollbackA = rollbackA.stats();
            fingerprint.rollbackB = rollbackB.stats();
            fingerprint.aToB = playerNetwork.stats(Direction::AtoB);
            fingerprint.bToA = playerNetwork.stats(Direction::BtoA);
        }

        bool spectatorsDone = !enableSpectators;
        if (enableSpectators && playerDone && overflowStarted) {
            spectatorsDone = source.viewerCount() == 0u;
            size_t completed = 0u;
            bool overflowRejected = false;
            for (const auto& holder : viewers) {
                const ViewerSlot& slot = *holder;
                if (slot.overflowProbe) {
                    spec::RejectPayload rejection{};
                    overflowRejected =
                        slot.viewer->phase()
                            == spec::ViewerPhase::Rejected
                        && slot.viewer->rejection(rejection)
                        && rejection.reason
                            == spec::RejectReason::CapacityReached;
                    continue;
                }
                if (slot.viewer->phase() != spec::ViewerPhase::Complete
                    || slot.viewer->receivedNextRecord() != targetFrames
                    || slot.viewer->playbackNextRecord() != targetFrames
                    || slot.playback.hash()
                        != archiveSimulation.hash()) {
                    spectatorsDone = false;
                } else {
                    ++completed;
                }
            }
            if (!overflowRejected) spectatorsDone = false;
            if (spectatorsDone) {
                result.completedViewers = completed;
                result.capacityRejectObserved =
                    result.capacityRejectObserved && overflowRejected;
            }
        }

        ++wallTick;
        if (playerDone && spectatorsDone) break;
        playerNowUs += kFrameDurationUs;
    }

    if (!playerDone) {
        result.error =
            "production player stack exceeded progress deadline";
        return result;
    }
    if (!layoutSubmitted
        || !layoutReceivedA || !layoutReceivedB
        || !layoutAcknowledgedA || !layoutAcknowledgedB
        || !layoutBarrierComplete
        || layoutBarrierDoneTick == 0u
        || firstGameplayTick <= layoutBarrierDoneTick) {
        result.error =
            "production layout barrier completion/order invariant failed";
        return result;
    }
    if (enableSpectators) {
        if (!pressureStarted || !lateStarted || !overflowStarted
            || !maximumPressureObserved
            || result.completedViewers != kMaximumViewers
            || !result.capacityRejectObserved
            || result.spectatorResumes != 1u
            || viewers.empty() || !viewers.front()->resumeRequested
            || viewers.front()->resumeCursor == 0u
            || !spectatorPacketInjected) {
            result.error =
                "production spectator pressure/resume/terminal "
                "coverage incomplete";
            return result;
        }
        if (!result.convergedToLiveTarget) {
            result.error =
                "production spectator playback never converged to the live "
                "target while the archive was still growing";
            return result;
        }
        for (const auto& holder : viewers) {
            const NetworkStats& toSource =
                holder->link->stats(Direction::AtoB);
            const NetworkStats& toViewer =
                holder->link->stats(Direction::BtoA);
            result.spectatorPackets +=
                toSource.logicalPackets + toViewer.logicalPackets;
            result.spectatorDropped +=
                toSource.droppedPackets + toViewer.droppedPackets;
            result.spectatorDuplicated +=
                toSource.duplicatedPackets
                + toViewer.duplicatedPackets;
            result.spectatorReordered +=
                toSource.reorderedPackets
                + toViewer.reorderedPackets;
        }
        if ((profile.aToB.duplicateBasisPoints != 0u
             || profile.bToA.duplicateBasisPoints != 0u)
            && targetFrames >= 1024u
            && result.spectatorDuplicated == 0u) {
            result.error =
                "production spectator duplication was not exercised";
            return result;
        }
        if ((profile.aToB.reorderBasisPoints != 0u
             || profile.bToA.reorderBasisPoints != 0u)
            && targetFrames >= 1024u
            && result.spectatorReordered == 0u) {
            result.error =
                "production spectator reordering was not exercised";
            return result;
        }
        if (profile.name == "packet_loss"
            && targetFrames >= 4096u) {
            const ViewerSlot& immediate = *viewers.front();
            const NetworkStats& toSource =
                immediate.link->stats(Direction::AtoB);
            const NetworkStats& toViewer =
                immediate.link->stats(Direction::BtoA);
            constexpr uint64_t forced = 29u + 30u + 31u;
            if (toSource.forcedDropPackets != forced
                || toViewer.forcedDropPackets != forced
                || toSource.blackoutDropPackets == 0u
                || toViewer.blackoutDropPackets == 0u) {
                result.error =
                    "production spectator loss bursts/blackout "
                    "were not fully exercised";
                return result;
            }
        }
    }

    if (profile.name == "packet_loss" && targetFrames >= 4096u) {
        constexpr uint64_t forced = 29u + 30u + 31u;
        if (result.fingerprint.aToB.forcedDropPackets != forced
            || result.fingerprint.bToA.forcedDropPackets != forced
            || result.fingerprint.aToB.blackoutDropPackets == 0u
            || result.fingerprint.bToA.blackoutDropPackets == 0u) {
            result.error =
                "production player loss bursts/blackout "
                "were not fully exercised";
            return result;
        }
    }
    const RollbackStats& rollbackStatsA =
        result.fingerprint.rollbackA;
    const RollbackStats& rollbackStatsB =
        result.fingerprint.rollbackB;
    if (rollbackStatsA.visibleAdvances != targetFrames
        || rollbackStatsB.visibleAdvances != targetFrames
        || rollbackStatsA.uniqueCaptures != targetFrames
        || rollbackStatsB.uniqueCaptures != targetFrames
        || rollbackStatsA.localSubmissions
            != static_cast<uint64_t>(targetFrames) + profile.delayA
        || rollbackStatsB.localSubmissions
            != static_cast<uint64_t>(targetFrames) + profile.delayB
        || rollbackStatsA.maxRollbackDepth > profile.rollbackA
        || rollbackStatsB.maxRollbackDepth > profile.rollbackB
        || rollbackStatsA.maxSpeculativeFrames > profile.rollbackA
        || rollbackStatsB.maxSpeculativeFrames > profile.rollbackB
        || rollbackStatsA.confirmedFrames != targetFrames
        || rollbackStatsB.confirmedFrames != targetFrames
        || !rollbackStatsInternallyConsistent(rollbackStatsA)
        || !rollbackStatsInternallyConsistent(rollbackStatsB)
        || rollbackA.maxConsecutiveStalls() > 1024u
        || rollbackB.maxConsecutiveStalls() > 1024u
        || rollbackStatsA.maxConsecutiveStallPolls
            != rollbackA.maxConsecutiveStalls()
        || rollbackStatsB.maxConsecutiveStallPolls
            != rollbackB.maxConsecutiveStalls()) {
        result.error =
            "production NetplayCore/RollbackSession bounds failed";
        return result;
    }
    if (profile.name != "stable_low" && targetFrames >= 1024u
        && rollbackStatsA.rollbackCount
            + rollbackStatsB.rollbackCount == 0u) {
        result.error =
            "production path did not exercise rollback correction";
        return result;
    }
    if (profile.name == "packet_loss" && targetFrames >= 4096u
        && rollbackStatsA.stallPolls
            + rollbackStatsB.stallPolls == 0u) {
        result.error =
            "production loss path did not exercise bounded stalls";
        return result;
    }
    if (!playerPacketInjected) {
        result.error = "production cross-protocol injection was not tested";
        return result;
    }

    result.ok = true;
    return result;
}

}
