#pragma once

#include "SpectatorProtocolV4.h"

#include "../replay/ReplayIntegrity.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qnetplay::spectator {

struct ConnectedArchiveConfigV4 {
    uint32_t maxItems = kMaxConnectedSetItemsV4;
    uint32_t maxRecords = kMaxConnectedSetRecordsV4;
    uint64_t maxBytes = kMaxConnectedArchiveBytesV4;
    uint64_t retentionMs = kSpectatorPostGameRetentionMsV4;
};

enum class ConnectedCommitResultV4 : uint8_t {
    Committed,
    ExactDuplicate,
    InvalidArgument,
    SequenceGap,
    ConflictingDuplicate,
    CursorMismatch,
    DigestMismatch,
    IdentityMismatch,
    RangeOverlap,
    LifecycleViolation,
    ReplayDigestMismatch,
    LimitReached,
    Sealed,
};

struct ConnectedGameRangeV4 {
    qreplay::GameIdentityV2 game{};
    uint32_t startRecordCursor = 0u;
    uint32_t nextRecordCursor = 0u;
    uint32_t startItemSequence = 0u;
    uint32_t nextItemSequence = 0u;
    qreplay::ConfirmedGameStartV2 start{};
    qreplay::ConfirmedGameEndV2 end{};
    bool complete = false;

    bool operator==(const ConnectedGameRangeV4& o) const {
        return game == o.game
            && startRecordCursor == o.startRecordCursor
            && nextRecordCursor == o.nextRecordCursor
            && startItemSequence == o.startItemSequence
            && nextItemSequence == o.nextItemSequence
            && start == o.start && end == o.end && complete == o.complete;
    }
};

struct StoredConnectedPublicationV4 {
    ConnectedPublicationKindV4 kind =
        ConnectedPublicationKindV4::ConfirmedRecord;
    uint32_t firstItemSequence = 0u;
    uint32_t nextItemSequence = 0u;
    uint32_t firstRecordCursor = 0u;
    uint32_t nextRecordCursor = 0u;
    uint64_t firstDigest = 0u;
    uint64_t finalDigest = 0u;
    uint64_t cumulativeBytesAfter = 0u;
    std::vector<uint8_t> canonicalBytes;
};

class SpectatorConnectedArchiveV4 {
public:
    explicit SpectatorConnectedArchiveV4(
        const ConnectedArchiveConfigV4& config = {});

    bool reset(
        const ConnectedSetDescriptorV4& descriptor,
        uint64_t monotonicNowMs = 0u);
    ConnectedCommitResultV4 commit(
        const ConnectedPublicationV4& publication,
        uint64_t monotonicNowMs);

    bool bound() const { return bound_; }
    bool failed() const { return failed_; }
    bool sealed() const { return sealed_; }
    uint64_t setStreamId() const { return descriptor_.setStreamId; }
    uint64_t genesisDigest() const { return genesisDigest_; }
    uint32_t nextItemSequence() const { return nextItemSequence_; }
    uint32_t nextRecordCursor() const { return nextRecordCursor_; }
    uint64_t digest() const { return digest_; }
    uint64_t retainedBytes() const { return retainedBytes_; }
    uint64_t retainedUntilMs() const { return retainedUntilMs_; }
    bool expired(uint64_t monotonicNowMs) const {
        return sealed_ && monotonicNowMs >= retainedUntilMs_;
    }

    const std::vector<ConnectedGameRangeV4>& ranges() const {
        return ranges_;
    }
    bool openRange(ConnectedGameRangeV4& out) const;
    bool rangeByIdentity(
        const qreplay::GameIdentityV2& game,
        ConnectedGameRangeV4& out) const;
    bool newestCompleteRange(ConnectedGameRangeV4& out) const;

    bool publicationAt(
        uint32_t firstItemSequence,
        ConnectedPublicationV4& out) const;
    bool storedPublicationAt(
        uint32_t firstItemSequence,
        StoredConnectedPublicationV4& out) const;
    bool resumeCursor(
        uint32_t nextItemSequence, uint32_t viewerGeneration,
        SpectatorResumeCursorV4& out) const;
    bool validateResume(const SpectatorResumeCursorV4& cursor) const;

    bool byteDistance(
        uint32_t firstItemSequence, uint32_t nextItemSequence,
        uint64_t& out) const;

    bool sealAck(SpectatorSetSealAckV4& out, uint64_t viewerId,
                 uint32_t viewerGeneration) const;

private:
    struct OpenReplayValidation {
        bool active = false;
        qreplay::GameIdentityV2 game{};
        uint32_t startCursor = 0u;
        uint32_t nextEventSequence = 0u;
        uint64_t rollingDigest = 0u;
        uint64_t lastPostStateHash = 0u;
        qreplay::CanonicalGameDigestV2 canonical{};
    };

    ConnectedArchiveConfigV4 config_{};
    ConnectedSetDescriptorV4 descriptor_{};
    uint64_t genesisDigest_ = 0u;
    uint32_t nextItemSequence_ = 0u;
    uint32_t nextRecordCursor_ = 0u;
    uint64_t digest_ = 0u;
    uint64_t lastPostStateHash_ = 0u;
    uint64_t retainedBytes_ = 0u;
    bool bound_ = false;
    bool failed_ = false;
    bool sealed_ = false;
    uint64_t retainedUntilMs_ = 0u;
    uint32_t sealItemSequence_ = 0u;
    ConnectedSetSealV4 seal_{};

    std::vector<StoredConnectedPublicationV4> publications_;
    std::vector<ConnectedGameRangeV4> ranges_;
    OpenReplayValidation replay_{};

    size_t findPublication(uint32_t firstItemSequence) const;
    bool isPublicationBoundary(uint32_t sequence) const;
    bool proofAtBoundary(
        uint32_t sequence, uint32_t& recordCursor,
        uint64_t& digest) const;
};

}
