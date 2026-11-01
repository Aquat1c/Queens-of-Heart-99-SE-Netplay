#include "SoakRunner.h"

#include "DeterministicNetwork.h"
#include "ProductionStackSoak.h"
#include "ToySimulation.h"
#include "../../RollbackSession.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <deque>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace qnetplay::soak {

namespace {

constexpr uint64_t kFrameDurationUs = 16500u;
constexpr uint32_t kInputPacketMagic = 0x314B5354u;
constexpr size_t kInputPacketHeaderSize = 12u;
constexpr size_t kInputEntrySize = 6u;
constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001B3ull;
constexpr uint64_t kBlackoutStartUs = 20ull * 1000ull * 1000ull;
constexpr uint64_t kBlackoutEndUs = 22ull * 1000ull * 1000ull;
constexpr uint32_t kHistoryCapacity = 4096u;
constexpr uint32_t kMaximumConsecutiveStalls = 1024u;

struct SoakProfile {
    const char* name = "";
    LinkFaults aToB{};
    LinkFaults bToA{};
    uint8_t delayA = 0;
    uint8_t delayB = 0;
    uint8_t rollbackA = 0;
    uint8_t rollbackB = 0;
    bool requiresRollback = false;
    bool requiresStall = false;
    bool stressRecovery = false;
};

struct InputSlot {
    uint16_t value = 0;
    bool valid = false;
};

struct SnapshotSlot {
    uint32_t frame = 0;
    ToyState state{};
    bool valid = false;
};

struct RunFingerprint {
    uint64_t networkDigest = 0;
    uint64_t executionDigestA = 0;
    uint64_t executionDigestB = 0;
    uint64_t confirmedDigest = 0;
    uint64_t finalHashA = 0;
    uint64_t finalHashB = 0;
    uint64_t oracleHash = 0;
    uint64_t wallTicks = 0;
    RollbackStats statsA{};
    RollbackStats statsB{};
    NetworkStats networkAtoB{};
    NetworkStats networkBtoA{};
};

struct RunResult {
    bool ok = false;
    std::string error;
    RunFingerprint fingerprint{};
};

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
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        digestByte(digest, static_cast<uint8_t>(value >> shift));
    }
}

void digestU64(uint64_t& digest, const uint64_t value) {
    for (unsigned shift = 0u; shift < 64u; shift += 8u) {
        digestByte(digest, static_cast<uint8_t>(value >> shift));
    }
}

void appendU32(std::vector<uint8_t>& bytes, const uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8u));
    bytes.push_back(static_cast<uint8_t>(value >> 16u));
    bytes.push_back(static_cast<uint8_t>(value >> 24u));
}

void appendU16(std::vector<uint8_t>& bytes, const uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8u));
}

bool readU32(const std::vector<uint8_t>& bytes,
             size_t& cursor,
             uint32_t& value) {
    if (cursor + 4u > bytes.size()) {
        return false;
    }
    value = static_cast<uint32_t>(bytes[cursor])
        | (static_cast<uint32_t>(bytes[cursor + 1u]) << 8u)
        | (static_cast<uint32_t>(bytes[cursor + 2u]) << 16u)
        | (static_cast<uint32_t>(bytes[cursor + 3u]) << 24u);
    cursor += 4u;
    return true;
}

bool readU16(const std::vector<uint8_t>& bytes,
             size_t& cursor,
             uint16_t& value) {
    if (cursor + 2u > bytes.size()) {
        return false;
    }
    value = static_cast<uint16_t>(
        static_cast<uint16_t>(bytes[cursor])
        | (static_cast<uint16_t>(bytes[cursor + 1u]) << 8u));
    cursor += 2u;
    return true;
}

uint16_t sourceInput(const uint64_t inputSeed,
                     const uint8_t canonicalPlayer,
                     const uint32_t sourceFrame,
                     const uint8_t delay) {
    return delayedScriptedInput(
        inputSeed,
        canonicalPlayer,
        sourceFrame + static_cast<uint32_t>(delay),
        delay);
}

