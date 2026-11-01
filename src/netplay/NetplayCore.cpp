#include "NetplayCore.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace qnetplay {
namespace {

bool serialAfter(uint32_t a, uint32_t b) {
    const uint32_t distance = a - b;
    return distance != 0 && distance < 0x80000000u;
}

bool serialBefore(uint32_t a, uint32_t b) {
    return serialAfter(b, a);
}

bool frameInRange(uint32_t frame,
                  uint32_t center,
                  uint32_t maxPast,
                  uint32_t maxFuture) {
    if (frame == center) return true;
    if (serialAfter(frame, center)) return frame - center <= maxFuture;
    if (serialBefore(frame, center)) return center - frame <= maxPast;
    return false;
}

bool tagAfter(const wire::SyncHashTag& a, const wire::SyncHashTag& b) {
    if (a.layoutGeneration != b.layoutGeneration)
        return serialAfter(a.layoutGeneration, b.layoutGeneration);
    if (a.epoch != b.epoch) return serialAfter(a.epoch, b.epoch);
    return serialAfter(a.frame, b.frame);
}

bool sameReadyState(const wire::ReadyPayload& a,
                    const wire::ReadyPayload& b) {
    return a.seedGameplay == b.seedGameplay
        && a.seedCosmetic == b.seedCosmetic
        && a.selector == b.selector
        && a.bootstrapHash == b.bootstrapHash
        && a.timerMode == b.timerMode
        && a.aiDifficulty == b.aiDifficulty
        && a.lifeMultiplier == b.lifeMultiplier
        && a.roundsToWinMinusOne == b.roundsToWinMinusOne;
}

bool sameHash(const wire::SyncHashPayload& a,
              const wire::SyncHashPayload& b) {
    return a.tag == b.tag
        && a.rngGameplay == b.rngGameplay
        && a.rngCosmetic == b.rngCosmetic
        && a.rngSelector == b.rngSelector
        && a.hp0 == b.hp0
        && a.hp1 == b.hp1
        && a.gameplayHash == b.gameplayHash;
}

bool validTerminalCode(wire::TerminalErrorCode code) {
    switch (code) {
        case wire::TerminalErrorCode::PeerRequested:
        case wire::TerminalErrorCode::ProtocolViolation:
        case wire::TerminalErrorCode::TransportFailure:
        case wire::TerminalErrorCode::Timeout:
        case wire::TerminalErrorCode::InternalFailure:
        case wire::TerminalErrorCode::ConfirmedDesync:
        case wire::TerminalErrorCode::InvalidConfiguration:
            return true;
    }
    return false;
}

IngressResult mapDecodeError(wire::DecodeError error) {
    switch (error) {
        case wire::DecodeError::None:            return IngressResult::Accepted;
        case wire::DecodeError::TooShort:        return IngressResult::TooShort;
        case wire::DecodeError::BadMagic:        return IngressResult::BadMagic;
        case wire::DecodeError::BadVersion:      return IngressResult::BadVersion;
        case wire::DecodeError::BadType:         return IngressResult::BadType;
        case wire::DecodeError::BadLength:       return IngressResult::BadLength;
        case wire::DecodeError::BadFlags:        return IngressResult::BadFlags;
        case wire::DecodeError::SessionMismatch: return IngressResult::SessionMismatch;
        case wire::DecodeError::BadCrc:          return IngressResult::BadCrc;
    }
    return IngressResult::BadPayload;
}

wire::PeerRole oppositeRole(wire::PeerRole role) {
    return role == wire::PeerRole::Host
        ? wire::PeerRole::Client
        : wire::PeerRole::Host;
}

}

const char* ingressResultName(const IngressResult result) {
    switch (result) {
        case IngressResult::Accepted: return "accepted";
        case IngressResult::TooShort: return "too-short";
        case IngressResult::BadMagic: return "bad-magic";
        case IngressResult::BadVersion: return "bad-version";
        case IngressResult::BadType: return "bad-type";
        case IngressResult::BadLength: return "bad-length";
        case IngressResult::BadFlags: return "bad-flags";
        case IngressResult::SessionMismatch: return "session-mismatch";
        case IngressResult::BadCrc: return "bad-crc";
        case IngressResult::WrongSource: return "wrong-source";
        case IngressResult::BadPayload: return "bad-payload";
        case IngressResult::UnexpectedMessage: return "unexpected-message";
        case IngressResult::CompatibilityMismatch:
            return "compatibility-mismatch";
        case IngressResult::PeerIdentityMismatch:
            return "peer-identity-mismatch";
        case IngressResult::InvalidConfiguration:
            return "invalid-configuration";
        case IngressResult::InvalidAck: return "invalid-ack";
        case IngressResult::FutureAck: return "future-ack";
        case IngressResult::FrameOutOfRange: return "frame-out-of-range";
        case IngressResult::ConflictingInput: return "conflicting-input";
        case IngressResult::InvalidControl: return "invalid-control";
        case IngressResult::ConflictingControl:
            return "conflicting-control";
        case IngressResult::QueueFull: return "queue-full";
        case IngressResult::Terminal: return "terminal";
        case IngressResult::Count: break;
    }
    return "unknown";
}

const char* syncHashSubmitResultName(
    const SyncHashSubmitResult result) {
    switch (result) {
        case SyncHashSubmitResult::Accepted: return "accepted";
        case SyncHashSubmitResult::ExactDuplicate:
            return "exact-duplicate";
        case SyncHashSubmitResult::InvalidState: return "invalid-state";
        case SyncHashSubmitResult::InvalidPayload: return "invalid-payload";
        case SyncHashSubmitResult::AlreadyAcknowledged:
            return "already-acknowledged";
        case SyncHashSubmitResult::OutOfOrder: return "out-of-order";
        case SyncHashSubmitResult::QueueFull: return "queue-full";
    }
    return "unknown";
}

NetplayCore::NetplayCore(const CoreConfig& config) {
    reset(config);
}

