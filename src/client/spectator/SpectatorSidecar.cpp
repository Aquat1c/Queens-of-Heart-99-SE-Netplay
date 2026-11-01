#include "client/spectator/SpectatorSidecar.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <limits>
#include <new>
#include <set>
#include <type_traits>
#include <utility>

#include "replay/ReplayIntegrity.h"
#include "net/Protocol.h"
#include "spectator/SpectatorGrant.h"
#include "spectator/SpectatorRelayAdapter.h"
#include "ipc/IpcConfig.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "Log.h"
#include "net/AutopunchSession.h"
#include "net/AutopunchPeerSet.h"
#include "net/RouteCandidates.h"
#include "net/Sockets.h"
#include <mstcpip.h>
#endif

namespace qclient::spectator {

using namespace qnetplay::spectator;

namespace {

uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

template <class C, class = void>
struct SpoolRetention {
    static constexpr bool supported = false;
    static void apply(C&, uint32_t, bool) {}
};
template <class C>
struct SpoolRetention<C, std::void_t<decltype(C::maxRetainedRecords),
                                     decltype(C::retainAllRecords)>> {
    static constexpr bool supported = true;
    static void apply(C& config, uint32_t maxRetained, bool retainAll) {
        config.maxRetainedRecords = maxRetained;
        config.retainAllRecords = retainAll;
    }
};

constexpr uint64_t kHandshakeRefreshCapMs = 30000;

RejectReason rejectForDeny(AdmissionDenyReason reason) {
    switch (reason) {
    case AdmissionDenyReason::CapacityFull:
    case AdmissionDenyReason::PerIpQuota:
    case AdmissionDenyReason::PerGrantQuota:
        return RejectReason::CapacityReached;
    case AdmissionDenyReason::GrantExpired:
        return RejectReason::GrantExpired;
    case AdmissionDenyReason::GrantInvalid:
    case AdmissionDenyReason::GrantRequired:
    case AdmissionDenyReason::GrantRevoked:
    case AdmissionDenyReason::HostDeclined:
    case AdmissionDenyReason::Kicked:
        return RejectReason::GrantInvalid;
    case AdmissionDenyReason::RateLimited:
    case AdmissionDenyReason::UploadBudgetExhausted:
        return RejectReason::RateLimited;
    case AdmissionDenyReason::ServiceFailed:
    case AdmissionDenyReason::ServiceExpired:
    case AdmissionDenyReason::PolicyDisabled:
        return RejectReason::ArchiveUnavailable;
    case AdmissionDenyReason::None:
        break;
    }
    return RejectReason::InvalidRequest;
}

uint64_t saturatingDeadline(uint64_t start, uint64_t duration) {
    const uint64_t limit = (std::numeric_limits<uint64_t>::max)();
    return duration > limit - start ? limit : start + duration;
}

bool isRelayIngress(const RouteEndpoint& endpoint) {
    return endpoint.ipv4BE
        == qnetplay::spectator::kRelayVirtualIpv4BE;
}

constexpr size_t kPublicationRecordBytes = 32;
class LeReader {
public:
    LeReader(const uint8_t* bytes, size_t length)
        : _bytes(bytes), _length(length) {}
    bool u8(uint8_t& out) { return read(&out, 1); }
    bool u16(uint16_t& out) {
        uint8_t raw[2];
        if (!read(raw, 2)) return false;
        out = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
        return true;
    }
    bool u32(uint32_t& out) {
        uint8_t raw[4];
        if (!read(raw, 4)) return false;
        out = static_cast<uint32_t>(raw[0]) | (static_cast<uint32_t>(raw[1]) << 8)
            | (static_cast<uint32_t>(raw[2]) << 16)
            | (static_cast<uint32_t>(raw[3]) << 24);
        return true;
    }
    bool u64(uint64_t& out) {
        uint32_t low = 0, high = 0;
        if (!u32(low) || !u32(high)) return false;
        out = static_cast<uint64_t>(low)
            | (static_cast<uint64_t>(high) << 32);
        return true;
    }
    bool blob(std::vector<uint8_t>& out, size_t count) {
        if (_offset + count > _length) return false;
        out.assign(_bytes + _offset, _bytes + _offset + count);
        _offset += count;
        return true;
    }
    size_t remaining() const { return _length - _offset; }

private:
    bool read(void* out, size_t count) {
        if (_offset + count > _length) return false;
        std::memcpy(out, _bytes + _offset, count);
        _offset += count;
        return true;
    }
    const uint8_t* _bytes;
    size_t _length;
    size_t _offset = 0;
};

bool expandPublicationFrame(PublicationWireKind kind,
                            uint32_t param0,
                            uint64_t gameId,
                            const uint8_t* bytes,
                            size_t length,
                            std::vector<PublicationEvent>& out) {
    LeReader reader(bytes, length);
    switch (kind) {
    case PublicationWireKind::Hello: {
        uint64_t helloGameId = 0;
        uint32_t schemaVersion = 0;
        uint32_t reserved = 0;
        if (!reader.u64(helloGameId) || !reader.u32(schemaVersion)
            || !reader.u32(reserved))
            return false;
        return schemaVersion == kPublicationVersion && reserved == 0;
    }
    case PublicationWireKind::Start: {
        PublicationEvent event{};
        event.kind = PublicationKind::Start;
        uint16_t reserved = 0;
        uint32_t metadataLength = 0;
        GameDescriptor& d = event.descriptor;
        if (!reader.u64(d.gameId) || !reader.u64(d.simulationContract)
            || !reader.u64(d.initialStateHash) || !reader.u64(d.initialDigest)
            || !reader.u32(d.firstTimelineFrame)
            || !reader.u16(d.playerOneDelay) || !reader.u16(d.playerTwoDelay)
            || !reader.u16(d.rollbackWindow) || !reader.u16(reserved)
            || !reader.u32(metadataLength))
            return false;
        if (reserved != 0 || metadataLength > kMaxGameMetadataSize)
            return false;
        if (!reader.blob(d.metadata, metadataLength)) return false;
        out.push_back(std::move(event));
        return true;
    }
    case PublicationWireKind::Confirmed: {
        if (param0 == 0) return false;
        if (static_cast<size_t>(param0) * kPublicationRecordBytes != length)
            return false;
        std::vector<PublicationEvent> staged;
        staged.reserve(param0);
        for (uint32_t i = 0; i < param0; ++i) {
            PublicationEvent event{};
            event.kind = PublicationKind::Record;
            ConfirmedRecord& r = event.record;
            uint32_t reserved = 0;
            if (!reader.u32(r.record) || !reader.u32(r.timelineFrame)
                || !reader.u16(r.playerOneInput)
                || !reader.u16(r.playerTwoInput) || !reader.u32(reserved)
                || !reader.u64(r.stateHash) || !reader.u64(r.rollingDigest))
                return false;
            if (reserved != 0) return false;
            staged.push_back(std::move(event));
        }
        for (PublicationEvent& event : staged) out.push_back(std::move(event));
        return true;
    }
    case PublicationWireKind::ChainedSetEvent: {
        if (param0 != 1) return false;
        std::vector<uint8_t> payload(bytes, bytes + length);
        qnetplay::spectator::SetEventBatchPayload batch{};
        if (!qnetplay::spectator::decodePayload(payload, batch)) return false;
        if (batch.gameId != gameId || batch.events.size() != 1u)
            return false;
        PublicationEvent event{};
        event.kind = PublicationKind::ChainedSetEvent;
        event.setEvent = batch.events[0];
        out.push_back(std::move(event));
        return true;
    }
    case PublicationWireKind::Checkpoint: {
        PublicationEvent event{};
        event.kind = PublicationKind::Checkpoint;
        uint32_t byteLength = 0;
        if (!reader.u32(event.checkpointRecord) || !reader.u32(byteLength))
            return false;
        if (event.checkpointRecord != param0) return false;
        if (byteLength > kMaxPublicationFrameBytes) return false;
        if (!reader.blob(event.checkpointBytes, byteLength)) return false;
        event.checkpointId = (gameId << 32) ^ event.checkpointRecord;
        event.checkpointDigest = 0;
        out.push_back(std::move(event));
        return true;
    }
    case PublicationWireKind::Seal: {
        PublicationEvent event{};
        event.kind = PublicationKind::Seal;
        uint16_t reason = 0;
        uint16_t reserved16 = 0;
        uint32_t recordCursor = 0;
        if (!reader.u16(reason) || !reader.u16(reserved16)
            || !reader.u32(recordCursor)
            || !reader.u64(event.terminalPrefixDigest)
            || !reader.u64(event.postTailStateHash))
            return false;
        if (reserved16 != 0 || reader.remaining() != 0) return false;
        if (reason < 1 || reason > 4 || reason != param0) return false;
        event.reason = static_cast<TerminalReason>(reason);
        event.terminalRecordCursor = recordCursor;
        event.hasTerminalPrefixProof =
            event.terminalPrefixDigest != 0u;
        event.hasPostTailProof = event.postTailStateHash != 0;
        out.push_back(std::move(event));
        return true;
    }
    case PublicationWireKind::TailProof: {
        PublicationEvent event{};
        event.kind = PublicationKind::TailProof;
        uint32_t reserved = 0;
        if (!reader.u32(event.tailRecordCursor) || !reader.u32(reserved)
            || !reader.u64(event.postTailStateHash))
            return false;
        if (reserved != 0) return false;
        if (event.tailRecordCursor != param0) return false;
        if (event.tailRecordCursor == 0 || event.postTailStateHash == 0)
            return false;
        event.hasPostTailProof = true;
        out.push_back(std::move(event));
        return true;
    }
    case PublicationWireKind::PublicationFault: {
        PublicationEvent event{};
        event.kind = PublicationKind::LossReport;
        uint16_t reason = 0;
        uint16_t reserved = 0;
        if (!reader.u16(reason) || !reader.u16(reserved)
            || !reader.u32(event.terminalRecordCursor)
            || !reader.u64(event.terminalPrefixDigest)
            || !reader.u64(event.postTailStateHash)) {
            return false;
        }
        if (reason == 0u || reason > 5u || reason != param0
            || reserved != 0u || reader.remaining() != 0u) {
            return false;
        }
        event.publicationFaultReason = reason;
        event.hasTerminalPrefixProof =
            event.terminalPrefixDigest != 0u;
        event.hasPostTailProof = event.postTailStateHash != 0u;
        out.push_back(std::move(event));
        return true;
    }
    case PublicationWireKind::ConnectedDescriptorV4: {
        if (param0 != 0u
            || length
                != qnetplay::spectator::kConnectedDescriptorBytesV4) {
            return false;
        }
        PublicationEvent event{};
        event.kind = PublicationKind::ConnectedDescriptorV4;
        if (qnetplay::spectator::decodeConnectedSetDescriptorV4(
                bytes, length, event.connectedDescriptor)
            != qnetplay::spectator::ConnectedCodecErrorV4::None
            || event.connectedDescriptor.setStreamId != gameId) {
            return false;
        }
        out.push_back(std::move(event));
        return true;
    }
    case PublicationWireKind::ConnectedPublicationV4: {
        PublicationEvent event{};
        event.kind = PublicationKind::ConnectedPublicationV4;
        if (qnetplay::spectator::decodeConnectedPublicationV4(
                bytes, length, event.connectedPublication)
            != qnetplay::spectator::ConnectedCodecErrorV4::None
            || qnetplay::spectator::firstItemSequenceV4(
                   event.connectedPublication) != param0
            || qnetplay::spectator::firstDigestV4(
                   event.connectedPublication) == 0u) {
            return false;
        }
        const uint64_t publicationStream =
            [&event]() {
                using qnetplay::spectator::
                    ConnectedPublicationKindV4;
                switch (event.connectedPublication.kind) {
                    case ConnectedPublicationKindV4::BeginGameBundle:
                        return event.connectedPublication.begin.start
                            .chain.setStreamId;
                    case ConnectedPublicationKindV4::ConfirmedRecord:
                        return event.connectedPublication.record.chain
                            .setStreamId;
                    case ConnectedPublicationKindV4::SetEvent:
                        return event.connectedPublication.setEvent.chain
                            .setStreamId;
                    case ConnectedPublicationKindV4::EndGameBundle:
                        return event.connectedPublication.end.end.chain
                            .setStreamId;
                    case ConnectedPublicationKindV4::SetSeal:
                        return event.connectedPublication.seal.chain
                            .setStreamId;
                    case ConnectedPublicationKindV4::Palette:
                        return event.connectedPublication.palette.chain
                            .setStreamId;
                }
                return uint64_t{0u};
            }();
        if (publicationStream == 0u || publicationStream != gameId)
            return false;
        out.push_back(std::move(event));
        return true;
    }
    }
    return false;
}

void pushLossReport(std::vector<PublicationEvent>& out) {
    PublicationEvent event{};
    event.kind = PublicationKind::LossReport;
    out.push_back(std::move(event));
}

}

bool decodePublicationFrame(const uint8_t* frame,
                            size_t length,
                            std::vector<PublicationEvent>& out) {
    if (frame == nullptr || length < kPublicationHeaderSize) return false;
    LeReader reader(frame, kPublicationHeaderSize);
    uint32_t magic = 0, sequence = 0, param0 = 0, payloadBytes = 0;
    uint32_t payloadCrc = 0, headerCrc = 0;
    uint16_t version = 0, kind = 0;
    uint64_t gameId = 0;
    if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(kind)
        || !reader.u32(sequence) || !reader.u32(param0)
        || !reader.u32(payloadBytes) || !reader.u32(payloadCrc)
        || !reader.u64(gameId) || !reader.u32(headerCrc))
        return false;
    if (magic != kPublicationMagic || version != kPublicationVersion) return false;
    if (payloadBytes > kMaxPublicationFrameBytes) return false;
    if (kind < static_cast<uint16_t>(PublicationWireKind::Hello)
        || kind > static_cast<uint16_t>(
                       PublicationWireKind::ChainedSetEvent))
        return false;
    if (qreplay::crc32c(frame, 32u) != headerCrc) return false;
    if (length != kPublicationHeaderSize + payloadBytes) return false;
    const uint8_t* payload = frame + kPublicationHeaderSize;
    const uint32_t computed =
        payloadBytes == 0 ? 0u : qreplay::crc32c(payload, payloadBytes);
    if (computed != payloadCrc) return false;

    std::vector<PublicationEvent> staged;
    if (!expandPublicationFrame(static_cast<PublicationWireKind>(kind), param0,
                                gameId, payload, payloadBytes, staged))
        return false;
    for (PublicationEvent& event : staged) out.push_back(std::move(event));
    return true;
}

const char* sidecarPhaseName(SidecarPhase phase) {
    switch (phase) {
    case SidecarPhase::Idle: return "idle";
    case SidecarPhase::Waiting: return "waiting";
    case SidecarPhase::Active: return "active";
    case SidecarPhase::Paused: return "paused";
    case SidecarPhase::Checkpointing: return "checkpointing";
    case SidecarPhase::Sealed: return "sealed";
    case SidecarPhase::Draining: return "draining";
    case SidecarPhase::Retained: return "retained";
    case SidecarPhase::Expired: return "expired";
    case SidecarPhase::Failed: return "failed";
    }
    return "idle";
}

SourcePhase toWireSourcePhase(SidecarPhase phase) {
    if (phase == SidecarPhase::Idle) return SourcePhase::Waiting;
    return static_cast<SourcePhase>(static_cast<uint8_t>(phase));
}

const char* sourceStatusText(SourceStatusCode code) {
    switch (code) {
    case SourceStatusCode::Disabled: return "spectators are disabled";
    case SourceStatusCode::WaitingForStart: return "waiting for match start";
    case SourceStatusCode::Streaming: return "streaming";
    case SourceStatusCode::PausedHeld: return "paused";
    case SourceStatusCode::Checkpointing: return "saving a join point";
    case SourceStatusCode::SealedDraining:
        return "match ended; delivering the final records";
    case SourceStatusCode::CompletedClean: return "completed";
    case SourceStatusCode::PeerQuitSourceStopped:
        return "the match ended because a player quit";
    case SourceStatusCode::ProtocolOrArchiveFailure:
        return "the recording failed and was truncated";
    case SourceStatusCode::CapacityRejection: return "spectator limit reached";
    case SourceStatusCode::RouteLostResuming: return "route lost; resuming";
    case SourceStatusCode::TerminalProofMismatch:
        return "the final proof did not match";
    case SourceStatusCode::RetainedPostGame:
        return "completed; still available to watch";
    case SourceStatusCode::RetentionExpired: return "the recording has expired";
    }
    return "unknown";
}

const char* viewerServiceStateName(ViewerServiceState state) {
    switch (state) {
    case ViewerServiceState::PendingApproval: return "pending";
    case ViewerServiceState::Handshaking: return "handshaking";
    case ViewerServiceState::Streaming: return "streaming";
    case ViewerServiceState::AwaitingTerminalAck: return "finishing";
    case ViewerServiceState::Completed: return "completed";
    case ViewerServiceState::DeadlineExpired: return "timed out";
    case ViewerServiceState::Detached: return "left";
    case ViewerServiceState::Kicked: return "removed";
    }
    return "unknown";
}

const char* admissionDenyText(AdmissionDenyReason reason) {
    switch (reason) {
    case AdmissionDenyReason::None: return "";
    case AdmissionDenyReason::PolicyDisabled: return "spectators are disabled";
    case AdmissionDenyReason::HostDeclined: return "the host declined";
    case AdmissionDenyReason::GrantRequired:
        return "this match is invite-only";
    case AdmissionDenyReason::GrantInvalid: return "the spectator pass is not valid";
    case AdmissionDenyReason::GrantExpired: return "the spectator pass expired";
    case AdmissionDenyReason::GrantRevoked: return "the spectator pass was revoked";
    case AdmissionDenyReason::CapacityFull: return "the spectator limit is reached";
    case AdmissionDenyReason::PerIpQuota:
        return "too many spectators from one address";
    case AdmissionDenyReason::PerGrantQuota: return "spectator pass quota reached";
    case AdmissionDenyReason::RateLimited: return "too many attempts";
    case AdmissionDenyReason::UploadBudgetExhausted:
        return "the host's spectator upload budget is exhausted";
    case AdmissionDenyReason::ServiceFailed:
        return "the host's recording service failed";
    case AdmissionDenyReason::ServiceExpired: return "the recording has expired";
    case AdmissionDenyReason::Kicked: return "removed by the host";
    }
    return "";
}

const char* ingressOutcomeName(IngressOutcome outcome) {
    switch (outcome) {
    case IngressOutcome::Accepted: return "accepted";
    case IngressOutcome::ServiceUnavailable: return "service-unavailable";
    case IngressOutcome::RateLimitedGlobal: return "rate-limited-global";
    case IngressOutcome::RateLimitedAddress: return "rate-limited-address";
    case IngressOutcome::RateLimitedGrant: return "rate-limited-grant";
    case IngressOutcome::AddressCooldown: return "address-cooldown";
    case IngressOutcome::Malformed: return "malformed";
    case IngressOutcome::AdmissionDenied: return "admission-denied";
    case IngressOutcome::ProtocolRejected: return "protocol-rejected";
    }
    return "unknown";
}

const char* mirrorResultText(MirrorResult result) {
    switch (result) {
    case MirrorResult::Accepted: return "prefix verified";
    case MirrorResult::NoLocalArchive: return "no local recording to compare";
    case MirrorResult::GameMismatch: return "a different match";
    case MirrorResult::FingerprintMismatch:
        return "a different start fingerprint";
    case MirrorResult::DigestDivergence:
        return "the confirmed prefix diverges";
    case MirrorResult::CursorAhead:
        return "the peer claims more records than can be verified";
    case MirrorResult::ServiceFailed: return "the local service has failed";
    }
    return "unknown";
}

const char* diagEventName(DiagEvent event) {
    switch (event) {
    case DiagEvent::MalformedDatagram: return "malformed";
    case DiagEvent::RateLimited: return "rate-limited";
    case DiagEvent::UnknownViewer: return "unknown-viewer";
    case DiagEvent::AdmissionDenied: return "admission-denied";
    case DiagEvent::CapacityRejected: return "capacity";
    case DiagEvent::SpoolFailure: return "spool-failure";
    case DiagEvent::PublicationGap: return "publication-gap";
    case DiagEvent::ViewerTimeout: return "viewer-timeout";
    case DiagEvent::TerminalDeadline: return "terminal-deadline";
    case DiagEvent::RouteLost: return "route-lost";
    case DiagEvent::MirrorDivergence: return "mirror-divergence";
    case DiagEvent::TransportError: return "transport-error";
    case DiagEvent::Count: break;
    }
    return "event";
}

CoalescedDiagnostics::CoalescedDiagnostics(const DiagnosticsConfig& config)
    : _config(config) {
    if (_config.windowMs == 0) _config.windowMs = 1;
    if (_config.maxEmitsPerWindow == 0) _config.maxEmitsPerWindow = 1;
}

uint64_t CoalescedDiagnostics::droppedFor(DiagEvent event) const {
    const size_t index = static_cast<size_t>(event);
    if (index >= static_cast<size_t>(DiagEvent::Count)) return 0;
    return _slots[index].suppressedTotal;
}

bool CoalescedDiagnostics::note(DiagEvent event, uint64_t nowMs,
                                const char* detail) {
    const size_t index = static_cast<size_t>(event);
    if (index >= static_cast<size_t>(DiagEvent::Count)) return false;
    Slot& slot = _slots[index];

    if (!_globalWindowStarted) {
        _globalWindowStarted = true;
        _globalWindowStartMs = nowMs;
    } else if (nowMs - _globalWindowStartMs >= _config.windowMs) {
        _globalWindowStartMs = nowMs;
        _emitsThisWindow = 0;
    }
    if (!slot.windowStarted) {
        slot.windowStarted = true;
        slot.windowStartMs = nowMs;
    } else if (nowMs - slot.windowStartMs >= _config.windowMs) {
        slot.windowStartMs = nowMs;
        slot.emittedThisWindow = false;
    }

    const bool budgetLeft = _bytes < _config.maxTotalBytes;
    if (slot.emittedThisWindow || _emitsThisWindow >= _config.maxEmitsPerWindow
        || !budgetLeft) {
        ++slot.suppressed;
        ++slot.suppressedTotal;
        ++_dropped;
        return false;
    }

    char line[256];
    const int written = std::snprintf(
        line, sizeof(line), "[spectator-sidecar] %s: %s (suppressed %llu)",
        diagEventName(event), detail ? detail : "",
        static_cast<unsigned long long>(slot.suppressed));
    slot.suppressed = 0;
    slot.emittedThisWindow = true;
    ++_emitsThisWindow;
    ++_emitted;
    _bytes += written > 0 ? static_cast<uint64_t>(written) : 0;
    if (_config.sink) _config.sink(line);
    return true;
}

void SpectatorSidecar::Bucket::configure(double perSecond, double burst,
                                         uint64_t nowMs) {
    capacity = burst > 0.0 ? burst : 1.0;
    refillPerMs = perSecond / 1000.0;
    tokens = capacity;
    lastMs = nowMs;
}

bool SpectatorSidecar::Bucket::take(double cost, uint64_t nowMs) {
    if (capacity <= 0.0) return true;
    if (nowMs > lastMs) {
        tokens += static_cast<double>(nowMs - lastMs) * refillPerMs;
        if (tokens > capacity) tokens = capacity;
        lastMs = nowMs;
    }
    if (tokens < cost) return false;
    tokens -= cost;
    return true;
}

SpectatorSidecar::SpectatorSidecar(const SidecarConfig& config)
    : _config(config),
      _policy(config.policy),
      _clientPrivateRootDormant(config.clientPrivateRoot.enabled),
      _maxViewers(config.maxViewers == 0 ? 1 : config.maxViewers),
      _diagnostics(config.diagnostics) {
    _config.resilientRoot = {};
}

SpectatorSidecar::~SpectatorSidecar() {
    stopResilientRoot(_lastServiceMs);
    if (_transport) _transport->close();
    if (_channel) _channel->close();
}

bool SpectatorSidecar::start(std::unique_ptr<SidecarTransport> transport,
                             std::unique_ptr<PublicationChannel> channel,
                             uint64_t nowMs,
                             std::string& error) {
    _transport = std::move(transport);
    _channel = std::move(channel);
    _started = true;

    if (_clientPrivateRootDormant) {
        if (_config.clientPrivateRoot.publisherGeneration == 0u
            || _config.clientPrivateRoot.publisherNonce == 0u
            || _policy != AdmissionPolicy::CodeOnly
            || _config.sourceAutopunch.enabled
            || _config.sourceRelay.enabled
            || _config.topologyActivation.requested
            || _config.signGrant || _config.verifyGrant) {
            failService("client-private sidecar configuration was not dormant",
                        nowMs);
            error = _failureReason;
            return false;
        }
    }

    _globalPackets.configure(_config.rateLimits.globalPacketsPerSecond,
                             _config.rateLimits.globalBurstPackets, nowMs);
    _globalBytes.configure(
        static_cast<double>(_config.rateLimits.globalBytesPerSecond),
        static_cast<double>(_config.rateLimits.globalBurstBytes), nowMs);
    _upload.configure(static_cast<double>(_config.uploadBudgetBytesPerSecond),
                      static_cast<double>(_config.uploadBurstBytes), nowMs);
    _overflowAddress = {};
    _overflowAddress.packets.configure(
        _config.rateLimits.perIpPacketsPerSecond,
        _config.rateLimits.perIpBurstPackets, nowMs);
    _overflowAddress.bytes.configure(
        static_cast<double>(_config.rateLimits.perIpBytesPerSecond),
        static_cast<double>(_config.rateLimits.perIpBurstBytes), nowMs);
    _overflowAddress.lastSeenMs = nowMs;

    if (_policy == AdmissionPolicy::Disabled) {
        _phase = SidecarPhase::Idle;
        _statusCode = SourceStatusCode::Disabled;
        return true;
    }

    ArchiveConfig archiveConfig{};
    archiveConfig.maxGames = 1;
    SpoolRetention<ArchiveConfig>::apply(archiveConfig,
                                         _config.maxRetainedRecords,
                                         _config.retainAllRecords);
    archiveConfig.maxRetainedBytes = _config.maxRetainedBytes;
    if (archiveConfig.maxRetainedBytes > 0) {
        const size_t recordsPerByteBudget =
            archiveConfig.maxRetainedBytes / sizeof(ConfirmedRecord);
        if (recordsPerByteBudget > 0) {
            const size_t uint32Limit =
                (std::numeric_limits<uint32_t>::max)();
            const uint32_t byteRecordLimit =
                static_cast<uint32_t>(recordsPerByteBudget > uint32Limit
                                          ? uint32Limit
                                          : recordsPerByteBudget);
            if (!archiveConfig.retainAllRecords
                && archiveConfig.maxRetainedRecords > byteRecordLimit) {
                archiveConfig.maxRetainedRecords = byteRecordLimit;
            }
            uint32_t segmentLimit = byteRecordLimit;
            if (!archiveConfig.retainAllRecords
                && archiveConfig.maxRetainedRecords < segmentLimit) {
                segmentLimit = archiveConfig.maxRetainedRecords;
            }
            if (archiveConfig.segmentRecords > segmentLimit)
                archiveConfig.segmentRecords = segmentLimit;

            if (!archiveConfig.retainAllRecords) {
                const size_t logical =
                    archiveConfig.maxRetainedRecords;
                auto rotatingSlots = [logical](size_t segment) {
                    if (logical == 0 || segment == 0)
                        return (std::numeric_limits<size_t>::max)();
                    const size_t oldRecords = logical - 1u;
                    const size_t oldSegments =
                        oldRecords / segment
                        + (oldRecords % segment == 0 ? 0u : 1u);
                    if (oldSegments
                        >= (std::numeric_limits<size_t>::max)() / segment) {
                        return (std::numeric_limits<size_t>::max)();
                    }
                    return (oldSegments + 1u) * segment;
                };
                while (archiveConfig.segmentRecords > 1u
                       && rotatingSlots(archiveConfig.segmentRecords)
                              > recordsPerByteBudget) {
                    --archiveConfig.segmentRecords;
                }
            }
        }
    }
    _archive.reset(new (std::nothrow) SpectatorArchive(archiveConfig));
    if (!_archive) {
        failService("spectator spool could not be allocated", nowMs);
        error = _failureReason;
        return false;
    }

    SourceConfig sourceConfig{};
    sourceConfig.identity = _config.identity;
    sourceConfig.simulationContract = _config.simulationContract;
    sourceConfig.compatibilityIdentity = _config.compatibilityIdentity;
    sourceConfig.maxViewers = _maxViewers;
    sourceConfig.globalPacketBudget =
        _config.dataPacketBudget + _config.controlPacketBudget;
    sourceConfig.controlPacketBudget = _config.controlPacketBudget;
    sourceConfig.idleTimeoutMs = _config.noProgressDeadlineMs;
    sourceConfig.handshakeDeadlineMs = _config.handshakeDeadlineMs;
    sourceConfig.noProgressDeadlineMs = _config.noProgressDeadlineMs;
    sourceConfig.terminalAckDeadlineMs = _config.terminalAckDeadlineMs;
    sourceConfig.heartbeatIntervalMs = _config.heartbeatIntervalMs;
    sourceConfig.maxUnackedRecords = _config.maxViewerUnackedRecords;
    sourceConfig.maxUnackedBytes = _config.maxViewerUnackedBytes;
    _source.reset(new (std::nothrow)
                      SpectatorSource(sourceConfig, *_archive));
    if (!_source || !_source->isConfigured()) {
        failService("spectator source could not be configured", nowMs);
        error = _failureReason;
        return false;
    }

    if (_transport) {
        std::string transportError;
        if (!_transport->open(_config.listenPort, transportError)) {
            _transport.reset();
            failService(transportError.empty()
                            ? "the spectator port could not be opened"
                            : transportError.c_str(),
                        nowMs);
            error = _failureReason;
            return false;
        }

        std::string traversalWarning;
        if (!_clientPrivateRootDormant
            && !_transport->configureSourceAutopunch(
                _config.sourceAutopunch, traversalWarning)) {
            _diagnostics.note(
                DiagEvent::RouteLost, nowMs,
                traversalWarning.empty()
                    ? "source AutoPunch is unavailable; direct spectators remain enabled"
                    : traversalWarning.c_str());
        }
        traversalWarning.clear();
        if (!_clientPrivateRootDormant
            && !_transport->configureSourceRelay(
                _config.sourceRelay, traversalWarning)) {
            _diagnostics.note(
                DiagEvent::RouteLost, nowMs,
                traversalWarning.empty()
                    ? "source spectator relay is unavailable; direct spectators remain enabled"
                    : traversalWarning.c_str());
        }
    }

    setPhase(SidecarPhase::Waiting, nowMs);
    _statusCode = SourceStatusCode::WaitingForStart;
    return true;
}

void SpectatorSidecar::setPhase(SidecarPhase phase, uint64_t nowMs) {
    (void)nowMs;
    _phase = phase;
    if (_source) _source->setSourcePhase(toWireSourcePhase(phase));
}

