#include "game/spectator/SpectatorPublisher.h"
#include "game/spectator/SpectatorReplayPublisherV4.h"

#include "Log.h"
#include "replay/ReplayCodec.h"
#include "spectator/SpectatorCheckpoint.h"
#include "replay/ReplayIntegrity.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

namespace qgame::spectator_publish {
namespace {

using qnetplay::spectator::ConfirmedRecord;

constexpr size_t kEncodedRecordBytes = 32u;

struct CheckpointSlot {
    std::atomic<uint8_t> state{0u};
    uint32_t record = 0u;
    uint32_t bytes = 0u;
    std::vector<uint8_t> buffer;
};

struct PublisherState {
    std::atomic<bool> active{false};
    std::atomic<bool> disabled{false};
    std::atomic<uint16_t> disableReason{0u};
    std::atomic<bool> stopRequested{false};

    BoundedSpscRing<ConfirmedRecord, kConfirmedRingCapacity> ring;

    std::unique_ptr<ByteChannel> channel;
    void* threadHandle = nullptr;
    bool threaded = false;
    SidecarPublisherOptions options{};

    uint64_t gameId = 0u;
    qnetplay::spectator::SessionIdentity sourceIdentity{};
    std::atomic<uint64_t> published{0u};
    std::atomic<uint64_t> dropped{0u};
    std::atomic<uint64_t> written{0u};
    std::atomic<uint64_t> checkpointsPublished{0u};
    std::atomic<uint64_t> checkpointsDropped{0u};
    std::atomic<uint64_t> checkpointsRejected{0u};
    std::atomic<uint8_t> connected{0u};

    std::atomic<bool> startPending{false};
    std::atomic<bool> startSent{false};
    SidecarStartInfo startInfo{};
    std::vector<uint8_t> startMetadata;

    std::atomic<bool> sealPending{false};
    std::atomic<bool> sealSent{false};
    std::atomic<uint8_t> sealClaim{0u};
    std::atomic<uint16_t> sealReason{0u};
    std::atomic<uint32_t> sealCursor{0u};
    std::atomic<uint64_t> sealPrefixDigest{0u};
    std::atomic<uint64_t> sealHash{0u};

    std::atomic<bool> faultPending{false};
    std::atomic<bool> faultSent{false};
    std::atomic<uint8_t> faultClaim{0u};
    std::atomic<uint16_t> faultReason{0u};
    std::atomic<uint32_t> faultCursor{0u};
    std::atomic<uint64_t> faultPrefixDigest{0u};
    std::atomic<uint64_t> faultPostTailHash{0u};

    std::atomic<uint64_t> producerPrefixDigest{0u};

    // SPSC. Each entry is folded into producerPrefixDigest at enqueue, so check space before the fold, never after.
    static constexpr size_t kChainedEventSlots = 32u;
    qnetplay::spectator::SetEvent chainedEvents[kChainedEventSlots]{};
    std::atomic<uint32_t> chainedEventHead{0u};
    std::atomic<uint32_t> chainedEventTail{0u};
    std::atomic<uint32_t> producedEvents{0u};

    std::atomic<uint32_t> tailProofCursor{0u};
    std::atomic<uint64_t> tailProofHash{0u};
    std::atomic<uint64_t> tailProofsWritten{0u};

    std::unique_ptr<ConnectedPublisherQueueV4> connectedQueue;
    std::unique_ptr<ConnectedReplayPublisherV4> connectedPublisher;
    std::atomic<bool> connectedDescriptorPending{false};
    std::atomic<bool> connectedDescriptorSent{false};
    qnetplay::spectator::ConnectedSetDescriptorV4 connectedDescriptor{};
    std::vector<uint8_t> connectedDescriptorBytes;
    std::atomic<uint64_t> connectedPublicationsWritten{0u};

    std::atomic<bool> overflowUnreported{false};

    std::unique_ptr<CheckpointSlot[]> checkpoints;
    size_t checkpointCount = 0u;
    size_t checkpointCursor = 0u;