bool NetplayCore::validConfig(const CoreConfig& config) {
    const bool validRole =
        config.localRole == wire::PeerRole::Host
        || config.localRole == wire::PeerRole::Client;
    if (!validRole || config.localPlayer > 1
        || config.compatibilityId == 0u
        || !compat::isValidCompatibilityIdentity(
               config.compatibilityIdentity)
        || config.compatibilityIdentity.protocolVersion != wire::kVersion
        || config.firstInputFrame == wire::kNoFrame
        || config.expectedPeer.ipv4 == 0
        || config.sessionId == 0
        || (config.cookie.low == 0 && config.cookie.high == 0)
        || config.helloResendMs == 0
        || config.inputResendMs == 0
        || config.readyResendMs == 0
        || config.hashResendMs == 0
        || config.pingIntervalMs == 0
        || config.disconnectResendMs == 0
        || config.livenessTimeoutMs <= config.pingIntervalMs
        || config.maxPastFrames >= kInputRingSize
        || config.maxFutureFrames >= kInputRingSize
        || static_cast<uint64_t>(config.maxPastFrames)
             + config.maxFutureFrames + 1 > kInputRingSize) {
        return false;
    }
    if (config.localRole == wire::PeerRole::Client)
        return !config.learnPeerPort && config.expectedPeer.port != 0;
    if (config.learnPeerPort) return config.expectedPeer.port == 0;
    return config.expectedPeer.port != 0;
}

void NetplayCore::reset(const CoreConfig& config) {
    _config = config;
    _configValid = validConfig(config);
    _lastCompatibilityIdentityMismatch = compat::IdentityMismatch::None;
    _peer = config.expectedPeer;
    _peerLocked = _configValid && _peer.port != 0;
    _established = false;
    _pollClockStarted = false;
    _firstPollMs = 0;
    _lastValidReceiveMs = 0;
    _nextSequence = 1;
    _simulationFrame = config.firstInputFrame;

    _localInputs = {};
    _remoteInputs = {};
    _hasLocalInput = false;
    _lowestLocalFrame = config.firstInputFrame;
    _highestLocalFrame = config.firstInputFrame;
    _hasSentLocalInput = false;
    _highestSentLocalFrame = config.firstInputFrame;
    _hasPeerAck = false;
    _peerAckThrough = 0;
    _hasRemoteAck = false;
    _remoteAckThrough = 0;
    _nextRemoteAckFrame = config.firstInputFrame;
    _hasRemoteNewest = false;
    _remoteNewest = config.firstInputFrame;
    _lastNewRemoteInputMs = 0;
    _hasSimulationFrame = false;
    _hasPeerFrontier = false;
    _peerSimFrontier = 0;
    _peerFrontierRecvMs = 0;
    _localPressure = {};
    _peerPressure = {};
    _hasPeerPressure = false;
    _hasPeerPressureFrontier = false;
    _peerPressureFrontier = 0;
    _hasPeerPressureSequence = false;
    _peerPressureSequence = 0;
    _peerPressureRecvMs = 0;

    _inputDirty = false;
    _ackDirty = false;
    _sentInputs = false;
    _lastInputSendMs = 0;
    _sentHello = false;
    _lastHelloSendMs = 0;
    _helloAckPending = false;

    _hasLocalReady = false;
    _localReady = {};
    _hasRemoteReady = false;
    _remoteReady = {};
    _localReadyAcknowledged = false;
    _readyDirty = false;
    _sentReady = false;
    _lastReadySendMs = 0;

    _localHashes.clear();
    _remoteHashes.clear();
    _remoteHashSeen.clear();
    _hashAcksPending.clear();
    _localHashAcked.clear();
    _hasLastLocalHashTag = false;
    _lastLocalHashTag = {};
    _hasLastRemoteHashTag = false;
    _lastRemoteHashTag = {};
    _hashDirty = false;
    _sentHash = false;
    _lastHashSendMs = 0;

    _pongPending = false;
    _pendingPong = {};
    _pingOutstanding = false;
    _outstandingPing = {};
    _outstandingPingRetransmitted = false;
    _nextPingId = 1;
    _lastPingSendMs = 0;
    _hasRtt = false;
    _rttMs = 0;

    _terminal = {};
    _hasRemoteDisconnect = false;
    _remoteDisconnect = {};
    _disconnectAckPending = false;
    _localDisconnectAcknowledged = false;
    _sentDisconnect = false;
    _lastDisconnectSendMs = 0;
    if (!_configValid) {
        setLocalTerminalError(
            wire::TerminalErrorCode::InvalidConfiguration, 0,
            "invalid NetplayCore configuration");
    }
}

bool NetplayCore::setSimulationFrame(uint32_t frame) {
    if (!_configValid || _terminal.active || frame == wire::kNoFrame)
        return false;
    _simulationFrame = frame;
    _hasSimulationFrame = true;
    return true;
}

bool NetplayCore::submitLocalInput(uint32_t frame, uint16_t input) {
    if (!_configValid || _terminal.active || frame == wire::kNoFrame)
        return false;
    if (!_hasLocalInput) {
        if (frame != _config.firstInputFrame) return false;
    } else {
        const InputSlot& existing =
            _localInputs[frame % kInputRingSize];
        if (existing.valid && existing.frame == frame)
            return existing.input == input;
        if (frame != _highestLocalFrame + 1) return false;
    }

    InputSlot& slot = _localInputs[frame % kInputRingSize];
    if (slot.valid && slot.frame != frame) {
        const bool oldStillUnacked =
            !_hasPeerAck || serialAfter(slot.frame, _peerAckThrough);
        if (oldStillUnacked) return false;
    }
    slot = InputSlot{ frame, input, true };
    if (!_hasLocalInput) {
        _hasLocalInput = true;
        _lowestLocalFrame = frame;
    }
    _highestLocalFrame = frame;
    _inputDirty = true;
    return true;
}