void SpectatorSidecar::publishResumeFloor() {
    if (!_archive || !_hasGame) return;
    {
        static uint64_t lastSetSeekLogMs = 0;
        const uint64_t nowTick = GetTickCount64();
        if (_source && nowTick - lastSetSeekLogMs >= 1000u) {
            for (const auto& pair : _viewers) {
                qnetplay::spectator::SourceViewerSnapshot snap{};
                const bool found =
                    _source->viewerSnapshot(pair.first, snap);
                lastSetSeekLogMs = nowTick;
                QLOG("[spectator-sidecar] set-seek push viewer=%016llX "
                     "found=%d wants=%d phase=%u offered=%llx chunks=%u/%u "
                     "blocked=%u",
                     static_cast<unsigned long long>(pair.first),
                     found ? 1 : 0,
                     found && snap.wantsNewestCheckpoint ? 1 : 0,
                     found ? static_cast<unsigned>(snap.phase) : 0u,
                     static_cast<unsigned long long>(
                         found ? snap.offeredCheckpointId : 0u),
                     found ? snap.checkpointChunksSent : 0u,
                     found ? snap.checkpointChunkCount : 0u,
                     found ? snap.checkpointPushBlocked : 0u);
            }
        }
    }
    if (_viewers.empty()) {
        _archive->clearResumeFloor(_gameId);
        return;
    }
    uint32_t floor = 0xFFFFFFFFu;
    for (const auto& entry : _viewers)
        floor = (std::min)(floor, entry.second.lastAckedRecord);
    if (floor == 0xFFFFFFFFu) floor = 0;
    _archive->setResumeFloor(_gameId, floor);
}

void SpectatorSidecar::failService(const char* reason, uint64_t nowMs) {
    if (_serviceFailed) return;
    _serviceFailed = true;
    ++_spoolFailures;
    _failureReason = reason ? reason : "spectator service failed";
    _statusCode = SourceStatusCode::ProtocolOrArchiveFailure;
    _diagnostics.note(DiagEvent::SpoolFailure, nowMs, _failureReason.c_str());
    if (_hasGame && !_sealed) sealTruncated(_failureReason.c_str(), nowMs);
    setPhase(SidecarPhase::Failed, nowMs);
    std::vector<uint64_t> tokens;
    tokens.reserve(_viewers.size());
    for (const auto& entry : _viewers) tokens.push_back(entry.first);
    for (uint64_t token : tokens)
        retireViewer(token, ViewerServiceState::DeadlineExpired, nowMs);
    _reservations.clear();
    stopResilientRoot(nowMs);
    if (_transport) {
        _transport->close();
        _transport.reset();
    }
    if (_channel) {
        _channel->close();
        _channel.reset();
    }
}

void SpectatorSidecar::injectSpoolFailure(const char* reason, uint64_t nowMs) {
    failService(reason ? reason : "injected spool failure", nowMs);
}

void SpectatorSidecar::publish(const PublicationEvent& event, uint64_t nowMs) {
    if (nowMs > _lastServiceMs) _lastServiceMs = nowMs;
    ++_publicationEvents;
    if (_serviceFailed || _policy == AdmissionPolicy::Disabled) return;

    if (event.sequence != 0) {
        if (event.sequence < _expectedPublicationSequence) return;
        if (event.sequence > _expectedPublicationSequence) {
            char detail[96];
            std::snprintf(detail, sizeof(detail),
                          "publication gap: expected %llu, saw %llu",
                          static_cast<unsigned long long>(
                              _expectedPublicationSequence),
                          static_cast<unsigned long long>(event.sequence));
            handleUpstreamLoss(detail, nowMs);
            return;
        }
        _expectedPublicationSequence = event.sequence + 1;
    }

    switch (event.kind) {
    case PublicationKind::Start:
        applyStart(event, nowMs);
        break;
    case PublicationKind::Record:
        applyRecord(event, nowMs);
        break;
    case PublicationKind::ChainedSetEvent:
        applyChainedSetEvent(event, nowMs);
        break;
    case PublicationKind::Checkpoint:
        applyCheckpoint(event, nowMs);
        break;
    case PublicationKind::Seal:
        requestSeal(event.reason, event.terminalRecordCursor,
                    event.terminalPrefixDigest,
                    event.postTailStateHash,
                    event.hasTerminalPrefixProof,
                    event.hasPostTailProof, nowMs);
        break;
    case PublicationKind::Pause:
        if (_phase == SidecarPhase::Active
            || _phase == SidecarPhase::Checkpointing) {
            setPhase(SidecarPhase::Paused, nowMs);
            _statusCode = SourceStatusCode::PausedHeld;
        }
        break;
    case PublicationKind::Resume:
        if (_phase == SidecarPhase::Paused) {
            setPhase(SidecarPhase::Active, nowMs);
            _statusCode = SourceStatusCode::Streaming;
        }
        break;
    case PublicationKind::LossReport:
        applyPublicationFault(event, nowMs);
        break;
    case PublicationKind::TailProof:
        applyTailProof(event, nowMs);
        break;
    case PublicationKind::ConnectedDescriptorV4:
        applyConnectedDescriptorV4(event, nowMs);
        break;
    case PublicationKind::ConnectedPublicationV4:
        applyConnectedPublicationV4(event, nowMs);
        break;
    }
}

void SpectatorSidecar::applyConnectedDescriptorV4(
    const PublicationEvent& event, uint64_t nowMs) {
    namespace qsp = qnetplay::spectator;
    const qsp::ConnectedSetDescriptorV4& descriptor =
        event.connectedDescriptor;
    if (_connectedV4Degraded) return;
    if (descriptor.sourceIdentity != _config.identity
        || descriptor.simulationContract != _config.simulationContract
        || descriptor.setStreamId == 0u
        || !qsp::connectedDescriptorMatchesCompatibilityIdentityV4(
               descriptor, _config.compatibilityIdentity)) {
        _connectedV4Degraded = true;
        _connectedV4Failure =
            "QSP4 descriptor did not match this spectator source identity";
        _diagnostics.note(
            DiagEvent::PublicationGap, nowMs,
            _connectedV4Failure.c_str());
        return;
    }
    if (_connectedArchiveV4 && _connectedArchiveV4->bound()) {
        if (_connectedArchiveV4->setStreamId()
            == descriptor.setStreamId
            && _connectedArchiveV4->genesisDigest()
                == qsp::computeConnectedGenesisDigestV4(descriptor)) {
            return;
        }
        _connectedV4Degraded = true;
        _connectedV4Failure =
            "QSP4 descriptor conflicted with the active connected set";
        _diagnostics.note(
            DiagEvent::PublicationGap, nowMs,
            _connectedV4Failure.c_str());
        return;
    }

    qsp::ConnectedArchiveConfigV4 archiveConfig{};
    archiveConfig.retentionMs =
        qsp::kSpectatorPostGameRetentionMsV4;
    std::unique_ptr<qsp::SpectatorConnectedArchiveV4> archive(
        new (std::nothrow)
            qsp::SpectatorConnectedArchiveV4(archiveConfig));
    std::unique_ptr<qsp::SpectatorConnectedSourceSessionV4> source(
        new (std::nothrow)
            qsp::SpectatorConnectedSourceSessionV4());
    if (!archive || !source
        || !archive->reset(descriptor, nowMs)
        || !source->bind(archive.get())) {
        _connectedV4Degraded = true;
        _connectedV4Failure =
            "QSP4 connected archive could not be allocated or bound";
        _diagnostics.note(
            DiagEvent::SpoolFailure, nowMs,
            _connectedV4Failure.c_str());
        return;
    }
    _connectedArchiveV4 = std::move(archive);
    _connectedSourceV4 = std::move(source);
}

void SpectatorSidecar::applyConnectedPublicationV4(
    const PublicationEvent& event, uint64_t nowMs) {
    namespace qsp = qnetplay::spectator;
    if (_connectedV4Degraded) return;
    if (!_connectedArchiveV4 || !_connectedSourceV4) {
        _connectedV4Degraded = true;
        _connectedV4Failure =
            "QSP4 publication arrived before its descriptor";
        _diagnostics.note(
            DiagEvent::PublicationGap, nowMs,
            _connectedV4Failure.c_str());
        return;
    }
    const qsp::ConnectedCommitResultV4 result =
        _connectedArchiveV4->commit(
            event.connectedPublication, nowMs);
    if (result == qsp::ConnectedCommitResultV4::Committed
        || result == qsp::ConnectedCommitResultV4::ExactDuplicate) {
        refreshConnectedPresence(event.connectedPublication);
        return;
    }
    _connectedV4Degraded = true;
    char detail[128];
    std::snprintf(
        detail, sizeof(detail),
        "QSP4 archive rejected publication item=%u result=%u",
        qsp::firstItemSequenceV4(event.connectedPublication),
        static_cast<unsigned>(result));
    _connectedV4Failure = detail;
    _diagnostics.note(
        DiagEvent::PublicationGap, nowMs,
        _connectedV4Failure.c_str());
}

void SpectatorSidecar::refreshConnectedPresence(
    const qnetplay::spectator::ConnectedPublicationV4& publication) {
    namespace qsp = qnetplay::spectator;

    const auto applyManifest = [this](
        const qreplay::GameManifestV2& manifest) {
        _presenceActiveFighterMask = manifest.activeFighterMask;
        for (size_t slot = 0u; slot < _presenceCharacterIds.size(); ++slot) {
            if ((manifest.activeFighterMask & (1u << slot)) != 0u) {
                _presenceCharacterIds[slot] =
                    manifest.slots[slot].identity.nativeCharacterId;
                _presencePaletteIndices[slot] =
                    manifest.slots[slot].identity.paletteIndex;
            } else {
                _presenceCharacterIds[slot] = 0xFFu;
                _presencePaletteIndices[slot] = 0u;
            }
        }
        for (size_t lane = 0u; lane < _presencePlayerNames.size(); ++lane) {
            const qreplay::ReplayTextV2& name = manifest.lanes[lane].name;
            _presencePlayerNames[lane].assign(
                name.data.data(), name.data.data() + name.bytes);
        }
    };

    const auto applyFrame = [this](
        const qsp::ConnectedConfirmedRecordV4& record) {
        _presenceHasCurrentGame = true;
        _presenceCurrentGameEnded = false;
        _presenceTimelineFrame = record.sourceTimelineFrame;
        _presenceRoundWins = record.frame.after.roundTallies;
    };

    switch (publication.kind) {
        case qsp::ConnectedPublicationKindV4::BeginGameBundle:
            applyManifest(publication.begin.start.start.manifest);
            applyFrame(publication.begin.frameZero);
            break;
        case qsp::ConnectedPublicationKindV4::ConfirmedRecord:
            applyFrame(publication.record);
            break;
        case qsp::ConnectedPublicationKindV4::SetEvent: {
            const qsp::SetEventPayloadV4& setEvent =
                publication.setEvent.event;
            if (setEvent.kind == qsp::SetEventKind::RosterSelected) {
                _presenceActiveFighterMask = setEvent.activeSlotMask;
                for (size_t slot = 0u;
                     slot < _presenceCharacterIds.size(); ++slot) {
                    if ((setEvent.activeSlotMask & (1u << slot)) != 0u) {
                        _presenceCharacterIds[slot] =
                            setEvent.roster[slot].nativeCharacterId;
                        _presencePaletteIndices[slot] =
                            setEvent.roster[slot].paletteIndex;
                    } else {
                        _presenceCharacterIds[slot] = 0xFFu;
                        _presencePaletteIndices[slot] = 0u;
                    }
                }
                _presenceRoundWins = {};
                _presenceCurrentGameEnded = false;
            } else if (setEvent.kind
                       == qsp::SetEventKind::ReturnedToCharacterSelect) {
                _presenceHasCurrentGame = false;
                _presenceCurrentGameEnded = false;
                _presenceTimelineFrame = setEvent.sourceTimelineFrame;
                _presenceActiveFighterMask = 0u;
                _presenceCharacterIds = {{0xFFu, 0xFFu, 0xFFu, 0xFFu}};
                _presencePaletteIndices = {};
                _presenceRoundWins = {};
            }
            break;
        }
        case qsp::ConnectedPublicationKindV4::EndGameBundle:
            if (publication.end.containsFinalRecord)
                applyFrame(publication.end.finalRecord);
            _presenceHasCurrentGame = true;
            _presenceCurrentGameEnded = true;
            _presenceRoundWins = publication.end.end.end.roundTallies;
            break;
        case qsp::ConnectedPublicationKindV4::SetSeal:
            break;
        case qsp::ConnectedPublicationKindV4::Palette:
            break;
    }

    _presenceMatchWins = {};
    if (_connectedArchiveV4) {
        for (const qsp::ConnectedGameRangeV4& range :
             _connectedArchiveV4->ranges()) {
            if (!range.complete) continue;
            bool winningSide[2] = {false, false};
            for (size_t slot = 0u; slot < qreplay::kFighterSlots; ++slot) {
                if ((range.end.winningSideMask & (1u << slot)) == 0u)
                    continue;
                const uint8_t side = range.start.manifest.slots[slot].side;
                if (side < 2u) winningSide[side] = true;
            }
            for (size_t side = 0u; side < 2u; ++side) {
                if (!winningSide[side] || winningSide[side ^ 1u]) continue;
                if (_presenceMatchWins[side]
                    != (std::numeric_limits<uint16_t>::max)()) {
                    ++_presenceMatchWins[side];
                }
            }
        }
    }
}

void SpectatorSidecar::applyStart(const PublicationEvent& event,
                                  uint64_t nowMs) {
    if (_hasGame || !_archive) return;
    if (!qnetplay::compat::isValidCompatibilityIdentity(
            _config.compatibilityIdentity)) {
        failService("the sidecar has no complete simulation compatibility "
                    "identity", nowMs);
        return;
    }
    GameDescriptor descriptor = event.descriptor;
    descriptor.compatibilityIdentity = _config.compatibilityIdentity;
    const ArchiveResult result = _archive->beginGame(descriptor);
    if (result != ArchiveResult::Ok
        && result != ArchiveResult::ExactDuplicate) {
        failService("the spectator spool refused the match descriptor", nowMs);
        return;
    }
    GameDescriptor acceptedDescriptor{};
    if (!_archive->descriptor(descriptor.gameId, acceptedDescriptor)) {
        failService("the spectator spool lost its accepted match descriptor",
                    nowMs);
        return;
    }
    _hasGame = true;
    _descriptor = acceptedDescriptor;
    _gameId = acceptedDescriptor.gameId;
    _startFingerprint = gameStartFingerprint(canonicalGameStart(_descriptor));
    _hasProvenPostTail = descriptor.initialStateHash != 0;
    _provenPostTailHash = descriptor.initialStateHash;
    setPhase(SidecarPhase::Active, nowMs);
    _statusCode = SourceStatusCode::Streaming;
    if (!_heldSetEvents.empty()) {
        QLOG("[spectator-sidecar] draining %zu set event(s) held from before "
             "the match archive opened",
             _heldSetEvents.size());
        drainHeldSetEvents(nowMs);
    }
    startResilientRoot(nowMs);
}

void SpectatorSidecar::startResilientRoot(const uint64_t nowMs) {
    if (_clientPrivateRootDormant) return;
    stopResilientRuntime(nowMs,
        topology::wire::AuthorityStateReason::ServiceStopping);
    if (!_topologyActivationConfigured || !_topologyQsr
        || !_topologyQsr->open() || !_topologyActivation.requested) {
        return;
    }
    if (_transport == nullptr || _topologyActivation.rootQsp.local.port
                              != _transport->boundPort()) {
        _diagnostics.note(
            DiagEvent::RouteLost, nowMs,
            "authenticated spectator topology rejected an unavailable QSP binding");
        stopResilientRoot(nowMs);
        return;
    }
    if (!qnetplay::spectator::isValidIdentity(_config.identity)) {
        _diagnostics.note(
            DiagEvent::RouteLost, nowMs,
            "authenticated spectator topology rejected an invalid session identity");
        stopResilientRoot(nowMs);
        return;
    }
    GameDescriptor acceptedDescriptor{};
    if (_archive == nullptr || !_archive->descriptor(_gameId,
                                                     acceptedDescriptor)) {
        _diagnostics.note(
            DiagEvent::RouteLost, nowMs,
            "authenticated spectator topology rejected a missing archive descriptor");
        stopResilientRoot(nowMs);
        return;
    }
    if (!qnetplay::compat::semanticallyEqual(
            _config.compatibilityIdentity,
            acceptedDescriptor.compatibilityIdentity)) {
        _diagnostics.note(
            DiagEvent::RouteLost, nowMs,
            "authenticated spectator topology rejected a descriptor/content identity mismatch");
        stopResilientRoot(nowMs);
        return;
    }

    qnetplay::spectator::resilient::StreamCompatibilityIdentity stream{};
    if (!qnetplay::spectator::resilient::makeStreamCompatibilityIdentity(
            acceptedDescriptor.compatibilityIdentity,
            _config.identity.sessionId,
            _config.identity.epoch,
            gameStartFingerprint(canonicalGameStart(acceptedDescriptor)),
            stream)) {
        _diagnostics.note(
            DiagEvent::RouteLost, nowMs,
            "authenticated spectator topology rejected incomplete stream identity");
        stopResilientRoot(nowMs);
        return;
    }

    std::shared_ptr<topology::TopologyAuthority> authority(
        new (std::nothrow) topology::TopologyAuthority());
    if (!authority) {
        _diagnostics.note(
            DiagEvent::RouteLost, nowMs,
            "authenticated spectator topology could not allocate its authority");
        stopResilientRoot(nowMs);
        return;
    }
    std::string authorityError;
    if (!authority->initialize(
            _config.identity, acceptedDescriptor.compatibilityIdentity,
            gameStartFingerprint(canonicalGameStart(acceptedDescriptor)),
            _topologyActivation, _topologyQsr->binding(), nowMs,
            authorityError)) {
        _diagnostics.note(
            DiagEvent::RouteLost, nowMs,
            authorityError.empty()
                ? "authenticated spectator topology could not initialize"
                : authorityError.c_str());
        stopResilientRoot(nowMs);
        return;
    }
    authority->setVerifiedCursor(
        qnetplay::spectator::resilient::VerifiedChainCursor{
            0u, 0u, acceptedDescriptor.initialDigest});

    const topology::TopologyAuthorityFacts& facts = authority->facts();
    qnetplay::spectator::resilient::NodeAdvertisement local{};
    local.nodeId = facts.rootNodeId;
    local.kind = qnetplay::spectator::resilient::NodeKind::HostRoot;
    local.stream = stream;
    local.route = facts.rootQsp.advertised;
    local.capabilityModelRevision =
        qnetplay::spectator::resilient::kResilientCapabilityModelRevision;
    local.capabilities =
        qnetplay::spectator::resilient::kCapabilityMultiViewerRoot;
    const size_t childCapacity = (std::min)(
        facts.limits.maxChildrenPerRoot,
        static_cast<size_t>((std::numeric_limits<uint32_t>::max)()));
    local.childCapacity = static_cast<uint32_t>(childCapacity);
    local.childCount = 0u;
    local.uploadCapacity = local.childCapacity;
    local.uploadInUse = 0u;
    local.sourceGeneration = facts.sourceGeneration;
    local.routeGeneration = facts.rootQsp.generation;
    local.lastProgressMs = 0u;
    local.leaseExpiryMs = facts.expiresMs;
    local.reachable = true;

    resilient::ResilientRootControlConfig completeConfig{};
    completeConfig.enabled = true;
    completeConfig.nodeId = local.nodeId;
    completeConfig.nodeKind = local.kind;
    completeConfig.route = local.route;
    completeConfig.capabilities = local.capabilities;
    completeConfig.childCapacity = local.childCapacity;
    completeConfig.uploadCapacity = local.uploadCapacity;
    completeConfig.sourceGeneration = local.sourceGeneration;
    completeConfig.lastVerifiedProgressMs = local.lastProgressMs;
    completeConfig.leaseExpiryMs = local.leaseExpiryMs;
    completeConfig.limits = facts.limits;
    completeConfig.parentLeaseKey = authority->leaseKeyForRuntime();
    completeConfig.authorizer = authority;

    resilient::ResilientRootRuntimeConfig runtimeConfig{};
    runtimeConfig.stream = stream;
    runtimeConfig.localAdvertisement = local;
    runtimeConfig.gameId = acceptedDescriptor.gameId;
    runtimeConfig.initialDigest = acceptedDescriptor.initialDigest;
    runtimeConfig.firstTimelineFrame = acceptedDescriptor.firstTimelineFrame;
    runtimeConfig.limits = completeConfig.limits;
    runtimeConfig.parentLeaseKey = completeConfig.parentLeaseKey;
    runtimeConfig.authorizer = completeConfig.authorizer;

    std::unique_ptr<resilient::ResilientSpectatorRootRuntime> runtime(
        new (std::nothrow) resilient::ResilientSpectatorRootRuntime());
    if (!runtime || !runtime->start(runtimeConfig, *_topologyQsr)) {
        _diagnostics.note(
            DiagEvent::RouteLost, nowMs,
            "authenticated spectator topology could not bind its root runtime");
        stopResilientRoot(nowMs);
        return;
    }
    _topologyQsr->attach(*authority, *runtime);
    _topologyAuthority = std::move(authority);
    _resilientRoot = std::move(runtime);
    _config.resilientRoot = std::move(completeConfig);
}

void SpectatorSidecar::stopResilientRuntime(
    const uint64_t nowMs, const topology::wire::AuthorityStateReason reason) {
    if (_topologyQsr && _topologyAuthority
        && _topologyAuthority->ready()) {
        (void)_topologyQsr->sendAuthorityState(
            topology::wire::AuthorityStateKind::Revocation, reason,
            nowMs);
    }
    if (_topologyQsr) _topologyQsr->detach();
    if (_resilientRoot) _resilientRoot->stop();
    _resilientRoot.reset();
    _resilientControlTransport.reset();
    if (_topologyAuthority) _topologyAuthority->revoke();
    _topologyAuthority.reset();
    _config.resilientRoot = {};
    _topologyRelayLaneReady = false;
}

void SpectatorSidecar::stopResilientRoot(
    const uint64_t nowMs, const topology::wire::AuthorityStateReason reason) {
    stopResilientRuntime(nowMs, reason);
    if (_topologyQsr) {
        _topologyQsr->close();
        _topologyQsr.reset();
    }
    _topologyActivation = {};
    _topologyActivationConfigured = false;
    _config.topologyActivation = {};
}

bool SpectatorSidecar::configureTopologyAuthority(
    const topology::TopologyActivationRequest& request,
    const uint64_t nowMs, std::string& error) {
    error.clear();
    stopResilientRoot(nowMs,
                      topology::wire::AuthorityStateReason::SessionReplaced);
    if (!_started || _serviceFailed || _clientPrivateRootDormant
        || _policy == AdmissionPolicy::Disabled
        || !request.requested || !_transport
        || !request.rootQsp.validForAuthority()
        || request.rootQsp.local.port != _transport->boundPort()) {
        error = "the spectator sidecar has no exact live QSP binding for topology";
        return false;
    }

    std::unique_ptr<topology::TopologyQsrControl> qsr(
        new (std::nothrow) topology::TopologyQsrControl());
    topology::EndpointBinding qsrBinding{};
    if (!qsr || !qsr->open(request, request.rootQsp, qsrBinding, error)) {
        if (error.empty())
            error = "the topology QSR control socket could not be allocated";
        return false;
    }
    if (qsrBinding.local.port == request.rootQsp.local.port
        || qsrBinding.advertised.port == request.rootQsp.advertised.port) {
        qsr->close();
        error = "the topology QSR control socket was not distinct from QSP";
        return false;
    }

    if (_config.sourceRelay.enabled) {
        topology::TopologyRelayLaneConfig laneConfig{};
        laneConfig.enabled = true;
        laneConfig.relay = _config.sourceRelay.relay;
        laneConfig.room = _config.sourceRelay.room;
        laneConfig.authorityToken = _config.sourceRelay.sourceToken;
        laneConfig.sessionId = _config.sourceRelay.identity.sessionId;
        std::string laneWarning;
        if (!qsr->configureRelayLane(laneConfig, laneWarning)) {
            _diagnostics.note(
                DiagEvent::RouteLost, nowMs,
                laneWarning.empty()
                    ? "topology relay lane unavailable; direct QSR continues"
                    : laneWarning.c_str());
        }
    }

    _topologyActivation = request;
    _topologyActivationConfigured = true;
    _topologyQsr = std::move(qsr);
    _config.topologyActivation = request;
    if (!_hasGame) return true;

    startResilientRoot(nowMs);
    if (_resilientRoot && _topologyAuthority && _topologyAuthority->ready())
        return true;

    error = "the authenticated topology authority could not activate for the accepted game descriptor";
    stopResilientRoot(nowMs);
    return false;
}

bool SpectatorSidecar::confirmClientPrivateRuntimeApplied(
    const qipc::IpcConfig& launchConfig,
    const qipc::SpectatorClientPublisherRuntimeAppliedReceipt& receipt,
    const uint32_t expectedProcessId, std::string& error) {
    error.clear();
    if (!_started || _serviceFailed || !_clientPrivateRootDormant) {
        error = "the client-private spectator collector is not running";
        return false;
    }
    if (_clientPrivateRuntimeApplied) {
        error = "the client-private RuntimeApplied receipt was already recorded";
        return false;
    }
    if (expectedProcessId == 0u
        || qipc::validateSpectatorClientPrivatePublisher(launchConfig)
               != nullptr
        || qipc::validateIpcCompatibilityIdentity(launchConfig) != nullptr
        || qipc::validateIpcConfigurationProvenance(launchConfig) != nullptr
        || !qipc::spectatorClientPublisherRuntimeAppliedReceiptMatches(
            receipt, launchConfig, expectedProcessId)) {
        error = "the client-private RuntimeApplied receipt did not match its launch";
        return false;
    }

    const qnetplay::spectator::SessionCookie launchSourceCookie{
        launchConfig.spectatorCookieLow, launchConfig.spectatorCookieHigh};
    if (_config.identity.sessionId != launchConfig.sessionId
        || _config.identity.cookie != launchSourceCookie
        || _config.simulationContract != launchConfig.compatibilityId
        || !qnetplay::compat::semanticallyEqual(
            _config.compatibilityIdentity,
            launchConfig.compatibilityIdentity)
        || _config.clientPrivateRoot.publisherGeneration
               != launchConfig.spectatorClientPublisherGeneration
        || _config.clientPrivateRoot.publisherNonce
               != launchConfig.spectatorClientPublisherNonce) {
        error = "the client-private collector configuration did not match its launch";
        return false;
    }

    _clientPrivateRuntimeApplied = true;
    _clientPrivateRuntimeReceipt.processId = receipt.processId;
    _clientPrivateRuntimeReceipt.playerSlot = receipt.localPlayer;
    _clientPrivateRuntimeReceipt.delay = receipt.delay;
    _clientPrivateRuntimeReceipt.rollback = receipt.rollback;
    _clientPrivateRuntimeReceipt.remoteDelay = receipt.remoteDelay;
    _clientPrivateRuntimeReceipt.remoteRollback = receipt.remoteRollback;
    _clientPrivateRuntimeReceipt.provenance =
        receipt.configurationProvenance;
    return true;
}

bool SpectatorSidecar::clientPrivateRootStreamFacts(
    ClientPrivateRootStreamFacts& out) const {
    out = {};
    if (!_started || _serviceFailed || !_clientPrivateRootDormant
        || !_clientPrivateRuntimeApplied || !_archive || !_hasGame) {
        return false;
    }
    qnetplay::spectator::GameDescriptor descriptor{};
    qnetplay::spectator::ArchiveExtent extent{};
    uint32_t firstRetained = 0u;
    if (!_archive->descriptor(_gameId, descriptor)
        || !_archive->extent(_gameId, extent)
        || !_archive->firstRetainedRecord(_gameId, firstRetained)
        || extent.sealed) {
        return false;
    }
    if (_config.identity.sessionId == 0u || _config.identity.epoch == 0u)
        return false;
    out.available = true;
    out.sessionId = _config.identity.sessionId;
    out.sessionEpoch = _config.identity.epoch;
    out.compatibility = descriptor.compatibilityIdentity;
    out.streamRootIdentity = _startFingerprint;
    out.gameStartFingerprint = _startFingerprint;
    out.collectorGeneration = _config.clientPrivateRoot.publisherGeneration;
    out.publisherNonce = _config.clientPrivateRoot.publisherNonce;
    out.playerSlot = _clientPrivateRuntimeReceipt.playerSlot;
    out.gameProcessId = _clientPrivateRuntimeReceipt.processId;
    out.appliedDelay = _clientPrivateRuntimeReceipt.delay;
    out.appliedRollback = _clientPrivateRuntimeReceipt.rollback;
    out.appliedRemoteDelay = _clientPrivateRuntimeReceipt.remoteDelay;
    out.appliedRemoteRollback = _clientPrivateRuntimeReceipt.remoteRollback;
    out.provenance = _clientPrivateRuntimeReceipt.provenance;
    out.cacheFirstCursor = firstRetained;
    out.cacheNextCursor = extent.nextRecord;
    out.nextEvent = extent.nextEvent;
    out.chainDigest = extent.digest;
    out.collectorQspPort = boundPort();
    out.clientRootQsrPort = _clientRoot.prepared ? _clientRoot.qsrPort : 0u;
    return true;
}

bool SpectatorSidecar::prepareClientPlayerRootQsr(uint16_t& outPort,
                                                  std::string& error) {
    outPort = 0u;
    error.clear();
    if (!_started || _serviceFailed || !_clientPrivateRootDormant
        || !_clientPrivateRuntimeApplied) {
        error = "the client-private collector has no confirmed runtime";
        return false;
    }
    if (_clientRoot.prepared) {
        outPort = _clientRoot.qsrPort;
        return true;
    }
    if (!_clientRoot.qsrSocket.open(0u)) {
        error = "the client-root QSR socket could not bind";
        return false;
    }
    const uint16_t port = _clientRoot.qsrSocket.boundPort();
    if (port == 0u || port == boundPort()) {
        _clientRoot.qsrSocket.close();
        error = "the client-root QSR socket did not bind a distinct port";
        return false;
    }
    _clientRoot.prepared = true;
    _clientRoot.qsrPort = port;
    outPort = port;
    return true;
}