std::vector<SoakProfile> makeProfiles() {
    std::vector<SoakProfile> profiles;

    SoakProfile stableLow{};
    stableLow.name = "stable_low";
    stableLow.aToB.baseDelayMs = 8u;
    stableLow.bToA.baseDelayMs = 8u;
    stableLow.delayA = 1u;
    stableLow.rollbackA = 6u;
    stableLow.delayB = 2u;
    stableLow.rollbackB = 8u;
    profiles.push_back(stableLow);

    SoakProfile stableHigh{};
    stableHigh.name = "stable_high";
    stableHigh.aToB.baseDelayMs = 100u;
    stableHigh.bToA.baseDelayMs = 100u;
    stableHigh.delayA = 3u;
    stableHigh.rollbackA = 12u;
    stableHigh.delayB = 5u;
    stableHigh.rollbackB = 15u;
    stableHigh.requiresRollback = true;
    profiles.push_back(stableHigh);

    SoakProfile jitterLow{};
    jitterLow.name = "jitter_low";
    jitterLow.aToB.baseDelayMs = 15u;
    jitterLow.aToB.jitterMs = 25u;
    jitterLow.aToB.duplicateBasisPoints = 200u;
    jitterLow.aToB.reorderBasisPoints = 400u;
    jitterLow.aToB.reorderExtraMs = 45u;
    jitterLow.bToA = jitterLow.aToB;
    jitterLow.delayA = 1u;
    jitterLow.rollbackA = 8u;
    jitterLow.delayB = 2u;
    jitterLow.rollbackB = 10u;
    jitterLow.requiresRollback = true;
    profiles.push_back(jitterLow);

    SoakProfile jitterHigh{};
    jitterHigh.name = "jitter_high";
    jitterHigh.aToB.baseDelayMs = 90u;
    jitterHigh.aToB.jitterMs = 70u;
    jitterHigh.aToB.duplicateBasisPoints = 250u;
    jitterHigh.aToB.reorderBasisPoints = 500u;
    jitterHigh.aToB.reorderExtraMs = 120u;
    jitterHigh.bToA = jitterHigh.aToB;
    jitterHigh.delayA = 3u;
    jitterHigh.rollbackA = 12u;
    jitterHigh.delayB = 5u;
    jitterHigh.rollbackB = 15u;
    jitterHigh.requiresRollback = true;
    profiles.push_back(jitterHigh);

    SoakProfile packetLoss{};
    packetLoss.name = "packet_loss";
    packetLoss.aToB.baseDelayMs = 35u;
    packetLoss.aToB.jitterMs = 10u;
    packetLoss.aToB.lossBasisPoints = 1500u;
    packetLoss.aToB.duplicateBasisPoints = 250u;
    packetLoss.aToB.reorderBasisPoints = 500u;
    packetLoss.aToB.reorderExtraMs = 80u;
    packetLoss.aToB.blackoutEnabled = true;
    packetLoss.aToB.blackoutInputsOnly = true;
    packetLoss.aToB.blackoutStartUs = kBlackoutStartUs;
    packetLoss.aToB.blackoutEndUs = kBlackoutEndUs;
    packetLoss.aToB.forcedDrops = {
        ForcedDropBurst{wire::MessageType::Inputs, 250u, 29u},
        ForcedDropBurst{wire::MessageType::Inputs, 800u, 30u},
        ForcedDropBurst{wire::MessageType::Inputs, 1550u, 31u},
    };

    packetLoss.bToA = packetLoss.aToB;
    packetLoss.bToA.baseDelayMs = 44u;
    packetLoss.bToA.jitterMs = 13u;
    packetLoss.bToA.lossBasisPoints = 1875u;
    packetLoss.bToA.reorderExtraMs = 100u;
    packetLoss.bToA.forcedDrops = {
        ForcedDropBurst{wire::MessageType::Inputs, 400u, 29u},
        ForcedDropBurst{wire::MessageType::Inputs, 1000u, 30u},
        ForcedDropBurst{wire::MessageType::Inputs, 1700u, 31u},
    };
    packetLoss.delayA = 2u;
    packetLoss.rollbackA = 12u;
    packetLoss.delayB = 4u;
    packetLoss.rollbackB = 15u;
    packetLoss.requiresRollback = true;
    packetLoss.requiresStall = true;
    packetLoss.stressRecovery = true;
    profiles.push_back(packetLoss);

    SoakProfile stable250{};
    stable250.name = "stable_250";
    stable250.aToB.baseDelayMs = 125u;
    stable250.bToA.baseDelayMs = 125u;
    stable250.delayA = 2u;
    stable250.rollbackA = 12u;
    stable250.delayB = 2u;
    stable250.rollbackB = 12u;
    stable250.requiresRollback = true;
    profiles.push_back(stable250);

    SoakProfile asymOneWay{};
    asymOneWay.name = "asym_oneway";
    asymOneWay.aToB.baseDelayMs = 50u;
    asymOneWay.bToA.baseDelayMs = 1u;
    asymOneWay.delayA = 2u;
    asymOneWay.rollbackA = 8u;
    asymOneWay.delayB = 2u;
    asymOneWay.rollbackB = 8u;
    asymOneWay.requiresRollback = true;
    profiles.push_back(asymOneWay);

    return profiles;
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

bool fingerprintsEqual(const RunFingerprint& left,
                       const RunFingerprint& right,
                       std::string& reason) {
#define CHECK_FINGERPRINT(field) \
    do { \
        if (left.field != right.field) { \
            reason = #field; \
            return false; \
        } \
    } while (false)
    CHECK_FINGERPRINT(networkDigest);
    CHECK_FINGERPRINT(executionDigestA);
    CHECK_FINGERPRINT(executionDigestB);
    CHECK_FINGERPRINT(confirmedDigest);
    CHECK_FINGERPRINT(finalHashA);
    CHECK_FINGERPRINT(finalHashB);
    CHECK_FINGERPRINT(oracleHash);
    CHECK_FINGERPRINT(wallTicks);
#undef CHECK_FINGERPRINT
    if (!rollbackStatsEqual(left.statsA, right.statsA)) {
        reason = "statsA";
        return false;
    }
    if (!rollbackStatsEqual(left.statsB, right.statsB)) {
        reason = "statsB";
        return false;
    }
    if (!networkStatsEqual(left.networkAtoB, right.networkAtoB)) {
        reason = "networkAtoB";
        return false;
    }
    if (!networkStatsEqual(left.networkBtoA, right.networkBtoA)) {
        reason = "networkBtoA";
        return false;
    }
    return true;
}

class InputEndpoint {
public:
    bool addLocalSubmission(const TaggedInput& input, std::string& error) {
        if (input.frame < _localInputs.size()) {
            if (_localInputs[input.frame] != input.value) {
                error = "local input mutation";
                return false;
            }
            return true;
        }
        if (input.frame != _localInputs.size()) {
            error = "non-contiguous local submission";
            return false;
        }
        _localInputs.push_back(input.value);
        return true;
    }

    bool makePacket(std::vector<uint8_t>& bytes, std::string& error) const {
        bytes.clear();
        bytes.reserve(kInputPacketHeaderSize
                      + wire::kInputWindowSize * kInputEntrySize);
        appendU32(bytes, kInputPacketMagic);
        appendU32(bytes, advertisedAckThrough());

        const uint32_t oldest = _hasPeerAck
            ? _peerAckThrough + 1u
            : 0u;
        const size_t begin = std::min<size_t>(oldest, _localInputs.size());
        const size_t end = std::min<size_t>(
            _localInputs.size(), begin + wire::kInputWindowSize);
        const size_t count = end - begin;
        if (count > std::numeric_limits<uint8_t>::max()) {
            error = "input packet count overflow";
            return false;
        }
        bytes.push_back(static_cast<uint8_t>(count));
        bytes.push_back(0u);
        bytes.push_back(0u);
        bytes.push_back(0u);
        for (size_t index = begin; index < end; ++index) {
            appendU32(bytes, static_cast<uint32_t>(index));
            appendU16(bytes, _localInputs[index]);
        }
        if (bytes.size() > wire::kMaxDatagramSize) {
            error = "input packet exceeds wire MTU";
            return false;
        }
        return true;
    }

    bool receivePacket(const std::vector<uint8_t>& bytes,
                       RollbackSession& session,
                       std::string& error) {
        if (bytes.size() < kInputPacketHeaderSize) {
            error = "truncated input packet";
            return false;
        }
        size_t cursor = 0u;
        uint32_t magic = 0u;
        uint32_t ackThrough = wire::kNoFrame;
        if (!readU32(bytes, cursor, magic)
            || !readU32(bytes, cursor, ackThrough)
            || magic != kInputPacketMagic) {
            error = "invalid input packet header";
            return false;
        }
        const uint8_t count = bytes[cursor++];
        if (bytes[cursor++] != 0u
            || bytes[cursor++] != 0u
            || bytes[cursor++] != 0u
            || count > wire::kInputWindowSize
            || bytes.size() != kInputPacketHeaderSize
                + static_cast<size_t>(count) * kInputEntrySize) {
            error = "invalid input packet shape";
            return false;
        }

        if (ackThrough != wire::kNoFrame) {
            if (ackThrough >= _localInputs.size()) {
                error = "peer acknowledged an unsent frame";
                return false;
            }
            if (!_hasPeerAck || ackThrough > _peerAckThrough) {
                for (uint32_t frame = _hasPeerAck
                         ? _peerAckThrough + 1u : 0u;
                     frame <= ackThrough;
                     ++frame) {
                    if (frame >= _localInputs.size()) {
                        error = "peer ACK crossed a local input gap";
                        return false;
                    }
                }
                _hasPeerAck = true;
                _peerAckThrough = ackThrough;
            }
        }

        uint32_t previousFrame = 0u;
        bool hasPrevious = false;
        for (uint8_t index = 0u; index < count; ++index) {
            uint32_t frame = 0u;
            uint16_t value = 0u;
            if (!readU32(bytes, cursor, frame)
                || !readU16(bytes, cursor, value)) {
                error = "truncated input entry";
                return false;
            }
            if (hasPrevious && frame != previousFrame + 1u) {
                error = "non-contiguous input packet window";
                return false;
            }
            hasPrevious = true;
            previousFrame = frame;
            if (frame >= _remoteInputs.size()) {
                _remoteInputs.resize(
                    static_cast<size_t>(frame) + 1u);
            }
            InputSlot& slot = _remoteInputs[frame];
            if (slot.valid) {
                if (slot.value != value) {
                    error = "remote input mutation";
                    return false;
                }
                ++_duplicateInputs;
                continue;
            }

            const RemoteInputResult result =
                session.receiveRemoteInput(frame, value);
            if (result != RemoteInputResult::Inserted) {
                std::ostringstream stream;
                stream << "remote input rejected (frame=" << frame
                       << ", result=" << static_cast<int>(result) << ')';
                error = stream.str();
                return false;
            }
            slot.value = value;
            slot.valid = true;
            ++_uniqueInputs;
        }

        while (_nextRemoteContiguous < _remoteInputs.size()
               && _remoteInputs[_nextRemoteContiguous].valid) {
            ++_nextRemoteContiguous;
        }
        return cursor == bytes.size();
    }

    uint32_t advertisedAckThrough() const {
        return _nextRemoteContiguous == 0u
            ? wire::kNoFrame
            : static_cast<uint32_t>(_nextRemoteContiguous - 1u);
    }

    bool peerAcknowledgedThrough(const uint32_t frame) const {
        return _hasPeerAck && _peerAckThrough >= frame;
    }

    size_t localSubmissionCount() const {
        return _localInputs.size();
    }
    uint64_t duplicateInputs() const { return _duplicateInputs; }
    uint64_t uniqueInputs() const { return _uniqueInputs; }

private:
    std::vector<uint16_t> _localInputs;
    std::vector<InputSlot> _remoteInputs;
    size_t _nextRemoteContiguous = 0u;
    bool _hasPeerAck = false;
    uint32_t _peerAckThrough = 0u;
    uint64_t _duplicateInputs = 0u;
    uint64_t _uniqueInputs = 0u;
};

class SoakPeer {
public:
    SoakPeer(const uint8_t player,
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
          _snapshots(kHistoryCapacity),
          _executionDigest(kFnvOffset) {
    }

    bool valid() const { return _session.valid(); }
    uint32_t frontier() const { return _session.currentFrame(); }
    bool pendingRollback() const {
        return _session.hasPendingRollback();
    }

    bool prepareLocal(const uint32_t targetFrames, std::string& error) {
        if (frontier() < targetFrames) {
            const uint32_t sourceFrame = frontier();
            const uint16_t value = sourceInput(
                _inputSeed, _player, sourceFrame, _delay);
            if (!_session.captureLocalInput(sourceFrame, value)) {
                error = "local capture rejected";
                return false;
            }
        }
        TaggedInput submission{};
        while (_session.popLocalSubmission(submission)) {
            const uint16_t expected = delayedScriptedInput(
                _inputSeed, _player, submission.frame, _delay);
            if (submission.value != expected) {
                error = "delayed local submission disagrees with input oracle";
                return false;
            }
            if (!_endpoint.addLocalSubmission(submission, error)) {
                return false;
            }
        }
        return true;
    }

    bool makePacket(std::vector<uint8_t>& bytes, std::string& error) const {
        return _endpoint.makePacket(bytes, error);
    }

    bool receivePacket(const std::vector<uint8_t>& bytes,
                       std::string& error) {
        return _endpoint.receivePacket(bytes, _session, error);
    }

    bool step(const uint32_t targetFrames, std::string& error) {
        if (frontier() >= targetFrames && !pendingRollback()) {
            return true;
        }

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
            if (_maxConsecutiveStalls > kMaximumConsecutiveStalls) {
                error = "bounded-progress violation: stall episode exceeded "
                        + std::to_string(kMaximumConsecutiveStalls);
                return false;
            }
            return true;
        }

        _consecutiveStalls = 0u;
        if (action.kind == ActionKind::Rollback) {
            return performRollback(action, error);
        }
        if (action.kind != ActionKind::Advance
            || action.inputs.frame != frontier()
            || _simulation.state().frame != frontier()) {
            error = "invalid advance action";
            return false;
        }

        SnapshotSlot& snapshot =
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
            error = "advance commit rejected";
            return false;
        }
        return _simulation.state().frame == frontier();
    }

    bool popConfirmed(ConfirmedFrame& frame) {
        return _session.popConfirmedFrame(frame);
    }

    bool acknowledgedThrough(const uint32_t frame) const {
        return _endpoint.peerAcknowledgedThrough(frame);
    }

    const RollbackStats& stats() const { return _session.stats(); }
    uint32_t maxConsecutiveStalls() const {
        return _maxConsecutiveStalls;
    }
    uint64_t executionDigest() const { return _executionDigest; }
    uint64_t finalHash() const { return _simulation.hash(); }
    const ToyState& finalState() const { return _simulation.state(); }
    size_t localSubmissionCount() const {
        return _endpoint.localSubmissionCount();
    }
    uint64_t duplicateInputs() const {
        return _endpoint.duplicateInputs();
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
        config.historyCapacity = kHistoryCapacity;
        return config;
    }

    bool performRollback(const FrameAction& action, std::string& error) {
        if (action.rollbackFrom >= action.replayUntilExclusive
            || action.replayUntilExclusive != frontier()) {
            error = "invalid rollback range";
            return false;
        }
        const uint32_t depth =
            action.replayUntilExclusive - action.rollbackFrom;
        if (depth > _rollback) {
            error = "rollback exceeded peer-local maximum";
            return false;
        }
        SnapshotSlot& restore =
            _snapshots[action.rollbackFrom % _snapshots.size()];
        if (!restore.valid || restore.frame != action.rollbackFrom) {
            error = "rollback snapshot missing or stale";
            return false;
        }
        if (!_session.beginRollback(
                action.rollbackFrom, action.replayUntilExclusive)) {
            error = "rollback transaction rejected";
            return false;
        }
        _simulation.restore(restore.state);

        for (uint32_t frame = action.rollbackFrom;
             frame < action.replayUntilExclusive;
             ++frame) {
            CanonicalInputs inputs{};
            if (!_session.inputsForFrame(frame, inputs)
                || inputs.frame != frame
                || _simulation.state().frame != frame) {
                error = "rollback replay input unavailable";
                return false;
            }
            SnapshotSlot& snapshot =
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
                error = "rollback replay commit rejected";
                return false;
            }
        }
        if (!_session.finishRollback()) {
            error = "rollback transaction did not finish";
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
    std::vector<SnapshotSlot> _snapshots;
    InputEndpoint _endpoint;
    uint32_t _consecutiveStalls = 0u;
    uint32_t _maxConsecutiveStalls = 0u;
    uint64_t _executionDigest = kFnvOffset;
};

class ConfirmedOracle {
public:
    ConfirmedOracle(const uint64_t inputSeed,
                    const uint32_t simulationSeed,
                    const uint8_t delayA,
                    const uint8_t delayB)
        : _inputSeed(inputSeed),
          _delayA(delayA),
          _delayB(delayB),
          _simulation(simulationSeed) {
    }

    void pushA(const ConfirmedFrame& frame) {
        _pendingA.push_back(frame);
    }
    void pushB(const ConfirmedFrame& frame) {
        _pendingB.push_back(frame);
    }

    bool verifyAvailable(std::string& error) {
        while (!_pendingA.empty() && !_pendingB.empty()) {
            const ConfirmedFrame a = _pendingA.front();
            const ConfirmedFrame b = _pendingB.front();
            if (a.frame != _nextFrame || b.frame != _nextFrame) {
                error = "confirmed stream is not contiguous";
                return false;
            }
            if (a.p1 != b.p1 || a.p2 != b.p2
                || a.preStateHash != b.preStateHash) {
                error = "peers disagree on a confirmed frame";
                return false;
            }
            const uint16_t expectedP1 = delayedScriptedInput(
                _inputSeed, 0u, _nextFrame, _delayA);
            const uint16_t expectedP2 = delayedScriptedInput(
                _inputSeed, 1u, _nextFrame, _delayB);
            const uint64_t expectedPreHash = _simulation.hash();
            if (a.p1 != expectedP1 || a.p2 != expectedP2
                || a.preStateHash != expectedPreHash) {
                std::ostringstream stream;
                stream << "confirmed frame " << _nextFrame
                       << " disagrees with exact oracle";
                error = stream.str();
                return false;
            }
            digestU32(_digest, a.frame);
            digestU16(_digest, a.p1);
            digestU16(_digest, a.p2);
            digestU64(_digest, a.preStateHash);
            _simulation.step(a.p1, a.p2);
            ++_nextFrame;
            _pendingA.pop_front();
            _pendingB.pop_front();
        }
        return true;
    }

    uint32_t verifiedFrames() const { return _nextFrame; }
    uint64_t digest() const { return _digest; }
    uint64_t hash() const { return _simulation.hash(); }
    const ToyState& state() const { return _simulation.state(); }
    bool queuesEmpty() const {
        return _pendingA.empty() && _pendingB.empty();
    }

private:
    uint64_t _inputSeed = 0u;
    uint8_t _delayA = 0u;
    uint8_t _delayB = 0u;
    ToySimulation _simulation;
    uint32_t _nextFrame = 0u;
    uint64_t _digest = kFnvOffset;
    std::deque<ConfirmedFrame> _pendingA;
    std::deque<ConfirmedFrame> _pendingB;
};

class TestSpectator {
public:
    explicit TestSpectator(const uint32_t simulationSeed)
        : _simulation(simulationSeed) {
    }

    bool consume(const std::vector<ConfirmedFrame>& archive,
                 const size_t limit,
                 std::string& error) {
        size_t consumed = 0u;
        while (_nextIndex < archive.size() && consumed < limit) {
            const ConfirmedFrame& record = archive[_nextIndex];
            if (record.frame != _nextIndex
                || record.preStateHash != _simulation.hash()) {
                error = "spectator archive is non-contiguous or has a bad hash";
                return false;
            }
            _simulation.step(record.p1, record.p2);
            ++_nextIndex;
            ++consumed;
        }
        return true;
    }

    size_t consumedFrames() const { return _nextIndex; }
    uint64_t hash() const { return _simulation.hash(); }

private:
    ToySimulation _simulation;
    size_t _nextIndex = 0u;
};

bool packetDecisionEqual(const PacketDecision& left,
                         const PacketDecision& right) {
    return left.direction == right.direction
        && left.type == right.type
        && left.sequence == right.sequence
        && left.logicalOrdinal == right.logicalOrdinal
        && left.typeOccurrence == right.typeOccurrence
        && left.sendTimeUs == right.sendTimeUs
        && left.deliveryTimeUs == right.deliveryTimeUs
        && left.dropped == right.dropped
        && left.duplicated == right.duplicated
        && left.reordered == right.reordered
        && left.forcedDrop == right.forcedDrop
        && left.blackoutDrop == right.blackoutDrop;
}

bool verifyIndependentNetworkStreams(std::string& error) {
    LinkFaults faults{};
    faults.baseDelayMs = 31u;
    faults.jitterMs = 29u;
    faults.lossBasisPoints = 2700u;
    faults.duplicateBasisPoints = 3100u;
    faults.reorderBasisPoints = 3300u;
    faults.reorderExtraMs = 77u;

    DeterministicNetwork isolated(0xC001D00D12345678ull,
                                  faults, faults);
    DeterministicNetwork interleaved(0xC001D00D12345678ull,
                                     faults, faults);
    for (uint32_t ordinal = 0u; ordinal < 2048u; ++ordinal) {
        std::vector<uint8_t> bytes{
            static_cast<uint8_t>(ordinal),
            static_cast<uint8_t>(ordinal >> 8u),
        };
        const uint64_t nowUs = ordinal * 7919ull;
        isolated.send(Direction::AtoB, wire::MessageType::Inputs,
                      ordinal, bytes, nowUs);
        for (uint32_t noise = 0u; noise < ordinal % 5u; ++noise) {
            interleaved.send(Direction::BtoA, wire::MessageType::Inputs,
                             ordinal * 5u + noise, bytes,
                             nowUs + noise);
        }
        interleaved.send(Direction::AtoB, wire::MessageType::Inputs,
                         ordinal, bytes, nowUs);
    }

    std::vector<PacketDecision> interleavedA;
    for (const PacketDecision& decision : interleaved.decisions()) {
        if (decision.direction == Direction::AtoB) {
            interleavedA.push_back(decision);
        }
    }
    if (isolated.decisions().size() != interleavedA.size()) {
        error = "direction-scoped RNG decision count changed";
        return false;
    }
    for (size_t index = 0u; index < interleavedA.size(); ++index) {
        if (!packetDecisionEqual(
                isolated.decisions()[index], interleavedA[index])) {
            error = "B-to-A traffic perturbed A-to-B fault RNG";
            return false;
        }
    }

    for (uint32_t frame = 0u; frame < 4096u; ++frame) {
        const uint16_t before =
            delayedScriptedInput(0x1234u, 0u, frame, 3u);
        (void)splitMix64(interleaved.traceDigest() + frame);
        const uint16_t after =
            delayedScriptedInput(0x1234u, 0u, frame, 3u);
        if (before != after) {
            error = "input oracle is not counter deterministic";
            return false;
        }
    }
    return true;
}

bool validateProfileCoverage(const SoakProfile& profile,
                             const uint32_t targetFrames,
                             const uint64_t finalNowUs,
                             const RunFingerprint& result,
                             std::string& error) {
    const NetworkStats& ab = result.networkAtoB;
    const NetworkStats& ba = result.networkBtoA;
    if (profile.aToB.lossBasisPoints == 0u
        && profile.aToB.forcedDrops.empty()
        && !profile.aToB.blackoutEnabled
        && ab.droppedPackets != 0u) {
        error = "drop-free A-to-B profile dropped a packet";
        return false;
    }
    if (profile.bToA.lossBasisPoints == 0u
        && profile.bToA.forcedDrops.empty()
        && !profile.bToA.blackoutEnabled
        && ba.droppedPackets != 0u) {
        error = "drop-free B-to-A profile dropped a packet";
        return false;
    }
    if (profile.aToB.duplicateBasisPoints != 0u
        && targetFrames >= 1024u
        && ab.duplicatedPackets == 0u) {
        error = "configured A-to-B duplication was not exercised";
        return false;
    }
    if (profile.bToA.duplicateBasisPoints != 0u
        && targetFrames >= 1024u
        && ba.duplicatedPackets == 0u) {
        error = "configured B-to-A duplication was not exercised";
        return false;
    }
    if (profile.aToB.reorderBasisPoints != 0u
        && targetFrames >= 1024u
        && ab.reorderedPackets == 0u) {
        error = "configured A-to-B reordering was not exercised";
        return false;
    }
    if (profile.bToA.reorderBasisPoints != 0u
        && targetFrames >= 1024u
        && ba.reorderedPackets == 0u) {
        error = "configured B-to-A reordering was not exercised";
        return false;
    }

    if (profile.stressRecovery && targetFrames >= 4096u
        && finalNowUs > kBlackoutEndUs) {
        constexpr uint64_t expectedForcedDrops = 29u + 30u + 31u;
        if (ab.forcedDropPackets != expectedForcedDrops
            || ba.forcedDropPackets != expectedForcedDrops) {
            error = "29/30/31 forced-loss bursts were not all exercised";
            return false;
        }
        if (ab.blackoutDropPackets == 0u
            || ba.blackoutDropPackets == 0u) {
            error = "two-second bidirectional blackout was not exercised";
            return false;
        }
        if (ab.droppedPackets <= ab.forcedDropPackets
            || ba.droppedPackets <= ba.forcedDropPackets) {
            error = "random loss did not supplement forced loss";
            return false;
        }
    }
    return true;
}

RunResult runScenario(const SoakProfile& profile,
                      const uint64_t scenarioSeed,
                      const uint32_t targetFrames,
                      const bool spectatorsEnabled) {
    RunResult result{};
    const uint64_t inputSeed =
        splitMix64(scenarioSeed ^ 0x49504E5554534545ull);
    const uint64_t networkSeed =
        splitMix64(scenarioSeed ^ 0x4E4554574F524B31ull);
    const uint32_t simulationSeed = static_cast<uint32_t>(
        splitMix64(scenarioSeed ^ 0x53494D554C415445ull));

    SoakPeer peerA(0u, profile.delayA, profile.rollbackA,
                   inputSeed, simulationSeed);
    SoakPeer peerB(1u, profile.delayB, profile.rollbackB,
                   inputSeed, simulationSeed);
    if (!peerA.valid() || !peerB.valid()) {
        result.error = "invalid RollbackSession configuration";
        return result;
    }

    DeterministicNetwork network(
        networkSeed, profile.aToB, profile.bToA);
    network.setDecisionRetention(false);
    ConfirmedOracle oracle(inputSeed, simulationSeed,
                           profile.delayA, profile.delayB);

    std::vector<ConfirmedFrame> spectatorArchive;
    if (spectatorsEnabled) {
        spectatorArchive.reserve(targetFrames);
    }
    TestSpectator immediateSpectator(simulationSeed);
    TestSpectator lateSpectator(simulationSeed);
    bool lateStarted = false;
    const size_t lateStartFrame = std::max<size_t>(
        1u, static_cast<size_t>(targetFrames) * 3u / 4u);
    size_t lateStartedAtArchiveSize = 0u;

    uint32_t sequenceA = 1u;
    uint32_t sequenceB = 1u;
    uint64_t nowUs = 0u;
    uint64_t wallTicks = 0u;
    const uint64_t maximumTicks =
        static_cast<uint64_t>(targetFrames) * 4ull + 100000ull;

    while (wallTicks < maximumTicks) {
        if (!peerA.prepareLocal(targetFrames, result.error)
            || !peerB.prepareLocal(targetFrames, result.error)) {
            return result;
        }

        std::vector<uint8_t> bytesA;
        std::vector<uint8_t> bytesB;
        if (!peerA.makePacket(bytesA, result.error)
            || !peerB.makePacket(bytesB, result.error)) {
            return result;
        }
        network.send(Direction::AtoB, wire::MessageType::Inputs,
                     sequenceA++, bytesA, nowUs);
        network.send(Direction::BtoA, wire::MessageType::Inputs,
                     sequenceB++, bytesB, nowUs);

        std::vector<ScheduledPacket> delivered;
        network.deliverDue(nowUs, delivered);
        for (const ScheduledPacket& packet : delivered) {
            const bool received = packet.direction == Direction::AtoB
                ? peerB.receivePacket(packet.bytes, result.error)
                : peerA.receivePacket(packet.bytes, result.error);
            if (!received) {
                return result;
            }
        }

        if (!peerA.step(targetFrames, result.error)
            || !peerB.step(targetFrames, result.error)) {
            return result;
        }

        ConfirmedFrame confirmed{};
        while (peerA.popConfirmed(confirmed)) {
            oracle.pushA(confirmed);
            if (spectatorsEnabled) {
                spectatorArchive.push_back(confirmed);
            }
        }
        while (peerB.popConfirmed(confirmed)) {
            oracle.pushB(confirmed);
        }
        if (!oracle.verifyAvailable(result.error)) {
            return result;
        }

        if (spectatorsEnabled) {
            if (!immediateSpectator.consume(
                    spectatorArchive,
                    std::numeric_limits<size_t>::max(),
                    result.error)) {
                return result;
            }
            if (!lateStarted
                && spectatorArchive.size() >= lateStartFrame) {
                lateStarted = true;
                lateStartedAtArchiveSize = spectatorArchive.size();
            }
            if (lateStarted) {
                const size_t backlog = spectatorArchive.size()
                    - lateSpectator.consumedFrames();
                const size_t catchup = backlog >= 240u ? 8u
                    : backlog >= 120u ? 4u
                    : backlog >= 30u ? 2u : 1u;
                if (!lateSpectator.consume(
                        spectatorArchive, catchup, result.error)) {
                    return result;
                }
            }
        }

        ++wallTicks;
        const bool allFramesConfirmed =
            oracle.verifiedFrames() == targetFrames
            && oracle.queuesEmpty();
        const bool bothAtTarget =
            peerA.frontier() == targetFrames
            && peerB.frontier() == targetFrames
            && !peerA.pendingRollback()
            && !peerB.pendingRollback();
        const bool allInputsAcknowledged = targetFrames == 0u
            || (peerA.acknowledgedThrough(targetFrames - 1u)
                && peerB.acknowledgedThrough(targetFrames - 1u));
        if (bothAtTarget && allFramesConfirmed
            && allInputsAcknowledged) {
            break;
        }
        nowUs += kFrameDurationUs;
    }

    if (wallTicks >= maximumTicks) {
        result.error = "scenario exceeded deterministic progress deadline";
        return result;
    }
    if (oracle.verifiedFrames() != targetFrames
        || peerA.frontier() != targetFrames
        || peerB.frontier() != targetFrames) {
        result.error = "scenario ended before both peers were confirmed";
        return result;
    }

    if (spectatorsEnabled) {
        if (!lateStarted
            || lateStartedAtArchiveSize < lateStartFrame) {
            result.error =
                "75%-late spectator never reached its activation frontier";
            return result;
        }
        if (!immediateSpectator.consume(
                spectatorArchive,
                std::numeric_limits<size_t>::max(),
                result.error)
            || !lateSpectator.consume(
                spectatorArchive,
                std::numeric_limits<size_t>::max(),
                result.error)) {
            return result;
        }
        if (spectatorArchive.size() != targetFrames
            || immediateSpectator.consumedFrames() != targetFrames
            || lateSpectator.consumedFrames() != targetFrames
            || immediateSpectator.hash() != oracle.hash()
            || lateSpectator.hash() != oracle.hash()) {
            result.error = "spectator playback did not reach exact host state";
            return result;
        }
    }

    if (peerA.finalState() != oracle.state()
        || peerB.finalState() != oracle.state()) {
        result.error = "final corrected peer state disagrees with oracle";
        return result;
    }
    const RollbackStats& statsA = peerA.stats();
    const RollbackStats& statsB = peerB.stats();
    if (statsA.visibleAdvances != targetFrames
        || statsB.visibleAdvances != targetFrames
        || statsA.uniqueCaptures != targetFrames
        || statsB.uniqueCaptures != targetFrames
        || statsA.localSubmissions
            != static_cast<uint64_t>(targetFrames) + profile.delayA
        || statsB.localSubmissions
            != static_cast<uint64_t>(targetFrames) + profile.delayB
        || peerA.localSubmissionCount()
            != static_cast<size_t>(targetFrames) + profile.delayA
        || peerB.localSubmissionCount()
            != static_cast<size_t>(targetFrames) + profile.delayB) {
        result.error = "capture/delay submission accounting failed";
        return result;
    }
    if (statsA.maxRollbackDepth > profile.rollbackA
        || statsB.maxRollbackDepth > profile.rollbackB
        || statsA.maxSpeculativeFrames > profile.rollbackA
        || statsB.maxSpeculativeFrames > profile.rollbackB) {
        result.error = "rollback/speculation exceeded peer-local bound";
        return result;
    }
    if (!rollbackStatsInternallyConsistent(statsA)
        || !rollbackStatsInternallyConsistent(statsB)
        || statsA.confirmedFrames != targetFrames
        || statsB.confirmedFrames != targetFrames) {
        result.error =
            "rollback diagnostic counter accounting failed";
        return result;
    }
    if (peerA.maxConsecutiveStalls() > kMaximumConsecutiveStalls
        || peerB.maxConsecutiveStalls() > kMaximumConsecutiveStalls
        || statsA.maxConsecutiveStallPolls
            != peerA.maxConsecutiveStalls()
        || statsB.maxConsecutiveStallPolls
            != peerB.maxConsecutiveStalls()) {
        result.error = "stall bound exceeded";
        return result;
    }
    if (profile.requiresRollback && targetFrames >= 1024u
        && statsA.rollbackCount + statsB.rollbackCount == 0u) {
        result.error = "profile did not exercise rollback";
        return result;
    }
    if (profile.requiresStall && targetFrames >= 1024u
        && statsA.stallPolls + statsB.stallPolls == 0u) {
        result.error = "profile did not exercise bounded stalls";
        return result;
    }
    if ((profile.aToB.duplicateBasisPoints != 0u
         || profile.bToA.duplicateBasisPoints != 0u)
        && targetFrames >= 1024u
        && peerA.duplicateInputs() + peerB.duplicateInputs() == 0u) {
        result.error = "duplicate datagrams never reached idempotent ingress";
        return result;
    }

    RunFingerprint& fingerprint = result.fingerprint;
    fingerprint.networkDigest = network.traceDigest();
    fingerprint.executionDigestA = peerA.executionDigest();
    fingerprint.executionDigestB = peerB.executionDigest();
    fingerprint.confirmedDigest = oracle.digest();
    fingerprint.finalHashA = peerA.finalHash();
    fingerprint.finalHashB = peerB.finalHash();
    fingerprint.oracleHash = oracle.hash();
    fingerprint.wallTicks = wallTicks;
    fingerprint.statsA = statsA;
    fingerprint.statsB = statsB;
    fingerprint.networkAtoB = network.stats(Direction::AtoB);
    fingerprint.networkBtoA = network.stats(Direction::BtoA);
    if (!validateProfileCoverage(
            profile, targetFrames, nowUs, fingerprint, result.error)) {
        return result;
    }

    result.ok = true;
    return result;
}

void addRunToSummary(const RunFingerprint& result,
                     SoakSummary& summary) {
    ++summary.scenarioRuns;
    summary.simulatedVisibleFrames +=
        result.statsA.visibleAdvances + result.statsB.visibleAdvances;
    summary.replayedFrames +=
        result.statsA.replayedFrames + result.statsB.replayedFrames;
    summary.rollbackCount +=
        result.statsA.rollbackCount + result.statsB.rollbackCount;
    summary.stallPolls +=
        result.statsA.stallPolls + result.statsB.stallPolls;
    summary.logicalPackets +=
        result.networkAtoB.logicalPackets
        + result.networkBtoA.logicalPackets;
    summary.droppedPackets +=
        result.networkAtoB.droppedPackets
        + result.networkBtoA.droppedPackets;
    summary.duplicatedPackets +=
        result.networkAtoB.duplicatedPackets
        + result.networkBtoA.duplicatedPackets;
    summary.reorderedPackets +=
        result.networkAtoB.reorderedPackets
        + result.networkBtoA.reorderedPackets;
}

bool profileSelected(const SoakProfile& profile,
                     const std::vector<std::string>& filters) {
    if (filters.empty()) {
        return true;
    }
    for (const std::string& filter : filters) {
        if (filter == profile.name
            || (filter == "loss"
                && std::string(profile.name) == "packet_loss")) {
            return true;
        }
    }
    return false;
}

}