bool NetplayCore::submitLocalReady(const wire::ReadyPayload& ready) {
    if (!_configValid || _terminal.active || ready.flags != 0
        || !wire::isValidReadyPayload(ready)) {
        return false;
    }
    if (_hasLocalReady) return sameReadyState(_localReady, ready);
    _localReady = ready;
    _localReady.flags = 0;
    _hasLocalReady = true;
    _readyDirty = true;
    return true;
}

bool NetplayCore::getRemoteReady(wire::ReadyPayload& ready) const {
    if (!_hasRemoteReady) return false;
    ready = _remoteReady;
    ready.flags = 0;
    return true;
}

bool NetplayCore::readyComplete() const {
    return _hasLocalReady && _hasRemoteReady && _localReadyAcknowledged
        && !_terminal.active;
}

SyncHashSubmitResult NetplayCore::submitLocalSyncHashDetailed(
    const wire::SyncHashPayload& hash) {
    if (!_configValid || _terminal.active)
        return SyncHashSubmitResult::InvalidState;
    if (!wire::isValidSyncHashPayload(hash))
        return SyncHashSubmitResult::InvalidPayload;
    if (_hasLastLocalHashTag) {
        if (hash.tag == _lastLocalHashTag) {
            if (!_localHashes.empty()
                && _localHashes.back().tag == hash.tag) {
                return sameHash(_localHashes.back(), hash)
                    ? SyncHashSubmitResult::ExactDuplicate
                    : SyncHashSubmitResult::OutOfOrder;
            }
            return SyncHashSubmitResult::AlreadyAcknowledged;
        }
        if (!tagAfter(hash.tag, _lastLocalHashTag))
            return SyncHashSubmitResult::OutOfOrder;
    }
    if (_localHashes.size() >= kHashQueueSize)
        return SyncHashSubmitResult::QueueFull;
    _localHashes.push_back(hash);
    _lastLocalHashTag = hash.tag;
    _hasLastLocalHashTag = true;
    _hashDirty = true;
    return SyncHashSubmitResult::Accepted;
}

bool NetplayCore::submitLocalSyncHash(
    const wire::SyncHashPayload& hash) {
    const SyncHashSubmitResult result =
        submitLocalSyncHashDetailed(hash);
    return result == SyncHashSubmitResult::Accepted
        || result == SyncHashSubmitResult::ExactDuplicate;
}

bool NetplayCore::takeRemoteSyncHash(wire::SyncHashPayload& hash) {
    if (_remoteHashes.empty()) return false;
    hash = _remoteHashes.front();
    _remoteHashes.pop_front();
    return true;
}

bool NetplayCore::peekRemoteSyncHash(wire::SyncHashPayload& hash) const {
    if (_remoteHashes.empty()) return false;
    hash = _remoteHashes.front();
    return true;
}

bool NetplayCore::localSyncHashAcknowledged(
    const wire::SyncHashTag& tag) const {
    return std::find(_localHashAcked.begin(), _localHashAcked.end(), tag)
        != _localHashAcked.end();
}

void NetplayCore::setLocalTerminalError(wire::TerminalErrorCode code,
                                        uint32_t detail,
                                        const char* reason) {
    if (_terminal.active) return;
    const wire::TerminalErrorCode canonicalCode =
        validTerminalCode(code)
            ? code
            : wire::TerminalErrorCode::ProtocolViolation;
    _terminal.active = true;
    _terminal.fromPeer = false;
    _terminal.code = canonicalCode;
    _terminal.detail = detail;
    const char* canonicalReason =
        reason && *reason
            ? reason
            : wire::terminalErrorCodeName(canonicalCode);
    if (!wire::setTerminalReason(_terminal.reason, canonicalReason)) {
        (void)wire::setTerminalReason(
            _terminal.reason, "unspecified netplay terminal");
    }
    _terminal.reasonId =
        wire::terminalReasonId(_terminal.reason.data());
    _localDisconnectAcknowledged = false;
    _sentDisconnect = false;
}

bool NetplayCore::getLocalInput(uint32_t frame, uint16_t& input) const {
    if (frame == wire::kNoFrame) return false;
    const InputSlot& slot = _localInputs[frame % kInputRingSize];
    if (!slot.valid || slot.frame != frame) return false;
    input = slot.input;
    return true;
}

bool NetplayCore::getRemoteInput(uint32_t frame, uint16_t& input) const {
    if (frame == wire::kNoFrame) return false;
    const InputSlot& slot = _remoteInputs[frame % kInputRingSize];
    if (!slot.valid || slot.frame != frame) return false;
    input = slot.input;
    return true;
}

void NetplayCore::advanceRemoteAck() {
    for (;;) {
        if (_nextRemoteAckFrame == wire::kNoFrame) break;
        const InputSlot& slot =
            _remoteInputs[_nextRemoteAckFrame % kInputRingSize];
        if (!slot.valid || slot.frame != _nextRemoteAckFrame) break;
        _remoteAckThrough = _nextRemoteAckFrame;
        _hasRemoteAck = true;
        ++_nextRemoteAckFrame;
    }
}