bool SpectatorSidecar::armClientPlayerRoot(
    const ClientPlayerRootArmFacts& facts, const uint64_t nowMs,
    std::string& error) {
    error.clear();
    if (!_started || _serviceFailed || !_clientPrivateRootDormant
        || !_clientPrivateRuntimeApplied || !_clientRoot.prepared) {
        error = "the client-root QSR socket is not prepared";
        return false;
    }
    if (_clientRoot.armed) {
        error = "the client root is already armed";
        return false;
    }
    if (facts.clientRootNodeId == 0u || facts.hostRootNodeId == 0u
        || facts.authorityGeneration == 0u || facts.hostSourceGeneration == 0u
        || facts.rootQsr.advertisedIpv4BE == 0u
        || facts.rootQsr.advertisedPort == 0u || facts.grantBlob.empty()) {
        error = "the client-root enrollment facts are incomplete";
        return false;
    }
    topology::wire::ControlDatagram grantDatagram{};
    if (topology::wire::decodeControlDatagram(facts.grantBlob.data(),
                                              facts.grantBlob.size(),
                                              grantDatagram)
            != topology::wire::DecodeResult::Ok
        || grantDatagram.type
            != topology::wire::ControlMessageType::ClientPlayerRootGrant) {
        error = "the enrollment grant blob is not a grant datagram";
        return false;
    }
    const topology::wire::ClientPlayerRootGrant& grant =
        grantDatagram.clientPlayerRootGrant;
    topology::TopologyAuthoritySignatureVerifier verifier;
    std::vector<uint8_t> attestation;
    const bool grantVerified =
        verifier.initialize(facts.authorityPublicKey.data(),
                            facts.authorityPublicKey.size())
        && topology::wire::encodeClientPlayerRootGrantAttestationInput(
            grant, attestation)
        && verifier.verify(attestation.data(), attestation.size(),
                           grant.hostSignature.data(),
                           grant.hostSignature.size());
    std::fill(attestation.begin(), attestation.end(), uint8_t{0u});
    if (!grantVerified
        || grant.authorityGeneration != facts.authorityGeneration
        || grant.hostSignature.size() == 0u
        || grant.binding.clientRootNodeId != facts.clientRootNodeId
        || grant.grantNonce == 0u) {
        error = "the enrollment grant did not verify against the authority key";
        return false;
    }
    if (grant.binding.qsrRoute.port != _clientRoot.qsrPort) {
        error = "the granted QSR route does not match the prepared socket";
        return false;
    }
    _clientRoot.macFacts = {};
    _clientRoot.macFacts.session.sessionId = _config.identity.sessionId;
    _clientRoot.macFacts.session.epoch = _config.identity.epoch;
    _clientRoot.macFacts.compatibility = grant.binding.stream.compatibility;
    _clientRoot.macFacts.streamRootIdentity =
        grant.binding.stream.streamRootIdentity;
    _clientRoot.macFacts.gameStartFingerprint =
        grant.binding.gameStartFingerprint;
    _clientRoot.macFacts.authorityGeneration = facts.authorityGeneration;
    _clientRoot.macFacts.rootNodeId = facts.hostRootNodeId;
    const auto bindingFromWire =
        [](const qnet::SpectateTopologyEndpointBindingWire& wire,
           topology::EndpointBinding& binding) {
            binding = {};
            binding.local.ipv4BE = wire.localIpv4BE;
            binding.local.port = wire.localPort;
            binding.advertised.kind =
                static_cast<qnetplay::spectator::resilient::RouteKind>(
                    wire.advertisedRouteKind);
            binding.advertised.ipv4BE = wire.advertisedIpv4BE;
            binding.advertised.port = wire.advertisedPort;
            binding.advertised.generation = wire.advertisedRouteGeneration;
            binding.advertised.routeProved = wire.advertisedRouteProved == 1u;
            binding.localProvenance =
                static_cast<topology::EndpointProvenance>(
                    wire.localProvenance);
            binding.advertisedProvenance =
                static_cast<topology::EndpointProvenance>(
                    wire.advertisedProvenance);
            binding.generation = wire.bindingGeneration;
        };
    bindingFromWire(facts.rootQsp, _clientRoot.macFacts.rootQsp);
    bindingFromWire(facts.rootQsr, _clientRoot.macFacts.rootQsr);
    _clientRoot.macFacts.sourceGeneration = facts.hostSourceGeneration;
    _clientRoot.macFacts.assignmentSigningPublicKey = facts.authorityPublicKey;
    _clientRoot.grantNonce = grant.grantNonce;
    _clientRoot.binding = grant.binding;
    _clientRoot.credential = facts.credential;
    _clientRoot.nextAssertSequence = 1u;
    _clientRoot.lastServerSequence = 0u;
    _clientRoot.nextAssertSendMs = nowMs;
    _clientRoot.assertDeadlineMs = nowMs + 15000u;
    _clientRoot.armed = true;
    _clientRoot.asserted = false;
    _clientRoot.committed = false;
    _clientRoot.activationRetained = false;
    return true;
}

void SpectatorSidecar::withdrawClientPlayerRoot(const char* reason) {
    if (!_clientRoot.prepared && !_clientRoot.armed) return;
    QLOG("[spectator.client-root] withdrawn: %s",
         reason != nullptr ? reason : "unspecified");
    deactivateClientRootServing(_lastServiceMs, "root withdrawal");
    _clientRoot.qsrSocket.close();
    _clientRoot.credential.fill(0u);
    _clientRoot.prepared = false;
    _clientRoot.armed = false;
    _clientRoot.asserted = false;
    _clientRoot.committed = false;
    _clientRoot.qsrPort = 0u;
    _clientRoot.macFacts = {};
    _clientRoot.grantNonce = 0u;
    _clientRoot.binding = {};
    _clientRoot.nextAssertSequence = 1u;
    _clientRoot.lastServerSequence = 0u;
    _clientRoot.nextAssertSendMs = 0u;
    _clientRoot.assertDeadlineMs = 0u;
    _clientRoot.activation = {};
    _clientRoot.activationRetained = false;
}

void SpectatorSidecar::activateClientRootServing(const uint64_t nowMs) {
    if (_clientRoot.servingActive || !_clientRoot.committed
        || !_clientRoot.activationRetained) {
        return;
    }
    _clientPrivateRootDormant = false;
    _clientRoot.servingActive = true;
    (void)nowMs;
    QLOG("[spectator.client-root] serving armed for the activation-bound "
         "viewer (capacity 1)");
}

void SpectatorSidecar::deactivateClientRootServing(const uint64_t nowMs,
                                                   const char* reason) {
    if (!_clientRoot.servingActive) return;
    _clientRoot.servingActive = false;
    _clientPrivateRootDormant = true;
    std::vector<uint64_t> attached;
    attached.reserve(_viewers.size());
    for (const auto& entry : _viewers) attached.push_back(entry.first);
    for (const uint64_t viewerId : attached)
        retireViewer(viewerId, ViewerServiceState::Detached, nowMs);
    QLOG("[spectator.client-root] serving withdrawn (%s); collector "
         "remains dormant-armed",
         reason != nullptr ? reason : "unspecified");
}

void SpectatorSidecar::serviceClientPlayerRoot(const uint64_t nowMs) {
    if (!_clientRoot.armed || !_clientRoot.prepared) return;
    if (!_clientRoot.committed && nowMs >= _clientRoot.assertDeadlineMs
        && !_clientRoot.asserted) {
        withdrawClientPlayerRoot("assert deadline passed without authority ack");
        return;
    }
    if (!_clientRoot.asserted && nowMs >= _clientRoot.nextAssertSendMs) {
        _clientRoot.nextAssertSendMs = nowMs + 500u;
        topology::wire::ControlDatagram assertion{};
        assertion.type =
            topology::wire::ControlMessageType::ClientPlayerRootAssertion;
        assertion.clientPlayerRootAssertion.authorityGeneration =
            _clientRoot.macFacts.authorityGeneration;
        assertion.clientPlayerRootAssertion.hostRootNodeId =
            _clientRoot.macFacts.rootNodeId;
        assertion.clientPlayerRootAssertion.grantNonce =
            _clientRoot.grantNonce;
        assertion.clientPlayerRootAssertion.binding = _clientRoot.binding;
        std::vector<uint8_t> inner;
        std::vector<uint8_t> wrapped;
        if (topology::wire::encodeControlDatagram(assertion, inner)
            && topology::encodeClientPlayerRootEnvelopeFromFacts(
                _clientRoot.macFacts, _clientRoot.grantNonce,
                _clientRoot.binding, _clientRoot.credential,
                _clientRoot.nextAssertSequence, inner.data(), inner.size(),
                wrapped)) {
            qnet::Endpoint destination{};
            destination.sa.sin_addr.s_addr =
                _clientRoot.macFacts.rootQsr.advertised.ipv4BE;
            destination.sa.sin_port =
                htons(_clientRoot.macFacts.rootQsr.advertised.port);
            if (_clientRoot.qsrSocket.sendTo(destination, wrapped.data(),
                                             wrapped.size())) {
                ++_clientRoot.nextAssertSequence;
            }
        }
    }
    std::array<uint8_t, 2048u> bytes{};
    for (size_t budget = 0u; budget < 16u; ++budget) {
        qnet::Endpoint peer{};
        const int received = _clientRoot.qsrSocket.recvFrom(
            bytes.data(), bytes.size(), peer);
        if (received <= 0) break;
        if (peer.sa.sin_addr.s_addr
                != _clientRoot.macFacts.rootQsr.advertised.ipv4BE
            || ntohs(peer.sa.sin_port)
                != _clientRoot.macFacts.rootQsr.advertised.port) {
            continue;
        }
        const uint8_t* payload = nullptr;
        size_t payloadLength = 0u;
        uint64_t sequence = 0u;
        if (!topology::decodeClientPlayerRootEnvelopeFromFacts(
                _clientRoot.macFacts, _clientRoot.grantNonce,
                _clientRoot.binding, _clientRoot.credential, bytes.data(),
                static_cast<size_t>(received),
                _clientRoot.lastServerSequence, payload, payloadLength,
                sequence)) {
            continue;
        }
        topology::wire::ControlDatagram decoded{};
        if (topology::wire::decodeControlDatagram(payload, payloadLength,
                                                  decoded)
            != topology::wire::DecodeResult::Ok) {
            continue;
        }
        _clientRoot.lastServerSequence = sequence;
        if (decoded.type
            == topology::wire::ControlMessageType::ClientPlayerRootGrant) {
            if (!_clientRoot.asserted
                && decoded.clientPlayerRootGrant.grantNonce
                    == _clientRoot.grantNonce) {
                _clientRoot.asserted = true;
                QLOG("[spectator.client-root] asserted with host authority");
            }
            continue;
        }
        if (decoded.type
            == topology::wire::ControlMessageType::ClientPlayerRootActivation) {
            const topology::wire::ClientPlayerRootActivation& activation =
                decoded.clientPlayerRootActivation;
            if (activation.binding.clientRootNodeId
                    == _clientRoot.binding.clientRootNodeId
                && activation.authorityGeneration
                    == _clientRoot.macFacts.authorityGeneration) {
                _clientRoot.activation = activation;
                _clientRoot.activationRetained = true;
                QLOG("[spectator.client-root] activation retained for "
                     "viewer generation %u",
                     activation.targetViewerNodeId != 0u ? 1u : 0u);
            }
            continue;
        }
        if (decoded.type
            == topology::wire::ControlMessageType::
                ClientPlayerRootActivationCommit) {
            const topology::wire::ClientPlayerRootActivationCommit& commit =
                decoded.clientPlayerRootActivationCommit;
            if (_clientRoot.activationRetained
                && commit.activation.activationNonce
                    == _clientRoot.activation.activationNonce) {
                _clientRoot.committed = true;
                QLOG("[spectator.client-root] final host commit observed; "
                     "activation-bound admission may arm");
                activateClientRootServing(nowMs);
            }
            continue;
        }
        if (decoded.type
            == topology::wire::ControlMessageType::
                ClientPlayerRootActivationRetirement) {
            const topology::wire::ClientPlayerRootActivationRetirement&
                retirement = decoded.clientPlayerRootActivationRetirement;
            if (_clientRoot.activationRetained
                && retirement.activation.activationNonce
                    == _clientRoot.activation.activationNonce) {
                _clientRoot.activationRetained = false;
                _clientRoot.committed = false;
                deactivateClientRootServing(nowMs,
                                            "signed host retirement");
                QLOG("[spectator.client-root] activation retired by host");
            }
            continue;
        }
    }
}

bool SpectatorSidecar::relayTopologyLaneAvailable() const {
    return _topologyRelayLaneReady && _transport != nullptr
        && _transport->relaySourceGeneration() != 0u
        && _topologyAuthority && _topologyAuthority->ready();
}

bool SpectatorSidecar::makeTopologyViewerProof(
    const uint64_t viewerId, const uint32_t viewerGeneration,
    const bool relayCapable, const bool requireStreaming,
    topology::TopologyViewerProof& out, std::string& error) const {
    out = {};
    error.clear();
    if (viewerId == 0u || viewerGeneration == 0u || !relayCapable
        || !_source) {
        error = "topology enrollment requires an authenticated relay-capable QSP viewer";
        return false;
    }
    const auto viewer = _viewers.find(viewerId);
    if (viewer == _viewers.end() || viewer->second.generation != viewerGeneration
        || !viewer->second.endpoint.valid()) {
        error = "topology enrollment did not match an active direct QSP viewer";
        return false;
    }
    const bool relayIngress = isRelayIngress(viewer->second.endpoint);
    if (relayIngress && !relayTopologyLaneAvailable()) {
        error = "topology enrollment did not match an active direct QSP viewer";
        return false;
    }
    SourceViewerSnapshot snapshot{};
    if (!_source->viewerSnapshot(viewerId, snapshot)
        || snapshot.viewerId != viewerId
        || snapshot.generation != viewerGeneration
        || snapshot.endpoint != viewer->second.endpointToken
        || snapshot.phase == SourceViewerPhase::Retired
        || (requireStreaming
            && snapshot.phase != SourceViewerPhase::Streaming)
        || (!requireStreaming
            && snapshot.phase != SourceViewerPhase::AwaitingInitAck
            && snapshot.phase != SourceViewerPhase::Streaming)) {
        error = "topology enrollment requires a current proved QSP streaming generation";
        return false;
    }

    out.viewerId = viewerId;
    out.viewerGeneration = viewerGeneration;
    out.endpoint = viewer->second.endpoint;
    out.provenance = relayIngress
        ? topology::EndpointProvenance::ProvenRelayQsp
        : (viewer->second.routeLabel
               == static_cast<uint16_t>(SpectatorRouteLabel::Punched)
               ? topology::EndpointProvenance::ProvenPunchedQsp
               : topology::EndpointProvenance::ProvenDirectQsp);
    out.routeGeneration = viewerGeneration;
    out.confirmedCursor = snapshot.acknowledgedNextRecord;
    out.qspStreaming = snapshot.phase == SourceViewerPhase::Streaming;
    out.relayCapable = relayCapable;
    return true;
}

bool SpectatorSidecar::enrollTopologyViewer(
    const uint64_t viewerId, const uint32_t viewerGeneration,
    const bool relayCapable, const uint64_t nowMs,
    topology::TopologyEnrollment& out, std::string& error) {
    out = {};
    if (!_resilientRoot || !_topologyAuthority
        || !_topologyAuthority->ready()) {
        error = "the authenticated spectator topology authority is unavailable";
        return false;
    }
    topology::TopologyViewerProof proof{};
    if (!makeTopologyViewerProof(viewerId, viewerGeneration, relayCapable,
                                 true, proof, error)) {
        return false;
    }
    if (topology::relayProvenance(proof.provenance)) {
        error = "topology enrollment did not match an active direct QSP viewer";
        return false;
    }
    return _topologyAuthority->enroll(proof, nowMs, out, error);
}

bool SpectatorSidecar::enrollTopologyViewerExact(
    const topology::TopologyViewerClaim& claim,
    const qnetplay::spectator::SpectatorGrant& grant,
    const uint64_t nowMs, topology::TopologyEnrollment& out,
    std::string& error) {
    out = {};
    error.clear();
    if (!_topologyAuthority || !_topologyAuthority->ready()) {
        error = "the authenticated spectator topology authority is unavailable";
        return false;
    }
    // Runs with g_sidecarMutex held; the grant, authority and QSP binding checks must stay in this one critical section.
    const topology::TopologyAuthorityFacts& facts = _topologyAuthority->facts();
    if (claim.sessionId == 0u || claim.sessionEpoch == 0u
        || claim.viewerId == 0u || claim.viewerGeneration == 0u
        || !claim.qspEndpoint.valid() || claim.qspRouteGeneration == 0u
        || claim.discoveryGeneration == 0u
        || !qnetplay::compat::isValidCompatibilityIdentity(
               claim.compatibility)
        || claim.streamRootIdentity == 0u
        || claim.sessionId != facts.session.sessionId
        || claim.sessionEpoch != facts.session.epoch
        || claim.discoveryGeneration != facts.sourceGeneration
        || !qnetplay::compat::semanticallyEqual(claim.compatibility,
                                                 facts.compatibility)
        || claim.streamRootIdentity != facts.streamRootIdentity
        || grant.viewerId != claim.viewerId
        || grant.sessionId != claim.sessionId
        || grant.sourceGeneration != claim.discoveryGeneration
        || !verifyTopologyViewerGrant(grant, claim.viewerGeneration, nowMs)) {
        error = "the released topology proof did not match the live viewer";
        return false;
    }
    topology::TopologyViewerProof live{};
    if (!makeTopologyViewerProof(claim.viewerId, claim.viewerGeneration,
                                 true,
                                 true, live, error)) {
        return false;
    }
    const bool relayIngress = topology::relayProvenance(live.provenance);
    const bool exactEndpoint = relayIngress
        ? (_config.sourceRelay.enabled
           && claim.qspEndpoint.ipv4BE == _config.sourceRelay.relay.ipv4BE
           && claim.qspEndpoint.port == _config.sourceRelay.relay.port)
        : live.endpoint == claim.qspEndpoint;
    if (!exactEndpoint
        || live.provenance != claim.qspProvenance
        || live.routeGeneration != claim.qspRouteGeneration
        || !live.qspStreaming || !live.relayCapable) {
        error = "the released topology proof did not match the proved QSP route";
        return false;
    }
    if (!_topologyAuthority->enroll(live, nowMs, out, error)) return false;
    if (relayIngress) {
        qnetplay::spectator::RelayQsrOffer offer{};
        const uint64_t nowUnix =
            static_cast<uint64_t>(std::time(nullptr));
        if (!_topologyQsr
            || !_topologyQsr->issueRelayQsrOffer(
                out.nodeId, out.session.sessionId, nowUnix, offer)) {
            detachTopologyViewer(claim.viewerId, nowMs);
            out = {};
            error = "the relayed topology lane could not issue an admission grant";
            return false;
        }
        out.relayQsrOffer = offer;
    }
    return true;
}

bool SpectatorSidecar::enrollClientPlayerRoot(
    const topology::ClientPlayerRootClaim& claim, const uint64_t nowMs,
    topology::ClientPlayerRootEnrollment& out,
    std::vector<uint8_t>& encodedGrant, std::string& error) {
    out = {};
    encodedGrant.clear();
    error.clear();
    if (_clientPrivateRootDormant) {
        error = "the client-private collector cannot enroll data roots";
        return false;
    }
    if (!_topologyAuthority || !_topologyAuthority->ready()) {
        error = "the authenticated spectator topology authority is unavailable";
        return false;
    }
    if (!_topologyAuthority->enrollClientPlayerRoot(claim, nowMs, out, error))
        return false;
    resilient::wire::ControlDatagram grantDatagram{};
    grantDatagram.type =
        resilient::wire::ControlMessageType::ClientPlayerRootGrant;
    grantDatagram.clientPlayerRootGrant = out.grant;
    if (!resilient::wire::encodeControlDatagram(grantDatagram, encodedGrant)
        || encodedGrant.empty()) {
        out = {};
        encodedGrant.clear();
        error = "the client-root grant could not be serialized";
        return false;
    }
    return true;
}

bool SpectatorSidecar::issueTopologyRelayChildAssignment(
    const qnetplay::spectator::resilient::NodeId relayNodeId,
    const qnetplay::spectator::resilient::NodeId childNodeId,
    const uint64_t nowMs, std::string& error) {
    error.clear();
    if (relayNodeId == qnetplay::spectator::resilient::kInvalidNodeId
        || childNodeId == qnetplay::spectator::resilient::kInvalidNodeId
        || relayNodeId == childNodeId) {
        error = "the topology relay and child selections must be distinct active nodes";
        return false;
    }
    if (!_topologyQsr || !_topologyQsr->open()
        || !_resilientRoot || !_resilientRoot->running()
        || !_topologyAuthority || !_topologyAuthority->ready()) {
        error = "the authenticated topology assignment authority is unavailable";
        return false;
    }
    if (!_topologyQsr->issueRelayChildAssignment(relayNodeId, childNodeId,
                                                  nowMs)) {
        error = "the coordinator-selected relay child assignment was rejected";
        return false;
    }
    return true;
}

bool SpectatorSidecar::issueTopologyRelayChildActivation(
    const qnetplay::spectator::resilient::NodeId relayNodeId,
    const qnetplay::spectator::resilient::NodeId childNodeId,
    const uint64_t nowMs, std::string& error) {
    error.clear();
    if (relayNodeId == qnetplay::spectator::resilient::kInvalidNodeId
        || childNodeId == qnetplay::spectator::resilient::kInvalidNodeId
        || relayNodeId == childNodeId) {
        error = "the topology relay and child selections must be distinct active nodes";
        return false;
    }
    if (!_topologyQsr || !_topologyQsr->open()
        || !_resilientRoot || !_resilientRoot->running()
        || !_topologyAuthority || !_topologyAuthority->ready()) {
        error = "the authenticated topology activation authority is unavailable";
        return false;
    }
    if (!_topologyQsr->issueRelayChildActivation(relayNodeId, childNodeId,
                                                  nowMs)) {
        error = "the coordinator-selected relay child activation was rejected";
        return false;
    }
    return true;
}

bool SpectatorSidecar::issueTopologyRelayChildActivationForViewers(
    const TopologyViewerSelection& relay,
    const TopologyViewerSelection& child, const uint64_t nowMs,
    std::string& error) {
    error.clear();
    if (relay.viewerId == 0u || relay.viewerGeneration == 0u
        || child.viewerId == 0u || child.viewerGeneration == 0u
        || relay.viewerId == child.viewerId) {
        error = "the topology relay and child selections must name distinct current viewers";
        return false;
    }
    if (!_topologyQsr || !_topologyQsr->open()
        || !_resilientRoot || !_resilientRoot->running()
        || !_topologyAuthority || !_topologyAuthority->ready()) {
        error = "the authenticated topology activation authority is unavailable";
        return false;
    }

    const auto resolve = [this](const TopologyViewerSelection& selected,
                                const char* role,
                                qnetplay::spectator::resilient::NodeId& nodeId,
                                std::string& detail) {
        nodeId = qnetplay::spectator::resilient::kInvalidNodeId;
        topology::TopologyViewerProof proof{};
        std::string proofError;
        if (!makeTopologyViewerProof(selected.viewerId,
                                     selected.viewerGeneration,
                                     true,
                                     true, proof,
                                     proofError)) {
            detail = std::string("the selected ") + role
                + " no longer matches a current streaming QSP viewer";
            return false;
        }
        if (!_topologyAuthority->nodeForViewer(selected.viewerId, nodeId)
            || nodeId == qnetplay::spectator::resilient::kInvalidNodeId) {
            detail = std::string("the selected ") + role
                + " is not currently enrolled for topology";
            return false;
        }
        topology::TopologyEnrollment enrollment{};
        if (!_topologyAuthority->enrollmentForNode(nodeId, enrollment)
            || enrollment.viewerId != selected.viewerId
            || enrollment.viewerGeneration != selected.viewerGeneration
            || enrollment.qspEndpoint != proof.endpoint
            || enrollment.qspProvenance != proof.provenance
            || enrollment.qspRouteGeneration != proof.routeGeneration
            || !proof.qspStreaming || !proof.relayCapable) {
            detail = std::string("the selected ") + role
                + " topology enrollment is no longer current";
            nodeId = qnetplay::spectator::resilient::kInvalidNodeId;
            return false;
        }
        return true;
    };

    qnetplay::spectator::resilient::NodeId relayNodeId =
        qnetplay::spectator::resilient::kInvalidNodeId;
    qnetplay::spectator::resilient::NodeId childNodeId =
        qnetplay::spectator::resilient::kInvalidNodeId;
    if (!resolve(relay, "relay", relayNodeId, error)
        || !resolve(child, "child", childNodeId, error)
        || relayNodeId == childNodeId) {
        if (error.empty()) {
            error = "the topology relay and child selections resolved to the same node";
        }
        return false;
    }

    return issueTopologyRelayChildActivation(relayNodeId, childNodeId,
                                             nowMs, error);
}

bool SpectatorSidecar::issueClientPlayerRootActivationForViewer(
    const TopologyViewerSelection& target, const uint64_t nowMs,
    std::string& error) {
    error.clear();
    if (target.viewerId == 0u || target.viewerGeneration == 0u) {
        error = "the client-root activation target must name a current viewer";
        return false;
    }
    if (!_topologyQsr || !_topologyQsr->open()
        || !_resilientRoot || !_resilientRoot->running()
        || !_topologyAuthority || !_topologyAuthority->ready()) {
        error = "the authenticated topology activation authority is unavailable";
        return false;
    }
    topology::TopologyViewerProof proof{};
    std::string proofError;
    if (!makeTopologyViewerProof(target.viewerId, target.viewerGeneration,
                                 true,
                                 true, proof,
                                 proofError)) {
        error = "the selected target no longer matches a current streaming "
                "QSP viewer";
        return false;
    }
    qnetplay::spectator::resilient::NodeId targetNodeId =
        qnetplay::spectator::resilient::kInvalidNodeId;
    if (!_topologyAuthority->nodeForViewer(target.viewerId, targetNodeId)
        || targetNodeId == qnetplay::spectator::resilient::kInvalidNodeId) {
        error = "the selected target is not currently enrolled for topology";
        return false;
    }
    qnetplay::spectator::resilient::NodeId clientRootNodeId =
        qnetplay::spectator::resilient::kInvalidNodeId;
    if (!_topologyAuthority->assertedClientPlayerRootNode(nowMs,
                                                          clientRootNodeId)) {
        error = "no asserted client-player data root is available";
        return false;
    }
    if (!_topologyQsr->issueClientPlayerRootActivation(clientRootNodeId,
                                                       targetNodeId, nowMs)) {
        error = "the client-root activation could not be issued/sent";
        return false;
    }
    return true;
}

void SpectatorSidecar::rollbackTopologyViewer(const uint64_t viewerId) {
    detachTopologyViewer(viewerId, _lastServiceMs);
}

bool SpectatorSidecar::verifyTopologyViewerGrant(
    const qnetplay::spectator::SpectatorGrant& grant,
    const uint32_t viewerGeneration, const uint64_t nowMs) const {
    if (grant.grantId == 0u || grant.viewerId == 0u
        || viewerGeneration == 0u || grant.sessionId != _config.identity.sessionId
        || grant.sourceGeneration == 0u
        || (grant.route != static_cast<uint8_t>(qnet::SPECTATE_ROUTE_DIRECT)
            && grant.route != static_cast<uint8_t>(
                qnet::SPECTATE_ROUTE_PUNCHED)
            && grant.route != static_cast<uint8_t>(
                qnet::SPECTATE_ROUTE_RELAY))) {
        return false;
    }
    const auto reservation = _reservations.find(grant.viewerId);
    const auto viewer = _viewers.find(grant.viewerId);
    if (reservation == _reservations.end() || viewer == _viewers.end()
        || !reservation->second.bound || reservation->second.revoked
        || reservationExpired(reservation->second, nowMs, nowUnix())
        || !reservationGrantValid(reservation->second, nowUnix())
        || viewer->second.generation != viewerGeneration
        || viewer->second.grantId != grant.grantId) {
        return false;
    }
    const qnetplay::spectator::SpectatorGrant& expected =
        reservation->second.grant;
    return expected.grantId == grant.grantId
        && expected.viewerId == grant.viewerId
        && expected.sessionId == grant.sessionId
        && expected.expiryUnix == grant.expiryUnix
        && expected.sourceGeneration == grant.sourceGeneration
        && expected.resumeRecord == grant.resumeRecord
        && expected.resumeDigest == grant.resumeDigest
        && expected.route == grant.route
        && expected.macLow == grant.macLow
        && expected.macHigh == grant.macHigh;
}

void SpectatorSidecar::detachTopologyViewer(const uint64_t viewerId,
                                            const uint64_t nowMs,
                                            const topology::wire::AuthorityStateReason reason,
                                            const bool emitRevocation) {
    if (!_topologyAuthority) return;
    qnetplay::spectator::resilient::NodeId childId =
        qnetplay::spectator::resilient::kInvalidNodeId;
    if (emitRevocation
        && _topologyAuthority->nodeForViewer(viewerId, childId)
        && _topologyQsr) {
        (void)_topologyQsr->sendAuthorityState(
            topology::wire::AuthorityStateKind::Revocation,
            reason,
            nowMs, childId);
    }
    topology::TopologyAuthority::ViewerLifecycleResult lifecycle{};
    if (_topologyAuthority->detachViewer(viewerId, lifecycle)
        && _resilientRoot) {
        if (lifecycle.requiresGenericRootDetach())
            (void)_resilientRoot->detachChild(lifecycle.nodeId);
        if (_topologyQsr) _topologyQsr->forgetNode(lifecycle.nodeId);
    }
}

bool SpectatorSidecar::refreshTopologyViewerBinding(
    const uint64_t viewerId, const uint32_t viewerGeneration,
    const uint64_t nowMs) {
    if (!_topologyAuthority || !_resilientRoot) return false;
    topology::TopologyViewerProof proof{};
    std::string error;
    topology::TopologyAuthority::ViewerLifecycleResult lifecycle{};
    if (!makeTopologyViewerProof(viewerId, viewerGeneration,
                                 true,
                                 true, proof, error)
        || !_topologyAuthority->refreshViewerBinding(
            proof, nowMs, lifecycle, error)) {
        return false;
    }
    if (lifecycle.requiresGenericRootDetach())
        (void)_resilientRoot->detachChild(lifecycle.nodeId);
    return true;
}

bool SpectatorSidecar::beginTopologyViewerRebind(
    const uint64_t viewerId, const uint32_t viewerGeneration,
    const uint64_t nowMs) {
    if (!_topologyAuthority || !_resilientRoot) return false;
    qnetplay::spectator::resilient::NodeId childId =
        qnetplay::spectator::resilient::kInvalidNodeId;
    const bool hadEnrollment =
        _topologyAuthority->nodeForViewer(viewerId, childId);
    topology::TopologyViewerProof proof{};
    std::string error;
    topology::TopologyAuthority::ViewerLifecycleResult lifecycle{};
    if (!makeTopologyViewerProof(viewerId, viewerGeneration,
                                 true,
                                 false, proof, error)) {
        detachTopologyViewer(viewerId, nowMs);
        return false;
    }
    // The old credential must be notified before beginViewerRebind clears its QSR binding.
    if (hadEnrollment && _topologyQsr) {
        (void)_topologyQsr->sendAuthorityState(
            topology::wire::AuthorityStateKind::Revocation,
            topology::wire::AuthorityStateReason::RouteReplaced,
            nowMs, childId);
    }
    if (!_topologyAuthority->beginViewerRebind(
            proof, nowMs, lifecycle, error)) {
        detachTopologyViewer(viewerId, nowMs,
                             topology::wire::AuthorityStateReason::ViewerDetached,
                             false);
        return false;
    }
    if (lifecycle.requiresGenericRootDetach())
        (void)_resilientRoot->detachChild(lifecycle.nodeId);
    return true;
}

bool SpectatorSidecar::flushPendingRecord(uint64_t postStateProof,
                                          uint64_t nowMs) {
    if (!_hasPending || postStateProof == 0) return false;
    ConfirmedRecord accepted{};
    const ArchiveResult result =
        _archive->appendConfirmed(_gameId, _pending, &accepted);
    if (result != ArchiveResult::Ok
        && result != ArchiveResult::ExactDuplicate) {
        failService(_archive->serviceDisabled()
                        ? "the spectator spool ran out of memory"
                        : "the spectator spool rejected a confirmed record",
                    nowMs);
        return false;
    }
    if (_resilientRoot) {
        const qnetplay::spectator::resilient::CacheAppendResult cached =
            _resilientRoot->acceptArchiveRecord(accepted);
        if (cached != qnetplay::spectator::resilient::CacheAppendResult::Accepted
            && cached != qnetplay::spectator::resilient::CacheAppendResult::ExactDuplicate) {
            _diagnostics.note(
                DiagEvent::RouteLost, nowMs,
                "resilient spectator control stopped after verified cache rejection");
            stopResilientRoot(nowMs);
        } else if (_topologyAuthority && _resilientRoot->cache() != nullptr) {
            _topologyAuthority->setVerifiedCursor(
                _resilientRoot->cache()->frontier());
            if (!_resilientRoot->noteVerifiedProgress(nowMs)) {
                _diagnostics.note(
                    DiagEvent::RouteLost, nowMs,
                    "authenticated spectator topology stopped after progress refresh failure");
                stopResilientRoot(nowMs);
            } else {
                _config.resilientRoot.lastVerifiedProgressMs = nowMs;
            }
        }
    }
    ++_committedRecords;
    _hasPending = false;
    _hasProvenPostTail = true;
    _provenPostTailHash = postStateProof;
    drainHeldSetEvents(nowMs);
    retryPendingCheckpointRef();
    return true;
}

