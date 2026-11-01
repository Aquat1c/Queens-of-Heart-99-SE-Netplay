#include "SpectatorConnectedArchiveV4.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace qnetplay::spectator {
namespace {

constexpr size_t kNotFound = (std::numeric_limits<size_t>::max)();

bool digestIsZero(const std::array<uint8_t, 32>& digest) {
    for (uint8_t byte : digest)
        if (byte != 0u) return false;
    return true;
}

bool appendTickToCanonical(
    qreplay::CanonicalGameDigestV2& canonical,
    const qreplay::CanonicalTickBundleV2& tick) {
    for (size_t i = 0; i < tick.preEvents.count; ++i)
        if (!canonical.append(tick.preEvents.events[i])) return false;
    if (!canonical.append(tick.frame)) return false;
    for (size_t i = 0; i < tick.postEvents.count; ++i)
        if (!canonical.append(tick.postEvents.events[i])) return false;
    return true;
}

bool hasTerminalGameResult(
    const qreplay::CanonicalTickBundleV2& tick) {
    size_t results = 0u;
    for (size_t i = 0; i < tick.preEvents.count; ++i)
        if (tick.preEvents.events[i].kind
            == qreplay::GameEventKindV2::GameResult) {
            ++results;
        }
    for (size_t i = 0; i < tick.postEvents.count; ++i)
        if (tick.postEvents.events[i].kind
            == qreplay::GameEventKindV2::GameResult) {
            ++results;
        }
    return results == 1u && tick.postEvents.count != 0u
        && tick.postEvents.events[tick.postEvents.count - 1u].kind
            == qreplay::GameEventKindV2::GameResult;
}

uint64_t saturatingAdd(uint64_t left, uint64_t right) {
    const uint64_t max = (std::numeric_limits<uint64_t>::max)();
    return left > max - right ? max : left + right;
}

}

SpectatorConnectedArchiveV4::SpectatorConnectedArchiveV4(
    const ConnectedArchiveConfigV4& config)
    : config_(config) {}

bool SpectatorConnectedArchiveV4::reset(
    const ConnectedSetDescriptorV4& descriptor, uint64_t) {
    const bool validConfig =
        config_.maxItems >= 4u
        && config_.maxItems <= kMaxConnectedSetItemsV4
        && config_.maxRecords >= 1u
        && config_.maxRecords <= kMaxConnectedSetRecordsV4
        && config_.maxBytes >= 4096u
        && config_.maxBytes <= kMaxConnectedArchiveBytesV4
        && config_.retentionMs == kSpectatorPostGameRetentionMsV4;
    const uint64_t genesis = computeConnectedGenesisDigestV4(descriptor);
    if (!validConfig || genesis == 0u) return false;

    descriptor_ = descriptor;
    genesisDigest_ = genesis;
    nextItemSequence_ = 0u;
    nextRecordCursor_ = 0u;
    digest_ = genesis;
    lastPostStateHash_ = 0u;
    retainedBytes_ = 0u;
    bound_ = true;
    failed_ = false;
    sealed_ = false;
    retainedUntilMs_ = 0u;
    sealItemSequence_ = 0u;
    seal_ = ConnectedSetSealV4{};
    publications_.clear();
    ranges_.clear();
    replay_ = OpenReplayValidation{};
    return true;
}

size_t SpectatorConnectedArchiveV4::findPublication(
    uint32_t firstItemSequence) const {
    const auto it = std::lower_bound(
        publications_.begin(), publications_.end(), firstItemSequence,
        [](const StoredConnectedPublicationV4& stored, uint32_t sequence) {
            return stored.firstItemSequence < sequence;
        });
    if (it == publications_.end()
        || it->firstItemSequence != firstItemSequence) {
        return kNotFound;
    }
    return static_cast<size_t>(it - publications_.begin());
}

bool SpectatorConnectedArchiveV4::isPublicationBoundary(
    uint32_t sequence) const {
    if (sequence == 0u || sequence == nextItemSequence_) return true;
    return findPublication(sequence) != kNotFound;
}

