#include "SpectatorReplayPublisherV4.h"

#include <limits>

namespace qgame::spectator_publish {
namespace qsp = qnetplay::spectator;

namespace {

void stripReplayEventFields(qreplay::FixedEventListV2& list) {
    for (size_t i = 0u; i < list.count; ++i) {
        list.events[i].replayFrame = 0u;
        list.events[i].eventSequence = 0u;
        list.events[i].rollingDigest = 0u;
    }
}

ConnectedReplayPublisherFaultV4 queueFault(
    ConnectedPublishResultV4 result) {
    switch (result) {
        case ConnectedPublishResultV4::Overflow:
        case ConnectedPublishResultV4::Disabled:
            return ConnectedReplayPublisherFaultV4::QueueOverflow;
        case ConnectedPublishResultV4::OrderViolation:
            return ConnectedReplayPublisherFaultV4::QueueOrderViolation;
        case ConnectedPublishResultV4::InvalidArgument:
            return ConnectedReplayPublisherFaultV4::
                InvalidCoordinatorRecord;
        case ConnectedPublishResultV4::Accepted:
            break;
    }
    return ConnectedReplayPublisherFaultV4::None;
}

}

ConnectedReplayPublisherV4::ConnectedReplayPublisherV4(
    ConnectedPublisherQueueV4& queue)
    : queue_(queue) {}

bool ConnectedReplayPublisherV4::reset(
    const qsp::ConnectedSetDescriptorV4& descriptor) {
    queue_.reset();
    builder_ = qsp::ConnectedChainBuilderV4{};
    openGame_ = qreplay::GameIdentityV2{};
    bound_ = builder_.reset(descriptor);
    ordinaryEnabled_ = bound_;
    gameOpen_ = false;
    sealed_ = false;
    fault_ = bound_
        ? ConnectedReplayPublisherFaultV4::None
        : ConnectedReplayPublisherFaultV4::NotBound;
    accepted_ = 0u;
    rejected_ = 0u;
    ackRead_ = 0u;
    ackWrite_ = 0u;
    acceptedEndAcks_ = {};
    return bound_;
}

void ConnectedReplayPublisherV4::reject(
    ConnectedReplayPublisherFaultV4 fault) noexcept {
    ++rejected_;
    if (fault_ == ConnectedReplayPublisherFaultV4::None)
        fault_ = fault;
    if (fault == ConnectedReplayPublisherFaultV4::QueueOverflow
        || fault
            == ConnectedReplayPublisherFaultV4::InvalidCoordinatorRecord) {
        ordinaryEnabled_ = false;
    }
}

bool ConnectedReplayPublisherV4::mapTick(
    const qreplay::CanonicalTickBundleV2& source,
    const qreplay::GameIdentityV2& game,
    qsp::ConnectedConfirmedRecordV4& out) const noexcept {
    if (!qreplay::isValidGameIdentityV2(game)
        || !qreplay::isValidConfirmedGameFrameCoreV2(
            source.frame.core, qreplay::kFighterMaskV2)
        || source.frame.rollingDigest == 0u
        || source.preEvents.count > qreplay::kMaxEventsPerTickV2
        || source.postEvents.count > qreplay::kMaxEventsPerTickV2) {
        return false;
    }
    qsp::ConnectedConfirmedRecordV4 record{};
    record.hasReplayGameIdentity = true;
    record.game = game;
    record.sourceTimelineFrame = source.frame.sourceFrame;
    record.preEvents = source.preEvents;
    record.frame = source.frame.core;
    record.postEvents = source.postEvents;
    stripReplayEventFields(record.preEvents);
    stripReplayEventFields(record.postEvents);
    if (!qsp::isCanonicalConnectedRecordV4(record))
        return false;
    out = record;
    return true;
}

bool ConnectedReplayPublisherV4::mapBegin(
    const qreplay::BeginGameBundleV2& source,
    qsp::ConnectedPublicationV4& out) const noexcept {
    if (!qreplay::isValidGameManifestV2(source.start.manifest)
        || !qreplay::isValidDeterministicGameStartV2(
            source.start.start)
        || source.start.start.initialRollingDigest == 0u) {
        return false;
    }
    qsp::ConnectedPublicationV4 publication{};
    publication.kind =
        qsp::ConnectedPublicationKindV4::BeginGameBundle;
    publication.begin.start.game = source.start.manifest.identity;
    publication.begin.start.start = source.start;
    if (!mapTick(
            source.frameZero, source.start.manifest.identity,
            publication.begin.frameZero)) {
        return false;
    }
    out = publication;
    return true;
}

bool ConnectedReplayPublisherV4::mapEnd(
    const qreplay::EndGameBundleV2& source,
    qsp::ConnectedPublicationV4& out) const noexcept {
    if (!gameOpen_ || !qreplay::isValidGameIdentityV2(openGame_))
        return false;
    qsp::ConnectedPublicationV4 publication{};
    publication.kind =
        qsp::ConnectedPublicationKindV4::EndGameBundle;
    publication.end.containsFinalRecord =
        source.containsFinalFrame;
    if (source.containsFinalFrame
        && !mapTick(
            source.finalFrame, openGame_,
            publication.end.finalRecord)) {
        return false;
    }
    publication.end.end.game = openGame_;
    publication.end.end.setNextRecordCursor =
        builder_.nextRecordCursor()
        + (source.containsFinalFrame ? 1u : 0u);
    if (publication.end.end.setNextRecordCursor == 0u)
        publication.end.end.setNextRecordCursor = 1u;
    publication.end.end.end = source.end;
    out = publication;
    return true;
}

bool ConnectedReplayPublisherV4::submit(
    qsp::ConnectedPublicationV4 publication,
    bool reservedEndOrSeal) noexcept {
    if (!bound_) {
        reject(ConnectedReplayPublisherFaultV4::NotBound);
        return false;
    }
    if (sealed_) {
        reject(ConnectedReplayPublisherFaultV4::AlreadySealed);
        return false;
    }
    if (!reservedEndOrSeal && !ordinaryEnabled_) {
        reject(ConnectedReplayPublisherFaultV4::QueueOverflow);
        return false;
    }

    // Stamp only after queue acceptance: advancing the builder before a failed push leaves a permanent sequence and digest hole.
    qsp::ConnectedChainBuilderV4 staged = builder_;
    if (!staged.stamp(publication)) {
        reject(
            ConnectedReplayPublisherFaultV4::InvalidCoordinatorRecord);
        return false;
    }
    const ConnectedPublishResultV4 queued = queue_.publish(publication);
    if (queued != ConnectedPublishResultV4::Accepted) {
        reject(queueFault(queued));
        return false;
    }
    builder_ = staged;
    ++accepted_;
    switch (publication.kind) {
        case qsp::ConnectedPublicationKindV4::BeginGameBundle:
            gameOpen_ = true;
            openGame_ = publication.begin.start.game;
            break;
        case qsp::ConnectedPublicationKindV4::EndGameBundle:
            gameOpen_ = false;
            openGame_ = qreplay::GameIdentityV2{};
            break;
        case qsp::ConnectedPublicationKindV4::SetSeal:
            sealed_ = true;
            ordinaryEnabled_ = false;
            break;
        case qsp::ConnectedPublicationKindV4::ConfirmedRecord:
        case qsp::ConnectedPublicationKindV4::SetEvent:
            break;
    }
    return true;
}

bool ConnectedReplayPublisherV4::tryPublishOrdinary(
    const qreplay::PublisherRecordV2& record) noexcept {
    qsp::ConnectedPublicationV4 publication{};
    switch (record.kind) {
        case qreplay::PublisherRecordKindV2::BeginGame:
            if (gameOpen_ || !mapBegin(record.begin, publication)) {
                reject(ConnectedReplayPublisherFaultV4::
                           InvalidCoordinatorRecord);
                return false;
            }
            break;
        case qreplay::PublisherRecordKindV2::Tick:
            if (!gameOpen_) {
                reject(ConnectedReplayPublisherFaultV4::
                           QueueOrderViolation);
                return false;
            }
            publication.kind =
                qsp::ConnectedPublicationKindV4::ConfirmedRecord;
            if (!mapTick(record.tick, openGame_, publication.record)) {
                reject(ConnectedReplayPublisherFaultV4::
                           InvalidCoordinatorRecord);
                return false;
            }
            break;
        case qreplay::PublisherRecordKindV2::Palette:
            if (!gameOpen_) return true;
            publication.kind = qsp::ConnectedPublicationKindV4::Palette;
            publication.palette.palette = record.palette;
            break;
        case qreplay::PublisherRecordKindV2::EndGame:
            reject(ConnectedReplayPublisherFaultV4::
                       QueueOrderViolation);
            return false;
        default:
            reject(ConnectedReplayPublisherFaultV4::
                       InvalidCoordinatorRecord);
            return false;
    }
    return submit(publication, false);
}

bool ConnectedReplayPublisherV4::queueAcceptedEndAck(
    const qreplay::ConfirmedGameEndV2& end) noexcept {
    if (ackWrite_ - ackRead_ >= acceptedEndAcks_.size()) {
        reject(
            ConnectedReplayPublisherFaultV4::CommitAckBacklog);
        return false;
    }
    qreplay::GameCommitAckV2 ack{};
    ack.identity = openGame_;
    ack.status = qreplay::GameCommitStatusV2::Committed;
    ack.nextRecord = end.nextRecord;
    ack.canonicalGameDigest = end.canonicalGameDigest;
    acceptedEndAcks_[
        ackWrite_ % acceptedEndAcks_.size()] = ack;
    ++ackWrite_;
    return true;
}

bool ConnectedReplayPublisherV4::publishEndGameReserved(
    const qreplay::PublisherRecordV2& record) noexcept {
    if (record.kind != qreplay::PublisherRecordKindV2::EndGame) {
        reject(ConnectedReplayPublisherFaultV4::QueueOrderViolation);
        return false;
    }
    qsp::ConnectedPublicationV4 publication{};
    if (!mapEnd(record.end, publication)) {
        reject(
            ConnectedReplayPublisherFaultV4::InvalidCoordinatorRecord);
        return false;
    }
    const size_t priorAckWrite = ackWrite_;
    if (!queueAcceptedEndAck(record.end.end))
        return false;
    if (!submit(publication, true)) {
        ackWrite_ = priorAckWrite;
        return false;
    }
    return true;
}

bool ConnectedReplayPublisherV4::publishOutOfGameConfirmed(
    uint32_t sourceTimelineFrame,
    const qreplay::ConfirmedGameFrameCoreV2& frame) noexcept {
    if (gameOpen_
        || !qreplay::isValidConfirmedGameFrameCoreV2(
            frame, qreplay::kFighterMaskV2)) {
        reject(
            ConnectedReplayPublisherFaultV4::QueueOrderViolation);
        return false;
    }
    qsp::ConnectedPublicationV4 publication{};
    publication.kind =
        qsp::ConnectedPublicationKindV4::ConfirmedRecord;
    publication.record.sourceTimelineFrame = sourceTimelineFrame;
    publication.record.frame = frame;
    return submit(publication, false);
}

bool ConnectedReplayPublisherV4::publishSetEvent(
    const qsp::SetEventPayloadV4& event) noexcept {
    if (!qsp::isValidSetEventPayloadV4(event)) {
        reject(
            ConnectedReplayPublisherFaultV4::InvalidCoordinatorRecord);
        return false;
    }
    qsp::ConnectedPublicationV4 publication{};
    publication.kind = qsp::ConnectedPublicationKindV4::SetEvent;
    publication.setEvent.event = event;
    return submit(publication, false);
}

bool ConnectedReplayPublisherV4::publishSetSeal(
    qsp::ConnectedSetTerminalReasonV4 reason,
    uint64_t postTailStateHash) noexcept {
    if (gameOpen_
        || !qsp::isKnownConnectedSetTerminalReasonV4(
            static_cast<uint16_t>(reason))
        || postTailStateHash == 0u) {
        reject(
            ConnectedReplayPublisherFaultV4::QueueOrderViolation);
        return false;
    }
    qsp::ConnectedPublicationV4 publication{};
    publication.kind = qsp::ConnectedPublicationKindV4::SetSeal;
    publication.seal.reason = reason;
    publication.seal.postTailStateHash = postTailStateHash;
    return submit(publication, true);
}

bool ConnectedReplayPublisherV4::popAcceptedEndAck(
    qreplay::GameCommitAckV2& out) noexcept {
    if (ackRead_ == ackWrite_) return false;
    out = acceptedEndAcks_[
        ackRead_ % acceptedEndAcks_.size()];
    ++ackRead_;
    return true;
}

ConnectedReplayPublisherStatusV4
ConnectedReplayPublisherV4::status() const noexcept {
    ConnectedReplayPublisherStatusV4 out{};
    out.bound = bound_;
    out.ordinaryEnabled = ordinaryEnabled_;
    out.gameOpen = gameOpen_;
    out.sealed = sealed_;
    out.fault = fault_;
    out.nextItemSequence = builder_.nextItemSequence();
    out.nextRecordCursor = builder_.nextRecordCursor();
    out.setDigest = builder_.digest();
    out.acceptedPublications = accepted_;
    out.rejectedPublications = rejected_;
    return out;
}

}