void SpectatorSidecar::applyChainedSetEvent(const PublicationEvent& event,
                                            uint64_t nowMs) {
    if (_sealed) {
        _diagnostics.note(
            DiagEvent::PublicationGap, nowMs,
            "a set event arrived after the match was sealed and was refused");
        return;
    }
    if (_heldSetEvents.size() >= 32u) {
        failService("the spectator set-event queue overflowed", nowMs);
        return;
    }
    _heldSetEvents.push_back(event.setEvent);
    if (!_hasGame || _archive == nullptr) {
        QLOG("[spectator-sidecar] set event %u held at record %u: the match "
             "archive is not open yet (held=%zu)",
             event.setEvent.eventIndex, event.setEvent.recordCursor,
             _heldSetEvents.size());
        return;
    }
    drainHeldSetEvents(nowMs);
}

void SpectatorSidecar::retryPendingCheckpointRef() {
    if (!_hasPendingCheckpointRef || !_archive) return;
    if (_pendingCheckpointRef.recordCursor > _committedRecords) return;
    if (_archive->addCheckpoint(_gameId, _pendingCheckpointRef)
        != qnetplay::spectator::ArchiveResult::Gap) {
        QLOG("[spectator-sidecar] set-seek checkpoint advertised at "
             "cursor=%u after records caught up",
             _pendingCheckpointRef.recordCursor);
        _hasPendingCheckpointRef = false;
    }
}

uint32_t SpectatorSidecar::archivedSetEventCount() const {
    if (_archive == nullptr || !_hasGame) return 0u;
    qnetplay::spectator::ArchiveExtent extent{};
    if (!_archive->extent(_gameId, extent)) return 0u;
    return extent.nextEvent;
}

void SpectatorSidecar::drainHeldSetEvents(uint64_t nowMs) {
    if (!_hasGame) return;
    while (!_heldSetEvents.empty() && _archive != nullptr) {
        const qnetplay::spectator::SetEvent& front = _heldSetEvents.front();
        if (front.recordCursor > _committedRecords) break;
        const qnetplay::spectator::ArchiveResult result =
            _archive->appendEvent(_gameId, front, nullptr);
        if (result != qnetplay::spectator::ArchiveResult::Ok
            && result != qnetplay::spectator::ArchiveResult::ExactDuplicate) {
            char detail[96];
            std::snprintf(detail, sizeof(detail),
                          "set event %u refused at record %u (result %u)",
                          front.eventIndex, front.recordCursor,
                          static_cast<unsigned>(result));
            failService(detail, nowMs);
            return;
        }
        _heldSetEvents.pop_front();
    }
}

void SpectatorSidecar::applyRecord(const PublicationEvent& event,
                                   uint64_t nowMs) {
    if (!_hasGame || _sealed || !_archive) return;
    const uint32_t expected =
        _committedRecords + (_hasPending ? 1u : 0u);
    if (event.record.record != expected) {
        char detail[96];
        std::snprintf(detail, sizeof(detail),
                      "record gap: expected %u, saw %u", expected,
                      event.record.record);
        if (event.record.record > expected)
            _truncatedRecords += event.record.record - expected;
        handleUpstreamLoss(detail, nowMs);
        return;
    }
    if (_hasPending) {
        if (!flushPendingRecord(event.record.stateHash, nowMs)) return;
    }
    _pending = event.record;
    _hasPending = true;
    if (_phase == SidecarPhase::Checkpointing || _phase == SidecarPhase::Paused
        || _phase == SidecarPhase::Waiting) {
        setPhase(SidecarPhase::Active, nowMs);
        _statusCode = SourceStatusCode::Streaming;
    }
}

void SpectatorSidecar::applyCheckpoint(const PublicationEvent& event,
                                       uint64_t nowMs) {
    QLOG("[spectator-sidecar] checkpoint arrived record=%u bytes=%zu "
         "hasGame=%d sealed=%d",
         event.checkpointRecord, event.checkpointBytes.size(),
         _hasGame ? 1 : 0, _sealed ? 1 : 0);
    if (!_hasGame || _sealed) return;
    if (event.checkpointBytes.size() > _config.maxCheckpointBytes) {
        _diagnostics.note(DiagEvent::SpoolFailure, nowMs,
                          "checkpoint exceeds the configured budget");
        return;
    }
    CheckpointEntry entry{};
    entry.checkpointId = event.checkpointId;
    entry.recordCursor = event.checkpointRecord;
    entry.digest = event.checkpointDigest;
    entry.bytes = event.checkpointBytes;
    _checkpointBytes += entry.bytes.size();
    _checkpoints.push_back(std::move(entry));
    while (_checkpoints.size() > _config.maxCheckpoints
           || (_checkpointBytes > _config.maxCheckpointBytes
               && _checkpoints.size() > 1)) {
        _checkpointBytes -= _checkpoints.front().bytes.size();
        _checkpoints.pop_front();
    }
    if (_archive) {
        ArchiveCheckpointRef ref{};
        ref.checkpointId = event.checkpointId;
        ref.recordCursor = event.checkpointRecord;
        ref.digest = event.checkpointDigest;
        const qnetplay::spectator::ArchiveResult advertised =
            _archive->addCheckpoint(_gameId, ref);
        if (advertised == qnetplay::spectator::ArchiveResult::Gap) {
            _pendingCheckpointRef = ref;
            _hasPendingCheckpointRef = true;
        }
        // Read from the deque's stored entry: `entry.bytes` has been moved from.
        const CheckpointEntry& stored = _checkpoints.back();
        if (stored.checkpointId == event.checkpointId
            && _archive->setNewestCheckpointBlob(
                   _gameId, ref.checkpointId, stored.bytes.data(),
                   stored.bytes.size())
                   == qnetplay::spectator::ArchiveResult::Ok) {
            QLOG("[spectator-sidecar] set-seek checkpoint stored id=%llx "
                 "cursor=%u bytes=%zu",
                 static_cast<unsigned long long>(ref.checkpointId),
                 ref.recordCursor, stored.bytes.size());
        }
    }
    setPhase(SidecarPhase::Checkpointing, nowMs);
    _statusCode = SourceStatusCode::Checkpointing;
}

void SpectatorSidecar::applyTailProof(const PublicationEvent& event,
                                      uint64_t nowMs) {
    if (!_hasGame || _sealed || !_archive) return;
    ++_tailProofs;
    if (!event.hasPostTailProof || event.postTailStateHash == 0) return;
    if (!_hasPending) {
        if (event.tailRecordCursor == _committedRecords) {
            _hasProvenPostTail = true;
            _provenPostTailHash = event.postTailStateHash;
        }
        return;
    }
    if (event.tailRecordCursor != _committedRecords + 1u) return;
    if (!flushPendingRecord(event.postTailStateHash, nowMs)) return;
    ++_tailProofCommits;
    if (_phase == SidecarPhase::Waiting) {
        setPhase(SidecarPhase::Active, nowMs);
        _statusCode = SourceStatusCode::Streaming;
    }
}

bool SpectatorSidecar::exactTerminalPrefix(uint32_t recordCursor,
                                           uint64_t prefixDigest) const {
    if (!_hasGame || !_archive || prefixDigest == 0u) return false;
    const uint32_t assembled =
        _committedRecords + (_hasPending ? 1u : 0u);
    if (recordCursor != assembled) return false;

    uint64_t digest = 0u;
    if (!_archive->digestAt(_gameId, _committedRecords, digest))
        return false;
    if (_hasPending) {
        digest = advanceRollingDigest(digest, _gameId, _pending);
        if (_pending.rollingDigest != 0u
            && _pending.rollingDigest != digest) {
            return false;
        }
    }
    return digest == prefixDigest;
}

bool SpectatorSidecar::rememberTerminalRequest(
    TerminalReason reason,
    uint32_t recordCursor,
    uint64_t prefixDigest,
    uint64_t postTailStateHash,
    bool prefixProof,
    bool postTailProof,
    uint64_t nowMs) {
    if (!_terminalRequested) {
        _terminalRequested = true;
        _requestedTerminalReason = reason;
        _requestedTerminalRecordCursor = recordCursor;
        _requestedTerminalPrefixDigest = prefixDigest;
        _requestedPostTailStateHash = postTailStateHash;
        _requestedPrefixProof = prefixProof;
        _requestedPostTailProof = postTailProof;
        return true;
    }

    const bool exactDuplicate =
        _requestedTerminalReason == reason
        && _requestedTerminalRecordCursor == recordCursor
        && _requestedTerminalPrefixDigest == prefixDigest
        && _requestedPrefixProof == prefixProof
        && _requestedPostTailStateHash == postTailStateHash
        && _requestedPostTailProof == postTailProof;
    if (!exactDuplicate) {
        ++_terminalConflicts;
        _diagnostics.note(
            DiagEvent::PublicationGap, nowMs,
            "conflicting terminal ignored; the first terminal is immutable");
    }
    return false;
}

void SpectatorSidecar::applyPublicationFault(const PublicationEvent& event,
                                             uint64_t nowMs) {
    char detail[128];
    if (event.publicationFaultReason != 0u) {
        std::snprintf(detail, sizeof(detail),
                      "publisher fault %u at accepted cursor %u",
                      static_cast<unsigned>(event.publicationFaultReason),
                      event.terminalRecordCursor);
    } else {
        std::snprintf(detail, sizeof(detail),
                      "the publisher reported dropped records");
    }

    if (!rememberTerminalRequest(
            TerminalReason::ProtocolFailure,
            event.terminalRecordCursor,
            event.terminalPrefixDigest,
            event.postTailStateHash,
            event.hasTerminalPrefixProof,
            event.hasPostTailProof,
            nowMs)) {
        return;
    }

    if (event.hasTerminalPrefixProof && event.hasPostTailProof
        && event.postTailStateHash != 0u
        && exactTerminalPrefix(event.terminalRecordCursor,
                               event.terminalPrefixDigest)) {
        if (!_hasPending
            || flushPendingRecord(event.postTailStateHash, nowMs)) {
            _hasProvenPostTail = true;
            _provenPostTailHash = event.postTailStateHash;
        }
    }
    ++_publicationGaps;
    _diagnostics.note(DiagEvent::PublicationGap, nowMs, detail);
    sealTruncated(detail, nowMs);
}

void SpectatorSidecar::handleUpstreamLoss(const char* detail, uint64_t nowMs) {
    ++_publicationGaps;
    _diagnostics.note(DiagEvent::PublicationGap, nowMs, detail);
    sealTruncated(detail, nowMs);
}

void SpectatorSidecar::sealTruncated(const char* reason, uint64_t nowMs) {
    if (_sealed) return;
    if (!_hasGame || !_archive) {
        (void)rememberTerminalRequest(
            TerminalReason::ProtocolFailure, 0u, 0u, 0u,
            false, false, nowMs);
        _sealed = true;
        _sealTruncated = true;
        _hasPostTailProof = false;
        _terminalRecordCursor = 0u;
        _terminalPrefixDigest = 0u;
        _terminalPrefixProven = false;
        _terminalReason = TerminalReason::ProtocolFailure;
        setPhase(SidecarPhase::Sealed, nowMs);
        _statusCode = SourceStatusCode::ProtocolOrArchiveFailure;
        _drainDeadlineMs = nowMs + _config.drainDeadlineMs;
        _retentionUntilMs = nowMs + _config.retentionMs;
        _failureReason = reason ? reason : "the recording was truncated";
        return;
    }

    uint64_t proof = 0;
    bool proven = false;
    if (_hasPending) {
        proof = _pending.stateHash;
        proven = true;
        _hasPending = false;
        ++_truncatedRecords;
    } else if (_hasProvenPostTail) {
        proof = _provenPostTailHash;
        proven = true;
    } else if (_committedRecords == 0) {
        proof = _descriptor.initialStateHash;
        proven = true;
    }

    uint64_t prefixDigest = 0u;
    const bool hasPrefix =
        _archive->digestAt(_gameId, _committedRecords, prefixDigest)
        && prefixDigest != 0u;
    (void)rememberTerminalRequest(
        TerminalReason::ProtocolFailure, _committedRecords, prefixDigest,
        proof, hasPrefix, proven, nowMs);

    const ArchiveResult result =
        _archive->sealGame(_gameId, TerminalReason::ProtocolFailure, proof);
    if (result != ArchiveResult::Ok
        && result != ArchiveResult::ExactDuplicate) {
        _statusCode = SourceStatusCode::TerminalProofMismatch;
    }
    _sealed = true;
    _sealTruncated = true;
    _hasPostTailProof = proven;
    _postTailStateHash = proof;
    _terminalRecordCursor = _committedRecords;
    _terminalPrefixDigest = hasPrefix ? prefixDigest : 0u;
    _terminalPrefixProven = hasPrefix;
    _terminalReason = TerminalReason::ProtocolFailure;
    setPhase(SidecarPhase::Sealed, nowMs);
    if (_statusCode != SourceStatusCode::TerminalProofMismatch)
        _statusCode = SourceStatusCode::ProtocolOrArchiveFailure;
    _drainDeadlineMs = nowMs + _config.drainDeadlineMs;
    _retentionUntilMs = nowMs + _config.retentionMs;
    if (_failureReason.empty())
        _failureReason = std::string("spectator recording truncated: ")
                       + (reason ? reason : "upstream publication stopped");
}

void SpectatorSidecar::requestSeal(TerminalReason reason,
                                   uint32_t authoritativeRecordCursor,
                                   uint64_t authoritativePrefixDigest,
                                   uint64_t postTailStateHash,
                                   bool hasPrefixProof,
                                   bool hasPostTailProof,
                                   uint64_t nowMs) {
    if (_policy == AdmissionPolicy::Disabled) return;
    if (!rememberTerminalRequest(
            reason, authoritativeRecordCursor,
            authoritativePrefixDigest, postTailStateHash,
            hasPrefixProof, hasPostTailProof, nowMs)) {
        return;
    }
    if (_sealed) return;

    if (!hasPrefixProof || !hasPostTailProof
        || authoritativePrefixDigest == 0u
        || postTailStateHash == 0u) {
        sealTruncated(
            "the game stopped without a complete cursor/digest/tail proof",
            nowMs);
        return;
    }
    if (!_hasGame || !_archive) {
        sealTruncated("the game sealed before publishing a match descriptor",
                      nowMs);
        return;
    }
    if (!exactTerminalPrefix(authoritativeRecordCursor,
                             authoritativePrefixDigest)) {
        sealTruncated(
            "the terminal cursor/digest did not match the assembled prefix",
            nowMs);
        return;
    }
    if (authoritativeRecordCursor == 0u
        && postTailStateHash != _descriptor.initialStateHash) {
        sealTruncated(
            "the empty-prefix terminal did not prove the initial state",
            nowMs);
        return;
    }
    if (_hasPending
        && !flushPendingRecord(postTailStateHash, nowMs)) {
        return;
    }

    uint64_t storedDigest = 0u;
    if (!_archive->digestAt(
            _gameId, authoritativeRecordCursor, storedDigest)
        || storedDigest != authoritativePrefixDigest) {
        sealTruncated(
            "the terminal prefix changed while committing its held record",
            nowMs);
        return;
    }

    _hasProvenPostTail = true;
    _provenPostTailHash = postTailStateHash;
    const ArchiveResult sealResult =
        _archive->sealGame(_gameId, reason, postTailStateHash);
    if (sealResult != ArchiveResult::Ok
        && sealResult != ArchiveResult::ExactDuplicate) {
        _statusCode = SourceStatusCode::TerminalProofMismatch;
        _failureReason = "the authoritative terminal proof was rejected";
        _diagnostics.note(DiagEvent::SpoolFailure, nowMs,
                          _failureReason.c_str());
        sealTruncated(_failureReason.c_str(), nowMs);
        return;
    }

    _statusCode = reason == TerminalReason::MatchEnded
                      ? SourceStatusCode::SealedDraining
                      : SourceStatusCode::PeerQuitSourceStopped;
    _sealed = true;
    _sealTruncated = false;
    _hasPostTailProof = true;
    _postTailStateHash = postTailStateHash;
    _terminalReason = reason;
    _terminalRecordCursor = authoritativeRecordCursor;
    _terminalPrefixDigest = authoritativePrefixDigest;
    _terminalPrefixProven = true;
    setPhase(SidecarPhase::Sealed, nowMs);
    _drainDeadlineMs = nowMs + _config.drainDeadlineMs;
    _retentionUntilMs = nowMs + _config.retentionMs;
}

void SpectatorSidecar::requestSeal(TerminalReason reason,
                                   uint64_t postTailStateHash,
                                   bool hasProof,
                                   uint64_t nowMs) {
    requestSeal(reason, 0u, 0u, postTailStateHash,
                false, hasProof, nowMs);
}

uint64_t SpectatorSidecar::endpointToken(const RouteEndpoint& endpoint) {
    return mix64((static_cast<uint64_t>(endpoint.ipv4BE) << 16)
                 | static_cast<uint64_t>(endpoint.port));
}

SpectatorSidecar::QuotaAddress SpectatorSidecar::directQuotaAddress(
    uint32_t ipv4BE) {
    QuotaAddress address{};
    address.value = ipv4BE;
    return address;
}

SpectatorSidecar::QuotaAddress SpectatorSidecar::ingressQuotaAddress(
    const RouteEndpoint& endpoint) const {
    if (!isRelayIngress(endpoint))
        return directQuotaAddress(endpoint.ipv4BE);

    uint64_t viewerId = 0;
    if (_transport
        && _transport->relayViewerIdentity(endpoint, viewerId)
        && viewerId != 0) {
        QuotaAddress address{};
        address.relay = true;
        address.value = viewerId;
        return address;
    }

    QuotaAddress unknownRelay{};
    unknownRelay.relay = true;
    return unknownRelay;
}

SpectatorSidecar::AddressState& SpectatorSidecar::addressState(
    const QuotaAddress& quotaAddress,
    uint64_t nowMs) {
    auto found = _addresses.find(quotaAddress);
    if (found != _addresses.end()) {
        found->second.lastSeenMs = nowMs;
        return found->second;
    }
    const size_t limit = _config.rateLimits.maxTrackedAddresses;
    if (_addresses.size() >= limit)
        pruneAddresses(nowMs);
    if (_addresses.size() >= limit) {
        ++_overflowAddressPackets;
        _overflowAddress.lastSeenMs = nowMs;
        return _overflowAddress;
    }
    AddressState& state = _addresses[quotaAddress];
    state.packets.configure(_config.rateLimits.perIpPacketsPerSecond,
                            _config.rateLimits.perIpBurstPackets, nowMs);
    state.bytes.configure(
        static_cast<double>(_config.rateLimits.perIpBytesPerSecond),
        static_cast<double>(_config.rateLimits.perIpBurstBytes), nowMs);
    for (const auto& viewer : _viewers) {
        if (viewer.second.ingressQuotaAddress == quotaAddress)
            ++state.viewers;
    }
    state.lastSeenMs = nowMs;
    return state;
}

void SpectatorSidecar::pruneAddresses(uint64_t nowMs) {
    for (auto it = _addresses.begin(); it != _addresses.end();) {
        const bool idle = it->second.viewers == 0
                       && nowMs >= it->second.lastSeenMs
                       && nowMs - it->second.lastSeenMs > 60000
                       && nowMs >= it->second.cooldownUntilMs;
        it = idle ? _addresses.erase(it) : std::next(it);
    }
}

const char* spectatorIngressResultName(
    const qnetplay::spectator::IngressResult result) {
    using R = qnetplay::spectator::IngressResult;
    switch (result) {
        case R::StaleAck:            return "stale-ack";
        case R::FutureAck:           return "future-ack";
        case R::AckDigestMismatch:   return "ack-digest-mismatch";
        case R::ReorderOverflow:     return "reorder-overflow";
        case R::ConflictingRecord:   return "conflicting-record";
        case R::RecordDigestMismatch:return "record-digest-mismatch";
        case R::TimelineMismatch:    return "timeline-mismatch";
        case R::ResumeMismatch:      return "resume-mismatch";
        case R::UnknownViewer:       return "unknown-viewer";
        case R::WrongGame:           return "wrong-game";
        case R::GameUnavailable:     return "game-unavailable";
        case R::CapacityReached:     return "capacity-reached";
        case R::UnexpectedMessage:   return "unexpected-message";
        case R::ContractMismatch:    return "contract-mismatch";
        case R::StateHashMismatch:   return "state-hash-mismatch";
        case R::TerminalMismatch:    return "terminal-mismatch";
        case R::PrematureTerminal:   return "premature-terminal";
        case R::InitMismatch:        return "init-mismatch";
        case R::CookieMismatch:      return "cookie-mismatch";
        case R::SessionMismatch:     return "session-mismatch";
        case R::EpochMismatch:       return "epoch-mismatch";
        case R::BadCrc:              return "bad-crc";
        case R::BadPayload:          return "bad-payload";
        default:                     return "malformed";
    }
}

IngressOutcome SpectatorSidecar::onDatagram(const RouteEndpoint& from,
                                            const uint8_t* bytes,
                                            size_t length,
                                            uint64_t nowMs) {
    ++_ingressPackets;
    _ingressBytes += length;

    if (_clientPrivateRootDormant || _policy == AdmissionPolicy::Disabled
        || _serviceFailed
        || _phase == SidecarPhase::Idle || _phase == SidecarPhase::Expired) {
        return IngressOutcome::ServiceUnavailable;
    }

    if (!_globalPackets.take(1.0, nowMs)
        || !_globalBytes.take(static_cast<double>(length), nowMs)) {
        ++_ingressRateLimited;
        _diagnostics.note(DiagEvent::RateLimited, nowMs, "global ingress");
        return IngressOutcome::RateLimitedGlobal;
    }
    const QuotaAddress datagramQuotaAddress =
        ingressQuotaAddress(from);
    AddressState& address =
        addressState(datagramQuotaAddress, nowMs);
    if (nowMs < address.cooldownUntilMs) {
        ++_ingressRateLimited;
        _diagnostics.note(DiagEvent::RateLimited, nowMs, "address cooldown");
        return IngressOutcome::AddressCooldown;
    }
    if (!address.packets.take(1.0, nowMs)
        || !address.bytes.take(static_cast<double>(length), nowMs)) {
        ++_ingressRateLimited;
        if (++address.violations
            >= _config.rateLimits.perIpViolationsBeforeCooldown) {
            address.violations = 0;
            address.cooldownUntilMs = nowMs + _config.rateLimits.perIpCooldownMs;
        }
        _diagnostics.note(DiagEvent::RateLimited, nowMs, "per-address ingress");
        return IngressOutcome::RateLimitedAddress;
    }

    const uint64_t token = endpointToken(from);
    ViewerRecord* viewer = nullptr;
    {
        auto mapped = _tokenToViewerId.find(token);
        if (mapped != _tokenToViewerId.end()) {
            auto found = _viewers.find(mapped->second);
            if (found != _viewers.end()) viewer = &found->second;
            else _tokenToViewerId.erase(mapped);
        }
    }
    if (viewer && !viewer->grantPackets.take(1.0, nowMs)) {
        ++_ingressRateLimited;
        _diagnostics.note(DiagEvent::RateLimited, nowMs, "per-grant ingress");
        return IngressOutcome::RateLimitedGrant;
    }

    if (qnetplay::spectator::resilient::wire::hasResilientControlMagic(
            bytes, length)
        || topology::hasTopologyControlEnvelopeMagic(bytes, length)) {
        ++_ingressDecoded;
        ++_ingressRejected;
        _diagnostics.note(DiagEvent::MalformedDatagram, nowMs,
                          "topology control arrived on the QSP socket");
        return IngressOutcome::ProtocolRejected;
    }

    if (bytes == nullptr || length < kSpectatorHeaderSize
        || length > kMaxSpectatorDatagramSize) {
        ++_ingressRejected;
        _diagnostics.note(DiagEvent::MalformedDatagram, nowMs, "bad size");
        return IngressOutcome::Malformed;
    }

    ++_ingressDecoded;
    Datagram datagram{};
    if (decodeDatagram(bytes, length, _config.identity, datagram)
        != DecodeError::None) {
        ++_ingressRejected;
        _diagnostics.note(DiagEvent::MalformedDatagram, nowMs, "decode failed");
        return IngressOutcome::Malformed;
    }
    if (_endpointsByToken.size() > 512) {
        for (auto it = _endpointsByToken.begin();
             it != _endpointsByToken.end();) {
            it = _tokenToViewerId.count(it->first)
                     ? std::next(it)
                     : _endpointsByToken.erase(it);
        }
    }
    _endpointsByToken[token] = from;

    bool helloAdmitted = false;
    uint64_t helloViewerId = 0;
    ViewerRecord* helloKnown = nullptr;
    bool helloSameEndpoint = false;
    bool provisionalViewer = false;
    bool migrateCandidate = false;
    MigratePayload migrate{};

    if (datagram.header.type == MessageType::Hello) {
        HelloPayload hello{};
        if (!decodePayload(datagram.payload, hello)) {
            ++_ingressRejected;
            return IngressOutcome::Malformed;
        }
        const bool logHello = helloLogDue(hello.viewerId, nowMs);
        if (logHello) {
            QLOG("[spectator-sidecar] hello on wire viewer=%016llX from=%s:%u "
                 "flags=%u gen=%u resume=%u/%u known=%d viewers=%zu",
                 static_cast<unsigned long long>(hello.viewerId),
                 qnet::formatIpv4BE(from.ipv4BE).c_str(),
                 static_cast<unsigned>(from.port),
                 hello.flags, hello.generation,
                 hello.resumeRecord, hello.resumeEvent,
                 _viewers.find(hello.viewerId) != _viewers.end() ? 1 : 0,
                 _viewers.size());
        }
        if ((hello.flags
             & qnetplay::spectator::kHelloFlagWantsNewestCheckpoint) != 0
            && _source) {
            (void)_source->setViewerWantsNewestCheckpoint(hello.viewerId);
        }
        auto known = _viewers.find(hello.viewerId);
        if (known == _viewers.end()) {
            AdmissionDenyReason deny = AdmissionDenyReason::None;
            const AdmissionDecision decision =
                admit(from, hello, nowMs, deny);
            if (decision == AdmissionDecision::Deny) {
                ++_admissionRejections;
                QLOG("[spectator-sidecar] hello DENIED viewer=%016llX "
                     "from=%s:%u reason=%s (rejections=%zu viewers=%zu)",
                     static_cast<unsigned long long>(hello.viewerId),
                     qnet::formatIpv4BE(from.ipv4BE).c_str(),
                     static_cast<unsigned>(from.port),
                     admissionDenyText(deny), _admissionRejections,
                     _viewers.size());
                if (deny == AdmissionDenyReason::CapacityFull) {
                    _diagnostics.note(DiagEvent::CapacityRejected, nowMs,
                                      admissionDenyText(deny));
                    _statusCode = SourceStatusCode::CapacityRejection;
                } else {
                    _diagnostics.note(DiagEvent::AdmissionDenied, nowMs,
                                      admissionDenyText(deny));
                }
                sendReject(from, hello.viewerId, hello.generation,
                           rejectForDeny(deny), nowMs,
                           hello.requestedGameId);
                return IngressOutcome::AdmissionDenied;
            }
            if (decision == AdmissionDecision::Pending) {
                QLOG("[spectator-sidecar] hello PENDING host approval "
                     "viewer=%016llX from=%s:%u",
                     static_cast<unsigned long long>(hello.viewerId),
                     qnet::formatIpv4BE(from.ipv4BE).c_str(),
                     static_cast<unsigned>(from.port));
                sendHeartbeat(from, nowMs);
                return IngressOutcome::Accepted;
            }
            ViewerRecord record{};
            record.viewerId = hello.viewerId;
            record.endpointToken = token;
            record.generation = hello.generation;
            record.endpoint = from;
            record.ingressQuotaAddress = datagramQuotaAddress;
            auto reservation = _reservations.find(hello.viewerId);
            record.capacityQuotaAddress =
                reservation != _reservations.end()
                && reservation->second.peerIpv4BE != 0
                    ? directQuotaAddress(
                          reservation->second.peerIpv4BE)
                    : datagramQuotaAddress;
            record.state = ViewerServiceState::Handshaking;
            record.bootstrapRecord = hello.resumeRecord;
            record.admittedAtMs = nowMs;
            record.firstAdmittedAtMs = nowMs;
            record.lastProgressMs = nowMs;
            record.grantPackets.configure(
                _config.rateLimits.perGrantPacketsPerSecond,
                _config.rateLimits.perGrantBurstPackets, nowMs);
            if (reservation != _reservations.end()) {
                record.grantId = reservation->second.grant.grantId;
                record.routeLabel = reservation->second.grant.route;
            }
            _viewers[hello.viewerId] = record;
            if (logHello) {
                QLOG("[spectator-sidecar] hello ADMITTED viewer=%016llX "
                     "from=%s:%u route=%u gen=%u resume=%u grant=%d phase=%s "
                     "committed=%u viewers=%zu",
                     static_cast<unsigned long long>(hello.viewerId),
                     qnet::formatIpv4BE(from.ipv4BE).c_str(),
                     static_cast<unsigned>(from.port),
                     static_cast<unsigned>(record.routeLabel),
                     hello.generation, hello.resumeRecord,
                     record.grantId != 0 ? 1 : 0, sidecarPhaseName(_phase),
                     _committedRecords, _viewers.size());
            }
            _tokenToViewerId[token] = hello.viewerId;
            if (&address != &_overflowAddress) ++address.viewers;
            viewer = &_viewers[hello.viewerId];
            provisionalViewer = true;
        } else if (hello.generation < known->second.generation) {
            _diagnostics.note(DiagEvent::UnknownViewer, nowMs,
                              "stale handshake generation ignored");
            return IngressOutcome::ProtocolRejected;
        } else {
            helloKnown = &known->second;
            helloSameEndpoint = known->second.endpointToken == token;
            viewer = &known->second;
        }
        helloAdmitted = true;
        helloViewerId = hello.viewerId;
    } else if (datagram.header.type == MessageType::Migrate) {
        if (!decodePayload(datagram.payload, migrate)) {
            ++_ingressRejected;
            return IngressOutcome::Malformed;
        }
        auto known = _viewers.find(migrate.viewerId);
        if (known != _viewers.end()) {
            if (viewer != &known->second
                && !known->second.grantPackets.take(1.0, nowMs)) {
                ++_ingressRateLimited;
                _diagnostics.note(DiagEvent::RateLimited, nowMs,
                                  "per-grant migration ingress");
                return IngressOutcome::RateLimitedGrant;
            }
            viewer = &known->second;
            migrateCandidate = true;
        }
    } else if (datagram.header.type == MessageType::Detach) {
        DetachPayload detach{};
        if (!decodePayload(datagram.payload, detach)) {
            ++_ingressRejected;
            return IngressOutcome::Malformed;
        }
        if (_viewers.find(detach.viewerId) == _viewers.end()) {
            if (consumeUnboundReservationFromDetach(
                    from, detach, nowMs)) {
                return IngressOutcome::Accepted;
            }
            ++_ingressRejected;
            return IngressOutcome::ProtocolRejected;
        }
    }

    if (!_source) return IngressOutcome::ServiceUnavailable;
    const IngressResult result =
        _source->onDatagram(token, bytes, length, nowMs);
    SourceViewerSnapshot admittedSnapshot{};
    const bool sourceAttached =
        provisionalViewer
        && result == IngressResult::Accepted
        && _source->viewerSnapshot(
            helloViewerId, admittedSnapshot)
        && admittedSnapshot.phase != SourceViewerPhase::Retired;
    if (provisionalViewer && !sourceAttached) {
        if (result != IngressResult::Accepted) {
            auto reservation = _reservations.find(helloViewerId);
            if (reservation != _reservations.end()) {
                reservation->second.consumed = true;
                reservation->second.bound = false;
                reservation->second.capacityHeldUntilMs = 0;
            }
        }
        removeProvisionalViewer(helloViewerId);
        viewer = nullptr;
    } else if (sourceAttached) {
        auto reservation = _reservations.find(helloViewerId);
        if (reservation != _reservations.end()) {
            reservation->second.consumed = true;
            reservation->second.bound = true;
            reservation->second.capacityHeldUntilMs = 0;
        }
    }
    switch (result) {
    case IngressResult::Accepted:
        if (helloKnown && helloSameEndpoint) {
            HelloPayload accepted{};
            if (decodePayload(datagram.payload, accepted)) {
                const bool newer =
                    accepted.generation > helloKnown->generation;
                QLOG("[spectator-sidecar] hello %s viewer=%016llX from=%s:%u "
                     "gen=%u->%u resume=%u acked=%u committed=%u",
                     newer ? "RESUMED" : "retransmit accepted",
                     static_cast<unsigned long long>(helloKnown->viewerId),
                     qnet::formatIpv4BE(from.ipv4BE).c_str(),
                     static_cast<unsigned>(from.port),
                     helloKnown->generation, accepted.generation,
                     accepted.resumeRecord, helloKnown->lastAckedRecord,
                     _committedRecords);
                helloKnown->generation = accepted.generation;
                if (newer) {
                    helloKnown->bootstrapRecord = accepted.resumeRecord;
                    helloKnown->admittedAtMs = nowMs;
                    helloKnown->firstAdmittedAtMs = nowMs;
                    helloKnown->lastProgressMs = nowMs;
                    (void)beginTopologyViewerRebind(
                        helloKnown->viewerId, accepted.generation, nowMs);
                } else if (helloKnown->state == ViewerServiceState::Handshaking
                           && nowMs >= helloKnown->firstAdmittedAtMs
                           && nowMs - helloKnown->firstAdmittedAtMs
                                  <= kHandshakeRefreshCapMs) {
                    helloKnown->admittedAtMs = nowMs;
                }
            }
        }
        if (migrateCandidate) {
            auto known = _viewers.find(migrate.viewerId);
            if (known != _viewers.end()
                && known->second.endpointToken != token) {
                _tokenToViewerId.erase(known->second.endpointToken);
                auto previous =
                    _addresses.find(
                        known->second.ingressQuotaAddress);
                if (previous != _addresses.end()
                    && previous->second.viewers > 0) {
                    --previous->second.viewers;
                }
                known->second.endpointToken = token;
                known->second.endpoint = from;
                known->second.ingressQuotaAddress =
                    datagramQuotaAddress;
                known->second.generation = migrate.newGeneration;
                known->second.bootstrapRecord = migrate.resumeRecord;
                known->second.admittedAtMs = nowMs;
                known->second.firstAdmittedAtMs = nowMs;
                known->second.lastProgressMs = nowMs;
                _tokenToViewerId[token] = known->second.viewerId;
                if (&address != &_overflowAddress) ++address.viewers;
                viewer = &known->second;
                (void)beginTopologyViewerRebind(
                    viewer->viewerId, viewer->generation, nowMs);
            }
        }
        if (helloAdmitted) {
            SourceViewerSnapshot snapshot{};
            if (!_source->viewerSnapshot(helloViewerId, snapshot))
                sendHeartbeat(from, nowMs);
        }
        if (datagram.header.type == MessageType::InitAck && viewer != nullptr)
            (void)refreshTopologyViewerBinding(
                viewer->viewerId, viewer->generation, nowMs);
        return IngressOutcome::Accepted;
    case IngressResult::Detached:
        if (viewer) retireViewer(viewer->viewerId,
                                 ViewerServiceState::Detached, nowMs);
        return IngressOutcome::Accepted;
    case IngressResult::StaleGeneration:
    case IngressResult::ReplayedSequence:
        return IngressOutcome::ProtocolRejected;
    default:
        break;
    }
    ++_ingressRejected;
    char detail[96];
    std::snprintf(detail, sizeof(detail),
                  "source rejected the message (%s)",
                  spectatorIngressResultName(result));
    _diagnostics.note(DiagEvent::MalformedDatagram, nowMs, detail);
    return IngressOutcome::ProtocolRejected;
}