bool SpectatorConnectedArchiveV4::proofAtBoundary(
    uint32_t sequence, uint32_t& recordCursor, uint64_t& digest) const {
    if (!bound_ || !isPublicationBoundary(sequence)) return false;
    if (sequence == 0u) {
        recordCursor = 0u;
        digest = genesisDigest_;
        return true;
    }
    if (sequence == nextItemSequence_) {
        recordCursor = nextRecordCursor_;
        digest = digest_;
        return true;
    }
    const size_t index = findPublication(sequence);
    if (index == kNotFound) return false;
    recordCursor = publications_[index].firstRecordCursor;
    digest = publications_[index].firstDigest;
    return true;
}

ConnectedCommitResultV4 SpectatorConnectedArchiveV4::commit(
    const ConnectedPublicationV4& publication,
    uint64_t monotonicNowMs) {
    if (!bound_ || failed_) return ConnectedCommitResultV4::InvalidArgument;

    std::vector<uint8_t> canonicalBytes;
    if (!encodeConnectedPublicationV4(publication, canonicalBytes))
        return ConnectedCommitResultV4::InvalidArgument;

    const uint32_t firstSequence = firstItemSequenceV4(publication);
    if (firstSequence < nextItemSequence_) {
        const size_t index = findPublication(firstSequence);
        if (index != kNotFound
            && publications_[index].canonicalBytes == canonicalBytes) {
            return ConnectedCommitResultV4::ExactDuplicate;
        }
        failed_ = true;
        return ConnectedCommitResultV4::ConflictingDuplicate;
    }
    if (sealed_) {
        failed_ = true;
        return ConnectedCommitResultV4::Sealed;
    }
    if (firstSequence > nextItemSequence_) {
        failed_ = true;
        return ConnectedCommitResultV4::SequenceGap;
    }
    if (firstRecordCursorV4(publication) != nextRecordCursor_) {
        failed_ = true;
        return ConnectedCommitResultV4::CursorMismatch;
    }
    if (firstDigestV4(publication) != digest_) {
        failed_ = true;
        return ConnectedCommitResultV4::DigestMismatch;
    }

    uint32_t stagedNextSequence = 0u;
    uint32_t stagedNextRecord = 0u;
    uint64_t stagedDigest = 0u;
    if (!verifyConnectedPublicationChainV4(
            publication, descriptor_.setStreamId, nextItemSequence_,
            nextRecordCursor_, digest_, stagedNextSequence,
            stagedNextRecord, stagedDigest)) {
        failed_ = true;
        return ConnectedCommitResultV4::DigestMismatch;
    }
    if (stagedNextSequence <= nextItemSequence_
        || stagedNextRecord < nextRecordCursor_) {
        failed_ = true;
        return ConnectedCommitResultV4::InvalidArgument;
    }
    const uint32_t addedItems =
        stagedNextSequence - nextItemSequence_;
    const uint32_t addedRecords =
        stagedNextRecord - nextRecordCursor_;
    if (addedItems > config_.maxItems - nextItemSequence_
        || addedRecords > config_.maxRecords - nextRecordCursor_) {
        failed_ = true;
        return ConnectedCommitResultV4::LimitReached;
    }
    const uint64_t memoryLiability =
        static_cast<uint64_t>(canonicalBytes.size())
        + sizeof(StoredConnectedPublicationV4);
    if (memoryLiability > config_.maxBytes - retainedBytes_) {
        failed_ = true;
        return ConnectedCommitResultV4::LimitReached;
    }

    OpenReplayValidation stagedReplay = replay_;
    ConnectedGameRangeV4 stagedRange{};
    bool appendRange = false;
    bool closeRange = false;
    uint64_t stagedLastPostStateHash = lastPostStateHash_;

    switch (publication.kind) {
        case ConnectedPublicationKindV4::BeginGameBundle: {
            const SpectatorPerGameStartV4& start =
                publication.begin.start;
            const ConnectedConfirmedRecordV4& frame =
                publication.begin.frameZero;
            if (stagedReplay.active
                || !qreplay::isValidGameIdentityV2(start.game)
                || start.game != start.start.manifest.identity
                || !frame.hasReplayGameIdentity
                || frame.game != start.game
                || frame.chain.recordCursorBefore != nextRecordCursor_
                || frame.frame.preStateHash
                       != start.start.start.frameZeroPreStateHash
                || start.start.start.initialRollingDigest
                       != qreplay::initialReplayDigestV2(start.start)) {
                failed_ = true;
                return ConnectedCommitResultV4::IdentityMismatch;
            }
            for (const ConnectedGameRangeV4& existing : ranges_) {
                if (existing.game == start.game) {
                    failed_ = true;
                    return ConnectedCommitResultV4::RangeOverlap;
                }
            }
            if (!ranges_.empty()
                && nextRecordCursor_
                    < ranges_.back().nextRecordCursor) {
                failed_ = true;
                return ConnectedCommitResultV4::RangeOverlap;
            }

            stagedReplay = OpenReplayValidation{};
            stagedReplay.active = true;
            stagedReplay.game = start.game;
            stagedReplay.startCursor = nextRecordCursor_;
            stagedReplay.rollingDigest =
                start.start.start.initialRollingDigest;
            if (!stagedReplay.canonical.begin(start.start)) {
                failed_ = true;
                return ConnectedCommitResultV4::ReplayDigestMismatch;
            }
            qreplay::CanonicalTickBundleV2 tick{};
            if (!rebaseConnectedRecordV4(
                    frame, stagedReplay.startCursor,
                    stagedReplay.nextEventSequence,
                    stagedReplay.rollingDigest, tick)
                || tick.frame.replayFrame != 0u
                || !appendTickToCanonical(
                    stagedReplay.canonical, tick)) {
                failed_ = true;
                return ConnectedCommitResultV4::ReplayDigestMismatch;
            }
            stagedReplay.lastPostStateHash = frame.frame.postStateHash;
            stagedLastPostStateHash = frame.frame.postStateHash;

            stagedRange.game = start.game;
            stagedRange.startRecordCursor = nextRecordCursor_;
            stagedRange.nextRecordCursor = stagedNextRecord;
            stagedRange.startItemSequence = nextItemSequence_;
            stagedRange.nextItemSequence = stagedNextSequence;
            stagedRange.start = start.start;
            appendRange = true;
            break;
        }

        case ConnectedPublicationKindV4::ConfirmedRecord: {
            const ConnectedConfirmedRecordV4& record =
                publication.record;
            if (stagedReplay.active) {
                if (!record.hasReplayGameIdentity
                    || record.game != stagedReplay.game) {
                    failed_ = true;
                    return ConnectedCommitResultV4::IdentityMismatch;
                }
                qreplay::CanonicalTickBundleV2 tick{};
                if (!rebaseConnectedRecordV4(
                        record, stagedReplay.startCursor,
                        stagedReplay.nextEventSequence,
                        stagedReplay.rollingDigest, tick)
                    || !appendTickToCanonical(
                        stagedReplay.canonical, tick)) {
                    failed_ = true;
                    return ConnectedCommitResultV4::ReplayDigestMismatch;
                }
                stagedReplay.lastPostStateHash =
                    record.frame.postStateHash;
            } else if (record.hasReplayGameIdentity) {
                failed_ = true;
                return ConnectedCommitResultV4::IdentityMismatch;
            }
            stagedLastPostStateHash = record.frame.postStateHash;
            break;
        }

        case ConnectedPublicationKindV4::SetEvent:
            if (!isValidSetEventPayloadV4(publication.setEvent.event)) {
                failed_ = true;
                return ConnectedCommitResultV4::InvalidArgument;
            }
            break;

        case ConnectedPublicationKindV4::Palette:
            if (publication.palette.palette.side > 3u
                || publication.palette.palette.variant > 3u
                || publication.palette.palette.present > 1u
                || publication.palette.palette.reserved != 0u) {
                failed_ = true;
                return ConnectedCommitResultV4::InvalidArgument;
            }
            break;

        case ConnectedPublicationKindV4::EndGameBundle: {
            if (!stagedReplay.active
                || publication.end.end.game != stagedReplay.game) {
                failed_ = true;
                return ConnectedCommitResultV4::IdentityMismatch;
            }
            const bool completed =
                publication.end.end.end.reason
                == qreplay::TerminalReason::Completed;
            if (completed != publication.end.containsFinalRecord) {
                failed_ = true;
                return ConnectedCommitResultV4::LifecycleViolation;
            }
            if (publication.end.containsFinalRecord) {
                const ConnectedConfirmedRecordV4& record =
                    publication.end.finalRecord;
                if (!record.hasReplayGameIdentity
                    || record.game != stagedReplay.game) {
                    failed_ = true;
                    return ConnectedCommitResultV4::IdentityMismatch;
                }
                qreplay::CanonicalTickBundleV2 tick{};
                if (!rebaseConnectedRecordV4(
                        record, stagedReplay.startCursor,
                        stagedReplay.nextEventSequence,
                        stagedReplay.rollingDigest, tick)
                    || !hasTerminalGameResult(tick)
                    || !appendTickToCanonical(
                        stagedReplay.canonical, tick)) {
                    failed_ = true;
                    return ConnectedCommitResultV4::LifecycleViolation;
                }
                stagedReplay.lastPostStateHash =
                    record.frame.postStateHash;
                stagedLastPostStateHash = record.frame.postStateHash;
            }
            const SpectatorPerGameEndV4& end = publication.end.end;
            const uint32_t expectedCount =
                stagedNextRecord - stagedReplay.startCursor;
            if (end.setNextRecordCursor != stagedNextRecord
                || end.end.nextRecord != expectedCount
                || end.end.postTailStateHash
                       != stagedReplay.lastPostStateHash
                || end.end.finalRollingDigest
                       != stagedReplay.rollingDigest
                || digestIsZero(end.end.canonicalGameDigest)) {
                failed_ = true;
                return ConnectedCommitResultV4::ReplayDigestMismatch;
            }
            std::array<uint8_t, 32> computed{};
            if (!stagedReplay.canonical.finish(end.end, computed)
                || computed != end.end.canonicalGameDigest) {
                failed_ = true;
                return ConnectedCommitResultV4::ReplayDigestMismatch;
            }
            stagedReplay = OpenReplayValidation{};
            stagedRange = ranges_.back();
            stagedRange.nextRecordCursor = stagedNextRecord;
            stagedRange.nextItemSequence = stagedNextSequence;
            stagedRange.end = end.end;
            stagedRange.complete = true;
            closeRange = true;
            break;
        }

        case ConnectedPublicationKindV4::SetSeal:
            if (stagedReplay.active
                || publication.seal.setNextRecordCursor
                       != stagedNextRecord
                || (stagedNextRecord != 0u
                    && publication.seal.postTailStateHash
                           != stagedLastPostStateHash)
                || (stagedNextRecord == 0u
                    && publication.seal.postTailStateHash != 0u)) {
                failed_ = true;
                return ConnectedCommitResultV4::LifecycleViolation;
            }
            break;
    }

    StoredConnectedPublicationV4 stored{};
    stored.kind = publication.kind;
    stored.firstItemSequence = nextItemSequence_;
    stored.nextItemSequence = stagedNextSequence;
    stored.firstRecordCursor = nextRecordCursor_;
    stored.nextRecordCursor = stagedNextRecord;
    stored.firstDigest = digest_;
    stored.finalDigest = stagedDigest;
    const uint64_t previousWireBytes = publications_.empty()
        ? 0u : publications_.back().cumulativeBytesAfter;
    stored.cumulativeBytesAfter =
        previousWireBytes + canonicalBytes.size();
    stored.canonicalBytes = std::move(canonicalBytes);

    publications_.push_back(std::move(stored));
    retainedBytes_ += memoryLiability;
    nextItemSequence_ = stagedNextSequence;
    nextRecordCursor_ = stagedNextRecord;
    digest_ = stagedDigest;
    replay_ = stagedReplay;
    lastPostStateHash_ = stagedLastPostStateHash;

    if (appendRange) ranges_.push_back(stagedRange);
    if (replay_.active && !ranges_.empty()) {
        ranges_.back().nextRecordCursor = nextRecordCursor_;
        ranges_.back().nextItemSequence = nextItemSequence_;
    }
    if (closeRange) ranges_.back() = stagedRange;

    if (publication.kind
        == ConnectedPublicationKindV4::SetSeal) {
        sealed_ = true;
        seal_ = publication.seal;
        sealItemSequence_ = publication.seal.chain.itemSequence;
        retainedUntilMs_ = saturatingAdd(
            monotonicNowMs, kSpectatorPostGameRetentionMsV4);
    }
    return ConnectedCommitResultV4::Committed;
}

