#pragma once

#include "SpectatorConnectedArchiveV4.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qnetplay::spectator {

struct ConnectedSessionConfigV4 {
    size_t maxViewers = 32u;
    uint32_t maxUnackedItems = kMaxConnectedUnackedItemsV4;
    uint64_t maxUnackedBytes = kMaxConnectedUnackedBytesV4;
};

enum class ConnectedViewerResultV4 : uint8_t {
    Ok,
    ExactDuplicate,
    UnknownViewer,
    CapacityReached,
    InvalidResume,
    StaleGeneration,
    ConflictingAck,
    StaleAck,
    NoData,
    LiabilityLimit,
    SealAckMismatch,
    Expired,
};

struct ConnectedViewerStateV4 {
    uint64_t viewerId = 0u;
    uint32_t generation = 0u;
    uint32_t acknowledgedItemSequence = 0u;
    uint32_t sendItemSequence = 0u;
    bool sealAcknowledged = false;
    bool failed = false;
};

class SpectatorConnectedSourceSessionV4 {
public:
    explicit SpectatorConnectedSourceSessionV4(
        const ConnectedSessionConfigV4& config = {});

    bool bind(const SpectatorConnectedArchiveV4* archive);
    ConnectedViewerResultV4 attach(
        uint64_t viewerId, const SpectatorResumeCursorV4& resume,
        uint64_t monotonicNowMs);
    ConnectedViewerResultV4 detach(uint64_t viewerId);

    ConnectedViewerResultV4 nextPublication(
        uint64_t viewerId, ConnectedPublicationV4& out);
    ConnectedViewerResultV4 rewindUnacked(uint64_t viewerId);

    ConnectedViewerResultV4 acknowledge(const SpectatorAckV4& ack);
    ConnectedViewerResultV4 acknowledgeSeal(
        const SpectatorSetSealAckV4& ack);
    void enforceRetention(uint64_t monotonicNowMs);

    bool viewer(uint64_t viewerId, ConnectedViewerStateV4& out) const;
    size_t viewerCount() const { return viewers_.size(); }

private:
    ConnectedSessionConfigV4 config_{};
    const SpectatorConnectedArchiveV4* archive_ = nullptr;
    std::vector<ConnectedViewerStateV4> viewers_;

    ConnectedViewerStateV4* find(uint64_t viewerId);
    const ConnectedViewerStateV4* find(uint64_t viewerId) const;
};

enum class ConnectedAssemblyResultV4 : uint8_t {
    Accepted,
    ExactDuplicate,
    Complete,
    Invalid,
    Conflict,
    LimitReached,
};

class ConnectedPublicationAssemblerV4 {
public:
    ConnectedAssemblyResultV4 accept(
        const ConnectedPublicationFragmentV4& fragment,
        ConnectedPublicationV4& completed);
    void reset();
    bool failed() const { return failed_; }

private:
    bool active_ = false;
    bool failed_ = false;
    uint64_t setStreamId_ = 0u;
    uint32_t firstItemSequence_ = 0u;
    ConnectedPublicationKindV4 kind_ =
        ConnectedPublicationKindV4::ConfirmedRecord;
    uint16_t fragmentCount_ = 0u;
    uint32_t totalBytes_ = 0u;
    uint32_t checksum_ = 0u;
    std::vector<uint8_t> bytes_;
    std::vector<uint8_t> received_;
    uint16_t receivedCount_ = 0u;
};

}