bool SpectatorSidecar::reservationExpired(
    const Reservation& reservation,
    uint64_t nowMs,
    uint64_t unixNow) const {
    const bool wallExpired =
        reservation.grant.expiryUnix != 0
        && unixNow > reservation.grant.expiryUnix;
    const bool monotonicExpired =
        reservation.expiresAtMs != 0
        && nowMs > reservation.expiresAtMs;
    return wallExpired || monotonicExpired;
}

bool SpectatorSidecar::reservationHoldsCapacity(
    const Reservation& reservation,
    uint64_t nowMs,
    uint64_t unixNow) const {
    return !reservation.bound
        && !reservation.consumed
        && !reservation.declined
        && !reservation.revoked
        && !reservationExpired(reservation, nowMs, unixNow)
        && reservation.capacityHeldUntilMs != 0
        && nowMs <= reservation.capacityHeldUntilMs;
}

bool SpectatorSidecar::reservationGrantValid(
    const Reservation& reservation,
    uint64_t unixNow) const {
    if (reservation.grant.grantId == 0) return false;
    const bool relayGrant =
        reservation.grant.route == static_cast<uint8_t>(
            qnet::relay::SpectatorRouteKind::Relay);
    return relayGrant
        ? (_transport
           && _transport->verifyRelayGrant(reservation.grant, unixNow))
        : (!_config.verifyGrant
           || _config.verifyGrant(reservation.grant, unixNow));
}

void SpectatorSidecar::sweepReservations(uint64_t nowMs) {
    const uint64_t unixNow = nowUnix();
    for (auto it = _reservations.begin(); it != _reservations.end();) {
        Reservation& reservation = it->second;
        if (reservation.bound
            && _viewers.find(it->first) == _viewers.end()) {
            reservation.bound = false;
        }
        const bool stalePending =
            !reservation.bound
            && !reservation.consumed
            && !reservation.approved
            && !reservation.declined
            && !reservation.revoked
            && reservation.capacityHeldUntilMs != 0
            && nowMs > reservation.capacityHeldUntilMs;
        const bool expired =
            !reservation.bound
            && reservationExpired(reservation, nowMs, unixNow);
        if (stalePending || expired)
            it = _reservations.erase(it);
        else
            ++it;
    }
}

bool SpectatorSidecar::consumeUnboundReservationFromDetach(
    const RouteEndpoint& from,
    const DetachPayload& detach,
    uint64_t nowMs) {
    auto found = _reservations.find(detach.viewerId);
    if (found == _reservations.end()) return false;
    Reservation& reservation = found->second;
    const uint64_t unixNow = nowUnix();
    if (reservation.bound || reservation.consumed
        || reservation.declined || reservation.revoked
        || reservationExpired(reservation, nowMs, unixNow)
        || !reservationGrantValid(reservation, unixNow)
        || detach.generation == 0
        || detach.nextRecord != reservation.grant.resumeRecord
        || detach.digest != reservation.grant.resumeDigest
        || detach.nextEvent != 0) {
        return false;
    }
    const bool relayGrant =
        reservation.grant.route == static_cast<uint8_t>(
            qnet::relay::SpectatorRouteKind::Relay);
    if (relayGrant != isRelayIngress(from)) return false;
    if (!relayGrant
        && reservation.peerIpv4BE != 0
        && reservation.peerIpv4BE != from.ipv4BE) {
        return false;
    }

    reservation.consumed = true;
    reservation.bound = false;
    reservation.capacityHeldUntilMs = 0;
    return true;
}

AdmissionDecision SpectatorSidecar::admit(
    const RouteEndpoint& from,
    const HelloPayload& hello,
    uint64_t nowMs,
    AdmissionDenyReason& denyReason) {
    const uint64_t viewerId = hello.viewerId;
    denyReason = AdmissionDenyReason::None;
    if (_serviceFailed) {
        denyReason = AdmissionDenyReason::ServiceFailed;
        return AdmissionDecision::Deny;
    }
    if (_phase == SidecarPhase::Expired) {
        denyReason = AdmissionDenyReason::ServiceExpired;
        return AdmissionDecision::Deny;
    }
    if (_phase == SidecarPhase::Sealed || _phase == SidecarPhase::Draining) {
        denyReason = AdmissionDenyReason::ServiceExpired;
        return AdmissionDecision::Deny;
    }
    if (_policy == AdmissionPolicy::Disabled) {
        denyReason = AdmissionDenyReason::PolicyDisabled;
        return AdmissionDecision::Deny;
    }
    if (_clientPrivateRootDormant) {
        denyReason = AdmissionDenyReason::PolicyDisabled;
        return AdmissionDecision::Deny;
    }
    if (_clientRoot.servingActive
        && (!_clientRoot.activationRetained || !_clientRoot.committed
            || viewerId
                != _clientRoot.activation.targetLease.childBinding
                       .childViewerId)) {
        denyReason = AdmissionDenyReason::GrantInvalid;
        return AdmissionDecision::Deny;
    }
    if (viewerId == 0) {
        denyReason = AdmissionDenyReason::GrantInvalid;
        return AdmissionDecision::Deny;
    }

    auto reservation = _reservations.find(viewerId);
    if (reservation != _reservations.end()) {
        Reservation& held = reservation->second;
        if (held.revoked) {
            denyReason = AdmissionDenyReason::GrantRevoked;
            return AdmissionDecision::Deny;
        }
        if (held.declined) {
            denyReason = AdmissionDenyReason::HostDeclined;
            return AdmissionDecision::Deny;
        }
        const uint64_t unixNow = nowUnix();
        if (reservationExpired(held, nowMs, unixNow)) {
            denyReason = AdmissionDenyReason::GrantExpired;
            return AdmissionDecision::Deny;
        }
        const bool reattaching =
            held.reattachUntilMs != 0 && nowMs < held.reattachUntilMs;
        if (held.consumed && !reattaching) {
            denyReason = AdmissionDenyReason::GrantInvalid;
            return AdmissionDecision::Deny;
        }
        const bool relayGrant =
            held.grant.route == static_cast<uint8_t>(
                qnet::relay::SpectatorRouteKind::Relay);
        const bool resumeMatchesGrant =
            hello.resumeRecord == held.grant.resumeRecord
            && hello.resumeDigest == held.grant.resumeDigest;
        const bool resumeVerifiedForReattach =
            reattaching && !resumeMatchesGrant
            && hello.resumeRecord >= held.grant.resumeRecord
            && resumePointIsOnOurChain(hello.resumeRecord, hello.resumeEvent,
                                       hello.resumeDigest);
        if (relayGrant != isRelayIngress(from)
            || (!resumeMatchesGrant && !resumeVerifiedForReattach)
            || !reservationGrantValid(held, unixNow)) {
            denyReason = AdmissionDenyReason::GrantInvalid;
            return AdmissionDecision::Deny;
        }
        if (reattaching) {
            QLOG("[spectator-sidecar] re-attach accepted viewer=%s "
                 "resume=%u/%s (grant was issued at %u): the pass this source "
                 "retired is redeemed once more",
                 qclient::spectator::redactedTag(viewerId).c_str(),
                 static_cast<unsigned>(hello.resumeRecord),
                 resumeMatchesGrant ? "grant" : "verified-forward",
                 static_cast<unsigned>(held.grant.resumeRecord));
            held.reattachUntilMs = 0;
            held.consumed = false;
        }
    } else if (isRelayIngress(from)
               && _policy != AdmissionPolicy::Public) {
        denyReason = AdmissionDenyReason::GrantRequired;
        return AdmissionDecision::Deny;
    }

    const uint64_t unixNow = nowUnix();
    size_t reservedOther = 0;
    for (const auto& entry : _reservations) {
        if (entry.first != viewerId
            && reservationHoldsCapacity(entry.second, nowMs, unixNow)) {
            ++reservedOther;
        }
    }
    if (_viewers.size() + reservedOther >= _maxViewers) {
        denyReason = AdmissionDenyReason::CapacityFull;
        return AdmissionDecision::Deny;
    }

    const QuotaAddress quotaAddress =
        reservation != _reservations.end()
        && reservation->second.peerIpv4BE != 0
            ? directQuotaAddress(reservation->second.peerIpv4BE)
            : ingressQuotaAddress(from);
    std::set<uint64_t> sameAddressViewers;
    for (const auto& entry : _viewers) {
        if (entry.second.capacityQuotaAddress == quotaAddress)
            sameAddressViewers.insert(entry.first);
    }
    for (const auto& entry : _reservations) {
        if (entry.second.peerIpv4BE != 0
            && directQuotaAddress(entry.second.peerIpv4BE)
                   == quotaAddress
            && (entry.second.bound
                || reservationHoldsCapacity(
                    entry.second, nowMs, unixNow))) {
            sameAddressViewers.insert(entry.first);
        }
    }
    sameAddressViewers.insert(viewerId);
    if (sameAddressViewers.size()
        > _config.rateLimits.maxViewersPerIp) {
        denyReason = AdmissionDenyReason::PerIpQuota;
        return AdmissionDecision::Deny;
    }

    if (reservation != _reservations.end()) {
        if (!reservation->second.approved)
            return AdmissionDecision::Pending;
        return AdmissionDecision::Admit;
    }

    if (_clientRoot.servingActive && _clientRoot.activationRetained
        && _clientRoot.committed
        && viewerId == _clientRoot.activation.targetLease.childBinding
                           .childViewerId
        && hello.resumeRecord
            == _clientRoot.activation.targetLease.childBinding
                   .childRequestedCursor.nextRecord
        && hello.resumeDigest
            == _clientRoot.activation.targetLease.childBinding
                   .childRequestedCursor.digest) {
        return AdmissionDecision::Admit;
    }

    switch (_policy) {
    case AdmissionPolicy::Public:
        return AdmissionDecision::Admit;
    case AdmissionPolicy::CodeOnly:
        denyReason = AdmissionDenyReason::GrantRequired;
        return AdmissionDecision::Deny;
    case AdmissionPolicy::AskHost:
        denyReason = AdmissionDenyReason::GrantRequired;
        return AdmissionDecision::Deny;
    case AdmissionPolicy::Disabled:
        break;
    }
    denyReason = AdmissionDenyReason::PolicyDisabled;
    return AdmissionDecision::Deny;
}

bool SpectatorSidecar::issueGrant(const GrantRequest& request,
                                  SpectatorGrant& out,
                                  AdmissionDenyReason& denyReason,
                                  uint64_t nowMs) {
    denyReason = AdmissionDenyReason::None;
    sweepReservations(nowMs);
    if (_serviceFailed) {
        denyReason = AdmissionDenyReason::ServiceFailed;
        return false;
    }
    if (_policy == AdmissionPolicy::Disabled) {
        denyReason = AdmissionDenyReason::PolicyDisabled;
        return false;
    }
    if (_clientPrivateRootDormant) {
        denyReason = AdmissionDenyReason::PolicyDisabled;
        return false;
    }
    if (_clientRoot.servingActive
        && (!_clientRoot.activationRetained || !_clientRoot.committed
            || request.viewerId
                != _clientRoot.activation.targetLease.childBinding
                       .childViewerId)) {
        denyReason = AdmissionDenyReason::GrantInvalid;
        return false;
    }
    if (_phase == SidecarPhase::Expired
        || _phase == SidecarPhase::Sealed
        || _phase == SidecarPhase::Draining) {
        denyReason = AdmissionDenyReason::ServiceExpired;
        return false;
    }

    const uint64_t unixNow = nowUnix();
    auto existing = request.viewerId == 0
        ? _reservations.end()
        : _reservations.find(request.viewerId);
    const bool replacing =
        existing != _reservations.end()
        && existing->second.grant.route != request.route;
    if (existing != _reservations.end() && !replacing) {
        const Reservation& held = existing->second;
        const bool exactRetry =
            !held.bound && !held.consumed && !held.declined && !held.revoked
            && !reservationExpired(held, nowMs, unixNow)
            && held.grant.resumeRecord == request.resumeRecord
            && held.grant.resumeDigest == request.resumeDigest
            && held.peerIpv4BE == request.peerIpv4BE;
        if (exactRetry) {
            out = held.grant;
            return true;
        }
        denyReason = AdmissionDenyReason::GrantInvalid;
        return false;
    }
    if (replacing) {
        const bool oldRelay =
            existing->second.grant.route == static_cast<uint8_t>(
                qnet::relay::SpectatorRouteKind::Relay);
        const bool newRelay =
            request.route == static_cast<uint8_t>(
                qnet::relay::SpectatorRouteKind::Relay);
        if (oldRelay || !newRelay) {
            denyReason = AdmissionDenyReason::GrantInvalid;
            return false;
        }
    }

    const size_t reservationLimit =
        _config.maxReservations == 0 ? 1u : _config.maxReservations;
    if (!replacing && _reservations.size() >= reservationLimit) {
        denyReason = AdmissionDenyReason::CapacityFull;
        return false;
    }

    size_t reserved = 0;
    for (const auto& entry : _reservations) {
        if (entry.first != request.viewerId
            && reservationHoldsCapacity(entry.second, nowMs, unixNow)) {
            ++reserved;
        }
    }
    size_t viewers = _viewers.size();
    if (replacing
        && _viewers.find(request.viewerId) != _viewers.end()
        && viewers > 0) {
        --viewers;
    }
    if (viewers + reserved >= _maxViewers) {
        denyReason = AdmissionDenyReason::CapacityFull;
        return false;
    }
    if (request.peerIpv4BE != 0) {
        std::set<uint64_t> sameAddress;
        for (const auto& entry : _reservations) {
            if (entry.second.peerIpv4BE == request.peerIpv4BE
                && (entry.second.bound
                    || reservationHoldsCapacity(
                        entry.second, nowMs, unixNow))) {
                sameAddress.insert(entry.first);
            }
        }
        for (const auto& entry : _viewers) {
            if (entry.second.capacityQuotaAddress
                == directQuotaAddress(request.peerIpv4BE)) {
                sameAddress.insert(entry.first);
            }
        }
        bool quotaFull = false;
        if (request.viewerId == 0) {
            quotaFull =
                sameAddress.size()
                >= _config.rateLimits.maxViewersPerIp;
        } else {
            sameAddress.insert(request.viewerId);
            quotaFull =
                sameAddress.size()
                > _config.rateLimits.maxViewersPerIp;
        }
        if (quotaFull) {
            denyReason = AdmissionDenyReason::PerIpQuota;
            return false;
        }
    }

    SpectatorGrant grant{};
    for (size_t attempt = 0; attempt < 16 && grant.grantId == 0; ++attempt) {
        const uint64_t candidate = mintU64();
        bool used = candidate == 0;
        for (const auto& entry : _reservations) {
            if (entry.second.grant.grantId == candidate) {
                used = true;
                break;
            }
        }
        if (!used) grant.grantId = candidate;
    }
    if (grant.grantId == 0) {
        denyReason = AdmissionDenyReason::RateLimited;
        return false;
    }
    grant.viewerId = request.viewerId;
    for (size_t attempt = 0;
         grant.viewerId == 0 && attempt < 16;
         ++attempt) {
        const uint64_t candidate = mintU64();
        if (candidate != 0
            && _reservations.find(candidate) == _reservations.end()
            && _viewers.find(candidate) == _viewers.end()) {
            grant.viewerId = candidate;
        }
    }
    if (grant.viewerId == 0) {
        denyReason = AdmissionDenyReason::RateLimited;
        return false;
    }
    grant.sessionId = _config.identity.sessionId;
    const uint64_t lifetimeSeconds =
        request.lifetimeSeconds == 0 ? 300u : request.lifetimeSeconds;
    grant.expiryUnix = saturatingDeadline(unixNow, lifetimeSeconds);
    const bool relayGrant =
        request.route == static_cast<uint8_t>(
            qnet::relay::SpectatorRouteKind::Relay);
    grant.sourceGeneration = relayGrant && _transport
        ? _transport->relaySourceGeneration() : 1;
    if (grant.sourceGeneration == 0) {
        denyReason = AdmissionDenyReason::ServiceFailed;
        return false;
    }
    grant.resumeRecord = request.resumeRecord;
    grant.resumeDigest = request.resumeDigest;
    grant.route = request.route;
    if (relayGrant) {
        if (!_transport || !_transport->signRelayGrant(grant)) {
            denyReason = AdmissionDenyReason::ServiceFailed;
            return false;
        }
    } else if (_config.signGrant) {
        _config.signGrant(grant);
    }

    Reservation reservation{};
    reservation.grant = grant;
    reservation.peerIpv4BE = request.peerIpv4BE;
    reservation.issuedAtMs = nowMs;
    reservation.expiresAtMs = saturatingDeadline(
        nowMs, lifetimeSeconds * 1000ull);
    reservation.capacityHeldUntilMs = saturatingDeadline(
        nowMs, _config.handshakeDeadlineMs);
    reservation.approved = _policy != AdmissionPolicy::AskHost;

    if (replacing) {
        if (_viewers.find(grant.viewerId) != _viewers.end()) {
            retireViewer(
                grant.viewerId, ViewerServiceState::Detached, nowMs);
        }
    }
    _reservations[grant.viewerId] = reservation;

    out = grant;
    return true;
}

std::vector<uint64_t> SpectatorSidecar::pendingApprovals() const {
    std::vector<uint64_t> pending;
    for (const auto& entry : _reservations)
        if (!entry.second.consumed
            && !entry.second.approved && !entry.second.declined
            && !entry.second.revoked)
            pending.push_back(entry.first);
    return pending;
}

bool SpectatorSidecar::approveViewer(uint64_t viewerId, uint64_t nowMs) {
    sweepReservations(nowMs);
    auto found = _reservations.find(viewerId);
    if (found == _reservations.end()
        || found->second.consumed
        || found->second.revoked
        || found->second.declined) {
        return false;
    }
    found->second.approved = true;
    found->second.declined = false;
    return true;
}

bool SpectatorSidecar::declineViewer(uint64_t viewerId, uint64_t nowMs) {
    sweepReservations(nowMs);
    auto found = _reservations.find(viewerId);
    if (found == _reservations.end()
        || (found->second.consumed && !found->second.bound)) {
        return false;
    }
    found->second.declined = true;
    found->second.approved = false;
    found->second.capacityHeldUntilMs = 0;
    retireViewer(viewerId, ViewerServiceState::Kicked, nowMs);
    return true;
}

bool SpectatorSidecar::kickViewer(uint64_t viewerId, uint64_t nowMs) {
    auto reservation = _reservations.find(viewerId);
    if (reservation != _reservations.end()) {
        reservation->second.revoked = true;
        reservation->second.capacityHeldUntilMs = 0;
    }
    auto viewer = _viewers.find(viewerId);
    if (viewer == _viewers.end()) return reservation != _reservations.end();
    sendReject(viewer->second.endpoint, viewerId, viewer->second.generation,
               RejectReason::GrantInvalid, nowMs,
               _hasGame ? _gameId : kLatestGameId);
    retireViewer(viewerId, ViewerServiceState::Kicked, nowMs);
    return true;
}

bool SpectatorSidecar::revokeGrant(uint64_t grantId, uint64_t nowMs) {
    bool revoked = false;
    uint64_t viewerId = 0;
    for (auto& entry : _reservations) {
        if (entry.second.grant.grantId != grantId) continue;
        entry.second.revoked = true;
        viewerId = entry.first;
        revoked = true;
    }
    if (viewerId != 0) kickViewer(viewerId, nowMs);
    return revoked;
}

static constexpr uint64_t kReattachWindowMs = 60000;

bool SpectatorSidecar::resumePointIsOnOurChain(
    const uint32_t resumeRecord, const uint32_t resumeEvent,
    const uint64_t resumeDigest) const {
    if (_archive == nullptr || _gameId == 0) return false;
    uint64_t expected = 0;
    if (!_archive->chainDigestAt(_gameId, resumeRecord, resumeEvent, expected))
        return false;
    return expected == resumeDigest;
}

void SpectatorSidecar::armReattach(const uint64_t viewerId,
                                   const uint64_t nowMs) {
    auto reservation = _reservations.find(viewerId);
    if (reservation == _reservations.end()) return;
    Reservation& held = reservation->second;
    if (held.revoked || held.declined) return;
    held.reattachUntilMs = nowMs + kReattachWindowMs;
    QLOG("[spectator-sidecar] re-attach armed viewer=%s for %llums: this "
         "source dropped it for no progress, so its pass stays redeemable",
         qclient::spectator::redactedTag(viewerId).c_str(),
         static_cast<unsigned long long>(kReattachWindowMs));
}

bool SpectatorSidecar::noteAdmittedViewer(
    const uint32_t peerIpv4NetworkOrder, const uint16_t controlPort,
    const uint16_t dataPort, uint64_t nowMs) {
    if (nowMs == 0) nowMs = _lastServiceMs;
    return _transport
        && _transport->noteAdmittedViewer(peerIpv4NetworkOrder, controlPort,
                                          dataPort, nowMs);
}

bool SpectatorSidecar::releaseReservation(uint64_t viewerId,
                                          uint64_t nowMs) {
    if (nowMs == 0) nowMs = _lastServiceMs;
    auto viewer = _viewers.find(viewerId);
    if (viewer != _viewers.end()) {
        retireViewer(viewerId, ViewerServiceState::Detached, nowMs);
        return true;
    }
    auto reservation = _reservations.find(viewerId);
    if (reservation == _reservations.end()) return false;
    if (reservation->second.consumed) {
        reservation->second.bound = false;
        reservation->second.capacityHeldUntilMs = 0;
        return true;
    }
    _reservations.erase(reservation);
    return true;
}

void SpectatorSidecar::setAdmissionPolicy(AdmissionPolicy policy) {
    if (_clientPrivateRootDormant) return;
    if (policy == AdmissionPolicy::Disabled
        && _policy != AdmissionPolicy::Disabled) {
        const uint64_t nowMs = _lastServiceMs;
        std::vector<uint64_t> viewers;
        viewers.reserve(_viewers.size());
        for (const auto& entry : _viewers)
            viewers.push_back(entry.first);
        for (const uint64_t viewerId : viewers) {
            retireViewer(
                viewerId, ViewerServiceState::Kicked, nowMs);
        }
        _reservations.clear();
        stopResilientRoot(
            nowMs, topology::wire::AuthorityStateReason::ServiceStopping);
        if (_transport) _transport->close();
        if (_channel) {
            _channel->close();
            _channel.reset();
        }
        setPhase(SidecarPhase::Expired, nowMs);
        _statusCode = SourceStatusCode::Disabled;
    }
    _policy = policy;
}

void SpectatorSidecar::setMaxViewers(size_t maxViewers) {
    _maxViewers = maxViewers == 0 ? 1 : maxViewers;
}

void SpectatorSidecar::setUploadBudgetBytesPerSecond(uint64_t bytesPerSecond) {
    _config.uploadBudgetBytesPerSecond = bytesPerSecond;
    _upload.configure(static_cast<double>(bytesPerSecond),
                      static_cast<double>(_config.uploadBurstBytes),
                      _upload.lastMs);
}

bool SpectatorSidecar::transmit(const RouteEndpoint& to,
                                const std::vector<uint8_t>& bytes,
                                bool control,
                                uint64_t nowMs) {
    if (bytes.empty()) return false;
    if (!control && !_upload.take(static_cast<double>(bytes.size()), nowMs)) {
        ++_uploadThrottled;
        return false;
    }
    if (_transport && !_transport->sendTo(to, bytes.data(), bytes.size())) {
        _diagnostics.note(DiagEvent::TransportError, nowMs, "send failed");
        return false;
    }
    ++_egressPackets;
    _egressBytes += bytes.size();
    return true;
}

void SpectatorSidecar::sendReject(const RouteEndpoint& to,
                                  uint64_t viewerId,
                                  uint32_t generation,
                                  RejectReason reason,
                                  uint64_t nowMs,
                                  uint64_t requestedGameId) {
    RejectPayload reject{};
    reject.requestedGameId = requestedGameId;
    reject.availableGameId = _hasGame ? _gameId : kLatestGameId;
    reject.availableNextRecord = _committedRecords;
    reject.viewerId = viewerId;
    reject.generation = generation;
    reject.viewerNonce = viewerGenerationNonce(viewerId, generation);
    reject.reason = reason;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    if (!encodePayload(reject, payload)) return;
    if (!encodeDatagram(MessageType::Reject, _config.identity,
                        nextOutboundSequence(), payload, datagram))
        return;
    transmit(to, datagram, true, nowMs);
}

bool SpectatorSidecar::helloLogDue(uint64_t viewerId, uint64_t nowMs) {
    auto found = _helloLogMs.find(viewerId);
    if (found != _helloLogMs.end() && nowMs >= found->second
        && nowMs - found->second < 5000u) {
        return false;
    }
    if (found == _helloLogMs.end() && _helloLogMs.size() >= 64u)
        _helloLogMs.clear();
    _helloLogMs[viewerId] = nowMs;
    return true;
}

void SpectatorSidecar::logViewerSummary(uint64_t nowMs) {
    const uint64_t interval = _viewers.empty() ? 30000u : 5000u;
    if (_lastViewerSummaryMs != 0 && nowMs - _lastViewerSummaryMs < interval)
        return;
    _lastViewerSummaryMs = nowMs;
    std::string traversal;
    if (!_transport || !_transport->traversalSummary(traversal))
        traversal = "traversal=n/a";
    QLOG("[spectator-sidecar] summary phase=%s game=%d sealed=%d committed=%u "
         "viewers=%zu ingress=%llu pkts (%llu rejected, %llu rate-limited) "
         "egress=%llu pkts %llu B throttled=%llu publication=%llu events "
         "%llu gaps archiveEvents=%u held=%zu %s",
         sidecarPhaseName(_phase), _hasGame ? 1 : 0, _sealed ? 1 : 0,
         _committedRecords, _viewers.size(),
         static_cast<unsigned long long>(_ingressPackets),
         static_cast<unsigned long long>(_ingressRejected),
         static_cast<unsigned long long>(_ingressRateLimited),
         static_cast<unsigned long long>(_egressPackets),
         static_cast<unsigned long long>(_egressBytes),
         static_cast<unsigned long long>(_uploadThrottled),
         static_cast<unsigned long long>(_publicationEvents),
         static_cast<unsigned long long>(_publicationGaps),
         archivedSetEventCount(), _heldSetEvents.size(),
         traversal.c_str());
    for (const auto& entry : _viewers) {
        const ViewerRecord& viewer = entry.second;
        SourceViewerSnapshot snapshot{};
        const bool haveSnapshot =
            _source && _source->viewerSnapshot(viewer.viewerId, snapshot);
        const uint32_t backlog = _committedRecords > viewer.lastAckedRecord
            ? _committedRecords - viewer.lastAckedRecord
            : 0u;
        const unsigned long long progressAge =
            nowMs >= viewer.lastProgressMs ? nowMs - viewer.lastProgressMs
                                            : 0ull;
        const long long receiveAge =
            haveSnapshot && snapshot.lastValidReceiveMs != 0
                    && nowMs >= snapshot.lastValidReceiveMs
                ? static_cast<long long>(nowMs - snapshot.lastValidReceiveMs)
                : -1;
        QLOG("[spectator-sidecar] viewer=%016llX from=%s:%u route=%u state=%s "
             "acked=%u backlog=%u sentUpTo=%u unacked=%u repairs=%llu/%llu "
             "unverifiedAcks=%llu@%u/%u "
             "caughtUp=%d progressAge=%llums recvAge=%lldms sent=%llu B "
             "connected=%llus",
             static_cast<unsigned long long>(viewer.viewerId),
             qnet::formatIpv4BE(viewer.endpoint.ipv4BE).c_str(),
             static_cast<unsigned>(viewer.endpoint.port),
             static_cast<unsigned>(viewer.routeLabel),
             viewerServiceStateName(viewer.state), viewer.lastAckedRecord,
             backlog, haveSnapshot ? snapshot.highestSentNextRecord : 0u,
             haveSnapshot ? snapshot.unackedRecords : 0u,
             static_cast<unsigned long long>(
                 haveSnapshot ? snapshot.recordRepairSends : 0ull),
             static_cast<unsigned long long>(
                 haveSnapshot ? snapshot.eventRepairSends : 0ull),
             static_cast<unsigned long long>(
                 haveSnapshot ? snapshot.unverifiedAcks : 0ull),
             static_cast<unsigned>(
                 haveSnapshot ? snapshot.lastUnverifiedAckRecord : 0u),
             static_cast<unsigned>(
                 haveSnapshot ? snapshot.lastUnverifiedAckEvent : 0u),
             haveSnapshot && snapshot.caughtUp ? 1 : 0, progressAge,
             receiveAge, static_cast<unsigned long long>(viewer.bytesSent),
             static_cast<unsigned long long>(
                 nowMs >= viewer.admittedAtMs
                     ? (nowMs - viewer.admittedAtMs) / 1000ull
                     : 0ull));
    }
}