IngressResult NetplayCore::onDatagram(const Endpoint& source,
                                      const uint8_t* bytes,
                                      size_t length,
                                      uint64_t nowMs) {
    wire::Datagram datagram{};
    const wire::DecodeError decode =
        wire::decodeDatagram(bytes, length, _config.sessionId,
                             _config.cookie, datagram);
    if (decode != wire::DecodeError::None) return mapDecodeError(decode);
    if (!_configValid) return IngressResult::InvalidConfiguration;

    if (_peerLocked) {
        if (source != _peer) return IngressResult::WrongSource;
    } else if (!_config.learnPeerPort
               || datagram.header.type != wire::MessageType::Hello) {
        return IngressResult::WrongSource;
    }

    if (_terminal.active
        && datagram.header.type != wire::MessageType::Disconnect
        && datagram.header.type != wire::MessageType::DisconnectAck) {
        return IngressResult::Terminal;
    }

    switch (datagram.header.type) {
        case wire::MessageType::Hello: {
            if (_config.localRole != wire::PeerRole::Host)
                return IngressResult::UnexpectedMessage;
            wire::HelloPayload hello{};
            if (!wire::decodePayload(datagram.payload, hello))
                return IngressResult::BadPayload;
            if (hello.compatibilityId != _config.compatibilityId) {
                _lastCompatibilityIdentityMismatch =
                    compat::IdentityMismatch::None;
                return IngressResult::CompatibilityMismatch;
            }
            _lastCompatibilityIdentityMismatch =
                compat::compatibilityMismatch(
                    _config.compatibilityIdentity,
                    hello.compatibilityIdentity);
            if (_lastCompatibilityIdentityMismatch
                != compat::IdentityMismatch::None) {
                return IngressResult::CompatibilityMismatch;
            }
            if (hello.role != oppositeRole(_config.localRole)
                || hello.player == _config.localPlayer) {
                return IngressResult::PeerIdentityMismatch;
            }

            if (!_peerLocked) {
                _peer = source;
                _peerLocked = true;
            }
            _established = true;
            _helloAckPending = true;
            break;
        }

        case wire::MessageType::HelloAck: {
            if (_config.localRole != wire::PeerRole::Client)
                return IngressResult::UnexpectedMessage;
            wire::HelloPayload hello{};
            if (!wire::decodePayload(datagram.payload, hello))
                return IngressResult::BadPayload;
            if (hello.compatibilityId != _config.compatibilityId) {
                _lastCompatibilityIdentityMismatch =
                    compat::IdentityMismatch::None;
                return IngressResult::CompatibilityMismatch;
            }
            _lastCompatibilityIdentityMismatch =
                compat::compatibilityMismatch(
                    _config.compatibilityIdentity,
                    hello.compatibilityIdentity);
            if (_lastCompatibilityIdentityMismatch
                != compat::IdentityMismatch::None) {
                return IngressResult::CompatibilityMismatch;
            }
            if (hello.role != oppositeRole(_config.localRole)
                || hello.player == _config.localPlayer) {
                return IngressResult::PeerIdentityMismatch;
            }
            _established = true;
            break;
        }

        case wire::MessageType::Ping: {
            if (!_established) return IngressResult::UnexpectedMessage;
            wire::PingPayload ping{};
            if (!wire::decodePayload(datagram.payload, ping)
                || ping.pingId == 0) {
                return IngressResult::BadPayload;
            }
            _pendingPong = ping;
            _pongPending = true;
            break;
        }

        case wire::MessageType::Pong: {
            if (!_established) return IngressResult::UnexpectedMessage;
            wire::PingPayload pong{};
            if (!wire::decodePayload(datagram.payload, pong)
                || pong.pingId == 0) {
                return IngressResult::BadPayload;
            }
            if (!_pingOutstanding
                || pong.pingId != _outstandingPing.pingId
                || pong.sentMs != _outstandingPing.sentMs
                || nowMs < pong.sentMs) {
                return IngressResult::InvalidControl;
            }
            const uint64_t elapsed = nowMs - pong.sentMs;
            if (!_outstandingPingRetransmitted) {
                _rttMs = static_cast<uint32_t>(
                    std::min<uint64_t>(elapsed,
                        std::numeric_limits<uint32_t>::max()));
                _hasRtt = true;
            }
            _pingOutstanding = false;
            _outstandingPingRetransmitted = false;
            break;
        }

        case wire::MessageType::Inputs: {
            if (!_established) return IngressResult::UnexpectedMessage;
            wire::InputsPayload inputs{};
            if (!wire::decodePayload(datagram.payload, inputs))
                return IngressResult::BadPayload;

            if (inputs.ackThrough != wire::kNoFrame) {
                if (!_hasLocalInput || !_hasSentLocalInput)
                    return IngressResult::FutureAck;
                if (serialAfter(inputs.ackThrough,
                                _highestSentLocalFrame))
                    return IngressResult::FutureAck;
                if (serialBefore(inputs.ackThrough, _lowestLocalFrame))
                    return IngressResult::InvalidAck;
            }

            struct PendingInput {
                uint32_t frame;
                uint16_t input;
            };
            std::array<PendingInput, wire::kInputWindowSize> pending{};
            size_t pendingCount = 0;

            for (size_t i = 0; i < wire::kInputWindowSize; ++i) {
                if ((inputs.validMask & (1u << i)) == 0) continue;
                const uint32_t frame =
                    inputs.newestFrame - static_cast<uint32_t>(i);
                if (frame == wire::kNoFrame
                    || !frameInRange(frame, _simulationFrame,
                                     _config.maxPastFrames,
                                     _config.maxFutureFrames)) {
                    return IngressResult::FrameOutOfRange;
                }
                const InputSlot& slot =
                    _remoteInputs[frame % kInputRingSize];
                if (slot.valid && slot.frame == frame
                    && slot.input != inputs.inputs[i]) {
                    return IngressResult::ConflictingInput;
                }
                pending[pendingCount++] =
                    PendingInput{ frame, inputs.inputs[i] };
            }

            if (inputs.ackThrough != wire::kNoFrame
                && (!_hasPeerAck
                    || serialAfter(inputs.ackThrough, _peerAckThrough))) {
                _peerAckThrough = inputs.ackThrough;
                _hasPeerAck = true;
                if (serialAfter(_highestLocalFrame, _peerAckThrough))
                    _inputDirty = true;
            }

            if (inputs.simFrontier != wire::kNoFrame
                && (!_hasPeerFrontier
                    || serialAfter(inputs.simFrontier, _peerSimFrontier)
                    || inputs.simFrontier == _peerSimFrontier)) {
                _peerSimFrontier = inputs.simFrontier;
                _hasPeerFrontier = true;
                _peerFrontierRecvMs = nowMs;
            }

            const bool newerPressureSequence =
                !_hasPeerPressureSequence
                || serialAfter(datagram.header.sequence,
                               _peerPressureSequence);
            const bool newerPressureFrontier =
                inputs.simFrontier != wire::kNoFrame
                && (!_hasPeerPressureFrontier
                    || serialAfter(inputs.simFrontier,
                                   _peerPressureFrontier));
            const bool equalPressureFrontier =
                inputs.simFrontier != wire::kNoFrame
                && _hasPeerPressureFrontier
                && inputs.simFrontier == _peerPressureFrontier;
            const bool pressureAtUsableFrontier =
                newerPressureFrontier
                || (equalPressureFrontier && newerPressureSequence)
                || (inputs.simFrontier == wire::kNoFrame
                    && !_hasPeerPressureFrontier
                    && newerPressureSequence);
            if (pressureAtUsableFrontier) {
                _peerPressure = inputs.pressure;
                _hasPeerPressure = true;
                _peerPressureSequence = datagram.header.sequence;
                _hasPeerPressureSequence = true;
                _peerPressureRecvMs = nowMs;
                if (inputs.simFrontier != wire::kNoFrame) {
                    _peerPressureFrontier = inputs.simFrontier;
                    _hasPeerPressureFrontier = true;
                }
            }

            bool receivedNewInput = false;
            for (size_t i = 0; i < pendingCount; ++i) {
                InputSlot& slot =
                    _remoteInputs[pending[i].frame % kInputRingSize];
                if (!slot.valid || slot.frame != pending[i].frame) {
                    slot = InputSlot{
                        pending[i].frame,
                        pending[i].input,
                        true,
                    };
                    if (!_hasRemoteNewest
                        || serialAfter(pending[i].frame, _remoteNewest)) {
                        _remoteNewest = pending[i].frame;
                        _hasRemoteNewest = true;
                    }
                    receivedNewInput = true;
                }
            }
            if (receivedNewInput) {
                _lastNewRemoteInputMs = nowMs;
                advanceRemoteAck();
                _ackDirty = true;
            } else if (inputs.validMask != 0
                       && (!_sentInputs
                           || nowMs - _lastInputSendMs
                               >= _config.inputResendMs)) {
                _ackDirty = true;
            }
            break;
        }

        case wire::MessageType::Ready: {
            if (!_established) return IngressResult::UnexpectedMessage;
            wire::ReadyPayload ready{};
            if (!wire::decodePayload(datagram.payload, ready))
                return IngressResult::BadPayload;
            const bool acknowledgesLocal =
                (ready.flags & wire::kReadyAcknowledgesPeer) != 0;
            if (acknowledgesLocal
                && (!_hasLocalReady || !_sentReady))
                return IngressResult::InvalidControl;
            if (_hasRemoteReady && !sameReadyState(_remoteReady, ready))
                return IngressResult::ConflictingControl;

            if (!_hasRemoteReady) {
                _remoteReady = ready;
                _remoteReady.flags = 0;
                _hasRemoteReady = true;
                if (_hasLocalReady) _readyDirty = true;
            }
            if (acknowledgesLocal) _localReadyAcknowledged = true;
            break;
        }

        case wire::MessageType::SyncHash: {
            if (!_established) return IngressResult::UnexpectedMessage;
            wire::SyncHashPayload hash{};
            if (!wire::decodePayload(datagram.payload, hash))
                return IngressResult::BadPayload;

            const auto seen = std::find_if(
                _remoteHashSeen.begin(), _remoteHashSeen.end(),
                [&hash](const wire::SyncHashPayload& item) {
                    return item.tag == hash.tag;
                });
            if (seen != _remoteHashSeen.end()) {
                if (!sameHash(*seen, hash))
                    return IngressResult::ConflictingControl;
                const bool ackAlreadyPending = std::any_of(
                    _hashAcksPending.begin(), _hashAcksPending.end(),
                    [&hash](const wire::SyncHashAckPayload& ack) {
                        return ack.tag == hash.tag;
                    });
                if (!ackAlreadyPending) {
                    if (_hashAcksPending.size() >= kHashQueueSize)
                        return IngressResult::QueueFull;
                    _hashAcksPending.push_back(
                        wire::SyncHashAckPayload{hash.tag});
                }
                break;
            }

            if (_hasLastRemoteHashTag
                && !tagAfter(hash.tag, _lastRemoteHashTag)) {
                if (tagAfter(_lastRemoteHashTag, hash.tag)) {
                    const bool ackAlreadyPending = std::any_of(
                        _hashAcksPending.begin(),
                        _hashAcksPending.end(),
                        [&hash](const wire::SyncHashAckPayload& ack) {
                            return ack.tag == hash.tag;
                        });
                    if (!ackAlreadyPending) {
                        if (_hashAcksPending.size() >= kHashQueueSize)
                            return IngressResult::QueueFull;
                        _hashAcksPending.push_back(
                            wire::SyncHashAckPayload{hash.tag});
                    }
                    break;
                }
                return IngressResult::InvalidControl;
            }
            if (_remoteHashes.size() >= kHashQueueSize
                || _hashAcksPending.size() >= kHashQueueSize) {
                return IngressResult::QueueFull;
            }

            _remoteHashes.push_back(hash);
            _remoteHashSeen.push_back(hash);
            if (_remoteHashSeen.size() > kHashSeenSize)
                _remoteHashSeen.pop_front();
            _hashAcksPending.push_back(
                wire::SyncHashAckPayload{hash.tag});
            _lastRemoteHashTag = hash.tag;
            _hasLastRemoteHashTag = true;
            break;
        }

        case wire::MessageType::SyncHashAck: {
            if (!_established) return IngressResult::UnexpectedMessage;
            wire::SyncHashAckPayload ack{};
            if (!wire::decodePayload(datagram.payload, ack))
                return IngressResult::BadPayload;
            const auto pending = std::find_if(
                _localHashes.begin(), _localHashes.end(),
                [&ack](const wire::SyncHashPayload& hash) {
                    return hash.tag == ack.tag;
                });
            const bool duplicate = std::find(
                _localHashAcked.begin(), _localHashAcked.end(), ack.tag)
                != _localHashAcked.end();
            if (pending == _localHashes.end() && !duplicate)
                return IngressResult::InvalidAck;
            if (pending != _localHashes.end()) {
                if (pending != _localHashes.begin())
                    return IngressResult::FutureAck;
                if (!_sentHash)
                    return IngressResult::FutureAck;
                _localHashAcked.push_back(ack.tag);
                if (_localHashAcked.size() > kHashSeenSize)
                    _localHashAcked.pop_front();
                _localHashes.pop_front();
                _sentHash = false;
                _hashDirty = !_localHashes.empty();
            }
            break;
        }

        case wire::MessageType::Disconnect: {
            if (!_established) return IngressResult::UnexpectedMessage;
            wire::DisconnectPayload disconnect{};
            if (!wire::decodePayload(datagram.payload, disconnect))
                return IngressResult::BadPayload;
            if (_hasRemoteDisconnect
                && (_remoteDisconnect.code != disconnect.code
                    || _remoteDisconnect.detail != disconnect.detail
                    || _remoteDisconnect.reasonId != disconnect.reasonId
                    || _remoteDisconnect.reason != disconnect.reason)) {
                return IngressResult::ConflictingControl;
            }
            if (!_hasRemoteDisconnect) {
                _remoteDisconnect = disconnect;
                _hasRemoteDisconnect = true;
            }
            _disconnectAckPending = true;
            if (!_terminal.active) {
                _terminal.active = true;
                _terminal.fromPeer = true;
                _terminal.code = disconnect.code;
                _terminal.detail = disconnect.detail;
                _terminal.reasonId = disconnect.reasonId;
                _terminal.reason = disconnect.reason;
            }
            break;
        }

        case wire::MessageType::DisconnectAck: {
            if (!_established) return IngressResult::UnexpectedMessage;
            wire::DisconnectAckPayload ack{};
            if (!wire::decodePayload(datagram.payload, ack))
                return IngressResult::BadPayload;
            if (!_terminal.active || _terminal.fromPeer
                || !_sentDisconnect
                || ack.code != _terminal.code
                || ack.detail != _terminal.detail
                || ack.reasonId != _terminal.reasonId) {
                return IngressResult::InvalidAck;
            }
            _localDisconnectAcknowledged = true;
            break;
        }

        case wire::MessageType::RouteProbe:
        case wire::MessageType::RouteProbeAck: {
            break;
        }

        case wire::MessageType::SessionEvent: {
            if (!_established) return IngressResult::UnexpectedMessage;
            wire::SessionEventPayload event{};
            if (!wire::decodePayload(datagram.payload, event))
                return IngressResult::BadPayload;
            _inboundSessionEvent = event;
            _hasInboundSessionEvent = true;
            break;
        }

        case wire::MessageType::RematchChoice: {
            if (!_established) return IngressResult::UnexpectedMessage;
            wire::RematchChoicePayload choice{};
            if (!wire::decodePayload(datagram.payload, choice))
                return IngressResult::BadPayload;
            _inboundRematchChoice = choice;
            _hasInboundRematchChoice = true;
            break;
        }

        case wire::MessageType::Palette: {
            if (!_established) return IngressResult::UnexpectedMessage;
            wire::PalettePayload palette{};
            if (!wire::decodePayload(datagram.payload, palette))
                return IngressResult::BadPayload;
            _inboundPalette = palette;
            _hasInboundPalette = true;
            break;
        }
    }

    _lastValidReceiveMs = nowMs;
    return IngressResult::Accepted;
}

