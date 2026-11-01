#pragma once

#include "WireProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace qnetplay {

struct Endpoint {
    uint32_t ipv4 = 0;
    uint16_t port = 0;

    bool operator==(const Endpoint& other) const {
        return ipv4 == other.ipv4 && port == other.port;
    }
    bool operator!=(const Endpoint& other) const { return !(*this == other); }
};

struct CoreConfig {
    wire::PeerRole localRole = wire::PeerRole::Host;
    uint8_t localPlayer = 0;
    uint64_t compatibilityId = 0;
    compat::CompatibilityIdentityWire compatibilityIdentity{};
    uint64_t sessionId = 0;
    wire::SessionCookie cookie{};

    Endpoint expectedPeer{};
    bool learnPeerPort = false;

    uint32_t firstInputFrame = 0;
    uint32_t helloResendMs = 200;
    uint32_t inputResendMs = 50;
    uint32_t readyResendMs = 100;
    uint32_t hashResendMs = 100;
    uint32_t pingIntervalMs = 1000;
    uint32_t disconnectResendMs = 100;
    uint32_t livenessTimeoutMs = 10000;
    uint32_t maxPastFrames = 1023;
    uint32_t maxFutureFrames = 120;
};

enum class IngressResult : uint8_t {
    Accepted = 0,
    TooShort = 1,
    BadMagic = 2,
    BadVersion = 3,
    BadType = 4,
    BadLength = 5,
    BadFlags = 6,
    SessionMismatch = 7,
    BadCrc = 8,
    WrongSource = 9,
    BadPayload = 10,
    UnexpectedMessage = 11,
    CompatibilityMismatch = 12,
    PeerIdentityMismatch = 13,
    InvalidConfiguration = 14,
    InvalidAck = 15,
    FutureAck = 16,
    FrameOutOfRange = 17,
    ConflictingInput = 18,
    InvalidControl = 19,
    ConflictingControl = 20,
    QueueFull = 21,
    Terminal = 22,
    Count = 23,
};

const char* ingressResultName(IngressResult result);

struct OutboundDatagram {
    Endpoint destination{};
    wire::MessageType type = wire::MessageType::Hello;
    uint32_t sequence = 0;
    std::vector<uint8_t> bytes;
};

struct TerminalState {
    bool active = false;
    bool fromPeer = false;
    wire::TerminalErrorCode code =
        wire::TerminalErrorCode::PeerRequested;
    uint32_t detail = 0;
    uint32_t reasonId = 0;
    std::array<char, wire::kTerminalReasonBytes> reason{};
};

struct CoreLinkDiagnostics {
    bool configValid = false;
    bool established = false;
    bool alive = false;
    bool terminal = false;
    uint32_t simulationFrame = 0;

    bool hasLocalInput = false;
    uint32_t highestLocalInput = 0;
    bool hasSentLocalInput = false;
    uint32_t highestSentLocalInput = 0;
    bool hasPeerAck = false;
    uint32_t peerAckThrough = wire::kNoFrame;

    bool hasRemoteContiguous = false;
    uint32_t remoteContiguousThrough = wire::kNoFrame;
    bool hasRemoteNewest = false;
    uint32_t remoteNewest = wire::kNoFrame;
    uint32_t remoteReorderSpan = 0;

    bool hasPeerSimFrontier = false;
    uint32_t peerSimFrontier = wire::kNoFrame;
    uint64_t peerFrontierAgeMs = ~0ull;
    uint64_t lastValidReceiveAgeMs = ~0ull;
    uint64_t lastNewRemoteInputAgeMs = ~0ull;
};

enum class SyncHashSubmitResult {
    Accepted,
    ExactDuplicate,
    InvalidState,
    InvalidPayload,
    AlreadyAcknowledged,
    OutOfOrder,
    QueueFull,
};

const char* syncHashSubmitResultName(SyncHashSubmitResult result);

class NetplayCore {
public:
    explicit NetplayCore(const CoreConfig& config);

    void reset(const CoreConfig& config);
    bool setSimulationFrame(uint32_t frame);

    void setPressureReport(const wire::PressureReport& report) {
        _localPressure = report;
    }
    bool peerPressureReport(wire::PressureReport& out,
                            uint64_t& ageMsOut, uint64_t nowMs) const {
        if (!_hasPeerPressure) return false;
        out = _peerPressure;
        ageMsOut = nowMs >= _peerPressureRecvMs
            ? nowMs - _peerPressureRecvMs : 0u;
        return true;
    }