bool SpectatorConnectedArchiveV4::openRange(
    ConnectedGameRangeV4& out) const {
    if (!replay_.active || ranges_.empty()) return false;
    out = ranges_.back();
    return true;
}

bool SpectatorConnectedArchiveV4::rangeByIdentity(
    const qreplay::GameIdentityV2& game,
    ConnectedGameRangeV4& out) const {
    for (const ConnectedGameRangeV4& range : ranges_) {
        if (range.game == game) {
            out = range;
            return true;
        }
    }
    return false;
}

bool SpectatorConnectedArchiveV4::newestCompleteRange(
    ConnectedGameRangeV4& out) const {
    for (auto it = ranges_.rbegin(); it != ranges_.rend(); ++it) {
        if (it->complete) {
            out = *it;
            return true;
        }
    }
    return false;
}

bool SpectatorConnectedArchiveV4::publicationAt(
    uint32_t firstItemSequence,
    ConnectedPublicationV4& out) const {
    const size_t index = findPublication(firstItemSequence);
    if (index == kNotFound) return false;
    const std::vector<uint8_t>& bytes =
        publications_[index].canonicalBytes;
    return decodeConnectedPublicationV4(
               bytes.data(), bytes.size(), out)
        == ConnectedCodecErrorV4::None;
}