bool NetplayCore::submitSessionEvent(
    const wire::SessionEventPayload& payload) {
    std::vector<uint8_t> encoded;
    if (!wire::encodePayload(payload, encoded)) return false;
    _outboundSessionEvent = payload;
    _hasOutboundSessionEvent = true;
    _sessionEventDirty = true;
    return true;
}

void NetplayCore::clearSessionEvent() {
    _hasOutboundSessionEvent = false;
    _sessionEventDirty = false;
}

bool NetplayCore::takeSessionEvent(wire::SessionEventPayload& out) {
    if (!_hasInboundSessionEvent) return false;
    out = _inboundSessionEvent;
    _hasInboundSessionEvent = false;
    return true;
}

bool NetplayCore::submitRematchChoice(
    const wire::RematchChoicePayload& payload) {
    std::vector<uint8_t> encoded;
    if (!wire::encodePayload(payload, encoded)) return false;
    _outboundRematchChoice = payload;
    _rematchChoiceDirty = true;
    return true;
}

bool NetplayCore::takeRematchChoice(wire::RematchChoicePayload& out) {
    if (!_hasInboundRematchChoice) return false;
    out = _inboundRematchChoice;
    _hasInboundRematchChoice = false;
    return true;
}

bool NetplayCore::submitPalette(const wire::PalettePayload& payload) {
    std::vector<uint8_t> encoded;
    if (!wire::encodePayload(payload, encoded)) return false;
    _outboundPalette = payload;
    _hasOutboundPalette = true;
    _paletteDirty = true;
    return true;
}

