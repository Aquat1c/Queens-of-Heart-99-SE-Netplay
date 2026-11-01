#include "SpectatorConnectedSessionV4.h"

#include "../replay/ReplayIntegrity.h"

#include <algorithm>
#include <cstring>

namespace qnetplay::spectator {

SpectatorConnectedSourceSessionV4::SpectatorConnectedSourceSessionV4(
    const ConnectedSessionConfigV4& config)
    : config_(config) {}

bool SpectatorConnectedSourceSessionV4::bind(
    const SpectatorConnectedArchiveV4* archive) {
    if (archive == nullptr || !archive->bound() || archive->failed()
        || config_.maxViewers == 0u || config_.maxViewers > 32u
        || config_.maxUnackedItems == 0u
        || config_.maxUnackedItems > kMaxConnectedUnackedItemsV4
        || config_.maxUnackedBytes == 0u
        || config_.maxUnackedBytes > kMaxConnectedUnackedBytesV4) {
        return false;
    }
    archive_ = archive;
    viewers_.clear();
    viewers_.reserve(config_.maxViewers);
    return true;
}

ConnectedViewerStateV4* SpectatorConnectedSourceSessionV4::find(
    uint64_t viewerId) {
    for (ConnectedViewerStateV4& viewer : viewers_)
        if (viewer.viewerId == viewerId) return &viewer;
    return nullptr;
}

const ConnectedViewerStateV4* SpectatorConnectedSourceSessionV4::find(
    uint64_t viewerId) const {
    for (const ConnectedViewerStateV4& viewer : viewers_)
        if (viewer.viewerId == viewerId) return &viewer;
    return nullptr;
}

ConnectedViewerResultV4 SpectatorConnectedSourceSessionV4::attach(
    uint64_t viewerId, const SpectatorResumeCursorV4& resume,
    uint64_t monotonicNowMs) {
    if (archive_ == nullptr || viewerId == 0u
        || !archive_->validateResume(resume)) {
        return ConnectedViewerResultV4::InvalidResume;
    }
    if (archive_->expired(monotonicNowMs))
        return ConnectedViewerResultV4::Expired;

    ConnectedViewerStateV4* existing = find(viewerId);
    if (existing != nullptr) {
        if (resume.viewerGeneration < existing->generation)
            return ConnectedViewerResultV4::StaleGeneration;
        if (resume.viewerGeneration == existing->generation) {
            if (resume.nextItemSequence
                == existing->acknowledgedItemSequence) {
                return ConnectedViewerResultV4::ExactDuplicate;
            }
            return ConnectedViewerResultV4::InvalidResume;
        }
        existing->generation = resume.viewerGeneration;
        existing->acknowledgedItemSequence =
            resume.nextItemSequence;
        existing->sendItemSequence = resume.nextItemSequence;
        existing->sealAcknowledged = false;
        existing->failed = false;
        return ConnectedViewerResultV4::Ok;
    }
    if (viewers_.size() >= config_.maxViewers)
        return ConnectedViewerResultV4::CapacityReached;
    ConnectedViewerStateV4 viewer{};
    viewer.viewerId = viewerId;
    viewer.generation = resume.viewerGeneration;
    viewer.acknowledgedItemSequence = resume.nextItemSequence;
    viewer.sendItemSequence = resume.nextItemSequence;
    viewers_.push_back(viewer);
    return ConnectedViewerResultV4::Ok;
}

ConnectedViewerResultV4 SpectatorConnectedSourceSessionV4::detach(
    uint64_t viewerId) {
    const auto it = std::find_if(
        viewers_.begin(), viewers_.end(),
        [viewerId](const ConnectedViewerStateV4& viewer) {
            return viewer.viewerId == viewerId;
        });
    if (it == viewers_.end())
        return ConnectedViewerResultV4::UnknownViewer;
    viewers_.erase(it);
    return ConnectedViewerResultV4::Ok;
}

ConnectedViewerResultV4
SpectatorConnectedSourceSessionV4::nextPublication(
    uint64_t viewerId, ConnectedPublicationV4& out) {
    ConnectedViewerStateV4* viewer = find(viewerId);
    if (viewer == nullptr) return ConnectedViewerResultV4::UnknownViewer;
    if (viewer->failed) return ConnectedViewerResultV4::LiabilityLimit;
    if (viewer->sendItemSequence >= archive_->nextItemSequence())
        return ConnectedViewerResultV4::NoData;

    StoredConnectedPublicationV4 stored{};
    if (!archive_->storedPublicationAt(
            viewer->sendItemSequence, stored)) {
        viewer->failed = true;
        return ConnectedViewerResultV4::InvalidResume;
    }
    const uint32_t unackedItems =
        stored.nextItemSequence - viewer->acknowledgedItemSequence;
    uint64_t unackedBytes = 0u;
    if (stored.nextItemSequence
            < viewer->acknowledgedItemSequence
        || !archive_->byteDistance(
            viewer->acknowledgedItemSequence,
            stored.nextItemSequence, unackedBytes)
        || unackedItems > config_.maxUnackedItems
        || unackedBytes > config_.maxUnackedBytes) {
        viewer->failed = true;
        return ConnectedViewerResultV4::LiabilityLimit;
    }
    if (!archive_->publicationAt(viewer->sendItemSequence, out)) {
        viewer->failed = true;
        return ConnectedViewerResultV4::InvalidResume;
    }
    viewer->sendItemSequence = stored.nextItemSequence;
    return ConnectedViewerResultV4::Ok;
}

ConnectedViewerResultV4
SpectatorConnectedSourceSessionV4::rewindUnacked(uint64_t viewerId) {
    ConnectedViewerStateV4* viewer = find(viewerId);
    if (viewer == nullptr) return ConnectedViewerResultV4::UnknownViewer;
    if (viewer->failed) return ConnectedViewerResultV4::LiabilityLimit;
    viewer->sendItemSequence = viewer->acknowledgedItemSequence;
    return ConnectedViewerResultV4::Ok;
}

ConnectedViewerResultV4
SpectatorConnectedSourceSessionV4::acknowledge(
    const SpectatorAckV4& ack) {
    ConnectedViewerStateV4* viewer = find(ack.viewerId);
    if (viewer == nullptr) return ConnectedViewerResultV4::UnknownViewer;
    if (ack.committed.viewerGeneration != viewer->generation)
        return ack.committed.viewerGeneration < viewer->generation
            ? ConnectedViewerResultV4::StaleGeneration
            : ConnectedViewerResultV4::ConflictingAck;
    if (!archive_->validateResume(ack.committed))
        return ConnectedViewerResultV4::ConflictingAck;
    if (ack.committed.nextItemSequence
        < viewer->acknowledgedItemSequence) {
        return ConnectedViewerResultV4::StaleAck;
    }
    if (ack.committed.nextItemSequence
        > viewer->sendItemSequence) {
        return ConnectedViewerResultV4::ConflictingAck;
    }
    if (ack.committed.nextItemSequence
        == viewer->acknowledgedItemSequence) {
        return ConnectedViewerResultV4::ExactDuplicate;
    }
    viewer->acknowledgedItemSequence =
        ack.committed.nextItemSequence;
    return ConnectedViewerResultV4::Ok;
}

ConnectedViewerResultV4
SpectatorConnectedSourceSessionV4::acknowledgeSeal(
    const SpectatorSetSealAckV4& ack) {
    ConnectedViewerStateV4* viewer = find(ack.viewerId);
    if (viewer == nullptr) return ConnectedViewerResultV4::UnknownViewer;
    if (ack.viewerGeneration != viewer->generation)
        return ack.viewerGeneration < viewer->generation
            ? ConnectedViewerResultV4::StaleGeneration
            : ConnectedViewerResultV4::SealAckMismatch;
    SpectatorSetSealAckV4 expected{};
    if (!archive_->sealAck(
            expected, ack.viewerId, ack.viewerGeneration)
        || !(expected == ack)) {
        return ConnectedViewerResultV4::SealAckMismatch;
    }
    if (viewer->sealAcknowledged)
        return ConnectedViewerResultV4::ExactDuplicate;
    viewer->sealAcknowledged = true;
    return ConnectedViewerResultV4::Ok;
}

void SpectatorConnectedSourceSessionV4::enforceRetention(
    uint64_t monotonicNowMs) {
    if (archive_ != nullptr && archive_->expired(monotonicNowMs))
        viewers_.clear();
}

bool SpectatorConnectedSourceSessionV4::viewer(
    uint64_t viewerId, ConnectedViewerStateV4& out) const {
    const ConnectedViewerStateV4* value = find(viewerId);
    if (value == nullptr) return false;
    out = *value;
    return true;
}

void ConnectedPublicationAssemblerV4::reset() {
    active_ = false;
    failed_ = false;
    setStreamId_ = 0u;
    firstItemSequence_ = 0u;
    kind_ = ConnectedPublicationKindV4::ConfirmedRecord;
    fragmentCount_ = 0u;
    totalBytes_ = 0u;
    checksum_ = 0u;
    bytes_.clear();
    received_.clear();
    receivedCount_ = 0u;
}

ConnectedAssemblyResultV4 ConnectedPublicationAssemblerV4::accept(
    const ConnectedPublicationFragmentV4& fragment,
    ConnectedPublicationV4& completed) {
    if (failed_) return ConnectedAssemblyResultV4::Conflict;
    std::vector<uint8_t> encodedFragment;
    if (!encodeConnectedFragmentV4(fragment, encodedFragment)) {
        failed_ = true;
        return ConnectedAssemblyResultV4::Invalid;
    }
    if (!active_) {
        if (fragment.totalBytes > kMaxConnectedPublicationBytesV4
            || fragment.fragmentCount > kMaxConnectedFragmentsV4) {
            failed_ = true;
            return ConnectedAssemblyResultV4::LimitReached;
        }
        active_ = true;
        setStreamId_ = fragment.setStreamId;
        firstItemSequence_ = fragment.firstItemSequence;
        kind_ = fragment.publicationKind;
        fragmentCount_ = fragment.fragmentCount;
        totalBytes_ = fragment.totalBytes;
        checksum_ = fragment.publicationCrc32c;
        bytes_.assign(totalBytes_, 0u);
        received_.assign(fragmentCount_, 0u);
    } else if (fragment.setStreamId != setStreamId_
               || fragment.firstItemSequence != firstItemSequence_
               || fragment.publicationKind != kind_
               || fragment.fragmentCount != fragmentCount_
               || fragment.totalBytes != totalBytes_
               || fragment.publicationCrc32c != checksum_) {
        failed_ = true;
        return ConnectedAssemblyResultV4::Conflict;
    }

    const size_t index = fragment.fragmentIndex;
    const size_t offset = fragment.byteOffset;
    if (received_[index] != 0u) {
        if (std::memcmp(
                bytes_.data() + offset, fragment.bytes.data(),
                fragment.bytes.size()) == 0) {
            return ConnectedAssemblyResultV4::ExactDuplicate;
        }
        failed_ = true;
        return ConnectedAssemblyResultV4::Conflict;
    }
    std::memcpy(
        bytes_.data() + offset, fragment.bytes.data(),
        fragment.bytes.size());
    received_[index] = 1u;
    ++receivedCount_;
    if (receivedCount_ != fragmentCount_)
        return ConnectedAssemblyResultV4::Accepted;

    if (qreplay::crc32c(bytes_.data(), bytes_.size()) != checksum_
        || decodeConnectedPublicationV4(
               bytes_.data(), bytes_.size(), completed)
               != ConnectedCodecErrorV4::None
        || completed.kind != kind_
        || firstItemSequenceV4(completed) != firstItemSequence_) {
        failed_ = true;
        return ConnectedAssemblyResultV4::Invalid;
    }
    active_ = false;
    bytes_.clear();
    received_.clear();
    receivedCount_ = 0u;
    return ConnectedAssemblyResultV4::Complete;
}

}