    uint32_t sequence = 0u;
    uint32_t reconnectAttempts = 0u;
    uint64_t firstFailureMs = 0u;
    uint64_t lastAttemptMs = 0u;
    uint64_t lastRecordWriteMs = 0u;
    uint32_t sentTailProofCursor = 0u;
    uint64_t sentTailProofHash = 0u;
    bool tailProofSent = false;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> header;
};

PublisherState& st() {
    static PublisherState state;
    return state;
}

uint64_t nowMs() { return static_cast<uint64_t>(GetTickCount64()); }

bool latchPublicationFault(PublishDisableReason reason,
                           uint32_t cursor,
                           uint64_t prefixDigest,
                           uint64_t postTailStateHash);

void disablePublication(PublishDisableReason reason,
                        uint64_t postTailStateHash = 0u) {
    PublisherState& s = st();
    bool expected = false;
    if (s.disabled.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
        s.disableReason.store(static_cast<uint16_t>(reason),
                              std::memory_order_release);
        s.overflowUnreported.store(true, std::memory_order_release);
        const uint64_t cursor64 =
            s.published.load(std::memory_order_acquire);
        const uint32_t cursor =
            cursor64 > UINT32_MAX ? UINT32_MAX
                                 : static_cast<uint32_t>(cursor64);
        if (postTailStateHash == 0u
            && s.tailProofCursor.load(std::memory_order_acquire) == cursor) {
            postTailStateHash =
                s.tailProofHash.load(std::memory_order_acquire);
        }
        (void)latchPublicationFault(
            reason, cursor,
            s.producerPrefixDigest.load(std::memory_order_acquire),
            postTailStateHash);
    }
}

enum : uint8_t {
    kFaultOpen = 0u,
    kFaultClaiming = 1u,
    kFaultReady = 2u,
};

bool latchPublicationFault(PublishDisableReason reason,
                           uint32_t cursor,
                           uint64_t prefixDigest,
                           uint64_t postTailStateHash) {
    PublisherState& s = st();
    static_assert(static_cast<uint16_t>(PublishDisableReason::InvalidRecord)
                      <= UINT16_MAX,
                  "fault reason must fit the wire");
    uint8_t expected = kFaultOpen;
    if (!s.faultClaim.compare_exchange_strong(expected, kFaultClaiming,
                                              std::memory_order_acq_rel)) {
        return false;
    }
    s.faultReason.store(static_cast<uint16_t>(reason),
                        std::memory_order_relaxed);
    s.faultCursor.store(cursor, std::memory_order_relaxed);
    s.faultPrefixDigest.store(prefixDigest, std::memory_order_relaxed);
    s.faultPostTailHash.store(postTailStateHash, std::memory_order_relaxed);
    s.faultPending.store(true, std::memory_order_release);
    s.faultClaim.store(kFaultReady, std::memory_order_release);
    return true;
}

void encodeHeader(std::vector<uint8_t>& out, SidecarMessageKind kind,
                  uint32_t sequence, uint32_t param0, uint32_t payloadBytes,
                  uint32_t payloadCrc, uint64_t gameId) {
    const size_t base = out.size();
    qreplay::putU32(out, kSidecarMagic);
    qreplay::putU16(out, kSidecarSchemaVersion);
    qreplay::putU16(out, static_cast<uint16_t>(kind));
    qreplay::putU32(out, sequence);
    qreplay::putU32(out, param0);
    qreplay::putU32(out, payloadBytes);
    qreplay::putU32(out, payloadCrc);
    qreplay::putU64(out, gameId);
    qreplay::putU32(out, qreplay::crc32c(out.data() + base, 32u));
}

bool sendMessage(SidecarMessageKind kind, uint32_t param0,
                 const std::vector<uint8_t>& payload) {
    PublisherState& s = st();
    if (!s.channel || !s.channel->connected()) return false;
    if (payload.size() > kMaxSidecarPayloadBytes) return false;
    const uint32_t crc = payload.empty()
        ? 0u
        : qreplay::crc32c(payload.data(), payload.size());
    s.header.clear();
    encodeHeader(s.header, kind, ++s.sequence, param0,
                 static_cast<uint32_t>(payload.size()), crc, s.gameId);
    if (s.header.size() != kSidecarHeaderBytes) return false;
    if (!s.channel->write(s.header.data(), s.header.size())) return false;
    if (!payload.empty() && !s.channel->write(payload.data(), payload.size()))
        return false;
    return true;
}

void encodeRecord(std::vector<uint8_t>& out, const ConfirmedRecord& record) {
    qreplay::putU32(out, record.record);
    qreplay::putU32(out, record.timelineFrame);
    qreplay::putU16(out, record.playerOneInput);
    qreplay::putU16(out, record.playerTwoInput);
    qreplay::putU32(out, 0u);
    qreplay::putU64(out, record.stateHash);
    qreplay::putU64(out, record.rollingDigest);
}

bool pumpOnce() {
    PublisherState& s = st();
    if (!s.active.load(std::memory_order_acquire)) return false;

    if (s.overflowUnreported.exchange(false, std::memory_order_acq_rel)) {
        QLOG("[spectator.publish] disabled reason=%s published=%llu "
             "dropped=%llu written=%llu highWater=%u",
             publishDisableReasonText(static_cast<PublishDisableReason>(
                 s.disableReason.load(std::memory_order_acquire))),
             static_cast<unsigned long long>(
                 s.published.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 s.dropped.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 s.written.load(std::memory_order_relaxed)),
             s.ring.highWater());
    }

    if (!s.channel) return false;

    if (!s.channel->connected()) {
        const uint64_t now = nowMs();
        if (s.firstFailureMs == 0u) s.firstFailureMs = now;
        if (s.lastAttemptMs != 0u
            && now - s.lastAttemptMs < s.options.reconnectBackoffMs) {
            return false;
        }
        s.lastAttemptMs = now == 0u ? 1u : now;
        if (s.channel->connect(s.options.connectTimeoutMs)) {
            s.reconnectAttempts = 0u;
            s.firstFailureMs = 0u;
            s.lastAttemptMs = 0u;
            s.sequence = 0u;
            s.connected.store(1u, std::memory_order_release);
            std::vector<uint8_t> hello;
            qreplay::putU64(hello, s.gameId);
            qreplay::putU32(hello, kSidecarSchemaVersion);
            qreplay::putU32(hello, 0u);
            if (!sendMessage(SidecarMessageKind::Hello, 0u, hello)) {
                s.channel->close();
                s.connected.store(0u, std::memory_order_release);
                return false;
            }
            s.startSent.store(false, std::memory_order_release);
            s.connectedDescriptorSent.store(
                false, std::memory_order_release);
            s.tailProofSent = false;
            s.lastRecordWriteMs = now;
        } else {
            ++s.reconnectAttempts;
            const bool attemptsGone =
                s.reconnectAttempts >= s.options.maxReconnectAttempts;
            const bool deadlineGone =
                (now - s.firstFailureMs) >= s.options.reconnectDeadlineMs;
            if (attemptsGone && deadlineGone)
                disablePublication(PublishDisableReason::ChannelFailed);
            return false;
        }
    }

    const auto fail = [&s]() {
        s.channel->close();
        s.connected.store(0u, std::memory_order_release);
        disablePublication(PublishDisableReason::ChannelFailed);
    };

    if (s.startPending.load(std::memory_order_acquire)
        && !s.startSent.load(std::memory_order_acquire)) {
        s.payload.clear();
        qreplay::putU64(s.payload, s.startInfo.gameId);
        qreplay::putU64(s.payload, s.startInfo.simulationContract);
        qreplay::putU64(s.payload, s.startInfo.initialStateHash);
        qreplay::putU64(s.payload, s.startInfo.initialDigest);
        qreplay::putU32(s.payload, s.startInfo.firstTimelineFrame);
        qreplay::putU16(s.payload, s.startInfo.playerOneDelay);
        qreplay::putU16(s.payload, s.startInfo.playerTwoDelay);
        qreplay::putU16(s.payload, s.startInfo.rollbackWindow);
        qreplay::putU16(s.payload, 0u);
        qreplay::putU32(s.payload,
                        static_cast<uint32_t>(s.startMetadata.size()));
        if (!s.startMetadata.empty())
            qreplay::putBytes(s.payload, s.startMetadata.data(),
                              s.startMetadata.size());
        if (!sendMessage(SidecarMessageKind::Start, 0u, s.payload)) {
            fail();
            return false;
        }
        s.startSent.store(true, std::memory_order_release);
    }

    if (s.connectedDescriptorPending.load(std::memory_order_acquire)
        && !s.connectedDescriptorSent.load(std::memory_order_acquire)) {
        if (s.connectedDescriptorBytes.empty()
            || !sendMessage(
                SidecarMessageKind::ConnectedDescriptorV4, 0u,
                s.connectedDescriptorBytes)) {
            fail();
            return false;
        }
        s.connectedDescriptorSent.store(
            true, std::memory_order_release);
    }

    size_t connectedDrained = 0u;
    if (s.connectedDescriptorSent.load(std::memory_order_acquire)
        && s.connectedQueue) {
        qnetplay::spectator::ConnectedPublicationV4 publication{};
        while (connectedDrained < s.options.drainPerTurn
               && s.connectedQueue->pop(publication)) {
            s.payload.clear();
            if (!qnetplay::spectator::encodeConnectedPublicationV4(
                    publication, s.payload)
                || !sendMessage(
                    SidecarMessageKind::ConnectedPublicationV4,
                    qnetplay::spectator::firstItemSequenceV4(
                        publication),
                    s.payload)) {
                fail();
                return false;
            }
            ++connectedDrained;
            s.connectedPublicationsWritten.fetch_add(
                1u, std::memory_order_relaxed);
        }
    }

    const bool publicationFault =
        s.faultPending.load(std::memory_order_acquire);
    const PublishDisableReason faultReason = publicationFault
        ? static_cast<PublishDisableReason>(
              s.faultReason.load(std::memory_order_acquire))
        : PublishDisableReason::None;
    const bool faultCanDrainAcceptedPrefix =
        publicationFault && faultReason != PublishDisableReason::ChannelFailed;

    const auto sendPublicationFault = [&s]() {
        s.payload.clear();
        const uint16_t reason =
            s.faultReason.load(std::memory_order_acquire);
        const uint32_t cursor =
            s.faultCursor.load(std::memory_order_acquire);
        const uint64_t prefix =
            s.faultPrefixDigest.load(std::memory_order_acquire);
        const uint64_t postTail =
            s.faultPostTailHash.load(std::memory_order_acquire);
        qreplay::putU16(s.payload, reason);
        qreplay::putU16(s.payload, 0u);
        qreplay::putU32(s.payload, cursor);
        qreplay::putU64(s.payload, prefix);
        qreplay::putU64(s.payload, postTail);
        if (!sendMessage(SidecarMessageKind::PublicationFault,
                         reason, s.payload)) {
            return false;
        }
        s.faultSent.store(true, std::memory_order_release);
        QLOG("[spectator.publish] FAULT reason=%s cursor=%u "
             "prefix=%016llX postTail=%016llX published=%llu written=%llu",
             publishDisableReasonText(
                 static_cast<PublishDisableReason>(reason)),
             cursor, static_cast<unsigned long long>(prefix),
             static_cast<unsigned long long>(postTail),
             static_cast<unsigned long long>(
                 s.published.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 s.written.load(std::memory_order_relaxed)));
        return true;
    };

    if (publicationFault && !faultCanDrainAcceptedPrefix
        && !s.faultSent.load(std::memory_order_acquire)) {
        if (!sendPublicationFault()) {
            s.channel->close();
            s.connected.store(0u, std::memory_order_release);
            return false;
        }
        return false;
    }

    size_t drained = 0u;
    size_t drainLimit = s.options.drainPerTurn;
    uint64_t terminalCursor = UINT64_MAX;
    if (publicationFault && faultCanDrainAcceptedPrefix) {
        terminalCursor =
            s.faultCursor.load(std::memory_order_acquire);
    } else if (s.sealPending.load(std::memory_order_acquire)) {
        terminalCursor =
            s.sealCursor.load(std::memory_order_acquire);
    }
    const uint64_t writtenBefore =
        s.written.load(std::memory_order_acquire);
    if (terminalCursor != UINT64_MAX) {
        if (writtenBefore >= terminalCursor) {
            drainLimit = 0u;
        } else {
            const uint64_t remaining = terminalCursor - writtenBefore;
            if (remaining < drainLimit)
                drainLimit = static_cast<size_t>(remaining);
        }
    }
    // An event stamped at cursor C must reach the sidecar before record C; split batches at event boundaries, never reorder.
    s.payload.clear();
    ConfirmedRecord record{};
    size_t batched = 0u;
    uint64_t sentTotal = 0u;
    const auto flushBatch = [&]() -> bool {
        if (batched == 0u) return true;
        if (!sendMessage(SidecarMessageKind::Confirmed,
                         static_cast<uint32_t>(batched), s.payload)) {
            return false;
        }
        s.written.fetch_add(batched, std::memory_order_relaxed);
        sentTotal += batched;
        batched = 0u;
        s.payload.clear();
        s.lastRecordWriteMs = nowMs();
        return true;
    };
    const auto nextChainedEvent =
        [&](qnetplay::spectator::SetEvent& out) -> bool {
        const uint32_t tail =
            s.chainedEventTail.load(std::memory_order_relaxed);
        if (tail == s.chainedEventHead.load(std::memory_order_acquire))
            return false;
        out = s.chainedEvents[tail % PublisherState::kChainedEventSlots];
        return true;
    };
    const auto sendChainedEvent =
        [&](const qnetplay::spectator::SetEvent& event) -> bool {
        qnetplay::spectator::SetEventBatchPayload batch{};
        batch.gameId = s.gameId;
        batch.baseDigest = event.recordDigest;
        batch.startEvent = event.eventIndex;
        batch.events.push_back(event);
        std::vector<uint8_t> encoded;
        if (!qnetplay::spectator::encodePayload(batch, encoded)
            || !sendMessage(SidecarMessageKind::ChainedSetEvent, 1u,
                            encoded)) {
            return false;
        }
        s.chainedEventTail.fetch_add(1u, std::memory_order_release);
        return true;
    };

    while (drained < drainLimit) {
        qnetplay::spectator::SetEvent event{};
        const uint64_t frontier =
            s.written.load(std::memory_order_relaxed) + batched;
        if (nextChainedEvent(event) && event.recordCursor <= frontier) {
            if (!flushBatch() || !sendChainedEvent(event)) {
                fail();
                return false;
            }
            continue;
        }
        if (!s.ring.pop(record)) break;
        encodeRecord(s.payload, record);
        ++batched;
        ++drained;
    }
    for (;;) {
        qnetplay::spectator::SetEvent event{};
        const uint64_t frontier =
            s.written.load(std::memory_order_relaxed) + batched;
        if (!nextChainedEvent(event) || event.recordCursor > frontier)
            break;
        if (!flushBatch() || !sendChainedEvent(event)) {
            fail();
            return false;
        }
    }
    if (!flushBatch()) {
        fail();
        return false;
    }
    const size_t drainedTotal = drained;
    (void)drainedTotal;
    drained = static_cast<size_t>(sentTotal);

    if (publicationFault
        && !s.faultSent.load(std::memory_order_acquire)) {
        const uint64_t cursor =
            s.faultCursor.load(std::memory_order_acquire);
        if (!faultCanDrainAcceptedPrefix
            || s.written.load(std::memory_order_acquire) == cursor) {
            if (!sendPublicationFault()) {
                s.channel->close();
                s.connected.store(0u, std::memory_order_release);
                return false;
            }
        }
        return drained > 0u;
    }

    for (size_t i = 0; i < s.checkpointCount; ++i) {
        CheckpointSlot& slot = s.checkpoints[i];
        if (slot.state.load(std::memory_order_acquire) != 1u) continue;
        s.payload.clear();
        qreplay::putU32(s.payload, slot.record);
        qreplay::putU32(s.payload, slot.bytes);
        qreplay::putBytes(s.payload, slot.buffer.data(), slot.bytes);
        const bool ok =
            sendMessage(SidecarMessageKind::Checkpoint, slot.record, s.payload);
        slot.state.store(0u, std::memory_order_release);
        QLOG("[spectator.publish] checkpoint frame record=%u bytes=%u "
             "sent=%d", slot.record, slot.bytes, ok ? 1 : 0);
        if (!ok) {
            fail();
            return false;
        }
    }

    if (!s.sealPending.load(std::memory_order_acquire)) {
        const uint32_t cursor = s.tailProofCursor.load(std::memory_order_acquire);
        const uint64_t hash = s.tailProofHash.load(std::memory_order_acquire);
        const uint64_t now = nowMs();
        const bool fresh = !s.tailProofSent
                        || cursor != s.sentTailProofCursor
                        || hash != s.sentTailProofHash;
        if (cursor != 0u && hash != 0u && fresh
            && s.startSent.load(std::memory_order_acquire)
            && now - s.lastRecordWriteMs >= s.options.tailProofIdleMs) {
            s.payload.clear();
            qreplay::putU32(s.payload, cursor);
            qreplay::putU32(s.payload, 0u);
            qreplay::putU64(s.payload, hash);
            if (!sendMessage(SidecarMessageKind::TailProof, cursor, s.payload)) {
                fail();
                return false;
            }
            s.sentTailProofCursor = cursor;
            s.sentTailProofHash = hash;
            s.tailProofSent = true;
            s.tailProofsWritten.fetch_add(1u, std::memory_order_relaxed);
            QLOG("[spectator.publish] tail proof cursor=%u postTail=%016llX "
                 "(source quiesced for %llu ms)",
                 cursor, static_cast<unsigned long long>(hash),
                 static_cast<unsigned long long>(now - s.lastRecordWriteMs));
        }
    }

    if (!s.faultPending.load(std::memory_order_acquire)
        && s.sealPending.load(std::memory_order_acquire)
        && !s.sealSent.load(std::memory_order_acquire)) {
        const uint32_t cursor =
            s.sealCursor.load(std::memory_order_acquire);
        if (s.written.load(std::memory_order_acquire) != cursor) {
            return drained > 0u;
        }
        s.payload.clear();
        qreplay::putU16(s.payload, s.sealReason.load(std::memory_order_relaxed));
        qreplay::putU16(s.payload, 0u);
        qreplay::putU32(s.payload, cursor);
        qreplay::putU64(
            s.payload,
            s.sealPrefixDigest.load(std::memory_order_relaxed));
        qreplay::putU64(s.payload, s.sealHash.load(std::memory_order_relaxed));
        if (!sendMessage(SidecarMessageKind::Seal,
                         s.sealReason.load(std::memory_order_relaxed),
                         s.payload)) {
            fail();
            return false;
        }
        s.sealSent.store(true, std::memory_order_release);
        QLOG("[spectator.publish] sealed reason=%u cursor=%u "
             "prefix=%016llX postTail=%016llX published=%llu written=%llu "
             "highWater=%u",
             s.sealReason.load(std::memory_order_relaxed),
             cursor,
             static_cast<unsigned long long>(
                 s.sealPrefixDigest.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 s.sealHash.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 s.published.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 s.written.load(std::memory_order_relaxed)),
             s.ring.highWater());
    }

    return drained > 0u || connectedDrained > 0u;
}

DWORD WINAPI publisherThreadEntry(LPVOID) {
    PublisherState& self = st();
    while (!self.stopRequested.load(std::memory_order_acquire)) {
        pumpOnce();
        Sleep(self.options.idleSleepMs);
    }
    return 0;
}

bool startInternal(const qipc::IpcConfig& cfg,
                   const SidecarPublisherOptions& options,
                   std::unique_ptr<ByteChannel> channel,
                   const bool clientPrivate) {
    PublisherState& s = st();
    if (s.active.load(std::memory_order_acquire)) return false;

    if ((cfg.spectatorClientPublisherEnabled == 1u) != clientPrivate) {
        QLOG("[spectator.publish] refused: %s publisher used for this IPC role",
             clientPrivate ? "client-private" : "public-host");
        return false;
    }
    if (const char* why = qipc::validateSpectatorClientPrivatePublisher(cfg)) {
        QLOG("[spectator.publish] refused %s publisher: %s",
             clientPrivate ? "client-private" : "public-host", why);
        return false;
    }

    if (const char* why = qipc::validateSpectatorService(cfg)) {
        QLOG("[spectator.publish] refused: %s", why);
        return false;
    }
    if (const char* why = qipc::validateSpectatorSocketHandoff(cfg)) {
        QLOG("[spectator.publish] refused: %s", why);
        return false;
    }
    if (cfg.spectatorServiceMode != qipc::SPECTATOR_SERVICE_SIDECAR)
        return false;

    s.options = options;
    if (s.options.drainPerTurn == 0u) s.options.drainPerTurn = 1u;
    if (s.options.checkpointSlots == 0u) s.options.checkpointSlots = 1u;

    if (!channel) {
        wchar_t name[128];
        qipc::ipcSpectatorSidecarPipeName(
            name, 128, static_cast<uint32_t>(GetCurrentProcessId()));
        channel = makeNamedPipeChannel(name, s.options.writeTimeoutMs);
    }
    if (!channel) return false;

    s.checkpoints.reset(new (std::nothrow)
                            CheckpointSlot[s.options.checkpointSlots]);
    if (!s.checkpoints) return false;
    s.checkpointCount = s.options.checkpointSlots;
    for (size_t i = 0; i < s.checkpointCount; ++i) {
        CheckpointSlot& slot = s.checkpoints[i];
        slot.buffer.assign(s.options.checkpointSlotBytes, 0u);
        slot.state.store(0u, std::memory_order_relaxed);
        slot.record = 0u;
        slot.bytes = 0u;
    }
    s.checkpointCursor = 0u;
    s.connectedQueue.reset(new (std::nothrow)
        ConnectedPublisherQueueV4(
            kConnectedPublisherDefaultCapacityV4));
    if (!s.connectedQueue || !s.connectedQueue->valid()) {
        s.connectedQueue.reset();
        s.checkpoints.reset();
        s.checkpointCount = 0u;
        return false;
    }
    s.connectedPublisher.reset(new (std::nothrow)
        ConnectedReplayPublisherV4(*s.connectedQueue));
    if (!s.connectedPublisher) {
        s.connectedQueue.reset();
        s.checkpoints.reset();
        s.checkpointCount = 0u;
        return false;
    }
    const size_t batchBytes = s.options.drainPerTurn * kEncodedRecordBytes;
    const size_t checkpointBytes = s.options.checkpointSlotBytes + 8u;
    s.payload.reserve(batchBytes > checkpointBytes ? batchBytes
                                                   : checkpointBytes);
    s.header.reserve(kSidecarHeaderBytes);
    s.startMetadata.clear();
    s.connectedDescriptorBytes.clear();

    s.ring.reset();
    s.channel = std::move(channel);
    s.gameId = 0u;
    s.sourceIdentity = {};
    s.sourceIdentity.sessionId = cfg.sessionId;
    s.sourceIdentity.epoch = 1u;
    s.sourceIdentity.cookie.low = cfg.spectatorCookieLow;
    s.sourceIdentity.cookie.high = cfg.spectatorCookieHigh;
    s.sequence = 0u;
    s.reconnectAttempts = 0u;
    s.firstFailureMs = 0u;
    s.lastAttemptMs = 0u;
    s.published.store(0u, std::memory_order_relaxed);
    s.dropped.store(0u, std::memory_order_relaxed);
    s.written.store(0u, std::memory_order_relaxed);
    s.checkpointsPublished.store(0u, std::memory_order_relaxed);
    s.checkpointsDropped.store(0u, std::memory_order_relaxed);
    s.checkpointsRejected.store(0u, std::memory_order_relaxed);
    s.connected.store(0u, std::memory_order_relaxed);
    s.startPending.store(false, std::memory_order_relaxed);
    s.startSent.store(false, std::memory_order_relaxed);
    s.sealPending.store(false, std::memory_order_relaxed);
    s.sealSent.store(false, std::memory_order_relaxed);
    s.sealClaim.store(0u, std::memory_order_relaxed);
    s.sealReason.store(0u, std::memory_order_relaxed);
    s.sealCursor.store(0u, std::memory_order_relaxed);
    s.sealPrefixDigest.store(0u, std::memory_order_relaxed);
    s.sealHash.store(0u, std::memory_order_relaxed);
    s.faultPending.store(false, std::memory_order_relaxed);
    s.faultSent.store(false, std::memory_order_relaxed);
    s.faultClaim.store(kFaultOpen, std::memory_order_relaxed);
    s.faultReason.store(0u, std::memory_order_relaxed);
    s.faultCursor.store(0u, std::memory_order_relaxed);
    s.faultPrefixDigest.store(0u, std::memory_order_relaxed);
    s.faultPostTailHash.store(0u, std::memory_order_relaxed);
    s.producerPrefixDigest.store(0u, std::memory_order_relaxed);
    s.tailProofCursor.store(0u, std::memory_order_relaxed);
    s.tailProofHash.store(0u, std::memory_order_relaxed);
    s.tailProofsWritten.store(0u, std::memory_order_relaxed);
    s.connectedDescriptorPending.store(
        false, std::memory_order_relaxed);
    s.connectedDescriptorSent.store(
        false, std::memory_order_relaxed);
    s.connectedDescriptor = {};
    s.connectedPublicationsWritten.store(
        0u, std::memory_order_relaxed);
    s.sentTailProofCursor = 0u;
    s.sentTailProofHash = 0u;
    s.tailProofSent = false;
    s.lastRecordWriteMs = nowMs();
    s.disabled.store(false, std::memory_order_relaxed);
    s.disableReason.store(0u, std::memory_order_relaxed);
    s.overflowUnreported.store(false, std::memory_order_relaxed);
    s.stopRequested.store(false, std::memory_order_relaxed);
    s.active.store(true, std::memory_order_release);

    if (!s.options.inlineWorker) {
        // Raw CreateThread: a loader-lock shutdown could deadlock joining a CRT thread object.
        s.threadHandle = CreateThread(nullptr, 0, &publisherThreadEntry,
                                      nullptr, 0, nullptr);
        s.threaded = (s.threadHandle != nullptr);
        if (!s.threaded)
            QLOG("[spectator.publish] drain thread creation failed (error=%lu)",
                 GetLastError());
    }

    QLOG("[spectator.publish] armed ring=%u checkpointSlots=%u "
         "checkpointBytes=%u",
         static_cast<unsigned>(kConfirmedRingCapacity),
         static_cast<unsigned>(s.options.checkpointSlots),
         static_cast<unsigned>(s.options.checkpointSlotBytes));
    return true;
}

}

const char* publishDisableReasonText(PublishDisableReason reason) {
    switch (reason) {
        case PublishDisableReason::None: return "none";
        case PublishDisableReason::RingOverflow: return "ring-overflow";
        case PublishDisableReason::CheckpointOverflow:
            return "checkpoint-overflow";
        case PublishDisableReason::ChannelFailed: return "channel-failed";
        case PublishDisableReason::Stopped: return "stopped";
        case PublishDisableReason::InvalidRecord: return "invalid-record";
    }
    return "unknown";
}

bool startSidecarPublisher(const qipc::IpcConfig& cfg) {
    return startInternal(cfg, SidecarPublisherOptions{}, nullptr,
                         false);
}

bool startClientPrivateSidecarPublisher(const qipc::IpcConfig& cfg) {
    return startInternal(cfg, SidecarPublisherOptions{}, nullptr,
                         true);
}

bool startSidecarPublisherForTest(const qipc::IpcConfig& cfg,
                                  const SidecarPublisherOptions& options,
                                  ByteChannel* channel) {
    return startInternal(cfg, options, std::unique_ptr<ByteChannel>(channel),
                         false);
}

bool startClientPrivateSidecarPublisherForTest(
    const qipc::IpcConfig& cfg, const SidecarPublisherOptions& options,
    ByteChannel* channel) {
    return startInternal(cfg, options, std::unique_ptr<ByteChannel>(channel),
                         true);
}

void stopSidecarPublisher() {
    PublisherState& s = st();
    if (!s.active.load(std::memory_order_acquire)) return;

    if (connectedReplayPublisherReadyV4()
        && s.connectedPublisher
        && !s.connectedPublisher->status().sealed) {
        const uint64_t tail =
            s.tailProofHash.load(std::memory_order_acquire);
        if (!s.connectedPublisher->status().gameOpen && tail != 0u) {
            (void)s.connectedPublisher->publishSetSeal(
                qnetplay::spectator::ConnectedSetTerminalReasonV4::
                    SourceStopped,
                tail);
        }
    }

    if (!s.sealPending.load(std::memory_order_acquire)
        && !s.faultPending.load(std::memory_order_acquire)) {
        disablePublication(PublishDisableReason::Stopped);
    }

    s.stopRequested.store(true, std::memory_order_release);
    if (s.threaded && s.threadHandle) {
        WaitForSingleObject(static_cast<HANDLE>(s.threadHandle), INFINITE);
        CloseHandle(static_cast<HANDLE>(s.threadHandle));
        s.threadHandle = nullptr;
    }
    s.threaded = false;

    const size_t perTurn = (std::max)(size_t{1}, s.options.drainPerTurn);
    const size_t maxTurns =
        (kConfirmedRingCapacity + perTurn - 1u) / perTurn + 4u;
    for (size_t turn = 0;
         turn < maxTurns && s.channel && s.channel->connected();
         ++turn) {
        const bool legacyTerminalSent =
            s.sealSent.load(std::memory_order_acquire)
            || s.faultSent.load(std::memory_order_acquire);
        const ConnectedPublisherStatsV4 connectedStats =
            s.connectedQueue
                ? s.connectedQueue->stats()
                : ConnectedPublisherStatsV4{};
        if (legacyTerminalSent
            && connectedStats.accepted == connectedStats.consumed) {
            break;
        }
        (void)pumpOnce();
    }
    s.active.store(false, std::memory_order_release);
    if (s.channel) s.channel->close();
    s.channel.reset();
    s.connectedPublisher.reset();
    s.connectedQueue.reset();
    s.connectedDescriptorBytes.clear();
    s.checkpoints.reset();
    s.checkpointCount = 0u;
    QLOG("[spectator.publish] stopped published=%llu dropped=%llu written=%llu "
         "checkpoints=%llu highWater=%u disabled=%u",
         static_cast<unsigned long long>(
             s.published.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             s.dropped.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             s.written.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             s.checkpointsPublished.load(std::memory_order_relaxed)),
         s.ring.highWater(),
         s.disabled.load(std::memory_order_relaxed) ? 1u : 0u);
}

bool sidecarPublisherActive() {
    return st().active.load(std::memory_order_acquire);
}

bool publishStart(const SidecarStartInfo& info, const void* metadata,
                  size_t metadataLen) {
    PublisherState& s = st();
    if (!s.active.load(std::memory_order_acquire)) return false;
    if (s.disabled.load(std::memory_order_acquire)
        || s.sealClaim.load(std::memory_order_acquire) != 0u
        || s.startPending.load(std::memory_order_acquire)) {
        return false;
    }
    if (metadataLen > kMaxSidecarPayloadBytes / 2u) return false;
    s.startInfo = info;
    s.gameId = info.gameId;
    s.startMetadata.clear();
    if (metadata && metadataLen > 0u) {
        const uint8_t* bytes = static_cast<const uint8_t*>(metadata);
        s.startMetadata.assign(bytes, bytes + metadataLen);
    }

    qnetplay::spectator::GameStartPayload canonical{};
    canonical.gameId = info.gameId;
    canonical.simulationContract = info.simulationContract;
    canonical.compatibilityIdentity = info.compatibilityIdentity;
    canonical.initialStateHash = info.initialStateHash;
    canonical.firstTimelineFrame = info.firstTimelineFrame;
    canonical.playerOneDelay = info.playerOneDelay;
    canonical.playerTwoDelay = info.playerTwoDelay;
    canonical.rollbackWindow = info.rollbackWindow;
    canonical.metadata = s.startMetadata;
    const uint64_t derivedInitialDigest =
        qnetplay::spectator::computeInitialDigest(canonical);
    if (info.initialDigest != 0u
        && info.initialDigest != derivedInitialDigest) {
        disablePublication(PublishDisableReason::InvalidRecord);
        return false;
    }
    s.producerPrefixDigest.store(
        derivedInitialDigest, std::memory_order_release);

    qnetplay::spectator::GameStartPayload connectedCanonical = canonical;
    connectedCanonical.compatibilityIdentity = {};
    connectedCanonical.initialDigest =
        qnetplay::spectator::computeInitialDigest(connectedCanonical);
    qnetplay::spectator::ConnectedSetDescriptorV4 descriptor{};
    descriptor.setStreamId = info.gameId;
    descriptor.sourceIdentity = s.sourceIdentity;
    descriptor.simulationContract = info.simulationContract;
    descriptor.initialDescriptorDigest =
        qnetplay::spectator::gameStartFingerprint(connectedCanonical);
    descriptor.compatibilityIdentity = info.compatibilityIdentity;
    descriptor.contentIdentityAttested =
        qnetplay::compat::isValidCompatibilityIdentity(
            descriptor.compatibilityIdentity);
    std::vector<uint8_t> descriptorBytes;
    const bool connectedReady =
        s.connectedPublisher
        && s.connectedPublisher->reset(descriptor)
        && qnetplay::spectator::encodeConnectedSetDescriptorV4(
            descriptor, descriptorBytes);
    if (connectedReady) {
        s.connectedDescriptor = descriptor;
        s.connectedDescriptorBytes.swap(descriptorBytes);
        s.connectedDescriptorSent.store(
            false, std::memory_order_release);
        s.connectedDescriptorPending.store(
            true, std::memory_order_release);
    } else {
        s.connectedDescriptorBytes.clear();
        s.connectedDescriptorPending.store(
            false, std::memory_order_release);
        QLOG("[spectator.publish.v4] descriptor unavailable; QSP3 remains "
             "active and the player session is unaffected");
    }
    s.startSent.store(false, std::memory_order_release);
    s.startPending.store(true, std::memory_order_release);
    return true;
}

bool publishChainedSetEvent(qnetplay::spectator::SetEvent event) {
    PublisherState& s = st();
    if (!s.active.load(std::memory_order_relaxed)) return false;
    if (s.disabled.load(std::memory_order_relaxed)) return false;
    if (s.sealClaim.load(std::memory_order_acquire) != 0u
        || !s.startPending.load(std::memory_order_acquire)) {
        return false;
    }

    const uint32_t head =
        s.chainedEventHead.load(std::memory_order_relaxed);
    const uint32_t tail =
        s.chainedEventTail.load(std::memory_order_acquire);
    if (head - tail >= PublisherState::kChainedEventSlots) return false;

    const uint64_t cursor64 = s.published.load(std::memory_order_relaxed);
    if (cursor64 > UINT32_MAX) return false;
    event.eventIndex = s.producedEvents.load(std::memory_order_relaxed);
    event.recordCursor = static_cast<uint32_t>(cursor64);
    event.timelineFrame =
        s.startInfo.firstTimelineFrame + event.recordCursor;
    event.recordDigest =
        s.producerPrefixDigest.load(std::memory_order_relaxed);
    event.rollingDigest = qnetplay::spectator::advanceSetEventDigest(
        event.recordDigest, s.gameId, event);
    if (!qnetplay::spectator::isValidSetEvent(event)) return false;

    s.chainedEvents[head % PublisherState::kChainedEventSlots] = event;
    s.chainedEventHead.store(head + 1u, std::memory_order_release);
    s.producerPrefixDigest.store(event.rollingDigest,
                                 std::memory_order_relaxed);
    s.producedEvents.store(event.eventIndex + 1u,
                           std::memory_order_relaxed);
    return true;
}

// Simulation thread hot path: no allocation, no lock, no wait, no log.
bool publishConfirmed(const ConfirmedRecord& record) {
    PublisherState& s = st();
    if (!s.active.load(std::memory_order_relaxed)) return false;
    if (s.disabled.load(std::memory_order_relaxed)) return false;
    if (s.sealClaim.load(std::memory_order_acquire) != 0u
        || !s.startPending.load(std::memory_order_acquire)) {
        return false;
    }

    const uint64_t cursor64 =
        s.published.load(std::memory_order_relaxed);
    if (cursor64 > UINT32_MAX
        || record.record != static_cast<uint32_t>(cursor64)
        || s.startInfo.firstTimelineFrame > UINT32_MAX - record.record
        || record.timelineFrame
            != s.startInfo.firstTimelineFrame + record.record
        || record.stateHash == 0u) {
        disablePublication(
            PublishDisableReason::InvalidRecord,
            cursor64 <= UINT32_MAX
                    && record.record == static_cast<uint32_t>(cursor64)
                ? record.stateHash
                : 0u);
        return false;
    }

    ConfirmedRecord canonical = record;
    const uint64_t previous =
        s.producerPrefixDigest.load(std::memory_order_relaxed);
    const uint64_t expectedDigest =
        qnetplay::spectator::advanceRollingDigest(
            previous, s.gameId, canonical);
    if (canonical.rollingDigest != 0u
        && canonical.rollingDigest != expectedDigest) {
        disablePublication(PublishDisableReason::InvalidRecord,
                           record.stateHash);
        return false;
    }
    canonical.rollingDigest = expectedDigest;

    if (!s.ring.push(canonical)) {
        s.dropped.fetch_add(1u, std::memory_order_relaxed);
        disablePublication(PublishDisableReason::RingOverflow,
                           record.stateHash);
        return false;
    }
    s.producerPrefixDigest.store(expectedDigest, std::memory_order_release);
    s.published.fetch_add(1u, std::memory_order_relaxed);
    return true;
}

bool publishStateCheckpoint(const uint8_t* snapshot,
                            size_t snapshotBytes,
                            uint32_t regionSchema,
                            uint32_t regionSchemaVersion,
                            uint64_t stateHash,
                            const uint8_t* bootDescriptor,
                            size_t bootDescriptorBytes,
                            const uint8_t* audioRegion,
                            size_t audioRegionBytes) {
    PublisherState& s = st();
    if (!s.active.load(std::memory_order_relaxed)) return false;
    if (s.disabled.load(std::memory_order_relaxed)) return false;
    if (!s.startPending.load(std::memory_order_acquire)) return false;
    if (snapshot == nullptr || snapshotBytes == 0u) return false;
    if (snapshotBytes > qnetplay::spectator::kMaxCheckpointRegionBytes)
        return false;

    const uint64_t cursor64 = s.published.load(std::memory_order_relaxed);
    if (cursor64 > UINT32_MAX) return false;

    qnetplay::spectator::Checkpoint checkpoint{};
    checkpoint.identity.sessionId = s.sourceIdentity.sessionId;
    checkpoint.identity.gameId = s.gameId;
    checkpoint.identity.checkpointId =
        (s.gameId << 32) ^ static_cast<uint32_t>(cursor64);
    checkpoint.identity.timelineFrame = s.startInfo.firstTimelineFrame
        + static_cast<uint32_t>(cursor64);
    checkpoint.identity.recordCursor = static_cast<uint32_t>(cursor64);
    checkpoint.eventCursor =
        s.producedEvents.load(std::memory_order_relaxed);
    checkpoint.simulationContract = s.startInfo.simulationContract;
    checkpoint.compatibilityIdentity = s.startInfo.compatibilityIdentity;
    checkpoint.bootstrapFingerprint = s.startInfo.initialDigest;
    checkpoint.regionSchema = regionSchema;
    checkpoint.regionSchemaVersion = regionSchemaVersion;
    checkpoint.digestChainPrefix =
        s.producerPrefixDigest.load(std::memory_order_relaxed);
    checkpoint.stateHash = stateHash;
    checkpoint.chunkBytes = static_cast<uint32_t>(
        qnetplay::spectator::kMaxCheckpointChunkBytes);
    qnetplay::spectator::CheckpointRegion region{};
    region.id = 1u;
    region.byteLength = static_cast<uint32_t>(snapshotBytes);
    region.bytes.assign(snapshot, snapshot + snapshotBytes);
    checkpoint.regions.push_back(std::move(region));
    if (bootDescriptor != nullptr && bootDescriptorBytes != 0u) {
        qnetplay::spectator::CheckpointRegion boot{};
        boot.id = 2u;
        boot.byteLength = static_cast<uint32_t>(bootDescriptorBytes);
        boot.bytes.assign(bootDescriptor,
                          bootDescriptor + bootDescriptorBytes);
        checkpoint.regions.push_back(std::move(boot));
    }
    if (audioRegion != nullptr && audioRegionBytes != 0u) {
        qnetplay::spectator::CheckpointRegion audio{};
        audio.id = 3u;
        audio.byteLength = static_cast<uint32_t>(audioRegionBytes);
        audio.bytes.assign(audioRegion, audioRegion + audioRegionBytes);
        checkpoint.regions.push_back(std::move(audio));
    }

    std::vector<uint8_t> encoded;
    if (!qnetplay::spectator::encodeCheckpoint(checkpoint, encoded)) {
        QLOG("[checkpoint] encode refused at record %u (regions %zu, "
             "snapshot %zu bytes)",
             checkpoint.identity.recordCursor, checkpoint.regions.size(),
             snapshotBytes);
        return false;
    }
    if (!publishCheckpoint(checkpoint.identity.recordCursor,
                           encoded.data(), encoded.size())) {
        QLOG("[checkpoint] slot refused %zu encoded bytes at record %u "
             "(slotBytes=%zu)",
             encoded.size(), checkpoint.identity.recordCursor,
             s.options.checkpointSlotBytes);
        return false;
    }
    QLOG("[checkpoint] captured record=%u event=%u snapshot=%zu "
         "encoded=%zu layoutGen=%u",
         checkpoint.identity.recordCursor, checkpoint.eventCursor,
         snapshotBytes, encoded.size(), regionSchemaVersion);
    return true;
}

bool publishCheckpoint(uint32_t record, const void* bytes, size_t len) {
    PublisherState& s = st();
    if (!s.active.load(std::memory_order_relaxed)) return false;
    if (s.disabled.load(std::memory_order_relaxed)) return false;
    if (!bytes || len == 0u) return false;
    if (!s.checkpoints || s.checkpointCount == 0u) return false;

    if (len > s.options.checkpointSlotBytes
        || len > kMaxSidecarPayloadBytes - 8u) {
        s.checkpointsRejected.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    const size_t count = s.checkpointCount;
    for (size_t i = 0; i < count; ++i) {
        const size_t index = (s.checkpointCursor + i) % count;
        CheckpointSlot& slot = s.checkpoints[index];
        if (slot.state.load(std::memory_order_acquire) != 0u) continue;
        std::memcpy(slot.buffer.data(), bytes, len);
        slot.record = record;
        slot.bytes = static_cast<uint32_t>(len);
        slot.state.store(1u, std::memory_order_release);
        s.checkpointCursor = (index + 1u) % count;
        s.checkpointsPublished.fetch_add(1u, std::memory_order_relaxed);
        return true;
    }

    s.checkpointsDropped.fetch_add(1u, std::memory_order_relaxed);
    disablePublication(PublishDisableReason::CheckpointOverflow);
    return false;
}

void publishSeal(uint16_t reason, uint64_t postTailStateHash) {
    PublisherState& s = st();
    if (!s.active.load(std::memory_order_relaxed)) return;
    qnetplay::spectator::ConnectedSetTerminalReasonV4 connectedReason =
        qnetplay::spectator::ConnectedSetTerminalReasonV4::SourceStopped;
    if (reason == static_cast<uint16_t>(
                      qnetplay::spectator::TerminalReason::
                          ProtocolFailure)) {
        connectedReason =
            qnetplay::spectator::ConnectedSetTerminalReasonV4::
                ProtocolFailure;
    }
    (void)publishConnectedSetSealV4(
        connectedReason, postTailStateHash);
    if (s.disabled.load(std::memory_order_acquire)
        || s.faultPending.load(std::memory_order_acquire)) {
        return;
    }
    uint8_t expected = 0u;
    if (!s.sealClaim.compare_exchange_strong(
            expected, 1u, std::memory_order_acq_rel)) {
        return;
    }
    const uint64_t cursor64 =
        s.published.load(std::memory_order_acquire);
    if (cursor64 > UINT32_MAX) {
        disablePublication(PublishDisableReason::InvalidRecord,
                           postTailStateHash);
        s.sealClaim.store(2u, std::memory_order_release);
        return;
    }
    s.sealReason.store(reason, std::memory_order_relaxed);
    s.sealCursor.store(
        static_cast<uint32_t>(cursor64), std::memory_order_relaxed);
    s.sealPrefixDigest.store(
        s.producerPrefixDigest.load(std::memory_order_acquire),
        std::memory_order_relaxed);
    s.sealHash.store(postTailStateHash, std::memory_order_relaxed);
    s.sealPending.store(true, std::memory_order_release);
    s.sealClaim.store(2u, std::memory_order_release);
}

void publishFault(PublishDisableReason reason,
                  uint64_t postTailStateHash) {
    PublisherState& s = st();
    if (!s.active.load(std::memory_order_relaxed)
        || reason == PublishDisableReason::None) {
        return;
    }
    disablePublication(reason, postTailStateHash);
}

void publishTailProof(uint32_t recordCursor, uint64_t postTailStateHash) {
    PublisherState& s = st();
    if (!s.active.load(std::memory_order_relaxed)) return;
    if (recordCursor == 0u || postTailStateHash == 0u) return;
    s.tailProofHash.store(postTailStateHash, std::memory_order_relaxed);
    s.tailProofCursor.store(recordCursor, std::memory_order_release);
}

SidecarPublisherStats sidecarStats() {
    PublisherState& s = st();
    SidecarPublisherStats out{};
    out.active = s.active.load(std::memory_order_relaxed) ? 1u : 0u;
    out.connected = s.connected.load(std::memory_order_relaxed);
    out.disabled = s.disabled.load(std::memory_order_relaxed) ? 1u : 0u;
    out.disableReason = s.disableReason.load(std::memory_order_relaxed);
    out.published = s.published.load(std::memory_order_relaxed);
    out.dropped = s.dropped.load(std::memory_order_relaxed);
    out.written = s.written.load(std::memory_order_relaxed);
    out.checkpointsPublished =
        s.checkpointsPublished.load(std::memory_order_relaxed);
    out.checkpointsDropped =
        s.checkpointsDropped.load(std::memory_order_relaxed);
    out.checkpointsRejected =
        s.checkpointsRejected.load(std::memory_order_relaxed);
    out.ringHighWater = s.ring.highWater();
    out.sealed = s.sealSent.load(std::memory_order_relaxed) ? 1u : 0u;
    out.faulted =
        s.faultPending.load(std::memory_order_relaxed) ? 1u : 0u;
    out.faultSent =
        s.faultSent.load(std::memory_order_relaxed) ? 1u : 0u;
    if (out.faulted) {
        out.terminalRecordCursor =
            s.faultCursor.load(std::memory_order_relaxed);
        out.terminalPrefixDigest =
            s.faultPrefixDigest.load(std::memory_order_relaxed);
    } else if (s.sealPending.load(std::memory_order_relaxed)) {
        out.terminalRecordCursor =
            s.sealCursor.load(std::memory_order_relaxed);
        out.terminalPrefixDigest =
            s.sealPrefixDigest.load(std::memory_order_relaxed);
    }
    out.tailProofsWritten =
        s.tailProofsWritten.load(std::memory_order_relaxed);
    out.connectedPublicationsWritten =
        s.connectedPublicationsWritten.load(std::memory_order_relaxed);
    if (s.connectedPublisher) {
        const ConnectedReplayPublisherStatusV4 connected =
            s.connectedPublisher->status();
        out.connectedNextItemSequence =
            connected.nextItemSequence;
        out.connectedNextRecordCursor =
            connected.nextRecordCursor;
        out.connectedEnabled = connected.bound ? 1u : 0u;
        out.connectedSealed = connected.sealed ? 1u : 0u;
    }
    return out;
}

void pumpSidecarForTest() { pumpOnce(); }

qreplay::ReplayRecordPublisherV2* connectedReplayPublisherV4() {
    PublisherState& s = st();
    if (!s.active.load(std::memory_order_acquire)
        || !s.connectedPublisher) {
        return nullptr;
    }
    return s.connectedPublisher.get();
}

bool connectedReplayPublisherReadyV4() {
    PublisherState& s = st();
    if (!s.active.load(std::memory_order_acquire)
        || !s.connectedPublisher
        || !s.connectedDescriptorPending.load(
            std::memory_order_acquire)) {
        return false;
    }
    return s.connectedPublisher->status().bound;
}

bool publishConnectedOutOfGameV4(
    uint32_t sourceTimelineFrame,
    const qreplay::ConfirmedGameFrameCoreV2& frame) {
    PublisherState& s = st();
    return connectedReplayPublisherReadyV4()
        && s.connectedPublisher->publishOutOfGameConfirmed(
            sourceTimelineFrame, frame);
}

bool publishConnectedSetEventV4(
    const qnetplay::spectator::SetEventPayloadV4& event) {
    PublisherState& s = st();
    return connectedReplayPublisherReadyV4()
        && s.connectedPublisher->publishSetEvent(event);
}

bool publishConnectedSetSealV4(
    qnetplay::spectator::ConnectedSetTerminalReasonV4 reason,
    uint64_t postTailStateHash) {
    PublisherState& s = st();
    return connectedReplayPublisherReadyV4()
        && s.connectedPublisher->publishSetSeal(
            reason, postTailStateHash);
}

bool popConnectedAcceptedEndAckV4(
    qreplay::GameCommitAckV2& out) {
    PublisherState& s = st();
    return s.connectedPublisher
        && s.connectedPublisher->popAcceptedEndAck(out);
}

}