void SpectatorSidecar::sendHeartbeat(const RouteEndpoint& to, uint64_t nowMs) {
    HeartbeatPayload beat{};
    beat.gameId = _hasGame ? _gameId : 0;
    beat.nextRecord = _committedRecords;
    if (_hasGame && _archive) {
        ArchiveExtent extent{};
        if (_archive->extent(_gameId, extent)) beat.digest = extent.digest;
    }
    beat.checkpointRecord =
        _checkpoints.empty() ? 0u : _checkpoints.back().recordCursor;
    beat.sourcePhase = toWireSourcePhase(_phase);
    beat.viewerCount = static_cast<uint16_t>(
        _viewers.size() > 0xFFFFu ? 0xFFFFu : _viewers.size());
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    if (!encodePayload(beat, payload)) return;
    if (!encodeDatagram(MessageType::Heartbeat, _config.identity,
                        nextOutboundSequence(), payload, datagram))
        return;
    transmit(to, datagram, true, nowMs);
}

// Every datagram sent to a viewer must share the source's monotonic sequence: viewers keep one 64-entry anti-replay window over all of it.
uint32_t SpectatorSidecar::nextOutboundSequence() {
    if (_source) return _source->takeOutboundSequence();
    return _fallbackSequence++;
}

void SpectatorSidecar::pumpOutbound(uint64_t nowMs) {
    if (!_source) return;
    const size_t budget = _config.dataPacketBudget + _config.controlPacketBudget;
    const std::vector<SourceDatagram> outbound =
        _source->pollOutbound(nowMs, budget);
    for (const SourceDatagram& datagram : outbound) {
        const bool control =
            datagram.type != MessageType::ConfirmedInputBatch;
        auto mappedViewer = _tokenToViewerId.find(datagram.destination);
        auto viewer = mappedViewer != _tokenToViewerId.end()
                          ? _viewers.find(mappedViewer->second)
                          : _viewers.end();
        if (viewer != _viewers.end()) {
            if (transmit(viewer->second.endpoint, datagram.bytes, control,
                         nowMs))
                viewer->second.bytesSent += datagram.bytes.size();
            continue;
        }
        auto endpoint = _endpointsByToken.find(datagram.destination);
        if (endpoint != _endpointsByToken.end())
            transmit(endpoint->second, datagram.bytes, control, nowMs);
    }

}

void SpectatorSidecar::retireViewer(uint64_t viewerId,
                                    ViewerServiceState finalState,
                                    uint64_t nowMs) {
    auto viewer = _viewers.find(viewerId);
    if (viewer == _viewers.end()) return;
    // Revoke the topology credential before erasing the QSP identity, or a retired viewer retains a QSR control lane.
    detachTopologyViewer(viewerId, nowMs);
    switch (finalState) {
    case ViewerServiceState::Completed: ++_viewersCompleted; break;
    case ViewerServiceState::DeadlineExpired:
        ++_viewersExpired;
        _diagnostics.note(DiagEvent::ViewerTimeout, nowMs,
                          "viewer retired at its deadline");
        break;
    case ViewerServiceState::Kicked: ++_viewersKicked; break;
    case ViewerServiceState::Detached: ++_viewersDetached; break;
    default: break;
    }
    if (_terminalOutcomes.size() >= kMaxTerminalOutcomes)
        _terminalOutcomes.erase(_terminalOutcomes.begin());
    TerminalOutcome outcome{};
    outcome.viewerId = viewerId;
    outcome.generation = viewer->second.generation;
    outcome.state = finalState;
    outcome.acknowledgedRecord = viewer->second.lastAckedRecord;
    _terminalOutcomes.push_back(outcome);
    auto address =
        _addresses.find(viewer->second.ingressQuotaAddress);
    if (address != _addresses.end() && address->second.viewers > 0)
        --address->second.viewers;
    auto reservation = _reservations.find(viewerId);
    if (reservation != _reservations.end()) {
        reservation->second.bound = false;
        reservation->second.capacityHeldUntilMs = 0;
        reservation->second.consumed = true;
    }
    _tokenToViewerId.erase(viewer->second.endpointToken);
    _viewers.erase(viewer);
    if (_source) _source->removeViewer(viewerId);
    publishResumeFloor();
}

void SpectatorSidecar::removeProvisionalViewer(uint64_t viewerId) {
    auto viewer = _viewers.find(viewerId);
    if (viewer == _viewers.end()) return;
    auto address =
        _addresses.find(viewer->second.ingressQuotaAddress);
    if (address != _addresses.end() && address->second.viewers > 0)
        --address->second.viewers;
    _tokenToViewerId.erase(viewer->second.endpointToken);
    _viewers.erase(viewer);
    publishResumeFloor();
}

void SpectatorSidecar::retireRelayIngress(uint64_t nowMs) {
    std::vector<uint64_t> relayViewers;
    relayViewers.reserve(_viewers.size());
    for (const auto& entry : _viewers) {
        if (isRelayIngress(entry.second.endpoint))
            relayViewers.push_back(entry.first);
    }
    for (uint64_t viewerId : relayViewers) {
        retireViewer(
            viewerId, ViewerServiceState::Detached, nowMs);
    }

    for (auto& entry : _reservations) {
        Reservation& reservation = entry.second;
        const bool relayGrant =
            reservation.grant.route == static_cast<uint8_t>(
                qnet::relay::SpectatorRouteKind::Relay);
        const auto viewer = _viewers.find(entry.first);
        const bool liveDirectViewer =
            viewer != _viewers.end()
            && !isRelayIngress(viewer->second.endpoint);
        if (!relayGrant || liveDirectViewer) continue;
        reservation.consumed = true;
        reservation.bound = false;
        reservation.capacityHeldUntilMs = 0;
    }
}

void SpectatorSidecar::enforceDeadlines(uint64_t nowMs) {
    std::vector<std::pair<uint64_t, ViewerServiceState>> retire;
    for (auto& entry : _viewers) {
        ViewerRecord& viewer = entry.second;
        bool caughtUp = false;
        bool sourceRetired = false;
        ViewerServiceState sourceFinal =
            ViewerServiceState::DeadlineExpired;
        SourceViewerSnapshot snapshot{};
        if (_source && _source->viewerSnapshot(viewer.viewerId, snapshot)) {
            if (snapshot.endpoint != 0) viewer.endpointToken = snapshot.endpoint;
            if (snapshot.acknowledgedNextRecord > viewer.lastAckedRecord) {
                viewer.lastAckedRecord = snapshot.acknowledgedNextRecord;
                viewer.lastProgressMs = nowMs;
            }
            caughtUp = snapshot.caughtUp;
            if (caughtUp) {
                viewer.lastProgressMs = nowMs;
            }
            switch (snapshot.phase) {
            case SourceViewerPhase::AwaitingInitAck:
                viewer.state = ViewerServiceState::Handshaking;
                break;
            case SourceViewerPhase::Streaming:
                viewer.state = ViewerServiceState::Streaming;
                break;
            case SourceViewerPhase::AwaitingTerminalAck:
                viewer.state = ViewerServiceState::AwaitingTerminalAck;
                if (!viewer.terminalPhaseEntered) {
                    viewer.terminalPhaseEntered = true;
                    viewer.terminalPhaseAtMs = nowMs;
                }
                break;
            case SourceViewerPhase::Complete:
                viewer.state = ViewerServiceState::Completed;
                break;
            case SourceViewerPhase::Retired:
                sourceRetired = true;
                switch (snapshot.retirementReason) {
                case SourceRetirementReason::Completed:
                    sourceFinal = ViewerServiceState::Completed;
                    break;
                case SourceRetirementReason::Detached:
                    sourceFinal = ViewerServiceState::Detached;
                    break;
                case SourceRetirementReason::Removed:
                    sourceFinal = ViewerServiceState::Kicked;
                    break;
                case SourceRetirementReason::None:
                case SourceRetirementReason::Idle:
                case SourceRetirementReason::HandshakeDeadline:
                case SourceRetirementReason::NoProgressDeadline:
                case SourceRetirementReason::TerminalAckDeadline:
                case SourceRetirementReason::LagLimit:
                case SourceRetirementReason::ArchiveUnavailable:
                    sourceFinal = ViewerServiceState::DeadlineExpired;
                    break;
                }
                break;
            }
        }

        if (sourceRetired) {
            retire.emplace_back(entry.first, sourceFinal);
            continue;
        }
        if (viewer.state == ViewerServiceState::Completed) {
            retire.emplace_back(entry.first, ViewerServiceState::Completed);
            continue;
        }
        if (viewer.state == ViewerServiceState::Handshaking
            && nowMs - viewer.admittedAtMs > _config.handshakeDeadlineMs) {
            retire.emplace_back(entry.first,
                                ViewerServiceState::DeadlineExpired);
            continue;
        }
        if (viewer.state == ViewerServiceState::AwaitingTerminalAck
            && viewer.terminalPhaseEntered
            && nowMs - viewer.terminalPhaseAtMs
                   > _config.terminalAckDeadlineMs) {
            _diagnostics.note(DiagEvent::TerminalDeadline, nowMs,
                              "viewer never acknowledged the terminal");
            retire.emplace_back(entry.first,
                                ViewerServiceState::DeadlineExpired);
            continue;
        }
        if (!caughtUp
            && nowMs - viewer.lastProgressMs
                   > _config.noProgressDeadlineMs) {
            armReattach(viewer.viewerId, nowMs);
            retire.emplace_back(entry.first,
                                ViewerServiceState::DeadlineExpired);
        }
    }
    for (const auto& item : retire) retireViewer(item.first, item.second, nowMs);
    publishResumeFloor();

    if (!_sealed) return;

    if (_phase == SidecarPhase::Sealed && !_viewers.empty())
        setPhase(SidecarPhase::Draining, nowMs);

    if (nowMs >= _drainDeadlineMs && !_viewers.empty()) {
        std::vector<uint64_t> tokens;
        tokens.reserve(_viewers.size());
        for (const auto& entry : _viewers) tokens.push_back(entry.first);
        for (uint64_t token : tokens)
            retireViewer(token, ViewerServiceState::DeadlineExpired, nowMs);
    }

    if (_viewers.empty()
        && (_phase == SidecarPhase::Sealed || _phase == SidecarPhase::Draining)) {
        setPhase(SidecarPhase::Retained, nowMs);
        if (_retentionUntilMs == 0)
            _retentionUntilMs = nowMs + _config.retentionMs;
        if (!_sealTruncated && !_serviceFailed)
            _statusCode = _viewersExpired == 0
                              ? SourceStatusCode::CompletedClean
                              : SourceStatusCode::RetainedPostGame;
    }

    if (_phase == SidecarPhase::Retained && nowMs >= _retentionUntilMs) {
        std::vector<uint64_t> tokens;
        tokens.reserve(_viewers.size());
        for (const auto& entry : _viewers) tokens.push_back(entry.first);
        for (uint64_t token : tokens)
            retireViewer(token, ViewerServiceState::DeadlineExpired, nowMs);
        setPhase(SidecarPhase::Expired, nowMs);
        _statusCode = SourceStatusCode::RetentionExpired;
        stopResilientRoot(nowMs);
        if (_transport) _transport->close();
        if (_channel) {
            _channel->close();
            _channel.reset();
        }
    }
}

void SpectatorSidecar::service(uint64_t nowMs) {
    if (nowMs > _lastServiceMs) _lastServiceMs = nowMs;
    if (!_started || _policy == AdmissionPolicy::Disabled) return;

    sweepReservations(nowMs);

    if (_channel) {
        std::vector<PublicationEvent> events;
        const int drained = _channel->drain(events, kPublicationDrainBudget);
        if (drained < 0) {
            if (!_sealed)
                handleUpstreamLoss("the publication channel failed", nowMs);
            _channel->close();
            _channel.reset();
        } else {
            for (const PublicationEvent& event : events) publish(event, nowMs);
            if (!_channel->connected()
                && !_channel->reconnecting()
                && drained < static_cast<int>(kPublicationDrainBudget)
                && !_sealed && _hasGame) {
                handleUpstreamLoss("the game closed the publication channel",
                                   nowMs);
            }
        }
    }

    if (_transport && !_clientPrivateRootDormant) {
        _transport->serviceTraversal(nowMs);

        uint8_t buffer[kMaxSpectatorDatagramSize];
        size_t remainingTransportEvents =
            (std::max)(size_t{1}, _config.ingressDatagramBudget);
        for (size_t i = 0; i < _config.ingressDatagramBudget; ++i) {
            RouteEndpoint from{};
            const int received =
                _transport->recvFrom(buffer, sizeof(buffer), from);
            SidecarTransportEvent event{};
            while (remainingTransportEvents != 0
                   && _transport->pollEvent(event)) {
                --remainingTransportEvents;
                handleTransportEvent(event, nowMs);
            }
            if (received == 0) break;
            if (received < 0) {
                _diagnostics.note(DiagEvent::TransportError, nowMs,
                                  "receive failed");
                break;
            }
            onDatagram(from, buffer, static_cast<size_t>(received), nowMs);
        }

        _transport->serviceTraversal(nowMs);
    }

    if (!_clientPrivateRootDormant && _topologyAuthority && _resilientRoot) {
        if (nowMs >= _topologyAuthority->facts().expiresMs && _topologyQsr) {
            (void)_topologyQsr->sendAuthorityState(
                topology::wire::AuthorityStateKind::Revocation,
                topology::wire::AuthorityStateReason::AuthorityExpired,
                nowMs);
        }
        std::vector<qnetplay::spectator::resilient::NodeId> expiredNodes;
        const bool authorityExpired =
            _topologyAuthority->serviceExpiry(nowMs, expiredNodes);
        for (const auto childId : expiredNodes) {
            (void)_resilientRoot->detachChild(childId);
            if (_topologyQsr) _topologyQsr->forgetNode(childId);
        }
        if (authorityExpired) {
            _diagnostics.note(
                DiagEvent::RouteLost, nowMs,
                "authenticated spectator topology authority expired; E0 continues");
            stopResilientRoot(
                nowMs, topology::wire::AuthorityStateReason::AuthorityExpired);
        } else if (_topologyQsr) {
            (void)_topologyQsr->sendAuthorityState(
                topology::wire::AuthorityStateKind::Liveness,
                topology::wire::AuthorityStateReason::None, nowMs);
        }
    }
    if (!_clientPrivateRootDormant && _topologyQsr && _resilientRoot
        && !_topologyQsr->service(nowMs)) {
        _diagnostics.note(
            DiagEvent::RouteLost, nowMs,
            "authenticated spectator QSR control failed; E0 continues");
        stopResilientRoot(nowMs);
    }
    _topologyRelayLaneReady =
        _topologyQsr != nullptr && _topologyQsr->relayLaneReady();
    serviceClientPlayerRoot(nowMs);

    enforceDeadlines(nowMs);
    logViewerSummary(nowMs);

    if (!_clientPrivateRootDormant) pumpOutbound(nowMs);
}

void SpectatorSidecar::handleTransportEvent(
    const SidecarTransportEvent& event,
    uint64_t nowMs) {
    if (event.kind == SidecarTransportEventKind::RelaySourceRetrying) {
        retireRelayIngress(nowMs);
        _diagnostics.note(
            DiagEvent::RouteLost, nowMs,
            "source spectator relay registration failed; retrying with bounded backoff");
        return;
    }
    if (event.kind == SidecarTransportEventKind::RelaySourceUnavailable) {
        retireRelayIngress(nowMs);
        _diagnostics.note(
            DiagEvent::RouteLost, nowMs,
            "source spectator relay registration stopped; direct spectators remain enabled");
        return;
    }
    if (event.kind == SidecarTransportEventKind::RelaySourceReady) {
        return;
    }
    if (event.kind != SidecarTransportEventKind::ViewerDetached
        || event.viewerId == 0 || !event.endpoint.valid()) {
        return;
    }

    auto viewer = _viewers.find(event.viewerId);
    if (viewer != _viewers.end()) {
        if (viewer->second.endpoint == event.endpoint) {
            retireViewer(
                event.viewerId, ViewerServiceState::Detached, nowMs);
        }
        return;
    }

    auto reservation = _reservations.find(event.viewerId);
    if (reservation == _reservations.end()
        || reservation->second.grant.route
               != static_cast<uint8_t>(
                   qnet::relay::SpectatorRouteKind::Relay)) {
        return;
    }
    reservation->second.consumed = true;
    reservation->second.bound = false;
    reservation->second.capacityHeldUntilMs = 0;
}

bool SpectatorSidecar::stop(bool drain, uint64_t nowMs) {
    if (_stopped) return true;
    if (_policy == AdmissionPolicy::Disabled || !_started) {
        _stopped = true;
        return true;
    }
    if (!drain) {
        if (!_sealed)
            sealTruncated("the caster stopped without draining", nowMs);
        std::vector<uint64_t> tokens;
        tokens.reserve(_viewers.size());
        for (const auto& entry : _viewers) tokens.push_back(entry.first);
        for (uint64_t token : tokens)
            retireViewer(token, ViewerServiceState::DeadlineExpired, nowMs);
        stopResilientRoot(nowMs);
        if (_transport) _transport->close();
        if (_channel) _channel->close();
        setPhase(SidecarPhase::Expired, nowMs);
        _stopped = true;
        return true;
    }
    if (!_sealed) {
        sealTruncated("the caster stopped the spectator service", nowMs);
    }
    service(nowMs);
    if (_viewers.empty()) {
        stopResilientRoot(nowMs);
        if (_transport) _transport->close();
        if (_channel) _channel->close();
        _stopped = true;
        return true;
    }
    return false;
}

MirrorResult SpectatorSidecar::verifyPeerPrefix(const MirrorPrefix& prefix) const {
    if (_serviceFailed) return MirrorResult::ServiceFailed;
    if (!_hasGame || !_archive) return MirrorResult::NoLocalArchive;
    if (prefix.gameId != _gameId) return MirrorResult::GameMismatch;
    if (prefix.startFingerprint != 0
        && prefix.startFingerprint != _startFingerprint)
        return MirrorResult::FingerprintMismatch;
    if (prefix.nextRecord > _committedRecords) return MirrorResult::CursorAhead;
    uint64_t digest = 0;
    if (!_archive->digestAt(_gameId, prefix.nextRecord, digest))
        return MirrorResult::CursorAhead;
    if (digest != prefix.digest) return MirrorResult::DigestDivergence;
    return MirrorResult::Accepted;
}

MirrorResult SpectatorSidecar::acceptPeerPrefix(const MirrorPrefix& prefix,
                                                uint64_t nowMs) {
    const MirrorResult result = verifyPeerPrefix(prefix);
    if (result != MirrorResult::Accepted) {
        _diagnostics.note(DiagEvent::MirrorDivergence, nowMs,
                          mirrorResultText(result));
        return result;
    }
    _alternateSource = true;
    return result;
}

MirrorPrefix SpectatorSidecar::localPrefix() const {
    MirrorPrefix prefix{};
    prefix.gameId = _gameId;
    prefix.startFingerprint = _startFingerprint;
    prefix.nextRecord = _committedRecords;
    if (_hasGame && _archive) {
        uint64_t digest = 0;
        if (_archive->digestAt(_gameId, _committedRecords, digest))
            prefix.digest = digest;
    }
    if (!_checkpoints.empty()) {
        prefix.checkpointId = _checkpoints.back().checkpointId;
        prefix.checkpointRecord = _checkpoints.back().recordCursor;
        prefix.checkpointDigest = _checkpoints.back().digest;
    }
    return prefix;
}

uint16_t SpectatorSidecar::boundPort() const {
    return _transport ? _transport->boundPort() : 0;
}

SidecarStatus SpectatorSidecar::status() const {
    SidecarStatus out{};
    out.phase = _phase;
    out.code = _statusCode;
    out.codeText = sourceStatusText(_statusCode);
    out.serving = _started && !_serviceFailed && !_clientPrivateRootDormant
               && _policy != AdmissionPolicy::Disabled
               && _phase != SidecarPhase::Expired;
    out.boundPort = boundPort();
    out.policy = _policy;
    out.clientPrivateRootDormant = _clientPrivateRootDormant;
    out.clientPrivateRuntimeApplied = _clientPrivateRuntimeApplied;
    out.clientRootPrepared = _clientRoot.prepared;
    out.clientRootAsserted = _clientRoot.asserted;
    out.clientRootCommitted = _clientRoot.committed;
    out.clientRootServing = _clientRoot.servingActive;
    out.clientRootQsrPort = _clientRoot.prepared ? _clientRoot.qsrPort : 0u;

    out.hasGame = _hasGame;
    out.gameId = _gameId;
    out.confirmedRecords = _committedRecords;
    if (_archive && _hasGame) {
        uint32_t first = 0;
        if (_archive->firstRetainedRecord(_gameId, first))
            out.firstAvailableRecord = first;
        out.retainedRecordCount = _archive->retainedRecordCount();
        out.allocatedRecordBytes = _archive->allocatedRecordBytes();
    }
    out.newestCheckpointRecord =
        _checkpoints.empty() ? 0u : _checkpoints.back().recordCursor;
    out.checkpointCount = _checkpoints.size();
    out.presenceHasCurrentGame = _presenceHasCurrentGame;
    out.presenceCurrentGameEnded = _presenceCurrentGameEnded;
    out.presenceTimelineFrame = _presenceTimelineFrame;
    out.presenceActiveFighterMask = _presenceActiveFighterMask;
    out.presenceCharacterIds = _presenceCharacterIds;
    out.presencePaletteIndices = _presencePaletteIndices;
    out.presenceRoundWins = _presenceRoundWins;
    out.presenceMatchWins = _presenceMatchWins;
    out.presencePlayerNames = _presencePlayerNames;
    out.connectedV4Failed = _connectedV4Degraded;
    if (_connectedArchiveV4 && _connectedArchiveV4->bound()) {
        out.connectedV4Bound = true;
        out.connectedV4Sealed = _connectedArchiveV4->sealed();
        out.connectedSetStreamId = _connectedArchiveV4->setStreamId();
        out.connectedNextItemSequence =
            _connectedArchiveV4->nextItemSequence();
        out.connectedNextRecordCursor =
            _connectedArchiveV4->nextRecordCursor();
        out.connectedCompleteGames = static_cast<size_t>(std::count_if(
            _connectedArchiveV4->ranges().begin(),
            _connectedArchiveV4->ranges().end(),
            [](const qnetplay::spectator::ConnectedGameRangeV4& range) {
                return range.complete;
            }));
        out.connectedRetainedBytes =
            _connectedArchiveV4->retainedBytes();
    }

    out.sealed = _sealed;
    out.sealedIsTruncation = _sealTruncated;
    out.terminalReason = _terminalReason;
    out.postTailStateHash = _postTailStateHash;
    out.hasPostTailProof = _hasPostTailProof;
    out.terminalRecordCursor = _terminalRecordCursor;
    out.terminalPrefixDigest = _terminalPrefixDigest;
    out.conflictingTerminalRequests = _terminalConflicts;
    out.truncatedRecords = _truncatedRecords;

    out.viewerCount = _viewers.size();
    out.viewerCapacity = _maxViewers;
    out.pendingApprovals = pendingApprovals().size();
    out.reservationCount = _reservations.size();
    const uint64_t reservationUnixNow = nowUnix();
    for (const auto& entry : _reservations) {
        if (reservationHoldsCapacity(
                entry.second, _lastServiceMs, reservationUnixNow)) {
            ++out.capacityReservations;
        }
        if (entry.second.consumed)
            ++out.consumedGrantTombstones;
    }
    out.viewersCompleted = _viewersCompleted;
    out.viewersDeadlineExpired = _viewersExpired;
    out.viewersKicked = _viewersKicked;
    out.viewersDetached = _viewersDetached;
    out.admissionRejections = _admissionRejections;
    out.terminalOutcomes = _terminalOutcomes;
    if (_topologyAuthority && _topologyAuthority->ready()) {
        const topology::TopologyAuthorityFacts& facts =
            _topologyAuthority->facts();
        out.topologyAuthorityReady = true;
        out.topologyAuthorityGeneration = facts.authorityGeneration;
        out.topologyRootNodeId = facts.rootNodeId;
        out.topologyQsrPort = facts.rootQsr.local.port;
        out.topologyEnrolled = _topologyAuthority->enrolledCount();
        out.topologyAttached = _topologyAuthority->attachedCount();
    }

    if (_sealed && _drainDeadlineMs > _lastServiceMs)
        out.drainDeadlineRemainingMs = _drainDeadlineMs - _lastServiceMs;
    if (_sealed && _phase != SidecarPhase::Expired
        && _retentionUntilMs > _lastServiceMs) {
        out.retentionRemainingMs = _retentionUntilMs - _lastServiceMs;
    }

    out.failureReason = _failureReason;
    out.playersUnaffected = true;

    out.ingressPackets = _ingressPackets;
    out.ingressBytes = _ingressBytes;
    out.ingressRateLimited = _ingressRateLimited;
    out.ingressDecoded = _ingressDecoded;
    out.ingressRejected = _ingressRejected;
    out.trackedAddressCount = _addresses.size();
    out.overflowAddressPackets = _overflowAddressPackets;
    out.egressPackets = _egressPackets;
    out.egressBytes = _egressBytes;
    out.uploadThrottledPackets = _uploadThrottled;
    out.publicationEvents = _publicationEvents;
    out.publicationGaps = _publicationGaps;
    out.tailProofs = _tailProofs;
    out.tailProofCommits = _tailProofCommits;
    out.spoolFailures = _spoolFailures;
    out.diagnosticsEmitted = _diagnostics.emitted();
    out.diagnosticsDropped = _diagnostics.dropped();
    out.diagnosticBytes = _diagnostics.bytes();

    if (_hasGame && _archive) {
        uint32_t firstRecord = 0;
        if (_archive->firstRetainedRecord(_gameId, firstRecord))
            out.firstAvailableRecord = firstRecord;
    }

    out.viewers.reserve(_viewers.size());
    for (const auto& entry : _viewers) {
        const ViewerRecord& record = entry.second;
        ViewerStatus viewer{};
        viewer.viewerId = record.viewerId;
        viewer.generation = record.generation;
        viewer.endpoint = record.endpoint;
        viewer.routeLabel = record.routeLabel;
        viewer.state = record.state;
        viewer.bootstrapRecord = record.bootstrapRecord;
        viewer.acknowledgedRecord = record.lastAckedRecord;
        viewer.backlogRecords = _committedRecords > record.lastAckedRecord
                                    ? _committedRecords - record.lastAckedRecord
                                    : 0u;
        viewer.admittedAtMs = record.admittedAtMs;
        viewer.lastProgressMs = record.lastProgressMs;
        viewer.bytesSent = record.bytesSent;
        viewer.grantTag = redactedTag(record.grantId);
        out.viewers.push_back(std::move(viewer));
    }
    return out;
}

uint64_t SpectatorSidecar::mintU64() {
    if (_config.randomU64) {
        const uint64_t value = _config.randomU64();
        if (value != 0) return value;
    }
    const uint64_t value =
        mix64(++_randomCounter
              ^ (reinterpret_cast<uintptr_t>(this) * 0x9E3779B97F4A7C15ull)
              ^ _config.identity.cookie.low);
    return value == 0 ? 1 : value;
}

uint64_t SpectatorSidecar::nowUnix() const {
    if (_config.unixClock) return _config.unixClock();
    return static_cast<uint64_t>(std::time(nullptr));
}

#ifdef _WIN32
namespace {

constexpr size_t kSidecarControlQueueCapacity = 256;
std::mutex g_sidecarControlMutex;
std::deque<SidecarControlDatagram> g_sidecarControlQueue;
bool g_sidecarControlEnabled = false;
uint64_t g_sidecarControlQueueDrops = 0;

bool isControlLinkDatagram(const uint8_t* bytes, const size_t length) {
    uint8_t type = 0;
    const uint8_t* body = nullptr;
    size_t bodyLength = 0;
    return qnet::unpackMsg(bytes, length, type, body, bodyLength)
        && (type == qnet::MSG_REL_DATA || type == qnet::MSG_REL_ACK
            || type == qnet::MSG_KEEPALIVE || type == qnet::MSG_LINK_HELLO
            || type == qnet::MSG_LINK_HELLO_ACK);
}

bool queueSidecarControlDatagram(const uint8_t* bytes, const size_t length,
                                 const qnet::Endpoint& from) {
    if (bytes == nullptr || length == 0
        || length > kSidecarControlDatagramMaxBytes
        || !isControlLinkDatagram(bytes, length)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_sidecarControlMutex);
    if (!g_sidecarControlEnabled) return false;
    if (g_sidecarControlQueue.size() >= kSidecarControlQueueCapacity) {
        ++g_sidecarControlQueueDrops;
        if (g_sidecarControlQueueDrops == 1
            || (g_sidecarControlQueueDrops % 128ull) == 0) {
            QLOG("[spectator-control-bridge] queue full drops=%llu capacity=%zu",
                 static_cast<unsigned long long>(
                     g_sidecarControlQueueDrops),
                 kSidecarControlQueueCapacity);
        }
        return true;
    }
    SidecarControlDatagram datagram{};
    std::memcpy(datagram.bytes.data(), bytes, length);
    datagram.length = length;
    datagram.from = from;
    g_sidecarControlQueue.push_back(std::move(datagram));
    return true;
}

void clearSidecarControlQueue() {
    std::lock_guard<std::mutex> lock(g_sidecarControlMutex);
    g_sidecarControlQueue.clear();
}

enum class SourceRelayRegistrationState : uint8_t {
    Disabled = 0,
    Backoff,
    Connecting,
    AwaitingWelcome,
    Active,
    Terminal,
};

constexpr uint64_t kSourceRelayConnectDeadlineMs = 3000;
constexpr uint64_t kSourceRelayWelcomeDeadlineMs = 2000;
constexpr uint64_t kSourceRelayInitialBackoffMs = 250;
constexpr uint64_t kSourceRelayMaximumBackoffMs = 4000;
constexpr uint64_t kSourceRelayRetryWindowMs = 30000;
constexpr uint32_t kSourceRelayMaximumAttempts = 8;
constexpr ULONG kSourceRelayKeepAliveIdleMs = 10000;
constexpr ULONG kSourceRelayKeepAliveIntervalMs = 2000;

class UdpSidecarTransport final : public SidecarTransport {
public:
    explicit UdpSidecarTransport(uintptr_t preboundSocket =
                                     kInvalidPreboundSpectatorSocket) {
        qnet::netInit();
        if (preboundSocket == kInvalidPreboundSpectatorSocket) return;
        const SOCKET socket = static_cast<SOCKET>(preboundSocket);
        if (!_socket.adopt(socket)) {
            _adoptionError = _socket.lastError();
            _socket.close();
        }
    }
    ~UdpSidecarTransport() override { close(); }