bool NetplayCore::takePalette(wire::PalettePayload& out) {
    if (!_hasInboundPalette) return false;
    out = _inboundPalette;
    _hasInboundPalette = false;
    return true;
}

bool NetplayCore::hasUnackedLocalInput() const {
    if (!_hasLocalInput) return false;
    return !_hasPeerAck || serialAfter(_highestLocalFrame, _peerAckThrough);
}

bool NetplayCore::appendOutbound(wire::MessageType type,
                                 const std::vector<uint8_t>& payload,
                                 std::vector<OutboundDatagram>& out) {
    OutboundDatagram packet{};
    packet.destination = _peer;
    packet.type = type;
    packet.sequence = _nextSequence++;
    if (!wire::encodeDatagram(type, _config.sessionId, _config.cookie,
                              packet.sequence,
                              payload, packet.bytes)) {
        return false;
    }
    out.push_back(std::move(packet));
    return true;
}

bool NetplayCore::isAlive(uint64_t nowMs) const {
    if (!_established || _terminal.active
        || nowMs < _lastValidReceiveMs) {
        return false;
    }
    return nowMs - _lastValidReceiveMs <= _config.livenessTimeoutMs;
}

CoreLinkDiagnostics NetplayCore::diagnostics(
    const uint64_t nowMs) const {
    CoreLinkDiagnostics out{};
    out.configValid = _configValid;
    out.established = _established;
    out.alive = isAlive(nowMs);
    out.terminal = _terminal.active;
    out.simulationFrame = _simulationFrame;

    out.hasLocalInput = _hasLocalInput;
    out.highestLocalInput =
        _hasLocalInput ? _highestLocalFrame : _config.firstInputFrame;
    out.hasSentLocalInput = _hasSentLocalInput;
    out.highestSentLocalInput =
        _hasSentLocalInput
            ? _highestSentLocalFrame : _config.firstInputFrame;
    out.hasPeerAck = _hasPeerAck;
    out.peerAckThrough =
        _hasPeerAck ? _peerAckThrough : wire::kNoFrame;

    out.hasRemoteContiguous = _hasRemoteAck;
    out.remoteContiguousThrough =
        _hasRemoteAck ? _remoteAckThrough : wire::kNoFrame;
    out.hasRemoteNewest = _hasRemoteNewest;
    out.remoteNewest =
        _hasRemoteNewest ? _remoteNewest : wire::kNoFrame;
    if (_hasRemoteNewest
        && (!_hasRemoteAck
            || serialAfter(_remoteNewest, _remoteAckThrough))) {
        const uint32_t next =
            _hasRemoteAck ? _remoteAckThrough + 1u
                          : _config.firstInputFrame;
        out.remoteReorderSpan = _remoteNewest - next + 1u;
    }

    out.hasPeerSimFrontier = _hasPeerFrontier;
    out.peerSimFrontier =
        _hasPeerFrontier ? _peerSimFrontier : wire::kNoFrame;
    if (_hasPeerFrontier && nowMs >= _peerFrontierRecvMs)
        out.peerFrontierAgeMs = nowMs - _peerFrontierRecvMs;
    if (_lastValidReceiveMs != 0 && nowMs >= _lastValidReceiveMs)
        out.lastValidReceiveAgeMs = nowMs - _lastValidReceiveMs;
    if (_lastNewRemoteInputMs != 0 && nowMs >= _lastNewRemoteInputMs)
        out.lastNewRemoteInputAgeMs =
            nowMs - _lastNewRemoteInputMs;
    return out;
}