bool SpectatorConnectedArchiveV4::storedPublicationAt(
    uint32_t firstItemSequence,
    StoredConnectedPublicationV4& out) const {
    const size_t index = findPublication(firstItemSequence);
    if (index == kNotFound) return false;
    out = publications_[index];
    return true;
}

bool SpectatorConnectedArchiveV4::resumeCursor(
    uint32_t nextItemSequence, uint32_t viewerGeneration,
    SpectatorResumeCursorV4& out) const {
    if (viewerGeneration == 0u) return false;
    uint32_t record = 0u;
    uint64_t proof = 0u;
    if (!proofAtBoundary(nextItemSequence, record, proof))
        return false;
    out.setStreamId = descriptor_.setStreamId;
    out.nextItemSequence = nextItemSequence;
    out.nextRecordCursor = record;
    out.setDigestBeforeNext = proof;
    out.viewerGeneration = viewerGeneration;
    return true;
}

bool SpectatorConnectedArchiveV4::validateResume(
    const SpectatorResumeCursorV4& cursor) const {
    if (cursor.setStreamId != descriptor_.setStreamId
        || cursor.viewerGeneration == 0u) {
        return false;
    }
    SpectatorResumeCursorV4 expected{};
    return resumeCursor(
               cursor.nextItemSequence, cursor.viewerGeneration,
               expected)
        && expected == cursor;
}