    bool open(uint16_t port, std::string& error) override {
        if (_adoptionError != 0) {
            error = "the pre-bound spectator UDP socket could not be adopted (WSA "
                  + std::to_string(_adoptionError) + ")";
            return false;
        }
        if (_socket.isOpen()) {
            const uint16_t bound = _socket.boundPort();
            if (bound == 0 || (port != 0 && bound != port)) {
                error = "the pre-bound spectator UDP socket does not match its advertised endpoint";
                return false;
            }
            return true;
        }
        if (!_socket.open(port)) {
            error = "the spectator UDP port could not be bound (WSA "
                  + std::to_string(_socket.lastError())
                  + "); spectators are disabled for this match";
            return false;
        }
        _socket.drain();
        return true;
    }
    uint16_t boundPort() const override {
        return _socket.isOpen() ? _socket.boundPort() : 0;
    }
    void close() override {
        if (_relaySource.enabled() && _socket.isOpen()) {
            std::vector<uint8_t> detach;
            if (_relaySource.encodeDetach(
                    0,
                    qnet::relay::SpectatorDetachReason::SourceLost,
                    detach)) {
                (void)_socket.sendTo(
                    _relayEndpoint, detach.data(), detach.size());
            }
        }
        closePendingRelaySocket();
        if (_relayRegistrationState
                == SourceRelayRegistrationState::Active) {
            _relayControl.closeGraceful(250);
        } else {
            _relayControl.close();
        }
        _relaySource.reset();
        _transportEvents.clear();
        _relayGrantKey = {};
        _relayEndpoint = {};
        _relayConfig = {};
        _relayHello.clear();
        _relayRegistrationState =
            SourceRelayRegistrationState::Disabled;
        _relayAttempts = 0;
        _relayNextAttemptMs = 0;
        _relayAttemptDeadlineMs = 0;
        _relayRetryDeadlineMs = 0;
        _relayBackoffMs = kSourceRelayInitialBackoffMs;
        _relayRetryEventEmitted = false;
        _relayTerminalEventEmitted = false;
        _autopunch.reset();
        _autopunchPeers.reset();
        _socket.close();
    }

    bool configureSourceAutopunch(const SourceAutopunchConfig& config,
                                  std::string& warning) override {
        warning.clear();
        _autopunch.reset();
        _autopunchPeers.reset();
        if (!config.enabled) return true;
        if (!_socket.isOpen() || _socket.boundPort() == 0) {
            warning =
                "source AutoPunch disabled because the spectator socket is not bound";
            return false;
        }
        if (!config.relay.valid()) {
            warning =
                "source AutoPunch disabled because its resolved relay endpoint is invalid";
            return false;
        }

        qnet::Endpoint relay;
        relay.sa.sin_addr.s_addr = config.relay.ipv4BE;
        relay.sa.sin_port = htons(config.relay.port);
        _autopunch.configure(_socket.boundPort(), relay);
        _autopunchPeers.configure(relay);
        _autopunchPeers.setLocalPort(_socket.boundPort());
        return _autopunch.configured() && _autopunchPeers.configured();
    }

    bool configureSourceRelay(const SourceRelayConfig& config,
                              std::string& warning) override {
        warning.clear();
        closePendingRelaySocket();
        _relayControl.close();
        _relaySource.reset();
        _transportEvents.clear();
        _relayGrantKey = {};
        _relayEndpoint = {};
        _relayConfig = {};
        _relayHello.clear();
        _relayRegistrationState =
            SourceRelayRegistrationState::Disabled;
        _relayAttempts = 0;
        _relayNextAttemptMs = 0;
        _relayAttemptDeadlineMs = 0;
        _relayRetryDeadlineMs = 0;
        _relayBackoffMs = kSourceRelayInitialBackoffMs;
        _relayRetryEventEmitted = false;
        _relayTerminalEventEmitted = false;
        if (!config.enabled) return true;
        if (!_socket.isOpen() || _socket.boundPort() == 0
            || !config.relay.valid()
            || !qnet::relay::isValidId(config.room)
            || !qnet::relay::isValidId(config.sourceToken)
            || !qnetplay::spectator::isValidIdentity(config.identity)) {
            warning =
                "source spectator relay disabled because its binding is incomplete";
            return false;
        }

        qnet::relay::SpectatorHello hello{};
        hello.room = config.room;
        hello.party = qnet::relay::SpectatorParty::Source;
        hello.admission = qnet::relay::SpectatorAdmission::None;
        hello.sourceToken = config.sourceToken;
        hello.sessionId = config.identity.sessionId;
        if (!qnet::relay::encodeSpectatorHello(hello, _relayHello)) {
            warning =
                "source spectator relay registration could not be sent";
            return false;
        }

        _relayConfig = config;
        _relayEndpoint.sa.sin_family = AF_INET;
        _relayEndpoint.sa.sin_addr.s_addr = config.relay.ipv4BE;
        _relayEndpoint.sa.sin_port = htons(config.relay.port);
        _relayRegistrationState =
            SourceRelayRegistrationState::Backoff;
        _relayNextAttemptMs = 0;
        return true;
    }

    uint32_t relaySourceGeneration() const override {
        return _relaySource.enabled()
            ? _relaySource.binding().sourceGeneration : 0;
    }

    bool relayViewerIdentity(const RouteEndpoint& endpoint,
                             uint64_t& viewerId) const override {
        viewerId = 0;
        if (!_relaySource.enabled() || !isRelayIngress(endpoint))
            return false;
        const qnetplay::spectator::RelayVirtualEndpoint virtualEndpoint{
            endpoint.ipv4BE, endpoint.port
        };
        return _relaySource.viewerForEndpoint(
            virtualEndpoint, viewerId)
            && viewerId != 0;
    }

    bool signRelayGrant(SpectatorGrant& grant) const override {
        if (!_relaySource.enabled()
            || grant.sourceGeneration
                   != _relaySource.binding().sourceGeneration) {
            return false;
        }
        qnetplay::spectator::issueGrant(_relayGrantKey, grant);
        return grant.macLow != 0 || grant.macHigh != 0;
    }

    bool verifyRelayGrant(const SpectatorGrant& grant,
                          uint64_t nowUnix) const override {
        return _relaySource.enabled()
            && grant.sourceGeneration
                   == _relaySource.binding().sourceGeneration
            && qnetplay::spectator::verifyGrant(
                   _relayGrantKey, grant, grant.viewerId,
                   grant.sessionId, nowUnix)
                   == qnetplay::spectator::GrantVerdict::Ok;
    }

    bool traversalSummary(std::string& out) const override {
        const qnet::autopunch::SessionStats& session = _autopunch.stats();
        const qnet::autopunch::PunchPeerStats& peers = _autopunchPeers.stats();
        char text[420];
        std::snprintf(
            text, sizeof(text),
            "autopunch=%s registers=%u lookups=%u mappings=%u(rej %u) "
            "punchesOut=%u punchesIn=%u viewerPeers=%zu peerMappings=%u"
            "(rej %u) peerProbes=%u peerPunches=%u peersExpired=%u "
            "peerLookups=%u seededPeers=%u relaySource=%d",
            _autopunch.configured() ? "on" : "off", session.registersSent,
            session.lookupsSent, session.mappingsReceived,
            session.mappingsRejected, session.punchesSent,
            session.punchesReceived, _autopunchPeers.size(),
            peers.mappingsAccepted, peers.mappingsRejected,
            peers.probesReceived, peers.punchesSent, peers.peersExpired,
            peers.lookupsSent, peers.seededPeers,
            _relaySource.enabled() ? 1 : 0);
        out = text;
        return true;
    }

    bool noteAdmittedViewer(uint32_t peerIpv4NetworkOrder,
                            uint16_t controlPort, uint16_t dataPort,
                            uint64_t nowMs) override {
        if (!_autopunchPeers.configured() || peerIpv4NetworkOrder == 0)
            return false;
        bool acted = false;
        if (dataPort != 0) {
            acted |= _autopunchPeers.addLookupTarget(
                peerIpv4NetworkOrder, dataPort, nowMs);
        }
        qnet::Endpoint direct{};
        direct.sa.sin_family = AF_INET;
        direct.sa.sin_addr.s_addr = peerIpv4NetworkOrder;
        if (dataPort != 0) {
            direct.sa.sin_port = htons(dataPort);
            acted |= _autopunchPeers.addKnownPeer(direct, nowMs);
        }
        if (controlPort != 0 && controlPort != dataPort) {
            direct.sa.sin_port = htons(controlPort);
            acted |= _autopunchPeers.addKnownPeer(direct, nowMs);
        }
        if (acted) {
            _autopunchPeers.service(&UdpSidecarTransport::sendAutopunch, this,
                                    nowMs);
        }
        QLOG("[spectator-autopunch] admitted-viewer ip=%s controlPort=%u "
             "dataPort=%u acted=%d peers=%zu",
             qnet::formatIpv4BE(peerIpv4NetworkOrder).c_str(),
             static_cast<unsigned>(controlPort),
             static_cast<unsigned>(dataPort), acted ? 1 : 0,
             _autopunchPeers.size());
        return acted;
    }

    void serviceTraversal(uint64_t nowMs) override {
        _lastTraversalMs = nowMs;
        _autopunch.service(&UdpSidecarTransport::sendAutopunch, this, nowMs);
        _autopunchPeers.service(&UdpSidecarTransport::sendAutopunch, this,
                                nowMs);
        serviceSourceRelay(nowMs);
        if (_relaySource.enabled()) {
            std::vector<uint8_t> registration;
            if (_relaySource.encodeRegistration(nowMs, registration)) {
                (void)_socket.sendTo(
                    _relayEndpoint, registration.data(),
                    registration.size());
            }
        }
    }

    int recvFrom(uint8_t* buffer, size_t capacity, RouteEndpoint& from) override {
        if (!_socket.isOpen()) return -1;

        constexpr size_t kTraversalConsumeBudget = 32;
        uint8_t wire[kMaxSpectatorDatagramSize
                     + qnet::relay::kSpectatorFrameHeaderSize]{};
        for (size_t i = 0; i < kTraversalConsumeBudget; ++i) {
            qnet::Endpoint source;
            const int received =
                _socket.recvFrom(wire, sizeof(wire), source);
            if (received <= 0) return received;
            qnet::autopunch::PunchMappingResult punchMapping{};
            if (_autopunchPeers.handleInboundMapping(
                    wire, static_cast<size_t>(received), source,
                    _lastTraversalMs, punchMapping)) {
                qnet::autopunch::Mapping mapping{};
                if (qnet::autopunch::decodeMapping(
                        wire, static_cast<size_t>(received), mapping)) {
                    qnet::Endpoint mapped{};
                    mapped.sa.sin_addr.s_addr = mapping.ipv4NetworkOrder;
                    mapped.sa.sin_port = htons(mapping.natPort);
                    QLOG("[spectator-autopunch] source-mapping result=%u "
                         "peer=%s internalPort=%u tracked=%zu",
                         static_cast<unsigned>(punchMapping),
                         mapped.str().c_str(),
                         static_cast<unsigned>(mapping.internalPort),
                         _autopunchPeers.size());
                } else {
                    QLOG("[spectator-autopunch] source-mapping result=%u "
                         "decode=failed tracked=%zu",
                         static_cast<unsigned>(punchMapping),
                         _autopunchPeers.size());
                }
                _autopunchPeers.service(
                    &UdpSidecarTransport::sendAutopunch, this,
                    _lastTraversalMs);
                continue;
            }
            if (_autopunchPeers.handleProbe(
                    wire, static_cast<size_t>(received), source,
                    _lastTraversalMs)) {
                continue;
            }
            if (queueSidecarControlDatagram(
                    wire, static_cast<size_t>(received), source)) {
                continue;
            }
            if (source == _relayEndpoint
                && _relayRegistrationState
                       != SourceRelayRegistrationState::Disabled
                && _relayRegistrationState
                       != SourceRelayRegistrationState::Terminal) {
                if (!_relaySource.enabled()) {
                    continue;
                }
                std::vector<uint8_t> inner;
                qnetplay::spectator::RelayVirtualEndpoint virtualFrom{};
                uint64_t detachedViewerId = 0;
                const auto ingress = _relaySource.decodeInbound(
                    true, wire, static_cast<size_t>(received),
                    inner, virtualFrom, &detachedViewerId);
                if (ingress
                        == qnetplay::spectator::RelaySourceIngress::Detached
                    && detachedViewerId != 0 && virtualFrom.valid()) {
                    SidecarTransportEvent event{};
                    event.kind =
                        SidecarTransportEventKind::ViewerDetached;
                    event.endpoint.ipv4BE = virtualFrom.ipv4BE;
                    event.endpoint.port = virtualFrom.port;
                    event.viewerId = detachedViewerId;
                    _transportEvents.push_back(event);
                }
                if (ingress
                    != qnetplay::spectator::RelaySourceIngress::Data) {
                    continue;
                }
                if (inner.size() > capacity || !virtualFrom.valid())
                    continue;
                std::memcpy(buffer, inner.data(), inner.size());
                from.ipv4BE = virtualFrom.ipv4BE;
                from.port = virtualFrom.port;
                return static_cast<int>(inner.size());
            }

            if (static_cast<size_t>(received) > capacity) continue;
            std::memcpy(buffer, wire, static_cast<size_t>(received));
            from.ipv4BE = source.sa.sin_addr.s_addr;
            from.port = ntohs(source.sa.sin_port);
            return received;
        }
        return 0;
    }

    bool sendTo(const RouteEndpoint& to, const uint8_t* bytes,
                size_t length) override {
        if (!_socket.isOpen() || !to.valid()) return false;
        if (to.ipv4BE
                == qnetplay::spectator::kRelayVirtualIpv4BE) {
            // A virtual relay endpoint is never a network destination: an unknown route must fail closed, not fall through to sendto().
            if (!_relaySource.enabled()) return false;
            const qnetplay::spectator::RelayVirtualEndpoint virtualTo{
                to.ipv4BE, to.port
            };
            uint64_t viewerId = 0;
            if (_relaySource.viewerForEndpoint(virtualTo, viewerId)) {
                std::vector<uint8_t> framed;
                if (!_relaySource.encodeOutbound(
                        virtualTo, bytes, length, framed)) {
                    return false;
                }
                return _socket.sendTo(
                    _relayEndpoint, framed.data(), framed.size());
            }
            return false;
        }
        qnet::Endpoint destination;
        destination.sa.sin_family = AF_INET;
        destination.sa.sin_addr.s_addr = to.ipv4BE;
        destination.sa.sin_port = htons(to.port);
        return _socket.sendTo(destination, bytes, length);
    }

    bool pollEvent(SidecarTransportEvent& event) override {
        if (_transportEvents.empty()) return false;
        event = _transportEvents.front();
        _transportEvents.pop_front();
        return true;
    }

private:
    static uint64_t relayDeadlineAfter(uint64_t nowMs, uint64_t deltaMs) {
        const uint64_t maximum =
            (std::numeric_limits<uint64_t>::max)();
        return deltaMs > maximum - nowMs ? maximum : nowMs + deltaMs;
    }

    void queueRelayEvent(SidecarTransportEventKind kind) {
        SidecarTransportEvent event{};
        event.kind = kind;
        _transportEvents.push_back(event);
    }

    void closePendingRelaySocket() {
        if (_relayConnectingSocket == INVALID_SOCKET) return;
        closesocket(_relayConnectingSocket);
        _relayConnectingSocket = INVALID_SOCKET;
    }

    void stopSourceRelayRegistration() {
        closePendingRelaySocket();
        _relayControl.close();
        _relaySource.reset();
        _relayGrantKey = {};
        _relayConfig = {};
        _relayHello.clear();
        _relayRegistrationState =
            SourceRelayRegistrationState::Terminal;
        _relayNextAttemptMs = 0;
        _relayAttemptDeadlineMs = 0;
        _relayRetryDeadlineMs = 0;
        if (!_relayTerminalEventEmitted) {
            queueRelayEvent(
                SidecarTransportEventKind::RelaySourceUnavailable);
            _relayTerminalEventEmitted = true;
        }
    }

    void scheduleSourceRelayRetry(uint64_t nowMs,
                                  uint32_t requestedDelayMs = 0) {
        closePendingRelaySocket();
        _relayControl.close();
        _relaySource.reset();
        _relayGrantKey = {};
        _relayAttemptDeadlineMs = 0;

        if (_relayRetryDeadlineMs == 0) {
            _relayRetryDeadlineMs =
                relayDeadlineAfter(nowMs, kSourceRelayRetryWindowMs);
        }
        if (_relayAttempts >= kSourceRelayMaximumAttempts
            || (_relayRetryDeadlineMs != 0
                && nowMs >= _relayRetryDeadlineMs)) {
            stopSourceRelayRegistration();
            return;
        }

        uint64_t delayMs = _relayBackoffMs;
        if (requestedDelayMs != 0)
            delayMs = (std::max<uint64_t>)(delayMs, requestedDelayMs);
        delayMs = (std::min)(
            delayMs, kSourceRelayMaximumBackoffMs);
        _relayNextAttemptMs = relayDeadlineAfter(nowMs, delayMs);
        if (_relayRetryDeadlineMs != 0
            && _relayNextAttemptMs > _relayRetryDeadlineMs) {
            _relayNextAttemptMs = _relayRetryDeadlineMs;
        }
        _relayBackoffMs = (std::min)(
            _relayBackoffMs * 2u, kSourceRelayMaximumBackoffMs);
        _relayRegistrationState =
            SourceRelayRegistrationState::Backoff;
        if (!_relayRetryEventEmitted) {
            queueRelayEvent(
                SidecarTransportEventKind::RelaySourceRetrying);
            _relayRetryEventEmitted = true;
        }
    }

    void finishSourceRelayConnect(uint64_t nowMs) {
        if (_relayConnectingSocket == INVALID_SOCKET) {
            scheduleSourceRelayRetry(nowMs);
            return;
        }
        const SOCKET connected = _relayConnectingSocket;
        _relayConnectingSocket = INVALID_SOCKET;
        BOOL noDelay = TRUE;
        BOOL keepAlive = TRUE;
        (void)setsockopt(
            connected, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        (void)setsockopt(
            connected, SOL_SOCKET, SO_KEEPALIVE,
            reinterpret_cast<const char*>(&keepAlive),
            sizeof(keepAlive));
        tcp_keepalive keepAliveTiming{};
        keepAliveTiming.onoff = 1;
        keepAliveTiming.keepalivetime =
            kSourceRelayKeepAliveIdleMs;
        keepAliveTiming.keepaliveinterval =
            kSourceRelayKeepAliveIntervalMs;
        DWORD keepAliveBytes = 0;
        (void)WSAIoctl(
            connected, SIO_KEEPALIVE_VALS,
            &keepAliveTiming, sizeof(keepAliveTiming),
            nullptr, 0, &keepAliveBytes, nullptr, nullptr);
        _relayControl = qnet::TcpConn(connected);
        if (!_relayControl.sendMsg(
                qnet::relay::kTcpEnvelopeType,
                _relayHello.data(), _relayHello.size())) {
            scheduleSourceRelayRetry(nowMs);
            return;
        }
        _relayRegistrationState =
            SourceRelayRegistrationState::AwaitingWelcome;
        _relayAttemptDeadlineMs =
            relayDeadlineAfter(nowMs, kSourceRelayWelcomeDeadlineMs);
    }

    void beginSourceRelayConnect(uint64_t nowMs) {
        if (_relayAttempts >= kSourceRelayMaximumAttempts) {
            stopSourceRelayRegistration();
            return;
        }
        ++_relayAttempts;
        closePendingRelaySocket();
        _relayConnectingSocket =
            ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_relayConnectingSocket == INVALID_SOCKET) {
            scheduleSourceRelayRetry(nowMs);
            return;
        }
        u_long nonBlocking = 1;
        if (ioctlsocket(
                _relayConnectingSocket, FIONBIO, &nonBlocking)
            != 0) {
            scheduleSourceRelayRetry(nowMs);
            return;
        }

        const int connected = ::connect(
            _relayConnectingSocket,
            reinterpret_cast<const sockaddr*>(&_relayEndpoint.sa),
            sizeof(_relayEndpoint.sa));
        if (connected == 0) {
            finishSourceRelayConnect(nowMs);
            return;
        }
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK
            && error != WSAEINPROGRESS
            && error != WSAEALREADY) {
            scheduleSourceRelayRetry(nowMs);
            return;
        }
        _relayRegistrationState =
            SourceRelayRegistrationState::Connecting;
        _relayAttemptDeadlineMs =
            relayDeadlineAfter(nowMs, kSourceRelayConnectDeadlineMs);
    }

    void pollSourceRelayConnect(uint64_t nowMs) {
        if (_relayConnectingSocket == INVALID_SOCKET) {
            scheduleSourceRelayRetry(nowMs);
            return;
        }
        fd_set writable;
        fd_set exceptional;
        FD_ZERO(&writable);
        FD_ZERO(&exceptional);
        FD_SET(_relayConnectingSocket, &writable);
        FD_SET(_relayConnectingSocket, &exceptional);
        timeval noWait{};
        const int selected = select(
            0, nullptr, &writable, &exceptional, &noWait);
        if (selected == SOCKET_ERROR) {
            scheduleSourceRelayRetry(nowMs);
            return;
        }
        if (selected == 0) {
            if (nowMs >= _relayAttemptDeadlineMs)
                scheduleSourceRelayRetry(nowMs);
            return;
        }
        int socketError = 0;
        int socketErrorBytes = sizeof(socketError);
        if (getsockopt(
                _relayConnectingSocket, SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char*>(&socketError),
                &socketErrorBytes) != 0
            || socketError != 0
            || FD_ISSET(_relayConnectingSocket, &exceptional)) {
            scheduleSourceRelayRetry(nowMs);
            return;
        }
        finishSourceRelayConnect(nowMs);
    }

    bool acceptSourceRelayWelcome(
        const qnet::relay::SpectatorWelcome& welcome,
        uint64_t nowMs) {
        if (welcome.party != qnet::relay::SpectatorParty::Source
            || welcome.room != _relayConfig.room
            || welcome.sessionId != _relayConfig.identity.sessionId
            || welcome.sourceGeneration == 0
            || !qnet::relay::isValidId(welcome.registrationToken)
            || welcome.grantKey.size()
                   != qnet::relay::kGrantKeyBytes) {
            return false;
        }

        qnetplay::spectator::RelaySourceBinding binding{};
        binding.enabled = true;
        binding.room = _relayConfig.room;
        binding.token = welcome.registrationToken;
        binding.sourceGeneration = welcome.sourceGeneration;
        binding.identity = _relayConfig.identity;
        if (!_relaySource.configure(binding)) return false;

        std::copy(welcome.grantKey.begin(), welcome.grantKey.end(),
                  _relayGrantKey.begin());
        _relayRegistrationState =
            SourceRelayRegistrationState::Active;
        _relayAttemptDeadlineMs = 0;
        _relayNextAttemptMs = 0;
        _relayRetryDeadlineMs = 0;
        _relayAttempts = 0;
        _relayBackoffMs = kSourceRelayInitialBackoffMs;
        _relayRetryEventEmitted = false;
        queueRelayEvent(SidecarTransportEventKind::RelaySourceReady);

        std::vector<uint8_t> registration;
        if (_relaySource.encodeRegistration(
                nowMs, registration, true)) {
            (void)_socket.sendTo(
                _relayEndpoint, registration.data(), registration.size());
        }
        return true;
    }

    void pollSourceRelayWelcome(uint64_t nowMs) {
        qnet::TcpConn::Received received;
        const qnet::TcpConn::Rs result =
            _relayControl.recvMsgEx(received, 0);
        if (result == qnet::TcpConn::Rs::Timeout) {
            if (nowMs >= _relayAttemptDeadlineMs)
                scheduleSourceRelayRetry(nowMs);
            return;
        }
        if (result == qnet::TcpConn::Rs::Closed
            || result == qnet::TcpConn::Rs::Error) {
            scheduleSourceRelayRetry(nowMs);
            return;
        }
        if (result == qnet::TcpConn::Rs::Corrupt
            || received.type != qnet::relay::kTcpEnvelopeType) {
            stopSourceRelayRegistration();
            return;
        }

        qnet::relay::SpectatorWelcome welcome{};
        if (qnet::relay::decodeSpectatorWelcome(
                received.body.data(), received.body.size(), welcome)
            != qnet::relay::DecodeError::None) {
            stopSourceRelayRegistration();
            return;
        }
        switch (welcome.status) {
        case qnet::relay::SpectatorStatus::Accepted:
            if (!acceptSourceRelayWelcome(welcome, nowMs))
                stopSourceRelayRegistration();
            return;
        case qnet::relay::SpectatorStatus::SourceUnavailable:
        case qnet::relay::SpectatorStatus::CapacityReached:
        case qnet::relay::SpectatorStatus::RateLimited:
            scheduleSourceRelayRetry(nowMs, welcome.retryAfterMs);
            return;
        case qnet::relay::SpectatorStatus::RoomNotFound:
        case qnet::relay::SpectatorStatus::GrantRejected:
        case qnet::relay::SpectatorStatus::GrantExpired:
        case qnet::relay::SpectatorStatus::Superseded:
        case qnet::relay::SpectatorStatus::Unsupported:
        case qnet::relay::SpectatorStatus::Waiting:
            stopSourceRelayRegistration();
            return;
        }
        stopSourceRelayRegistration();
    }

    void pollActiveSourceRelay(uint64_t nowMs) {
        qnet::TcpConn::Received received;
        const qnet::TcpConn::Rs result =
            _relayControl.recvMsgEx(received, 0);
        if (result == qnet::TcpConn::Rs::Timeout) return;
        if (result == qnet::TcpConn::Rs::Closed
            || result == qnet::TcpConn::Rs::Error) {
            scheduleSourceRelayRetry(nowMs);
            return;
        }

        stopSourceRelayRegistration();
    }

    void serviceSourceRelay(uint64_t nowMs) {
        if (_relayRegistrationState
                == SourceRelayRegistrationState::Disabled
            || _relayRegistrationState
                == SourceRelayRegistrationState::Terminal) {
            return;
        }
        if (_relayRegistrationState
                == SourceRelayRegistrationState::Active) {
            pollActiveSourceRelay(nowMs);
            return;
        }
        if (_relayRetryDeadlineMs == 0) {
            _relayRetryDeadlineMs =
                relayDeadlineAfter(nowMs, kSourceRelayRetryWindowMs);
        }
        if (nowMs >= _relayRetryDeadlineMs) {
            stopSourceRelayRegistration();
            return;
        }

        switch (_relayRegistrationState) {
        case SourceRelayRegistrationState::Backoff:
            if (nowMs >= _relayNextAttemptMs)
                beginSourceRelayConnect(nowMs);
            return;
        case SourceRelayRegistrationState::Connecting:
            pollSourceRelayConnect(nowMs);
            return;
        case SourceRelayRegistrationState::AwaitingWelcome:
            pollSourceRelayWelcome(nowMs);
            return;
        case SourceRelayRegistrationState::Disabled:
        case SourceRelayRegistrationState::Active:
        case SourceRelayRegistrationState::Terminal:
            return;
        }
    }

    static bool sendAutopunch(void* context,
                              const qnet::Endpoint& destination,
                              const void* bytes,
                              size_t length) {
        auto* self = static_cast<UdpSidecarTransport*>(context);
        return self != nullptr && self->_socket.isOpen()
            && self->_socket.sendTo(destination, bytes, length);
    }

    qnet::UdpSock _socket;
    qnet::autopunch::AutopunchSession _autopunch;
    qnet::autopunch::AutopunchPeerSet _autopunchPeers;
    int _adoptionError = 0;
    SOCKET _relayConnectingSocket = INVALID_SOCKET;
    qnet::TcpConn _relayControl;
    qnet::Endpoint _relayEndpoint{};
    SourceRelayConfig _relayConfig{};
    std::vector<uint8_t> _relayHello;
    SourceRelayRegistrationState _relayRegistrationState =
        SourceRelayRegistrationState::Disabled;
    uint32_t _relayAttempts = 0;
    uint64_t _relayNextAttemptMs = 0;
    uint64_t _relayAttemptDeadlineMs = 0;
    uint64_t _relayRetryDeadlineMs = 0;
    uint64_t _relayBackoffMs = kSourceRelayInitialBackoffMs;
    bool _relayRetryEventEmitted = false;
    bool _relayTerminalEventEmitted = false;
    qnetplay::spectator::RelaySourceAdapter _relaySource;
    std::deque<SidecarTransportEvent> _transportEvents;
    qnetplay::spectator::SpectatorGrantKey _relayGrantKey{};
    uint64_t _lastTraversalMs = 0;
};

class PipePublicationChannel final : public PublicationChannel {
public:
    explicit PipePublicationChannel(HANDLE pipe) : _pipe(pipe) {
        _reader = std::thread([this] { readLoop(); });
    }
    ~PipePublicationChannel() override { close(); }

    int drain(std::vector<PublicationEvent>& out, size_t maxEvents) override {
        std::lock_guard<std::mutex> lock(_mutex);
        int taken = 0;
        while (!_queue.empty() && static_cast<size_t>(taken) < maxEvents) {
            out.push_back(std::move(_queue.front()));
            _queue.pop_front();
            ++taken;
        }
        if (taken != 0) return taken;
        if (_failed) return -1;
        if (_reconnecting.load()) {
            const uint64_t disconnectedAt = _disconnectedAtMs.load();
            const uint64_t now = GetTickCount64();
            if (disconnectedAt != 0
                && now >= disconnectedAt
                && now - disconnectedAt >= kReconnectAcceptMs) {
                _failed = true;
                return -1;
            }
        }
        return taken;
    }

    bool connected() const override { return _connected.load(); }
    bool reconnecting() const override { return _reconnecting.load(); }

    void close() override {
        if (_closing.exchange(true)) return;
        const HANDLE pipe = _pipe;
        if (pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(pipe, nullptr);
            DisconnectNamedPipe(pipe);
        }
        if (_reader.joinable()) _reader.join();
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        _pipe = INVALID_HANDLE_VALUE;
        _connected.store(false);
        _reconnecting.store(false);
    }

private:
    void fail() {
        std::lock_guard<std::mutex> lock(_mutex);
        _failed = true;
    }

    bool readExact(uint8_t* buffer, DWORD count) {
        DWORD total = 0;
        while (total < count) {
            DWORD read = 0;
            if (!ReadFile(_pipe, buffer + total, count - total, &read, nullptr)
                || read == 0)
                return false;
            total += read;
        }
        return true;
    }