std::vector<OutboundDatagram> NetplayCore::pollOutbound(uint64_t nowMs) {
    std::vector<OutboundDatagram> out;
    if (!_configValid) return out;

    if (!_pollClockStarted) {
        _pollClockStarted = true;
        _firstPollMs = nowMs;
    }

    const uint64_t livenessBaseMs =
        _established ? _lastValidReceiveMs : _firstPollMs;
    if (!_terminal.active
        && nowMs >= livenessBaseMs
        && nowMs - livenessBaseMs > _config.livenessTimeoutMs) {
        setLocalTerminalError(
            wire::TerminalErrorCode::Timeout, 0,
            _established
                ? "peer liveness timeout"
                : "peer handshake timeout");
    }

    if (_terminal.active) {
        if (_disconnectAckPending) {
            const wire::DisconnectAckPayload ack{
                _remoteDisconnect.code,
                _remoteDisconnect.detail,
                _remoteDisconnect.reasonId,
            };
            std::vector<uint8_t> payload;
            if (wire::encodePayload(ack, payload)
                && appendOutbound(wire::MessageType::DisconnectAck,
                                  payload, out)) {
                _disconnectAckPending = false;
            }
        }
        if (!_terminal.fromPeer && !_localDisconnectAcknowledged
            && _peerLocked
            && (!_sentDisconnect
                || nowMs - _lastDisconnectSendMs
                    >= _config.disconnectResendMs)) {
            wire::DisconnectPayload disconnect{};
            disconnect.code = _terminal.code;
            disconnect.detail = _terminal.detail;
            disconnect.reasonId = _terminal.reasonId;
            disconnect.reason = _terminal.reason;
            std::vector<uint8_t> payload;
            if (wire::encodePayload(disconnect, payload)
                && appendOutbound(wire::MessageType::Disconnect,
                                  payload, out)) {
                _sentDisconnect = true;
                _lastDisconnectSendMs = nowMs;
            }
        }
        return out;
    }

    if (_config.localRole == wire::PeerRole::Client && !_established) {
        if (!_sentHello
            || nowMs - _lastHelloSendMs >= _config.helloResendMs) {
            wire::HelloPayload hello{
                _config.compatibilityId,
                _config.localRole,
                _config.localPlayer,
                _config.compatibilityIdentity,
            };
            std::vector<uint8_t> payload;
            if (wire::encodePayload(hello, payload)
                && appendOutbound(wire::MessageType::Hello, payload, out)) {
                _sentHello = true;
                _lastHelloSendMs = nowMs;
            }
        }
        return out;
    }

    if (_helloAckPending) {
        wire::HelloPayload hello{
            _config.compatibilityId,
            _config.localRole,
            _config.localPlayer,
            _config.compatibilityIdentity,
        };
        std::vector<uint8_t> payload;
        if (wire::encodePayload(hello, payload)
            && appendOutbound(wire::MessageType::HelloAck, payload, out)) {
            _helloAckPending = false;
        }
    }
    if (!_established) return out;

    const bool unacked = hasUnackedLocalInput();
    const bool resendDue = unacked
        && (!_sentInputs
            || nowMs - _lastInputSendMs >= _config.inputResendMs);
    if (_inputDirty || _ackDirty || resendDue) {
        wire::InputsPayload inputs{};
        inputs.ackThrough =
            _hasRemoteAck ? _remoteAckThrough : wire::kNoFrame;
        inputs.simFrontier =
            _hasSimulationFrame ? _simulationFrame : wire::kNoFrame;
        inputs.pressure = _localPressure;

        if (unacked) {
            const uint32_t oldest = _hasPeerAck
                ? _peerAckThrough + 1
                : _lowestLocalFrame;
            const uint32_t distance = _highestLocalFrame - oldest;
            inputs.newestFrame = distance >= wire::kInputWindowSize - 1
                ? oldest + static_cast<uint32_t>(wire::kInputWindowSize - 1)
                : _highestLocalFrame;

            for (size_t i = 0; i < wire::kInputWindowSize; ++i) {
                const uint32_t frame =
                    inputs.newestFrame - static_cast<uint32_t>(i);
                const InputSlot& slot = _localInputs[frame % kInputRingSize];
                if (!slot.valid || slot.frame != frame
                    || serialBefore(frame, oldest)) {
                    continue;
                }
                inputs.validMask |= 1u << i;
                inputs.inputs[i] = slot.input;
            }
        } else {
            inputs.newestFrame = _hasLocalInput
                ? _highestLocalFrame
                : _config.firstInputFrame;
        }

        std::vector<uint8_t> payload;
        if (wire::encodePayload(inputs, payload)
            && appendOutbound(wire::MessageType::Inputs, payload, out)) {
            if (inputs.validMask != 0) {
                if (!_hasSentLocalInput
                    || serialAfter(inputs.newestFrame,
                                   _highestSentLocalFrame)) {
                    _highestSentLocalFrame = inputs.newestFrame;
                }
                _hasSentLocalInput = true;
            }
            _inputDirty = false;
            _ackDirty = false;
            _sentInputs = true;
            _lastInputSendMs = nowMs;
        }
    }

    // CommitAck must not repeat on the Ready cadence: each echo clears the host's outbound slot and can wipe the next event's Quiesce.
    if (_hasOutboundSessionEvent
        && (_sessionEventDirty
            || (nowMs - _lastSessionEventSendMs >= _config.readyResendMs
                && _outboundSessionEvent.stage
                       != wire::SessionEventStage::CommitAck))) {
        std::vector<uint8_t> payload;
        if (wire::encodePayload(_outboundSessionEvent, payload)
            && appendOutbound(wire::MessageType::SessionEvent, payload,
                              out)) {
            _sessionEventDirty = false;
            _lastSessionEventSendMs = nowMs;
        }
    }

    while (!_hashAcksPending.empty()) {
        std::vector<uint8_t> payload;
        if (!wire::encodePayload(_hashAcksPending.front(), payload)
            || !appendOutbound(wire::MessageType::SyncHashAck,
                               payload, out)) {
            break;
        }
        _hashAcksPending.pop_front();
    }

    if (!_localHashes.empty()
        && (_hashDirty || !_sentHash
            || nowMs - _lastHashSendMs >= _config.hashResendMs)) {
        std::vector<uint8_t> payload;
        if (wire::encodePayload(_localHashes.front(), payload)
            && appendOutbound(wire::MessageType::SyncHash, payload, out)) {
            _hashDirty = false;
            _sentHash = true;
            _lastHashSendMs = nowMs;
        }
    }

    if (_pongPending) {
        std::vector<uint8_t> payload;
        if (wire::encodePayload(_pendingPong, payload)
            && appendOutbound(wire::MessageType::Pong, payload, out)) {
            _pongPending = false;
        }
    }

    const bool pingDue =
        (!_pingOutstanding
            && nowMs - _lastPingSendMs >= _config.pingIntervalMs)
        || (_pingOutstanding
            && nowMs - _lastPingSendMs >= _config.pingIntervalMs);
    if (pingDue) {
        if (!_pingOutstanding) {
            if (_nextPingId == 0) ++_nextPingId;
            _outstandingPing.pingId = _nextPingId++;
            _outstandingPing.sentMs = nowMs;
            _pingOutstanding = true;
            _outstandingPingRetransmitted = false;
        } else {
            _outstandingPingRetransmitted = true;
        }
        std::vector<uint8_t> payload;
        if (wire::encodePayload(_outstandingPing, payload)
            && appendOutbound(wire::MessageType::Ping, payload, out)) {
            _lastPingSendMs = nowMs;
        }
    }

    if (_rematchChoiceDirty) {
        std::vector<uint8_t> payload;
        if (wire::encodePayload(_outboundRematchChoice, payload)
            && appendOutbound(wire::MessageType::RematchChoice, payload,
                              out)) {
            _rematchChoiceDirty = false;
        }
    }

    if (_hasOutboundPalette
        && (_paletteDirty
            || nowMs - _lastPaletteSendMs >= _config.readyResendMs)) {
        std::vector<uint8_t> payload;
        if (wire::encodePayload(_outboundPalette, payload)
            && appendOutbound(wire::MessageType::Palette, payload, out)) {
            _paletteDirty = false;
            _lastPaletteSendMs = nowMs;
        }
    }

    if (_hasLocalReady
        && (_readyDirty || !_sentReady
            || nowMs - _lastReadySendMs >= _config.readyResendMs)) {
        wire::ReadyPayload ready = _localReady;
        if (_hasRemoteReady)
            ready.flags |= wire::kReadyAcknowledgesPeer;
        std::vector<uint8_t> payload;
        if (wire::encodePayload(ready, payload)
            && appendOutbound(wire::MessageType::Ready, payload, out)) {
            _readyDirty = false;
            _sentReady = true;
            _lastReadySendMs = nowMs;
        }
    }

    return out;
}

}