bool SpectatorConnectedArchiveV4::byteDistance(
    uint32_t firstItemSequence, uint32_t nextItemSequence,
    uint64_t& out) const {
    uint32_t ignoredRecord = 0u;
    uint64_t ignoredDigest = 0u;
    if (firstItemSequence > nextItemSequence
        || !proofAtBoundary(
            firstItemSequence, ignoredRecord, ignoredDigest)
        || !proofAtBoundary(
            nextItemSequence, ignoredRecord, ignoredDigest)) {
        return false;
    }
    auto prefixAt = [&](uint32_t sequence, uint64_t& prefix) {
        if (sequence == 0u) {
            prefix = 0u;
            return true;
        }
        if (sequence == nextItemSequence_) {
            prefix = publications_.empty()
                ? 0u : publications_.back().cumulativeBytesAfter;
            return true;
        }
        const size_t index = findPublication(sequence);
        if (index == kNotFound) return false;
        prefix = index == 0u
            ? 0u : publications_[index - 1u].cumulativeBytesAfter;
        return true;
    };
    uint64_t first = 0u;
    uint64_t next = 0u;
    if (!prefixAt(firstItemSequence, first)
        || !prefixAt(nextItemSequence, next)
        || next < first) {
        return false;
    }
    out = next - first;
    return true;
}

bool SpectatorConnectedArchiveV4::sealAck(
    SpectatorSetSealAckV4& out, uint64_t viewerId,
    uint32_t viewerGeneration) const {
    if (!sealed_ || viewerId == 0u || viewerGeneration == 0u)
        return false;
    out.viewerId = viewerId;
    out.setStreamId = descriptor_.setStreamId;
    out.sealItemSequence = sealItemSequence_;
    out.setNextRecordCursor = seal_.setNextRecordCursor;
    out.finalSetDigest = seal_.chain.setDigestAfter;
    out.viewerGeneration = viewerGeneration;
    return true;
}

}