    bool submitLocalInput(uint32_t frame, uint16_t input);

    bool submitLocalReady(const wire::ReadyPayload& ready);
    bool getRemoteReady(wire::ReadyPayload& ready) const;

    bool submitSessionEvent(const wire::SessionEventPayload& payload);
    void clearSessionEvent();
    bool takeSessionEvent(wire::SessionEventPayload& out);

    bool submitRematchChoice(const wire::RematchChoicePayload& payload);
    bool takeRematchChoice(wire::RematchChoicePayload& out);
    bool submitPalette(const wire::PalettePayload& payload);
    bool takePalette(wire::PalettePayload& out);
    bool readyComplete() const;
    SyncHashSubmitResult submitLocalSyncHashDetailed(
        const wire::SyncHashPayload& hash);
    bool submitLocalSyncHash(const wire::SyncHashPayload& hash);
    bool peekRemoteSyncHash(wire::SyncHashPayload& hash) const;
    bool takeRemoteSyncHash(wire::SyncHashPayload& hash);
    bool localSyncHashAcknowledged(const wire::SyncHashTag& tag) const;
    size_t pendingLocalSyncHashCount() const {
        return _localHashes.size();
    }
    bool lastLocalSyncHashTag(wire::SyncHashTag& tag) const {
        if (!_hasLastLocalHashTag) return false;
        tag = _lastLocalHashTag;
        return true;
    }

    void setLocalTerminalError(wire::TerminalErrorCode code, uint32_t detail,
                               const char* reason = nullptr);
    TerminalState terminalState() const { return _terminal; }
    bool hasTerminalError() const { return _terminal.active; }
    compat::IdentityMismatch lastCompatibilityIdentityMismatch() const {
        return _lastCompatibilityIdentityMismatch;
    }
    const char* lastCompatibilityIdentityMismatchReason() const {
        return compat::identityMismatchReason(
            _lastCompatibilityIdentityMismatch);
    }

    IngressResult onDatagram(const Endpoint& source,
                             const uint8_t* bytes,
                             size_t length,
                             uint64_t nowMs);

    std::vector<OutboundDatagram> pollOutbound(uint64_t nowMs);

    bool isEstablished() const { return _established; }
    bool peerLocked() const { return _peerLocked; }
    Endpoint peerEndpoint() const { return _peer; }
    uint64_t lastValidReceiveMs() const { return _lastValidReceiveMs; }
    bool isAlive(uint64_t nowMs) const;
    bool hasRtt() const { return _hasRtt; }
    uint32_t rttMs() const { return _rttMs; }

    bool hasPeerSimFrontier() const { return _hasPeerFrontier; }
    uint32_t peerSimFrontier() const {
        return _hasPeerFrontier ? _peerSimFrontier : wire::kNoFrame;
    }
    uint64_t peerFrontierAgeMs(uint64_t nowMs) const {
        return _hasPeerFrontier && nowMs >= _peerFrontierRecvMs
            ? nowMs - _peerFrontierRecvMs
            : ~0ull;
    }

    bool getLocalInput(uint32_t frame, uint16_t& input) const;
    bool getRemoteInput(uint32_t frame, uint16_t& input) const;
    uint32_t remoteAckThrough() const {
        return _hasRemoteAck ? _remoteAckThrough : wire::kNoFrame;
    }
    uint32_t peerAckThrough() const {
        return _hasPeerAck ? _peerAckThrough : wire::kNoFrame;
    }
    CoreLinkDiagnostics diagnostics(uint64_t nowMs) const;

private:
    static constexpr size_t kInputRingSize = 2048;
    static constexpr size_t kHashQueueSize = 64;
    static constexpr size_t kHashSeenSize = 128;

    struct InputSlot {
        uint32_t frame = 0;
        uint16_t input = 0;
        bool valid = false;
    };

    CoreConfig _config{};
    bool _configValid = false;
    compat::IdentityMismatch _lastCompatibilityIdentityMismatch =
        compat::IdentityMismatch::None;
    Endpoint _peer{};
    bool _peerLocked = false;
    bool _established = false;
    bool _pollClockStarted = false;
    uint64_t _firstPollMs = 0;
    uint64_t _lastValidReceiveMs = 0;
    uint32_t _nextSequence = 1;
    uint32_t _simulationFrame = 0;

