#pragma once

#include "SpectatorPublisher.h"

#include "../../replay/ReplayRecordingCoordinator.h"

#include <array>
#include <cstdint>

namespace qgame::spectator_publish {

enum class ConnectedReplayPublisherFaultV4 : uint8_t {
    None = 0,
    NotBound,
    InvalidCoordinatorRecord,
    QueueOverflow,
    QueueOrderViolation,
    AlreadySealed,
    CommitAckBacklog,
};

struct ConnectedReplayPublisherStatusV4 {
    bool bound = false;
    bool ordinaryEnabled = false;
    bool gameOpen = false;
    bool sealed = false;
    ConnectedReplayPublisherFaultV4 fault =
        ConnectedReplayPublisherFaultV4::None;
    uint32_t nextItemSequence = 0u;
    uint32_t nextRecordCursor = 0u;
    uint64_t setDigest = 0u;
    uint64_t acceptedPublications = 0u;
    uint64_t rejectedPublications = 0u;
};

class ConnectedReplayPublisherV4 final
    : public qreplay::ReplayRecordPublisherV2 {
public:
    explicit ConnectedReplayPublisherV4(
        ConnectedPublisherQueueV4& queue);

    bool reset(
        const qnetplay::spectator::ConnectedSetDescriptorV4& descriptor);

    bool tryPublishOrdinary(
        const qreplay::PublisherRecordV2& record) noexcept override;
    bool publishEndGameReserved(
        const qreplay::PublisherRecordV2& record) noexcept override;

    bool publishOutOfGameConfirmed(
        uint32_t sourceTimelineFrame,
        const qreplay::ConfirmedGameFrameCoreV2& frame) noexcept;
    bool publishSetEvent(
        const qnetplay::spectator::SetEventPayloadV4& event) noexcept;

    bool publishSetSeal(
        qnetplay::spectator::ConnectedSetTerminalReasonV4 reason,
        uint64_t postTailStateHash) noexcept;

    bool popAcceptedEndAck(qreplay::GameCommitAckV2& out) noexcept;

    ConnectedReplayPublisherStatusV4 status() const noexcept;

private:
    static constexpr size_t kAcceptedEndAckCapacity =
        qreplay::kMaxPendingReplayCommitsV2;

    ConnectedPublisherQueueV4& queue_;
    qnetplay::spectator::ConnectedChainBuilderV4 builder_{};
    qreplay::GameIdentityV2 openGame_{};
    bool bound_ = false;
    bool ordinaryEnabled_ = false;
    bool gameOpen_ = false;
    bool sealed_ = false;
    ConnectedReplayPublisherFaultV4 fault_ =
        ConnectedReplayPublisherFaultV4::None;
    uint64_t accepted_ = 0u;
    uint64_t rejected_ = 0u;
    std::array<qreplay::GameCommitAckV2, kAcceptedEndAckCapacity>
        acceptedEndAcks_{};
    size_t ackRead_ = 0u;
    size_t ackWrite_ = 0u;

    bool submit(
        qnetplay::spectator::ConnectedPublicationV4 publication,
        bool reservedEndOrSeal) noexcept;
    bool mapBegin(
        const qreplay::BeginGameBundleV2& source,
        qnetplay::spectator::ConnectedPublicationV4& out) const noexcept;
    bool mapTick(
        const qreplay::CanonicalTickBundleV2& source,
        const qreplay::GameIdentityV2& game,
        qnetplay::spectator::ConnectedConfirmedRecordV4& out) const noexcept;
    bool mapEnd(
        const qreplay::EndGameBundleV2& source,
        qnetplay::spectator::ConnectedPublicationV4& out) const noexcept;
    bool queueAcceptedEndAck(
        const qreplay::ConfirmedGameEndV2& end) noexcept;
    void reject(ConnectedReplayPublisherFaultV4 fault) noexcept;
};

}