    void readLoop() {
        std::vector<uint8_t> payload;
        std::vector<PublicationEvent> events;
        uint32_t expectedSequence = 0;
        bool sawHello = false;
        while (!_closing.load()) {
            if (!ConnectNamedPipe(_pipe, nullptr)
                && GetLastError() != ERROR_PIPE_CONNECTED) {
                if (!_closing.load()) fail();
                break;
            }
            _connected.store(true);
            _reconnecting.store(false);
            _disconnectedAtMs.store(0);

            bool fatal = false;
            while (!_closing.load()) {
                uint8_t header[kPublicationHeaderSize];
                if (!readExact(header, static_cast<DWORD>(sizeof(header))))
                    break;
                LeReader reader(header, sizeof(header));
                uint32_t magic = 0, sequence = 0, param0 = 0;
                uint32_t payloadBytes = 0, payloadCrc = 0, headerCrc = 0;
                uint16_t version = 0, kind = 0;
                uint64_t gameId = 0;
                if (!reader.u32(magic) || !reader.u16(version)
                    || !reader.u16(kind) || !reader.u32(sequence)
                    || !reader.u32(param0) || !reader.u32(payloadBytes)
                    || !reader.u32(payloadCrc) || !reader.u64(gameId)
                    || !reader.u32(headerCrc)) {
                    fail();
                    fatal = true;
                    break;
                }
                if (magic != kPublicationMagic
                    || version != kPublicationVersion
                    || payloadBytes > kMaxPublicationFrameBytes
                    || kind
                        < static_cast<uint16_t>(PublicationWireKind::Hello)
                    || kind > static_cast<uint16_t>(
                                   PublicationWireKind::ChainedSetEvent)
                    || qreplay::crc32c(header, 32u) != headerCrc) {
                    fail();
                    fatal = true;
                    break;
                }
                payload.resize(payloadBytes);
                if (payloadBytes != 0
                    && !readExact(payload.data(),
                                  static_cast<DWORD>(payloadBytes))) {
                    break;
                }
                const uint32_t computed =
                    payloadBytes == 0
                        ? 0u
                        : qreplay::crc32c(payload.data(), payloadBytes);
                if (computed != payloadCrc) {
                    fail();
                    fatal = true;
                    break;
                }

                const PublicationWireKind wireKind =
                    static_cast<PublicationWireKind>(kind);
                events.clear();
                if (wireKind == PublicationWireKind::Hello) {
                    if (sawHello) pushLossReport(events);
                    sawHello = true;
                    expectedSequence = sequence;
                } else if (sequence != expectedSequence + 1) {
                    pushLossReport(events);
                }
                expectedSequence = sequence;

                if (!expandPublicationFrame(
                        wireKind, param0, gameId, payload.data(),
                        payloadBytes, events)) {
                    fail();
                    fatal = true;
                    break;
                }
                if (events.empty()) continue;

                std::lock_guard<std::mutex> lock(_mutex);
                if (_queue.size() + events.size() > kMaxQueuedEvents) {
                    _failed = true;
                    fatal = true;
                    break;
                }
                for (PublicationEvent& event : events)
                    _queue.push_back(std::move(event));
            }

            if (_closing.load() || fatal) {
                _connected.store(false);
                break;
            }
            // Publish reconnect intent before dropping `connected`: the service thread must never see a false/false pair and mistake it for final EOF.
            _reconnecting.store(true);
            _disconnectedAtMs.store(GetTickCount64());
            _connected.store(false);
            DisconnectNamedPipe(_pipe);
        }
        _connected.store(false);
        _reconnecting.store(false);
    }

    static constexpr size_t kMaxQueuedEvents = 65536;
    static constexpr uint64_t kReconnectAcceptMs = 2000;

    HANDLE _pipe = INVALID_HANDLE_VALUE;
    std::thread _reader;
    std::mutex _mutex;
    std::deque<PublicationEvent> _queue;
    std::atomic<bool> _connected{false};
    std::atomic<bool> _reconnecting{false};
    std::atomic<uint64_t> _disconnectedAtMs{0};
    std::atomic<bool> _closing{false};
    bool _failed = false;
};

}

void setSidecarControlIngressEnabled(const bool enabled) {
    std::lock_guard<std::mutex> lock(g_sidecarControlMutex);
    g_sidecarControlEnabled = enabled;
    g_sidecarControlQueue.clear();
    if (enabled) g_sidecarControlQueueDrops = 0;
}

bool pollSidecarControlDatagram(SidecarControlDatagram& out) {
    std::lock_guard<std::mutex> lock(g_sidecarControlMutex);
    out = {};
    if (g_sidecarControlQueue.empty()) return false;
    out = std::move(g_sidecarControlQueue.front());
    g_sidecarControlQueue.pop_front();
    return true;
}

std::unique_ptr<SidecarTransport> makeUdpSidecarTransport(
    uintptr_t preboundSocket) {
    return std::unique_ptr<SidecarTransport>(
        new UdpSidecarTransport(preboundSocket));
}

struct ClientPrivateRuntimeAppliedReceiver::Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    uint32_t gamePid = 0u;
    uint32_t generation = 0u;
};

ClientPrivateRuntimeAppliedReceiver::ClientPrivateRuntimeAppliedReceiver()
    : _impl(new Impl()) {}

ClientPrivateRuntimeAppliedReceiver::~ClientPrivateRuntimeAppliedReceiver() {
    close();
}

void ClientPrivateRuntimeAppliedReceiver::close() {
    if (!_impl || _impl->pipe == INVALID_HANDLE_VALUE) return;
    (void)CancelIoEx(_impl->pipe, nullptr);
    (void)DisconnectNamedPipe(_impl->pipe);
    CloseHandle(_impl->pipe);
    _impl->pipe = INVALID_HANDLE_VALUE;
    _impl->gamePid = 0u;
    _impl->generation = 0u;
}

bool ClientPrivateRuntimeAppliedReceiver::open(
    const uint32_t gamePid, const uint32_t publisherGeneration,
    std::string& error) {
    error.clear();
    close();
    if (!_impl || gamePid == 0u || publisherGeneration == 0u) {
        error = "the client-private receipt endpoint needs a process and generation";
        return false;
    }
    wchar_t pipeName[128]{};
    qipc::ipcSpectatorClientPublisherPipeName(
        pipeName, sizeof(pipeName) / sizeof(pipeName[0]), gamePid,
        publisherGeneration);
    const HANDLE pipe = CreateNamedPipeW(
        pipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
            | PIPE_REJECT_REMOTE_CLIENTS,
        1, 0,
        static_cast<DWORD>(
            qipc::kSpectatorClientPublisherRuntimeAppliedReceiptBytes),
        0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        error = "the client-private RuntimeApplied pipe could not be created ("
            + std::to_string(GetLastError()) + ")";
        return false;
    }
    _impl->pipe = pipe;
    _impl->gamePid = gamePid;
    _impl->generation = publisherGeneration;
    return true;
}

namespace {

bool waitForClientPrivatePipeOperation(HANDLE pipe, OVERLAPPED& operation,
                                       const DWORD timeoutMs,
                                       DWORD& transferred,
                                       const char* operationName,
                                       std::string& error) {
    const DWORD waited = WaitForSingleObject(operation.hEvent, timeoutMs);
    if (waited == WAIT_OBJECT_0) {
        if (GetOverlappedResult(pipe, &operation, &transferred, FALSE))
            return true;
        error = std::string("the client-private RuntimeApplied ")
            + operationName + " failed (" + std::to_string(GetLastError())
            + ")";
        return false;
    }
    if (waited == WAIT_TIMEOUT) {
        (void)CancelIoEx(pipe, &operation);
        DWORD ignored = 0u;
        (void)GetOverlappedResult(pipe, &operation, &ignored, TRUE);
        error = std::string("the client-private RuntimeApplied ")
            + operationName + " timed out";
        return false;
    }
    (void)CancelIoEx(pipe, &operation);
    error = std::string("the client-private RuntimeApplied ")
        + operationName + " wait failed (" + std::to_string(GetLastError())
        + ")";
    return false;
}

DWORD remainingClientPrivateReceiptMs(const ULONGLONG deadline) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) return 0u;
    const ULONGLONG remaining = deadline - now;
    return static_cast<DWORD>((std::min)(
        remaining, static_cast<ULONGLONG>((std::numeric_limits<DWORD>::max)())));
}

}

bool ClientPrivateRuntimeAppliedReceiver::receive(
    const uint32_t timeoutMs,
    qipc::SpectatorClientPublisherRuntimeAppliedReceipt& out,
    std::string& error) {
    out = qipc::SpectatorClientPublisherRuntimeAppliedReceipt{};
    error.clear();
    if (!_impl || _impl->pipe == INVALID_HANDLE_VALUE || timeoutMs == 0u) {
        error = "the client-private RuntimeApplied receiver is not open";
        return false;
    }

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        error = "the client-private RuntimeApplied receipt event could not be created";
        close();
        return false;
    }
    OVERLAPPED operation{};
    operation.hEvent = event;
    DWORD transferred = 0u;
    bool connected = ConnectNamedPipe(_impl->pipe, &operation) != FALSE;
    if (!connected) {
        const DWORD connectError = GetLastError();
        if (connectError == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (connectError == ERROR_IO_PENDING) {
            const DWORD remaining = remainingClientPrivateReceiptMs(deadline);
            if (remaining != 0u) {
                connected = waitForClientPrivatePipeOperation(
                    _impl->pipe, operation, remaining, transferred,
                    "connection", error);
            } else {
                error = "the client-private RuntimeApplied connection timed out";
            }
        } else {
            error = "the client-private RuntimeApplied connection failed ("
                + std::to_string(connectError) + ")";
        }
    }
    if (!connected) {
        CloseHandle(event);
        close();
        return false;
    }

    uint8_t bytes[qipc::kSpectatorClientPublisherRuntimeAppliedReceiptBytes]{};
    size_t total = 0u;
    bool readOk = true;
    while (total < sizeof(bytes)) {
        ResetEvent(event);
        operation = {};
        operation.hEvent = event;
        transferred = 0u;
        const DWORD wanted = static_cast<DWORD>(sizeof(bytes) - total);
        const BOOL read = ReadFile(_impl->pipe, bytes + total, wanted,
                                   &transferred, &operation);
        if (!read) {
            const DWORD readError = GetLastError();
            if (readError == ERROR_IO_PENDING) {
                const DWORD remaining = remainingClientPrivateReceiptMs(deadline);
                if (remaining == 0u
                    || !waitForClientPrivatePipeOperation(
                        _impl->pipe, operation, remaining, transferred,
                        "read", error)) {
                    readOk = false;
                    break;
                }
            } else {
                error = "the client-private RuntimeApplied receipt read failed ("
                    + std::to_string(readError) + ")";
                readOk = false;
                break;
            }
        }
        if (transferred == 0u) {
            error = "the client-private RuntimeApplied receipt was truncated";
            readOk = false;
            break;
        }
        total += transferred;
    }
    CloseHandle(event);
    close();
    if (!readOk || total != sizeof(bytes)) return false;
    if (!qipc::decodeSpectatorClientPublisherRuntimeAppliedReceipt(
            bytes, sizeof(bytes), out)) {
        error = "the client-private RuntimeApplied receipt was malformed";
        return false;
    }
    return true;
}

std::unique_ptr<PublicationChannel> makePipePublicationChannel(
    uint32_t gamePid, std::string& error) {
    wchar_t name[128];
    qipc::ipcSpectatorSidecarPipeName(name, 128, gamePid);
    const HANDLE pipe = CreateNamedPipeW(
        name, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
        0, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        error = "the spectator publication pipe could not be created ("
              + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    return std::unique_ptr<PublicationChannel>(
        new PipePublicationChannel(pipe));
}

namespace {

std::mutex g_sidecarMutex;
std::unique_ptr<SpectatorSidecar> g_sidecar;
std::thread g_worker;
std::atomic<bool> g_workerStop{false};
SidecarStatus g_lastStatus;

uint64_t monotonicMs() { return qnet::nowUs() / 1000ull; }

struct SidecarProcessCleanup {
    ~SidecarProcessCleanup() {
        g_workerStop.store(true);
        if (g_worker.joinable()) g_worker.join();
        std::lock_guard<std::mutex> lock(g_sidecarMutex);
        g_sidecar.reset();
    }
};

SidecarProcessCleanup g_sidecarProcessCleanup;

void workerLoop() {
    while (!g_workerStop.load()) {
        {
            std::lock_guard<std::mutex> lock(g_sidecarMutex);
            if (g_sidecar) {
                g_sidecar->service(monotonicMs());
                g_lastStatus = g_sidecar->status();
            }
        }
        Sleep(4);
    }
}

}

static bool startSidecarInternal(const SidecarConfig& config,
                                 uint32_t gamePid,
                                 const bool clientPrivate,
                                 std::string& error) {
    clearSidecarControlQueue();
    // Main has already released this SOCKET, so this owner must close it exactly once on every early refusal.
    SidecarConfig effective = config;
    std::unique_ptr<SidecarTransport> transport =
        makeUdpSidecarTransport(effective.preboundSocket);
    effective.preboundSocket = kInvalidPreboundSpectatorSocket;
    if (effective.clientPrivateRoot.enabled != clientPrivate) {
        error = clientPrivate
            ? "the client-private sidecar entry point requires a private collector configuration"
            : "a client-private collector cannot use the public sidecar entry point";
        return false;
    }
    bool replaceRetainedOwner = false;
    {
        std::lock_guard<std::mutex> lock(g_sidecarMutex);
        if (g_sidecar) {
            const SidecarPhase phase = g_sidecar->phase();
            replaceRetainedOwner =
                phase == SidecarPhase::Retained
                || phase == SidecarPhase::Expired
                || phase == SidecarPhase::Failed;
            if (!replaceRetainedOwner) {
                error = "the spectator sidecar is already running";
                return false;
            }
        }
    }
    if (replaceRetainedOwner)
        (void)stopSidecar(false);

    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (g_sidecar) {
        error = "the previous spectator sidecar did not stop";
        return false;
    }
    if (!effective.diagnostics.sink) {
        effective.diagnostics.sink = [](const char* line) {
            QLOG("%s", line);
        };
    }

    std::unique_ptr<SpectatorSidecar> sidecar(
        new SpectatorSidecar(effective));

    std::unique_ptr<PublicationChannel> channel;
    if (gamePid != 0) {
        std::string channelError;
        channel = makePipePublicationChannel(gamePid, channelError);
        if (!channel) {
            error = channelError;
            return false;
        }
    }
    std::string startError;
    const bool ok = sidecar->start(std::move(transport), std::move(channel),
                                   monotonicMs(),
                                   startError);
    g_lastStatus = sidecar->status();
    if (!ok) {
        error = startError;
        g_sidecar = std::move(sidecar);
        return false;
    }
    g_sidecar = std::move(sidecar);
    g_workerStop.store(false);
    g_worker = std::thread(workerLoop);
    SetThreadPriority(g_worker.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
    return true;
}

bool startSidecar(const SidecarConfig& config, uint32_t gamePid,
                  std::string& error) {
    return startSidecarInternal(config, gamePid, false,
                                error);
}

bool startClientPrivateSidecar(const SidecarConfig& config,
                               uint32_t gamePid, std::string& error) {
    return startSidecarInternal(config, gamePid, true,
                                error);
}

SidecarStatus sidecarStatus() {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (g_sidecar) g_lastStatus = g_sidecar->status();
    return g_lastStatus;
}

bool sidecarConfirmClientPrivateRuntimeApplied(
    const qipc::IpcConfig& launchConfig,
    const qipc::SpectatorClientPublisherRuntimeAppliedReceipt& receipt,
    const uint32_t expectedProcessId, std::string& error) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar) {
        error = "the client-private spectator collector is not running";
        return false;
    }
    const bool confirmed = g_sidecar->confirmClientPrivateRuntimeApplied(
        launchConfig, receipt, expectedProcessId, error);
    g_lastStatus = g_sidecar->status();
    return confirmed;
}

void sealSidecar(TerminalReason reason, uint64_t postTailStateHash,
                 bool hasProof) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (g_sidecar)
        g_sidecar->requestSeal(reason, postTailStateHash, hasProof,
                               monotonicMs());
}

static void logDrainOutcomes(const SpectatorSidecar& sidecar, bool complete,
                             uint64_t waitedMs, uint64_t budgetMs) {
    const SidecarStatus status = sidecar.status();
    QLOG("[spectator.sidecar] drain %s after %llu ms (budget %llu ms): "
         "completed=%zu expired=%zu kicked=%zu detached=%zu stillAttached=%zu",
         complete ? "finished" : "hit its deadline",
         static_cast<unsigned long long>(waitedMs),
         static_cast<unsigned long long>(budgetMs), status.viewersCompleted,
         status.viewersDeadlineExpired, status.viewersKicked,
         status.viewersDetached, status.viewerCount);
    for (const SpectatorSidecar::TerminalOutcome& outcome :
         sidecar.terminalOutcomes()) {
        QLOG("[spectator.sidecar] viewer %016llX generation=%u %s at record %u",
             static_cast<unsigned long long>(outcome.viewerId),
             outcome.generation,
             viewerServiceStateName(outcome.state),
             outcome.acknowledgedRecord);
    }
}

bool stopSidecar(bool drain) {
    const uint64_t startedMs = monotonicMs();
    uint64_t budget = 0;
    uint64_t deadline = 0;
    {
        std::lock_guard<std::mutex> lock(g_sidecarMutex);
        if (!g_sidecar) return true;
        budget = drain ? g_sidecar->drainDeadlineMs() : 0ull;
        deadline = startedMs + budget;
    }
    bool complete = false;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(g_sidecarMutex);
            if (!g_sidecar) return true;
            complete = g_sidecar->stop(drain, monotonicMs());
            g_lastStatus = g_sidecar->status();
            if (complete || monotonicMs() >= deadline) break;
        }
        Sleep(10);
    }
    g_workerStop.store(true);
    if (g_worker.joinable()) g_worker.join();
    {
        std::lock_guard<std::mutex> lock(g_sidecarMutex);
        if (g_sidecar) {
            if (!complete) complete = g_sidecar->stop(false, monotonicMs());
            g_lastStatus = g_sidecar->status();
            if (drain)
                logDrainOutcomes(*g_sidecar, complete,
                                 monotonicMs() - startedMs, budget);
            g_sidecar.reset();
        }
    }
    clearSidecarControlQueue();
    return complete;
}

bool retainSidecarAfterGame() {
    const uint64_t startedMs = monotonicMs();
    uint64_t drainBudget = 0;
    {
        std::lock_guard<std::mutex> lock(g_sidecarMutex);
        if (!g_sidecar) return true;
        drainBudget = g_sidecar->drainDeadlineMs();
    }

    constexpr uint64_t kPublicationReconnectGraceMs = 2500;
    const uint64_t publicationDeadline =
        startedMs + kPublicationReconnectGraceMs;
    uint64_t sealedAtMs = 0;
    bool retained = false;
    for (;;) {
        const uint64_t now = monotonicMs();
        {
            std::lock_guard<std::mutex> lock(g_sidecarMutex);
            if (!g_sidecar) return true;
            g_sidecar->service(now);
            SidecarStatus status = g_sidecar->status();
            g_lastStatus = status;
            if (status.phase == SidecarPhase::Retained
                || status.phase == SidecarPhase::Expired) {
                retained = true;
                break;
            }
            if (status.phase == SidecarPhase::Failed) break;
            if (status.sealed && sealedAtMs == 0)
                sealedAtMs = now;
            if (!status.sealed && now >= publicationDeadline) {
                g_sidecar->requestSeal(
                    TerminalReason::ProtocolFailure,
                    0u, false, now);
                sealedAtMs = now;
                status = g_sidecar->status();
                g_lastStatus = status;
            }
            if (sealedAtMs != 0
                && now - sealedAtMs >= drainBudget) {
                g_sidecar->service(now);
                g_lastStatus = g_sidecar->status();
                retained =
                    g_lastStatus.phase == SidecarPhase::Retained
                    || g_lastStatus.phase == SidecarPhase::Expired;
                break;
            }
        }
        Sleep(10);
    }

    {
        std::lock_guard<std::mutex> lock(g_sidecarMutex);
        if (g_sidecar) {
            logDrainOutcomes(*g_sidecar, retained,
                             monotonicMs() - startedMs,
                             drainBudget);
        }
    }
    return retained;
}

bool sidecarSetAdmissionPolicy(AdmissionPolicy policy) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar) return false;
    g_sidecar->setAdmissionPolicy(policy);
    return true;
}

bool sidecarKickViewer(uint64_t viewerId) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    return g_sidecar && g_sidecar->kickViewer(viewerId, monotonicMs());
}

bool sidecarRevokeGrant(uint64_t grantId) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    return g_sidecar && g_sidecar->revokeGrant(grantId, monotonicMs());
}

bool sidecarReleaseViewerReservation(uint64_t viewerId) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    return g_sidecar
        && g_sidecar->releaseReservation(viewerId, monotonicMs());
}

bool sidecarNoteAdmittedViewer(uint32_t peerIpv4NetworkOrder,
                               uint16_t controlPort, uint16_t dataPort) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    return g_sidecar
        && g_sidecar->noteAdmittedViewer(peerIpv4NetworkOrder, controlPort,
                                         dataPort, monotonicMs());
}

bool sidecarApproveViewer(uint64_t viewerId) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    return g_sidecar
        && g_sidecar->approveViewer(viewerId, monotonicMs());
}

bool sidecarDeclineViewer(uint64_t viewerId) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    return g_sidecar
        && g_sidecar->declineViewer(viewerId, monotonicMs());
}

bool sidecarIssueGrant(const GrantRequest& request, SpectatorGrant& out,
                       AdmissionDenyReason& denyReason) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar) {
        denyReason = AdmissionDenyReason::ServiceFailed;
        return false;
    }
    return g_sidecar->issueGrant(request, out, denyReason, monotonicMs());
}

bool configureSidecarTopologyAuthority(
    const topology::TopologyActivationRequest& request,
    std::string& error) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar) {
        error = "the spectator sidecar is not running";
        return false;
    }
    const bool configured = g_sidecar->configureTopologyAuthority(
        request, monotonicMs(), error);
    g_lastStatus = g_sidecar->status();
    return configured;
}

bool sidecarEnrollTopologyViewer(
    const uint64_t viewerId, const uint32_t viewerGeneration,
    const bool relayCapable, const uint64_t nowMs,
    topology::TopologyEnrollment& out, std::string& error) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar) {
        out = {};
        error = "the spectator sidecar is not running";
        return false;
    }
    const uint64_t effectiveNow = nowMs == 0u ? monotonicMs() : nowMs;
    const bool enrolled = g_sidecar->enrollTopologyViewer(
        viewerId, viewerGeneration, relayCapable, effectiveNow, out, error);
    g_lastStatus = g_sidecar->status();
    return enrolled;
}

bool sidecarEnrollTopologyViewerExact(
    const topology::TopologyViewerClaim& claim,
    const SpectatorGrant& grant, const uint64_t nowMs,
    topology::TopologyEnrollment& out, std::string& error) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar) {
        out = {};
        error = "the spectator sidecar is not running";
        return false;
    }
    const uint64_t effectiveNow = nowMs == 0u ? monotonicMs() : nowMs;
    const bool enrolled = g_sidecar->enrollTopologyViewerExact(
        claim, grant, effectiveNow, out, error);
    g_lastStatus = g_sidecar->status();
    return enrolled;
}

bool sidecarEnrollClientPlayerRoot(
    const topology::ClientPlayerRootClaim& claim, const uint64_t nowMs,
    topology::ClientPlayerRootEnrollment& out,
    std::vector<uint8_t>& encodedGrant, std::string& error) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    out = {};
    encodedGrant.clear();
    if (!g_sidecar) {
        error = "the spectator sidecar is not running";
        return false;
    }
    const uint64_t effectiveNow = nowMs == 0u ? monotonicMs() : nowMs;
    const bool enrolled = g_sidecar->enrollClientPlayerRoot(
        claim, effectiveNow, out, encodedGrant, error);
    g_lastStatus = g_sidecar->status();
    return enrolled;
}

bool sidecarClientPrivateRootStreamFacts(ClientPrivateRootStreamFacts& out) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    out = {};
    if (!g_sidecar) return false;
    return g_sidecar->clientPrivateRootStreamFacts(out);
}

bool sidecarPrepareClientPlayerRootQsr(uint16_t& outPort,
                                       std::string& error) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    outPort = 0u;
    if (!g_sidecar) {
        error = "the spectator sidecar is not running";
        return false;
    }
    const bool prepared = g_sidecar->prepareClientPlayerRootQsr(outPort,
                                                               error);
    g_lastStatus = g_sidecar->status();
    return prepared;
}

bool sidecarArmClientPlayerRoot(const ClientPlayerRootArmFacts& facts,
                                std::string& error) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar) {
        error = "the spectator sidecar is not running";
        return false;
    }
    const bool armed = g_sidecar->armClientPlayerRoot(facts, monotonicMs(),
                                                      error);
    g_lastStatus = g_sidecar->status();
    return armed;
}

void sidecarWithdrawClientPlayerRoot(const char* reason) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar) return;
    g_sidecar->withdrawClientPlayerRoot(reason);
    g_lastStatus = g_sidecar->status();
}

bool sidecarIssueTopologyRelayChildAssignment(
    const qnetplay::spectator::resilient::NodeId relayNodeId,
    const qnetplay::spectator::resilient::NodeId childNodeId,
    std::string& error) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar) {
        error = "the spectator sidecar is not running";
        return false;
    }
    const bool issued = g_sidecar->issueTopologyRelayChildAssignment(
        relayNodeId, childNodeId, monotonicMs(), error);
    g_lastStatus = g_sidecar->status();
    return issued;
}

bool sidecarIssueTopologyRelayChildActivationForViewers(
    const TopologyViewerSelection& relay,
    const TopologyViewerSelection& child, std::string& error) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar) {
        error = "the spectator sidecar is not running";
        return false;
    }
    const bool issued = g_sidecar->issueTopologyRelayChildActivationForViewers(
        relay, child, monotonicMs(), error);
    g_lastStatus = g_sidecar->status();
    return issued;
}

bool sidecarIssueClientPlayerRootActivationForViewer(
    const TopologyViewerSelection& target, std::string& error) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar) {
        error = "the spectator sidecar is not running";
        return false;
    }
    const bool issued = g_sidecar->issueClientPlayerRootActivationForViewer(
        target, monotonicMs(), error);
    g_lastStatus = g_sidecar->status();
    return issued;
}

void sidecarDetachTopologyViewer(const uint64_t viewerId) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar || viewerId == 0u) return;
    g_sidecar->rollbackTopologyViewer(viewerId);
    g_lastStatus = g_sidecar->status();
}

bool sidecarVerifyTopologyViewerGrant(
    const SpectatorGrant& grant, const uint32_t viewerGeneration,
    const uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (!g_sidecar) return false;
    const uint64_t effectiveNow = nowMs == 0u ? monotonicMs() : nowMs;
    return g_sidecar->verifyTopologyViewerGrant(
        grant, viewerGeneration, effectiveNow);
}

bool sidecarRunning() {
    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    return static_cast<bool>(g_sidecar);
}

#else

struct ClientPrivateRuntimeAppliedReceiver::Impl {};

ClientPrivateRuntimeAppliedReceiver::ClientPrivateRuntimeAppliedReceiver()
    : _impl(new Impl()) {}
ClientPrivateRuntimeAppliedReceiver::~ClientPrivateRuntimeAppliedReceiver() =
    default;
bool ClientPrivateRuntimeAppliedReceiver::open(uint32_t, uint32_t,
                                               std::string& error) {
    error = "the client-private RuntimeApplied pipe is Windows-only";
    return false;
}
bool ClientPrivateRuntimeAppliedReceiver::receive(
    uint32_t, qipc::SpectatorClientPublisherRuntimeAppliedReceipt& out,
    std::string& error) {
    out = qipc::SpectatorClientPublisherRuntimeAppliedReceipt{};
    error = "the client-private RuntimeApplied pipe is Windows-only";
    return false;
}
void ClientPrivateRuntimeAppliedReceiver::close() {}

std::unique_ptr<SidecarTransport> makeUdpSidecarTransport(uintptr_t) {
    return nullptr;
}

std::unique_ptr<PublicationChannel> makePipePublicationChannel(
    uint32_t, std::string& error) {
    error = "the spectator publication pipe is Windows-only";
    return nullptr;
}

bool startSidecar(const SidecarConfig&, uint32_t, std::string& error) {
    error = "the spectator sidecar is Windows-only";
    return false;
}
bool startClientPrivateSidecar(const SidecarConfig&, uint32_t,
                               std::string& error) {
    error = "the client-private spectator sidecar is Windows-only";
    return false;
}
void setSidecarControlIngressEnabled(bool) {}
bool pollSidecarControlDatagram(SidecarControlDatagram& out) {
    out = {};
    return false;
}
SidecarStatus sidecarStatus() { return SidecarStatus{}; }
bool sidecarConfirmClientPrivateRuntimeApplied(
    const qipc::IpcConfig&,
    const qipc::SpectatorClientPublisherRuntimeAppliedReceipt&,
    uint32_t, std::string& error) {
    error = "the client-private spectator sidecar is Windows-only";
    return false;
}
void sealSidecar(TerminalReason, uint64_t, bool) {}
bool stopSidecar(bool) { return true; }
bool retainSidecarAfterGame() { return true; }
bool sidecarSetAdmissionPolicy(AdmissionPolicy) { return false; }
bool sidecarKickViewer(uint64_t) { return false; }
bool sidecarRevokeGrant(uint64_t) { return false; }
bool sidecarReleaseViewerReservation(uint64_t) { return false; }
bool sidecarNoteAdmittedViewer(uint32_t, uint16_t, uint16_t) { return false; }
bool sidecarApproveViewer(uint64_t) { return false; }
bool sidecarDeclineViewer(uint64_t) { return false; }
bool sidecarIssueGrant(const GrantRequest&, SpectatorGrant&,
                       AdmissionDenyReason& denyReason) {
    denyReason = AdmissionDenyReason::ServiceFailed;
    return false;
}
bool sidecarVerifyTopologyViewerGrant(const SpectatorGrant&, uint32_t,
                                      uint64_t) {
    return false;
}
bool configureSidecarTopologyAuthority(
    const topology::TopologyActivationRequest&, std::string& error) {
    error = "the spectator sidecar is Windows-only";
    return false;
}
bool sidecarEnrollTopologyViewer(
    uint64_t, uint32_t, bool, uint64_t,
    topology::TopologyEnrollment& out, std::string& error) {
    out = {};
    error = "the spectator sidecar is Windows-only";
    return false;
}
bool sidecarEnrollTopologyViewerExact(
    const topology::TopologyViewerClaim&, const SpectatorGrant&, uint64_t,
    topology::TopologyEnrollment& out, std::string& error) {
    out = {};
    error = "the spectator sidecar is Windows-only";
    return false;
}
bool sidecarEnrollClientPlayerRoot(
    const topology::ClientPlayerRootClaim&, uint64_t,
    topology::ClientPlayerRootEnrollment& out,
    std::vector<uint8_t>& encodedGrant, std::string& error) {
    out = {};
    encodedGrant.clear();
    error = "the spectator sidecar is Windows-only";
    return false;
}
bool sidecarClientPrivateRootStreamFacts(ClientPrivateRootStreamFacts& out) {
    out = {};
    return false;
}
bool sidecarPrepareClientPlayerRootQsr(uint16_t& outPort,
                                       std::string& error) {
    outPort = 0u;
    error = "the spectator sidecar is Windows-only";
    return false;
}
bool sidecarArmClientPlayerRoot(const ClientPlayerRootArmFacts&,
                                std::string& error) {
    error = "the spectator sidecar is Windows-only";
    return false;
}
void sidecarWithdrawClientPlayerRoot(const char*) {}
bool sidecarIssueTopologyRelayChildAssignment(
    qnetplay::spectator::resilient::NodeId,
    qnetplay::spectator::resilient::NodeId, std::string& error) {
    error = "the spectator sidecar is Windows-only";
    return false;
}
bool sidecarIssueTopologyRelayChildActivationForViewers(
    const TopologyViewerSelection&, const TopologyViewerSelection&,
    std::string& error) {
    error = "the spectator sidecar is Windows-only";
    return false;
}
bool sidecarIssueClientPlayerRootActivationForViewer(
    const TopologyViewerSelection&, std::string& error) {
    error = "the spectator sidecar is Windows-only";
    return false;
}
void sidecarDetachTopologyViewer(uint64_t) {}
bool sidecarRunning() { return false; }

#endif

}