    std::array<InputSlot, kInputRingSize> _localInputs{};
    std::array<InputSlot, kInputRingSize> _remoteInputs{};
    bool _hasLocalInput = false;
    uint32_t _lowestLocalFrame = 0;
    uint32_t _highestLocalFrame = 0;
    bool _hasSentLocalInput = false;
    uint32_t _highestSentLocalFrame = 0;
    bool _hasPeerAck = false;
    uint32_t _peerAckThrough = 0;
    bool _hasRemoteAck = false;
    uint32_t _remoteAckThrough = 0;
    uint32_t _nextRemoteAckFrame = 0;
    bool _hasRemoteNewest = false;
    uint32_t _remoteNewest = 0;
    uint64_t _lastNewRemoteInputMs = 0;
    bool _hasSimulationFrame = false;
    bool _hasPeerFrontier = false;
    uint32_t _peerSimFrontier = 0;
    uint64_t _peerFrontierRecvMs = 0;
    wire::PressureReport _localPressure{};
    wire::PressureReport _peerPressure{};
    bool _hasPeerPressure = false;
    bool _hasPeerPressureFrontier = false;
    uint32_t _peerPressureFrontier = 0;
    bool _hasPeerPressureSequence = false;
    uint32_t _peerPressureSequence = 0;
    uint64_t _peerPressureRecvMs = 0;

    bool _inputDirty = false;
    bool _ackDirty = false;
    bool _sentInputs = false;
    uint64_t _lastInputSendMs = 0;
    bool _sentHello = false;
    uint64_t _lastHelloSendMs = 0;
    bool _helloAckPending = false;

    bool _hasLocalReady = false;
    wire::ReadyPayload _localReady{};
    bool _hasRemoteReady = false;
    wire::ReadyPayload _remoteReady{};
    bool _localReadyAcknowledged = false;
    bool _hasOutboundSessionEvent = false;
    wire::SessionEventPayload _outboundSessionEvent{};
    bool _sessionEventDirty = false;
    uint64_t _lastSessionEventSendMs = 0;
    bool _hasInboundSessionEvent = false;
    wire::SessionEventPayload _inboundSessionEvent{};
    bool _rematchChoiceDirty = false;
    wire::RematchChoicePayload _outboundRematchChoice{};
    bool _hasInboundRematchChoice = false;
    wire::RematchChoicePayload _inboundRematchChoice{};
    bool _hasOutboundPalette = false;
    bool _paletteDirty = false;
    uint64_t _lastPaletteSendMs = 0;
    wire::PalettePayload _outboundPalette{};
    bool _hasInboundPalette = false;
    wire::PalettePayload _inboundPalette{};
    bool _readyDirty = false;
    bool _sentReady = false;
    uint64_t _lastReadySendMs = 0;

    std::deque<wire::SyncHashPayload> _localHashes;
    std::deque<wire::SyncHashPayload> _remoteHashes;
    std::deque<wire::SyncHashPayload> _remoteHashSeen;
    std::deque<wire::SyncHashAckPayload> _hashAcksPending;
    std::deque<wire::SyncHashTag> _localHashAcked;
    bool _hasLastLocalHashTag = false;
    wire::SyncHashTag _lastLocalHashTag{};
    bool _hasLastRemoteHashTag = false;
    wire::SyncHashTag _lastRemoteHashTag{};
    bool _hashDirty = false;
    bool _sentHash = false;
    uint64_t _lastHashSendMs = 0;

    bool _pongPending = false;
    wire::PingPayload _pendingPong{};
    bool _pingOutstanding = false;
    wire::PingPayload _outstandingPing{};
    bool _outstandingPingRetransmitted = false;
    uint32_t _nextPingId = 1;
    uint64_t _lastPingSendMs = 0;
    bool _hasRtt = false;
    uint32_t _rttMs = 0;

    TerminalState _terminal{};
    bool _hasRemoteDisconnect = false;
    wire::DisconnectPayload _remoteDisconnect{};
    bool _disconnectAckPending = false;
    bool _localDisconnectAcknowledged = false;
    bool _sentDisconnect = false;
    uint64_t _lastDisconnectSendMs = 0;

    bool appendOutbound(wire::MessageType type,
                        const std::vector<uint8_t>& payload,
                        std::vector<OutboundDatagram>& out);
    bool hasUnackedLocalInput() const;
    void advanceRemoteAck();
    static bool validConfig(const CoreConfig& config);
};

}