const char* soakModeName(const SoakMode mode) {
    switch (mode) {
    case SoakMode::Quick: return "quick";
    case SoakMode::Full: return "full";
    case SoakMode::Nightly: return "nightly";
    }
    return "unknown";
}

uint32_t soakFramesForMode(const SoakMode mode) {
    switch (mode) {
    case SoakMode::Quick: return 16384u;
    case SoakMode::Full: return 120000u;
    case SoakMode::Nightly: return 1000000u;
    }
    return 16384u;
}

std::vector<uint64_t> soakSeedsForMode(const SoakMode mode) {
    static constexpr std::array<uint64_t, 16> seeds{{
        0x0000000000000001ull,
        0x00000000C0DEC0DEull,
        0x9E3779B97F4A7C15ull,
        0xD1B54A32D192ED03ull,
        0xA0761D6478BD642Full,
        0xE7037ED1A0B428DBull,
        0x8EBC6AF09C88C6E3ull,
        0x589965CC75374CC3ull,
        0x1D8E4E27C47D124Full,
        0xEB44ACCAB455D165ull,
        0xC6BC279692B5CC83ull,
        0xDB4F0B9175AE2165ull,
        0xBBE0563303A4615Full,
        0xA0F2EC75A1FE1575ull,
        0x89E182857D9ED689ull,
        0xF1357AEA2E62A9C5ull,
    }};
    size_t count = 3u;
    if (mode == SoakMode::Full) {
        count = 8u;
    } else if (mode == SoakMode::Nightly) {
        count = seeds.size();
    }
    return std::vector<uint64_t>(seeds.begin(), seeds.begin() + count);
}

std::vector<std::string> soakProfileNames() {
    std::vector<std::string> names;
    for (const SoakProfile& profile : makeProfiles()) {
        names.emplace_back(profile.name);
    }
    return names;
}

int runSoak(const SoakOptions& options, SoakSummary* const outputSummary) {
    SoakSummary summary{};
    std::string selfTestError;
    if (!verifyIndependentNetworkStreams(selfTestError)) {
        std::fprintf(stderr, "[FAIL] deterministic RNG isolation: %s\n",
                     selfTestError.c_str());
        return 1;
    }

    const uint32_t frames = options.frameOverride != 0u
        ? options.frameOverride
        : soakFramesForMode(options.mode);
    const std::vector<uint64_t> seeds =
        options.seedOverride.empty()
        ? soakSeedsForMode(options.mode)
        : options.seedOverride;
    const std::vector<SoakProfile> profiles = makeProfiles();

    size_t selectedProfiles = 0u;
    for (const SoakProfile& profile : profiles) {
        if (profileSelected(profile, options.profileFilter)) {
            ++selectedProfiles;
        }
    }
    if (selectedProfiles == 0u) {
        std::fprintf(stderr, "no matching soak profile selected\n");
        return 2;
    }
    for (const std::string& filter : options.profileFilter) {
        bool found = false;
        for (const SoakProfile& profile : profiles) {
            if (filter == profile.name
                || (filter == "loss"
                    && std::string(profile.name) == "packet_loss")) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::fprintf(stderr, "unknown soak profile: %s\n",
                         filter.c_str());
            return 2;
        }
    }

    std::printf(
        "qoh99 deterministic netplay soak: mode=%s frames=%" PRIu32
        " profiles=%zu seeds=%zu spectators=%s\n",
        soakModeName(options.mode), frames, selectedProfiles, seeds.size(),
        options.verifySpectatorInvariance ? "paired" : "off");
    std::printf(
        "fault coverage: stable 8/100ms, jitter 15+/-25 and 90+/-70ms, "
        "15%%/18.75%% loss, 25%% asymmetry, dup/reorder, 29/30/31 bursts, "
        "2s blackout\n");

    for (const SoakProfile& profile : profiles) {
        if (!profileSelected(profile, options.profileFilter)) {
            continue;
        }
        for (const uint64_t seed : seeds) {
            const RunResult baseline =
                runScenario(profile, seed, frames, false);
            if (!baseline.ok) {
                std::fprintf(
                    stderr,
                    "[FAIL] %s seed=0x%016" PRIX64 " baseline: %s\n",
                    profile.name, seed, baseline.error.c_str());
                return 1;
            }
            addRunToSummary(baseline.fingerprint, summary);

            if (options.verifySpectatorInvariance) {
                const RunResult observed =
                    runScenario(profile, seed, frames, true);
                if (!observed.ok) {
                    std::fprintf(
                        stderr,
                        "[FAIL] %s seed=0x%016" PRIX64
                        " spectators: %s\n",
                        profile.name, seed, observed.error.c_str());
                    return 1;
                }
                std::string differingField;
                if (!fingerprintsEqual(
                        baseline.fingerprint,
                        observed.fingerprint,
                        differingField)) {
                    std::fprintf(
                        stderr,
                        "[FAIL] %s seed=0x%016" PRIX64
                        " spectator influenced player field %s\n",
                        profile.name, seed, differingField.c_str());
                    return 1;
                }
                addRunToSummary(observed.fingerprint, summary);
            }

            ProductionStackProfile productionProfile{};
            productionProfile.name = profile.name;
            productionProfile.aToB = profile.aToB;
            productionProfile.bToA = profile.bToA;
            productionProfile.delayA = profile.delayA;
            productionProfile.delayB = profile.delayB;
            productionProfile.rollbackA = profile.rollbackA;
            productionProfile.rollbackB = profile.rollbackB;
            const ProductionStackRun productionBaseline =
                runProductionStack(
                    productionProfile, seed, frames, false);
            if (!productionBaseline.ok) {
                std::fprintf(
                    stderr,
                    "[FAIL] %s seed=0x%016" PRIX64
                    " production baseline: %s\n",
                    profile.name, seed,
                    productionBaseline.error.c_str());
                return 1;
            }
            ++summary.productionRuns;
            summary.productionPlayerPackets +=
                productionBaseline.fingerprint.aToB.logicalPackets
                + productionBaseline.fingerprint.bToA.logicalPackets;

            ProductionStackRun productionObserved{};
            if (options.verifySpectatorInvariance) {
                productionObserved = runProductionStack(
                    productionProfile, seed, frames, true);
                if (!productionObserved.ok) {
                    std::fprintf(
                        stderr,
                        "[FAIL] %s seed=0x%016" PRIX64
                        " production spectators: %s\n",
                        profile.name, seed,
                        productionObserved.error.c_str());
                    return 1;
                }
                std::string productionDifference;
                if (!productionFingerprintsEqual(
                        productionBaseline.fingerprint,
                        productionObserved.fingerprint,
                        productionDifference)) {
                    std::fprintf(
                        stderr,
                        "[FAIL] %s seed=0x%016" PRIX64
                        " production spectator influenced player field %s\n",
                        profile.name, seed,
                        productionDifference.c_str());
                    return 1;
                }
                ++summary.productionRuns;
                summary.productionPlayerPackets +=
                    productionObserved.fingerprint.aToB.logicalPackets
                    + productionObserved.fingerprint.bToA.logicalPackets;
                summary.productionSpectatorPackets +=
                    productionObserved.spectatorPackets;
                summary.productionSpectatorDropped +=
                    productionObserved.spectatorDropped;
            }

            const RunFingerprint& fp = baseline.fingerprint;
            std::printf(
                "[ OK ] %-12s seed=0x%016" PRIX64
                " ticks=%" PRIu64 " rb=%" PRIu64 "/%" PRIu64
                " stalls=%" PRIu64 "/%" PRIu64
                " dropped=%" PRIu64 " digest=%016" PRIX64 "\n",
                profile.name, seed, fp.wallTicks,
                fp.statsA.rollbackCount, fp.statsB.rollbackCount,
                fp.statsA.stallPolls, fp.statsB.stallPolls,
                fp.networkAtoB.droppedPackets
                    + fp.networkBtoA.droppedPackets,
                fp.confirmedDigest);
            if (options.verifySpectatorInvariance) {
                std::printf(
                    "      production: player-packets=%" PRIu64
                    " viewers=%" PRIu64 " spectator-packets=%" PRIu64
                    " spectator-drop=%" PRIu64 " resume=%" PRIu64
                    " rb=%" PRIu64 "/%" PRIu64
                    " stalls=%" PRIu64 "/%" PRIu64 "\n",
                    productionBaseline.fingerprint.aToB.logicalPackets
                        + productionBaseline.fingerprint.bToA.logicalPackets,
                    productionObserved.completedViewers,
                    productionObserved.spectatorPackets,
                    productionObserved.spectatorDropped,
                    productionObserved.spectatorResumes,
                    productionBaseline.fingerprint.rollbackA.rollbackCount,
                    productionBaseline.fingerprint.rollbackB.rollbackCount,
                    productionBaseline.fingerprint.rollbackA.stallPolls,
                    productionBaseline.fingerprint.rollbackB.stallPolls);
            }
            if (options.verbose) {
                std::printf(
                    "      net=%016" PRIX64 " exec=%016" PRIX64
                    "/%016" PRIX64 " replay=%" PRIu64 "/%" PRIu64
                    " max-rb=%" PRIu32 "/%" PRIu32 "\n",
                    fp.networkDigest, fp.executionDigestA,
                    fp.executionDigestB,
                    fp.statsA.replayedFrames, fp.statsB.replayedFrames,
                    fp.statsA.maxRollbackDepth,
                    fp.statsB.maxRollbackDepth);
                std::printf(
                    "      diag: predicted=%" PRIu64 "/%" PRIu64
                    " rb-frames=%" PRIu64 "/%" PRIu64
                    " mismatch=%" PRIu64 "/%" PRIu64
                    " confirmed=%" PRIu64 "/%" PRIu64
                    " episodes=%" PRIu64 "/%" PRIu64
                    " max-stall=%" PRIu32 "/%" PRIu32
                    " out-of-order=%" PRIu64 "/%" PRIu64
                    " max-hole=%" PRIu32 "/%" PRIu32 "\n",
                    fp.statsA.predictedAdvances,
                    fp.statsB.predictedAdvances,
                    fp.statsA.rollbackFrames,
                    fp.statsB.rollbackFrames,
                    fp.statsA.mismatchingRemoteInputs,
                    fp.statsB.mismatchingRemoteInputs,
                    fp.statsA.confirmedFrames,
                    fp.statsB.confirmedFrames,
                    fp.statsA.stallEpisodes,
                    fp.statsB.stallEpisodes,
                    fp.statsA.maxConsecutiveStallPolls,
                    fp.statsB.maxConsecutiveStallPolls,
                    fp.statsA.remoteOutOfOrderInputs,
                    fp.statsB.remoteOutOfOrderInputs,
                    fp.statsA.maxRemoteHoleSpan,
                    fp.statsB.maxRemoteHoleSpan);
            }
        }
    }

    std::printf(
        "PASS: runs=%" PRIu64 " visible=%" PRIu64
        " replay=%" PRIu64 " rollbacks=%" PRIu64
        " stalls=%" PRIu64 " packets=%" PRIu64
        " drop=%" PRIu64 " dup=%" PRIu64
        " reorder=%" PRIu64 "\n",
        summary.scenarioRuns, summary.simulatedVisibleFrames,
        summary.replayedFrames, summary.rollbackCount,
        summary.stallPolls, summary.logicalPackets,
        summary.droppedPackets, summary.duplicatedPackets,
        summary.reorderedPackets);
    std::printf(
        "production PASS: runs=%" PRIu64
        " player-packets=%" PRIu64
        " spectator-packets=%" PRIu64
        " spectator-drop=%" PRIu64 "\n",
        summary.productionRuns,
        summary.productionPlayerPackets,
        summary.productionSpectatorPackets,
        summary.productionSpectatorDropped);
    if (outputSummary != nullptr) {
        *outputSummary = summary;
    }
    return 0;
}

}
