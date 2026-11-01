#include "ReplayRecordingCoordinator.h"

#include <limits>

namespace qreplay {
namespace {

constexpr uint32_t kKnownRoundTransitionFlagsV2 =
    RoundBecameActiveV2
    | RoundOutcomeCommittedV2
    | FinalGameResultCommittedV2;
ReplayPublishResultV2 result(
    const ReplayPublishCodeV2 code,
    const ReplayRecordingFaultV2 fault =
        ReplayRecordingFaultV2::None) {
    ReplayPublishResultV2 out{};
    out.code = code;
    out.fault = fault;
    return out;
}

bool isValidStageProof(const StageIdentityV2& stage) {
    if (!stage.verified) {
        return stage.nativeStageId == 0u
            && stage.catalogEntryId == 0u;
    }
    return stage.nativeStageId >= kMinNativeStageIdV2
        && stage.nativeStageId <= kMaxNativeStageIdV2
        && stage.catalogEntryId != 0u;
}

bool exactAck(
    const GameCommitAckV2& a, const GameCommitAckV2& b) {
    return a.identity == b.identity
        && a.status == b.status
        && a.nextRecord == b.nextRecord
        && a.canonicalGameDigest == b.canonicalGameDigest
        && a.fileDigest == b.fileDigest;
}

bool isKnownCloseProvenance(
    const ReplayCloseProvenanceV2 provenance) {
    const uint8_t value = static_cast<uint8_t>(provenance);
    return value >= static_cast<uint8_t>(
                        ReplayCloseProvenanceV2::PeerOrderly)
        && value <= static_cast<uint8_t>(
                        ReplayCloseProvenanceV2::RecorderChannel);
}

bool isRuntimeAbnormalReason(const TerminalReason reason) {
    return reason == TerminalReason::Disconnected
        || reason == TerminalReason::ProcessExit
        || reason == TerminalReason::RecordingFailure;
}

bool isOneHotWinner(
    const uint8_t mask, const int32_t winnerSlot) {
    if (winnerSlot < 0 || winnerSlot > 1) return false;
    return mask
        == static_cast<uint8_t>(
            1u << static_cast<uint32_t>(winnerSlot));
}

bool incrementedExactlyOneTally(
    const std::array<uint8_t, 2>& before,
    const std::array<uint8_t, 2>& after,
    const int32_t winnerSlot) {
    if (winnerSlot < 0 || winnerSlot > 1) return false;
    const size_t winner = static_cast<size_t>(winnerSlot);
    const size_t loser = 1u - winner;
    return before[winner] != std::numeric_limits<uint8_t>::max()
        && after[winner]
            == static_cast<uint8_t>(before[winner] + 1u)
        && after[loser] == before[loser];
}

}

const char* replayRecordingFaultTextV2(
    const ReplayRecordingFaultV2 fault) {
    switch (fault) {
        case ReplayRecordingFaultV2::None:
            return "none";
        case ReplayRecordingFaultV2::InvalidFreshGame:
            return "invalid fresh-game descriptor";
        case ReplayRecordingFaultV2::UnsupportedTopology:
            return "unsupported replay topology";
        case ReplayRecordingFaultV2::ConflictingFreshGame:
            return "conflicting fresh-game descriptor";
        case ReplayRecordingFaultV2::CommitBacklog:
            return "replay commit acknowledgement backlog";
        case ReplayRecordingFaultV2::WrongGameIdentity:
            return "frame belongs to another game";
        case ReplayRecordingFaultV2::NonContiguousSource:
            return "confirmed source frame is not contiguous";
        case ReplayRecordingFaultV2::InvalidFrame:
            return "invalid proven game frame";
        case ReplayRecordingFaultV2::InvalidRoundTransition:
            return "invalid retained round transition proof";
        case ReplayRecordingFaultV2::MissingInitialSetup:
            return "missing exact initial -1 to 0 setup transition";
        case ReplayRecordingFaultV2::FinalWinScreenPredicateMismatch:
            return "first winscreen frame predicate mismatch";
        case ReplayRecordingFaultV2::TimelineValidationFailure:
            return "confirmed replay timeline validation failed";
        case ReplayRecordingFaultV2::PublisherOverflow:
            return "ordinary replay publisher capacity exhausted";
        case ReplayRecordingFaultV2::ReservedEndFailure:
            return "reserved replay terminal publication failed";
        case ReplayRecordingFaultV2::InvalidTerminalReason:
            return "invalid runtime replay terminal reason";
        case ReplayRecordingFaultV2::CommitAckStale:
            return "stale replay commit acknowledgement";
        case ReplayRecordingFaultV2::CommitAckConflict:
            return "conflicting replay commit acknowledgement";
    }
    return "unknown";
}

ReplayRecordingCoordinatorV2::ReplayRecordingCoordinatorV2(
    ReplayRecordPublisherV2& publisher)
    : publisher_(publisher) {}

void ReplayRecordingCoordinatorV2::reset(const bool enabled) {
    clearCurrent();
    pendingCommits_ = {};
    commitSerial_ = 0u;
    lastFault_ = ReplayRecordingFaultV2::None;
    lastTimelineFault_ = ConfirmedTimelineFaultV2::None;
    timelineExpectedLifecycle_ = {};
    timelineObservedLifecycle_ = {};
    timelineExpectedStateProof_ = 0u;
    timelineObservedStateProof_ = 0u;
    state_ = enabled
        ? ReplayGameRecordStateV2::WaitingForFreshGame
        : ReplayGameRecordStateV2::Disabled;
}

ReplayPublishResultV2 ReplayRecordingCoordinatorV2::armFreshGame(
    const FreshGameDescriptorV2& descriptor) {
    if (state_ == ReplayGameRecordStateV2::Disabled)
        return result(ReplayPublishCodeV2::Ignored);
    if (state_ == ReplayGameRecordStateV2::Failed)
        return result(ReplayPublishCodeV2::Failed, lastFault_);

    if (state_ == ReplayGameRecordStateV2::WaitingForFrameZero) {
        if (descriptor.gameEpoch == current_.descriptor.gameEpoch
            && descriptor.launchSourceFrame
                == current_.descriptor.launchSourceFrame
            && descriptor.start == current_.descriptor.start) {
            return result(ReplayPublishCodeV2::Armed);
        }
        return failWithoutFile(
            ReplayRecordingFaultV2::ConflictingFreshGame);
    }

    if (state_ == ReplayGameRecordStateV2::Recording
        || state_
            == ReplayGameRecordStateV2::AwaitFirstWinScreenTick) {
        return failCurrent(
            ReplayRecordingFaultV2::ConflictingFreshGame,
            TerminalReason::RecordingFailure);
    }

    if (state_
        != ReplayGameRecordStateV2::WaitingForFreshGame) {
        return result(ReplayPublishCodeV2::Ignored);
    }

    const ConfirmedGameStartV2& start = descriptor.start;
    if (descriptor.gameEpoch == 0u
        || descriptor.launchSourceFrame
            == std::numeric_limits<uint32_t>::max()
        || descriptor.gameEpoch
            != start.manifest.identity.gameGeneration
        || !isValidStagedFreshGameStartV2(start)
        || start.start.frameZeroPreStateHash != 0u
        || start.start.initialRollingDigest != 0u
        || !isValidStageProof(start.manifest.stage)) {
        return failWithoutFile(
            ReplayRecordingFaultV2::InvalidFreshGame);
    }
    if (!topologyQualified(descriptor)) {
        lastFault_ = ReplayRecordingFaultV2::UnsupportedTopology;
        return result(
            ReplayPublishCodeV2::Unsupported, lastFault_);
    }

    clearCurrent();
    current_.descriptor = descriptor;
    current_.start = descriptor.start;
    state_ = ReplayGameRecordStateV2::WaitingForFrameZero;
    lastFault_ = ReplayRecordingFaultV2::None;
    return result(ReplayPublishCodeV2::Armed);
}

ReplayPublishResultV2
ReplayRecordingCoordinatorV2::acceptConfirmedFrame(
    const ProvenGameFrameV2& source) {
    switch (state_) {
        case ReplayGameRecordStateV2::Disabled:
        case ReplayGameRecordStateV2::WaitingForFreshGame:
            return result(ReplayPublishCodeV2::Ignored);
        case ReplayGameRecordStateV2::WaitingForFrameZero:
            if (source.gameEpoch != current_.descriptor.gameEpoch)
                return result(ReplayPublishCodeV2::Ignored);
            return acceptWaitingForFrameZero(source);
        case ReplayGameRecordStateV2::Recording:
            return acceptRecording(source);
        case ReplayGameRecordStateV2::AwaitFirstWinScreenTick:
            return acceptFirstWinScreen(source);
        case ReplayGameRecordStateV2::Failed:
            return result(ReplayPublishCodeV2::Failed, lastFault_);
    }
    return result(ReplayPublishCodeV2::Ignored);
}

ReplayPublishResultV2
ReplayRecordingCoordinatorV2::acceptWaitingForFrameZero(
    const ProvenGameFrameV2& source) {
    if (source.game != current_.start.manifest.identity)
        return failWithoutFile(
            ReplayRecordingFaultV2::WrongGameIdentity);
    if (!validateProvenFrame(source))
        return failWithoutFile(
            ReplayRecordingFaultV2::InvalidFrame);
    if (!validateRoundProof(source))
        return failWithoutFile(
            ReplayRecordingFaultV2::InvalidRoundTransition);

    if (!current_.initialSetupSeen) {
        if (!isInitialSetup(source)) {
            if ((source.core.before.scene == 2u
                    && source.core.before.matchSub == 0)
                || (source.core.after.scene == 2u
                    && source.core.after.matchSub == 0)) {
                return failWithoutFile(
                    ReplayRecordingFaultV2::MissingInitialSetup);
            }
            return result(ReplayPublishCodeV2::Ignored);
        }
        if (source.roundTransition.roundOrdinal != 1u
            || source.roundTransition.talliesAfter
                != std::array<uint8_t, 2>{}
            || source.sourceFrame
                == std::numeric_limits<uint32_t>::max()) {
            return failWithoutFile(
                ReplayRecordingFaultV2::MissingInitialSetup);
        }
        current_.initialSetupSeen = true;
        current_.expectedFrameZeroSource = source.sourceFrame + 1u;
        current_.expectedFrameZeroBefore = source.core.after;
        current_.initialTransitionSiteId =
            source.roundTransition.transitionSiteId;
        return result(ReplayPublishCodeV2::Armed);
    }

    if (source.sourceFrame != current_.expectedFrameZeroSource)
        return failWithoutFile(
            ReplayRecordingFaultV2::NonContiguousSource);
    if (!isFirstActiveFrame(source)
        || source.core.before != current_.expectedFrameZeroBefore) {
        return failWithoutFile(
            ReplayRecordingFaultV2::MissingInitialSetup);
    }
    if (!commitSlotAvailable()) {
        clearCurrent();
        state_ = ReplayGameRecordStateV2::Disabled;
        lastFault_ = ReplayRecordingFaultV2::CommitBacklog;
        return result(ReplayPublishCodeV2::Failed, lastFault_);
    }

    if (!isValidStageProof(source.stage) || !source.stage.verified) {
        return failWithoutFile(
            ReplayRecordingFaultV2::InvalidFreshGame);
    }
    if (current_.start.manifest.stage.verified
        && current_.start.manifest.stage != source.stage) {
        return failWithoutFile(
            ReplayRecordingFaultV2::InvalidFreshGame);
    }
    current_.start.manifest.stage = source.stage;
    current_.start.start.frameZeroPreStateHash =
        source.core.preStateHash;
    current_.start.start.initialRollingDigest =
        initialReplayDigestV2(current_.start);

    ConfirmedTimelineValidatorV2 candidateValidator{};
    CanonicalGameDigestV2 candidateCanonical{};
    ParseStatus why = ParseStatus::Ok;
    if (!candidateValidator.begin(current_.start, why)
        || !candidateCanonical.begin(current_.start)) {
        return failWithoutFile(
            ReplayRecordingFaultV2::TimelineValidationFailure);
    }

    CanonicalTickBundleV2 tick{};
    tick.frame.sourceFrame = source.sourceFrame;
    tick.frame.replayFrame = 0u;
    tick.frame.core = source.core;
    tick.preEvents.count = 2u;
    tick.preEvents.events[0] = makeEvent(
        GameEventKindV2::GameStarted,
        GameEventPhaseV2::PreFrame, 0u, 0u, 0u, 0u,
        std::array<uint8_t, 2>{}, -1,
        source.core.preStateHash);
    tick.preEvents.events[1] = makeEvent(
        GameEventKindV2::RoundStarted,
        GameEventPhaseV2::PreFrame, 0u,
        current_.initialTransitionSiteId, 1u, 0u,
        std::array<uint8_t, 2>{}, -1,
        source.core.preStateHash);
    if (!stampAndValidateTick(
            tick, candidateValidator, candidateCanonical)) {
        return failWithoutFile(
            ReplayRecordingFaultV2::TimelineValidationFailure);
    }

    PublisherRecordV2 record{};
    record.kind = PublisherRecordKindV2::BeginGame;
    record.begin.start = current_.start;
    record.begin.frameZero = tick;
    if (!publisher_.tryPublishOrdinary(record)) {
        return failWithoutFile(
            ReplayRecordingFaultV2::PublisherOverflow);
    }

    validator_ = candidateValidator;
    canonical_ = candidateCanonical;
    current_.lastAccepted = source;
    current_.hasAcceptedFrame = true;
    current_.activeRoundOrdinal = 1u;
    current_.roundEndedForActive = false;
    state_ = ReplayGameRecordStateV2::Recording;
    lastFault_ = ReplayRecordingFaultV2::None;
    return result(ReplayPublishCodeV2::Published);
}

ReplayPublishResultV2
ReplayRecordingCoordinatorV2::acceptRecording(
    const ProvenGameFrameV2& source) {
    if (source.gameEpoch != current_.descriptor.gameEpoch
        || source.game != current_.start.manifest.identity) {
        return failCurrent(
            ReplayRecordingFaultV2::WrongGameIdentity,
            TerminalReason::RecordingFailure);
    }
    if (current_.lastAccepted.sourceFrame
            == std::numeric_limits<uint32_t>::max()
        || source.sourceFrame
            != current_.lastAccepted.sourceFrame + 1u) {
        return failCurrent(
            ReplayRecordingFaultV2::NonContiguousSource,
            TerminalReason::RecordingFailure);
    }
    if (!validateProvenFrame(source)) {
        return failCurrent(
            ReplayRecordingFaultV2::InvalidFrame,
            TerminalReason::RecordingFailure);
    }
    if (!validateRoundProof(source)) {
        return failCurrent(
            ReplayRecordingFaultV2::InvalidRoundTransition,
            TerminalReason::RecordingFailure);
    }

    const uint32_t flags = source.roundTransition.flags;
    if (current_.pendingRoundStarted) {
        if (flags != RoundTransitionNoneV2
            || source.core.before.scene != 2u
            || source.core.before.matchSub != 0) {
            return failCurrent(
                ReplayRecordingFaultV2::InvalidRoundTransition,
                TerminalReason::RecordingFailure);
        }
    }
    if (flags == RoundOutcomeCommittedV2) {
        if (current_.roundEndedForActive
            || source.roundTransition.roundOrdinal
                != current_.activeRoundOrdinal) {
            return failCurrent(
                ReplayRecordingFaultV2::InvalidRoundTransition,
                TerminalReason::RecordingFailure);
        }
    } else if (flags == RoundBecameActiveV2) {
        if (!current_.roundEndedForActive
            || current_.pendingRoundStarted
            || current_.activeRoundOrdinal
                == std::numeric_limits<uint8_t>::max()
            || source.roundTransition.roundOrdinal
                != static_cast<uint8_t>(
                    current_.activeRoundOrdinal + 1u)) {
            return failCurrent(
                ReplayRecordingFaultV2::InvalidRoundTransition,
                TerminalReason::RecordingFailure);
        }
    } else if (flags == FinalGameResultCommittedV2) {
        if (!current_.roundEndedForActive
            || current_.pendingRoundStarted
            || source.roundTransition.roundOrdinal
                != current_.activeRoundOrdinal) {
            return failCurrent(
                ReplayRecordingFaultV2::InvalidRoundTransition,
                TerminalReason::RecordingFailure);
        }
    }

    CanonicalTickBundleV2 tick{};
    tick.frame.sourceFrame = source.sourceFrame;
    tick.frame.replayFrame = validator_.nextRecord();
    tick.frame.core = source.core;

    const bool startsRound = current_.pendingRoundStarted;
    if (startsRound) {
        tick.preEvents.count = 1u;
        tick.preEvents.events[0] = makeEvent(
            GameEventKindV2::RoundStarted,
            GameEventPhaseV2::PreFrame, tick.frame.replayFrame,
            current_.pendingRoundTransitionSiteId,
            current_.pendingRoundOrdinal, 0u,
            std::array<uint8_t, 2>{}, -1,
            source.core.preStateHash);
    }
    if (flags == RoundOutcomeCommittedV2) {
        tick.postEvents.count = 1u;
        tick.postEvents.events[0] = makeEvent(
            GameEventKindV2::RoundEnded,
            GameEventPhaseV2::PostFrame, tick.frame.replayFrame,
            source.roundTransition.transitionSiteId,
            source.roundTransition.roundOrdinal,
            source.roundTransition.winningSideMask,
            source.roundTransition.talliesAfter,
            source.roundTransition.winnerSlot,
            source.core.postStateHash);
    }

    ConfirmedTimelineValidatorV2 candidateValidator = validator_;
    CanonicalGameDigestV2 candidateCanonical = canonical_;
    timelineExpectedLifecycle_ = current_.lastAccepted.core.after;
    timelineObservedLifecycle_ = source.core.before;
    timelineExpectedStateProof_ =
        current_.lastAccepted.core.postStateHash;
    timelineObservedStateProof_ = source.core.preStateHash;
    if (!stampAndValidateTick(
            tick, candidateValidator, candidateCanonical)) {
        return failCurrent(
            ReplayRecordingFaultV2::TimelineValidationFailure,
            TerminalReason::RecordingFailure);
    }

    PublisherRecordV2 record{};
    record.kind = PublisherRecordKindV2::Tick;
    record.tick = tick;
    if (!publisher_.tryPublishOrdinary(record)) {
        return failCurrent(
            ReplayRecordingFaultV2::PublisherOverflow,
            TerminalReason::RecorderOverflow);
    }

    validator_ = candidateValidator;
    canonical_ = candidateCanonical;
    current_.lastAccepted = source;
    current_.hasAcceptedFrame = true;

    if (startsRound) {
        current_.activeRoundOrdinal =
            current_.pendingRoundOrdinal;
        current_.pendingRoundStarted = false;
        current_.pendingRoundOrdinal = 0u;
        current_.pendingRoundTransitionSiteId = 0u;
        current_.roundEndedForActive = false;
    }
    if (flags == RoundOutcomeCommittedV2) {
        current_.roundEndedForActive = true;
    } else if (flags == RoundBecameActiveV2) {
        current_.pendingRoundStarted = true;
        current_.pendingRoundOrdinal =
            source.roundTransition.roundOrdinal;
        current_.pendingRoundTransitionSiteId =
            source.roundTransition.transitionSiteId;
    } else if (flags == FinalGameResultCommittedV2) {
        FinalLatch latch{};
        latch.active = true;
        latch.expectedSourceFrame = source.sourceFrame + 1u;
        latch.gameEpoch = source.gameEpoch;
        latch.transitionSiteId =
            source.roundTransition.transitionSiteId;
        latch.roundOrdinal =
            source.roundTransition.roundOrdinal;
        latch.winningSideMask =
            source.roundTransition.winningSideMask;
        latch.winnerSlot = source.roundTransition.winnerSlot;
        latch.roundTallies =
            source.roundTransition.talliesAfter;
        current_.finalLatch = latch;
        state_ =
            ReplayGameRecordStateV2::AwaitFirstWinScreenTick;
    }

    lastFault_ = ReplayRecordingFaultV2::None;
    return result(ReplayPublishCodeV2::Published);
}

ReplayPublishResultV2
ReplayRecordingCoordinatorV2::acceptFirstWinScreen(
    const ProvenGameFrameV2& source) {
    if (source.gameEpoch != current_.descriptor.gameEpoch
        || source.game != current_.start.manifest.identity) {
        return failCurrent(
            ReplayRecordingFaultV2::WrongGameIdentity,
            TerminalReason::RecordingFailure);
    }
    if (!validateProvenFrame(source)
        || !validateRoundProof(source)
        || !matchesFirstWinScreenTick(source)) {
        return failCurrent(
            ReplayRecordingFaultV2::FinalWinScreenPredicateMismatch,
            TerminalReason::RecordingFailure);
    }

    CanonicalTickBundleV2 tick{};
    tick.frame.sourceFrame = source.sourceFrame;
    tick.frame.replayFrame = validator_.nextRecord();
    tick.frame.core = source.core;
    tick.postEvents.count = 1u;
    tick.postEvents.events[0] = makeEvent(
        GameEventKindV2::GameResult,
        GameEventPhaseV2::PostFrame, tick.frame.replayFrame,
        current_.finalLatch.transitionSiteId,
        current_.finalLatch.roundOrdinal,
        current_.finalLatch.winningSideMask,
        current_.finalLatch.roundTallies,
        current_.finalLatch.winnerSlot,
        source.core.postStateHash);

    ConfirmedTimelineValidatorV2 candidateValidator = validator_;
    CanonicalGameDigestV2 candidateCanonical = canonical_;
    if (!stampAndValidateTick(
            tick, candidateValidator, candidateCanonical)) {
        return failCurrent(
            ReplayRecordingFaultV2::TimelineValidationFailure,
            TerminalReason::RecordingFailure);
    }

    ConfirmedGameEndV2 end{};
    end.reason = TerminalReason::Completed;
    end.nextRecord = candidateValidator.nextRecord();
    end.winningSideMask =
        current_.finalLatch.winningSideMask;
    end.roundTallies = current_.finalLatch.roundTallies;
    end.winnerSlot = current_.finalLatch.winnerSlot;
    end.postTailStateHash = source.core.postStateHash;
    end.finalRollingDigest =
        candidateValidator.rollingDigest();
    if (!finalizeEnd(
            end, candidateValidator, candidateCanonical)) {
        return failCurrent(
            ReplayRecordingFaultV2::TimelineValidationFailure,
            TerminalReason::RecordingFailure);
    }

    PublisherRecordV2 record{};
    record.kind = PublisherRecordKindV2::EndGame;
    record.end.containsFinalFrame = true;
    record.end.finalFrame = tick;
    record.end.end = end;
    if (!publisher_.publishEndGameReserved(record)) {
        clearCurrent();
        state_ = ReplayGameRecordStateV2::Failed;
        lastFault_ = ReplayRecordingFaultV2::ReservedEndFailure;
        return result(ReplayPublishCodeV2::Failed, lastFault_);
    }

    validator_ = candidateValidator;
    canonical_ = candidateCanonical;
    current_.lastAccepted = source;
    current_.hasAcceptedFrame = true;
    if (!retireCurrent(end)) {
        clearCurrent();
        state_ = ReplayGameRecordStateV2::Failed;
        lastFault_ = ReplayRecordingFaultV2::CommitBacklog;
        return result(ReplayPublishCodeV2::Failed, lastFault_);
    }
    clearCurrent();
    state_ = ReplayGameRecordStateV2::WaitingForFreshGame;
    lastFault_ = ReplayRecordingFaultV2::None;
    return result(ReplayPublishCodeV2::TerminalPublished);
}

bool ReplayRecordingCoordinatorV2::publishPalette(
    const ReplayPaletteV2& palette) {
    if (state_ != ReplayGameRecordStateV2::Recording) return false;
    if (palette.side > 3u || palette.present == 0u) return false;
    PublisherRecordV2 record{};
    record.kind = PublisherRecordKindV2::Palette;
    record.palette = palette;
    return publisher_.tryPublishOrdinary(record);
}

ReplayPublishResultV2
ReplayRecordingCoordinatorV2::requestAbnormalEnd(
    const TerminalReason reason,
    const ReplayCloseProvenanceV2 provenance) {
    if (state_ == ReplayGameRecordStateV2::Disabled
        || state_
            == ReplayGameRecordStateV2::WaitingForFreshGame) {
        return result(ReplayPublishCodeV2::Ignored);
    }
    if (state_ == ReplayGameRecordStateV2::Failed)
        return result(ReplayPublishCodeV2::Failed, lastFault_);

    if (!isRuntimeAbnormalReason(reason)
        || !isKnownCloseProvenance(provenance)) {
        if (!current_.hasAcceptedFrame)
            return failWithoutFile(
                ReplayRecordingFaultV2::InvalidTerminalReason);
        return failCurrent(
            ReplayRecordingFaultV2::InvalidTerminalReason,
            TerminalReason::RecordingFailure);
    }
    if (!current_.hasAcceptedFrame) {
        clearCurrent();
        state_ = ReplayGameRecordStateV2::WaitingForFreshGame;
        lastFault_ = ReplayRecordingFaultV2::None;
        return result(ReplayPublishCodeV2::NoFile);
    }
    return publishAbnormalEnd(
        reason, provenance, false,
        ReplayRecordingFaultV2::None);
}

ReplayPublishResultV2
ReplayRecordingCoordinatorV2::failWithoutFile(
    const ReplayRecordingFaultV2 fault) {
    clearCurrent();
    state_ = ReplayGameRecordStateV2::Failed;
    lastFault_ = fault;
    return result(ReplayPublishCodeV2::Failed, fault);
}

ReplayPublishResultV2 ReplayRecordingCoordinatorV2::failCurrent(
    const ReplayRecordingFaultV2 fault,
    const TerminalReason reason) {
    if (!current_.hasAcceptedFrame)
        return failWithoutFile(fault);
    return publishAbnormalEnd(
        reason, ReplayCloseProvenanceV2::RecorderChannel,
        true, fault);
}

ReplayPublishResultV2
ReplayRecordingCoordinatorV2::publishAbnormalEnd(
    const TerminalReason reason,
    const ReplayCloseProvenanceV2 provenance,
    const bool enterFailedState,
    const ReplayRecordingFaultV2 fault) {
    (void)provenance;
    if (!current_.hasAcceptedFrame || !validator_.started())
        return failWithoutFile(
            fault == ReplayRecordingFaultV2::None
                ? ReplayRecordingFaultV2::TimelineValidationFailure
                : fault);

    ConfirmedTimelineValidatorV2 candidateValidator = validator_;
    CanonicalGameDigestV2 candidateCanonical = canonical_;
    ConfirmedGameEndV2 end{};
    end.reason = reason;
    end.nextRecord = candidateValidator.nextRecord();
    end.winningSideMask = 0u;
    end.roundTallies =
        current_.lastAccepted.core.after.roundTallies;
    end.winnerSlot = -1;
    end.postTailStateHash =
        current_.lastAccepted.core.postStateHash;
    end.finalRollingDigest =
        candidateValidator.rollingDigest();
    if (!finalizeEnd(
            end, candidateValidator, candidateCanonical)) {
        clearCurrent();
        state_ = ReplayGameRecordStateV2::Failed;
        lastFault_ =
            ReplayRecordingFaultV2::TimelineValidationFailure;
        return result(ReplayPublishCodeV2::Failed, lastFault_);
    }

    PublisherRecordV2 record{};
    record.kind = PublisherRecordKindV2::EndGame;
    record.end.containsFinalFrame = false;
    record.end.end = end;
    if (!publisher_.publishEndGameReserved(record)) {
        clearCurrent();
        state_ = ReplayGameRecordStateV2::Failed;
        lastFault_ = ReplayRecordingFaultV2::ReservedEndFailure;
        return result(ReplayPublishCodeV2::Failed, lastFault_);
    }

    validator_ = candidateValidator;
    canonical_ = candidateCanonical;
    if (!retireCurrent(end)) {
        clearCurrent();
        state_ = ReplayGameRecordStateV2::Failed;
        lastFault_ = ReplayRecordingFaultV2::CommitBacklog;
        return result(ReplayPublishCodeV2::Failed, lastFault_);
    }
    clearCurrent();
    state_ = enterFailedState
        ? ReplayGameRecordStateV2::Failed
        : ReplayGameRecordStateV2::WaitingForFreshGame;
    lastFault_ = fault;
    if (enterFailedState)
        return result(ReplayPublishCodeV2::Failed, fault);
    return result(ReplayPublishCodeV2::TerminalPublished);
}

bool ReplayRecordingCoordinatorV2::topologyQualified(
    const FreshGameDescriptorV2& descriptor) const {
    const GameManifestV2& manifest = descriptor.start.manifest;
    if (manifest.vsRow != 1u
        || manifest.activeLaneMask != 0x03u
        || manifest.activeFighterMask != 0x03u
        || manifest.cpuMask != 0u
        || manifest.cpuContract != CpuContract::None
        || manifest.slotToSourceRoute[0] != 0u
        || manifest.slotToSourceRoute[1] != 1u) {
        return false;
    }
    for (size_t lane = 0u; lane < kSourceLanes; ++lane) {
        if (manifest.lanes[lane].active != (lane < 2u))
            return false;
    }
    for (size_t slot = 0u; slot < kFighterSlots; ++slot) {
        const SlotOwnership expected = slot < 2u
            ? SlotOwnership::RecordedHuman
            : SlotOwnership::Inactive;
        if (manifest.slots[slot].ownership != expected)
            return false;
    }
    return true;
}

bool ReplayRecordingCoordinatorV2::validateProvenFrame(
    const ProvenGameFrameV2& source) const {
    const GameManifestV2& manifest = current_.start.manifest;
    if (source.gameEpoch != current_.descriptor.gameEpoch
        || source.game != manifest.identity
        || source.sourceFrame <= current_.descriptor.launchSourceFrame
        || source.sourceFrame
            == std::numeric_limits<uint32_t>::max()
        || source.core.inputDomain
            != InputDomain::LogicalSourceV1
        || source.core.activeLaneMask
            != manifest.activeLaneMask
        || !isValidConfirmedGameFrameCoreV2(
            source.core, manifest.activeLaneMask)
        || source.core.preStateHash == 0u
        || source.core.postStateHash == 0u
        || source.activeFighterMask
            != manifest.activeFighterMask
        || !isValidStageProof(source.stage)) {
        return false;
    }

    for (size_t slot = 0u; slot < kFighterSlots; ++slot) {
        if (source.roster[slot]
            != manifest.slots[slot].identity) {
            return false;
        }
    }

    if (validator_.started() && source.stage != manifest.stage)
        return false;

    const auto lifecycleMatchesManifest =
        [&manifest](const GameLifecycleV2& lifecycle) {
            return lifecycle.routeEpoch == manifest.routeEpoch
                && lifecycle.rosterEpoch == manifest.rosterEpoch
                && lifecycle.layoutGeneration
                    == manifest.layoutGeneration
                && (lifecycle.winnerSlot < 0
                    || lifecycle.winnerSlot < 2);
        };
    return lifecycleMatchesManifest(source.core.before)
        && lifecycleMatchesManifest(source.core.after);
}

bool ReplayRecordingCoordinatorV2::validateRoundProof(
    const ProvenGameFrameV2& source) const {
    const RoundTransitionProofV2& proof = source.roundTransition;
    const GameLifecycleV2& before = source.core.before;
    const GameLifecycleV2& after = source.core.after;
    if ((proof.flags & ~kKnownRoundTransitionFlagsV2) != 0u
        || (proof.flags != RoundTransitionNoneV2
            && proof.flags != RoundBecameActiveV2
            && proof.flags != RoundOutcomeCommittedV2
            && proof.flags != FinalGameResultCommittedV2)
        || proof.talliesBefore != before.roundTallies
        || proof.talliesAfter != after.roundTallies
        || proof.roundStateBefore != before.roundState
        || proof.roundStateAfter != after.roundState
        || proof.roundCounterBefore != before.roundCounter
        || proof.roundCounterAfter != after.roundCounter
        || proof.resultCounterBefore != before.resultCounter
        || proof.resultCounterAfter != after.resultCounter) {
        return false;
    }

    switch (proof.flags) {
        case RoundTransitionNoneV2:
            return proof.transitionSiteId == 0u
                && proof.roundOrdinal == 0u
                && proof.winningSideMask == 0u
                && proof.winnerSlot == -1
                && before.roundTallies == after.roundTallies
                && !(before.scene == 2u
                    && before.matchSub == -1
                    && after.scene == 2u
                    && after.matchSub == 0)
                && !isFinalOneToTwoTransition(source);

        case RoundBecameActiveV2:
            {
            const bool initialRound =
                before.scene == 2u && before.matchSub == -1
                && after.scene == 2u && after.matchSub == 0
                && after.roundCounter == 0u
                && after.roundTallies
                    == std::array<uint8_t, 2>{};
            const bool laterRound =
                before.scene == 2u && before.matchSub == 0
                && after.scene == 2u && after.matchSub == 0
                && before.roundCounter
                    != std::numeric_limits<uint32_t>::max()
                && after.roundCounter == before.roundCounter + 1u;
            return proof.transitionSiteId != 0u
                && proof.roundOrdinal != 0u
                && proof.winningSideMask == 0u
                && proof.winnerSlot == -1
                && (initialRound
                    || (laterRound
                        && before.roundTallies == after.roundTallies))
                && after.winnerSlot == -1;
            }

        case RoundOutcomeCommittedV2:
            return proof.transitionSiteId != 0u
                && proof.roundOrdinal != 0u
                && isOneHotWinner(
                    proof.winningSideMask, proof.winnerSlot)
                && incrementedExactlyOneTally(
                    proof.talliesBefore, proof.talliesAfter,
                    proof.winnerSlot)
                && before.scene == 2u
                && before.matchSub == 0
                && after.scene == 2u
                && after.matchSub == 0
                && before.winnerSlot == -1
                && after.winnerSlot == -1;

        case FinalGameResultCommittedV2: {
            if (proof.transitionSiteId == 0u
                || proof.roundOrdinal == 0u
                || !isOneHotWinner(
                    proof.winningSideMask, proof.winnerSlot)
                || before.roundTallies != after.roundTallies
                || !isFinalOneToTwoTransition(source)
                || after.winnerSlot != proof.winnerSlot) {
                return false;
            }
            const int32_t nativeWinner =
                after.roundTallies[0] > after.roundTallies[1]
                ? 0
                : 1;
            return proof.winnerSlot == nativeWinner;
        }
    }
    return false;
}

bool ReplayRecordingCoordinatorV2::isInitialSetup(
    const ProvenGameFrameV2& source) const {
    return source.roundTransition.flags
            == RoundBecameActiveV2
        && source.core.before.scene == 2u
        && source.core.before.matchSub == -1
        && source.core.after.scene == 2u
        && source.core.after.matchSub == 0;
}

bool ReplayRecordingCoordinatorV2::isFirstActiveFrame(
    const ProvenGameFrameV2& source) const {
    return source.roundTransition.flags
            == RoundTransitionNoneV2
        && source.core.before.scene == 2u
        && source.core.before.matchSub == 0;
}

bool ReplayRecordingCoordinatorV2::isFinalOneToTwoTransition(
    const ProvenGameFrameV2& source) const {
    return source.core.before.scene == 2u
        && source.core.before.matchSub == 1
        && source.core.after.scene == 2u
        && source.core.after.matchSub == 2
        && source.core.after.roundCounter == 5u;
}

bool ReplayRecordingCoordinatorV2::matchesFirstWinScreenTick(
    const ProvenGameFrameV2& source) const {
    const FinalLatch& latch = current_.finalLatch;
    return latch.active
        && source.sourceFrame == latch.expectedSourceFrame
        && source.gameEpoch == latch.gameEpoch
        && source.roundTransition.flags
            == RoundTransitionNoneV2
        && source.core.before.scene == 2u
        && source.core.before.matchSub == 2
        && source.core.before.resultCounter == 0u
        && source.core.before.roundCounter == 5u
        && source.core.before.winnerSlot == latch.winnerSlot
        && source.core.before.roundTallies == latch.roundTallies;
}

ConfirmedGameEventV2 ReplayRecordingCoordinatorV2::makeEvent(
    const GameEventKindV2 kind, const GameEventPhaseV2 phase,
    const uint32_t replayFrame,
    const uint32_t transitionSiteId,
    const uint8_t roundOrdinal,
    const uint8_t winningSideMask,
    const std::array<uint8_t, 2>& roundTallies,
    const int32_t winnerSlot, const uint64_t stateProof) const {
    ConfirmedGameEventV2 event{};
    event.replayFrame = replayFrame;
    event.kind = kind;
    event.phase = phase;
    event.transitionSiteId = transitionSiteId;
    event.roundOrdinal = roundOrdinal;
    event.winningSideMask = winningSideMask;
    event.roundTallies = roundTallies;
    event.winnerSlot = winnerSlot;
    event.stateProof = stateProof;
    return event;
}

bool ReplayRecordingCoordinatorV2::stampAndValidateTick(
    CanonicalTickBundleV2& tick,
    ConfirmedTimelineValidatorV2& validator,
    CanonicalGameDigestV2& canonical) {
    lastTimelineFault_ = ConfirmedTimelineFaultV2::None;
    if (!validator.started() || !canonical.started()
        || tick.frame.replayFrame != validator.nextRecord()
        || tick.preEvents.count > kMaxEventsPerTickV2
        || tick.postEvents.count > kMaxEventsPerTickV2) {
        return false;
    }

    uint64_t rolling = validator.rollingDigest();
    uint32_t sequence = validator.nextEventSequence();
    for (size_t i = 0u; i < tick.preEvents.count; ++i) {
        ConfirmedGameEventV2& event =
            tick.preEvents.events[i];
        event.eventSequence = sequence++;
        rolling = advanceReplayDigestV2(rolling, event);
        event.rollingDigest = rolling;
        if (!canonical.append(event)) return false;
    }
    tick.frame.rollingDigest =
        advanceReplayDigestV2(rolling, tick.frame);
    rolling = tick.frame.rollingDigest;
    if (!canonical.append(tick.frame)) return false;
    for (size_t i = 0u; i < tick.postEvents.count; ++i) {
        ConfirmedGameEventV2& event =
            tick.postEvents.events[i];
        event.eventSequence = sequence++;
        rolling = advanceReplayDigestV2(rolling, event);
        event.rollingDigest = rolling;
        if (!canonical.append(event)) return false;
    }

    ParseStatus why = ParseStatus::Ok;
    if (!validator.append(tick, why)) {
        lastTimelineFault_ = validator.lastFault();
        return false;
    }
    return validator.rollingDigest() == rolling
        && validator.nextEventSequence() == sequence;
}

bool ReplayRecordingCoordinatorV2::finalizeEnd(
    ConfirmedGameEndV2& end,
    ConfirmedTimelineValidatorV2& validator,
    CanonicalGameDigestV2& canonical) const {
    std::array<uint8_t, 32> digest{};
    if (!canonical.finish(end, digest)) return false;
    end.canonicalGameDigest = digest;
    ParseStatus why = ParseStatus::Ok;
    return validator.seal(end, why);
}

bool ReplayRecordingCoordinatorV2::retireCurrent(
    const ConfirmedGameEndV2& end) {
    size_t chosen = pendingCommits_.size();
    uint64_t oldestResolved = std::numeric_limits<uint64_t>::max();
    for (size_t i = 0u; i < pendingCommits_.size(); ++i) {
        const PendingCommitEntry& entry = pendingCommits_[i];
        if (!entry.occupied) {
            chosen = i;
            break;
        }
        if (entry.resolved && entry.serial < oldestResolved) {
            chosen = i;
            oldestResolved = entry.serial;
        }
    }
    if (chosen == pendingCommits_.size()) return false;

    PendingCommitEntry entry{};
    entry.occupied = true;
    entry.identity = current_.start.manifest.identity;
    entry.nextRecord = end.nextRecord;
    entry.canonicalGameDigest = end.canonicalGameDigest;
    entry.serial = ++commitSerial_;
    pendingCommits_[chosen] = entry;
    return true;
}

bool ReplayRecordingCoordinatorV2::commitSlotAvailable() const {
    for (const PendingCommitEntry& entry : pendingCommits_) {
        if (!entry.occupied || entry.resolved) return true;
    }
    return false;
}

void ReplayRecordingCoordinatorV2::clearCurrent() {
    current_ = ReplayGameCapture{};
    validator_ = ConfirmedTimelineValidatorV2{};
    canonical_ = CanonicalGameDigestV2{};
}

size_t ReplayRecordingCoordinatorV2::pendingCommitCount() const {
    size_t count = 0u;
    for (const PendingCommitEntry& entry : pendingCommits_)
        if (entry.occupied && !entry.resolved) ++count;
    return count;
}

CommitAckResultV2
ReplayRecordingCoordinatorV2::onCasterCommitAck(
    const GameCommitAckV2& ack) {
    for (PendingCommitEntry& entry : pendingCommits_) {
        if (!entry.occupied || entry.identity != ack.identity)
            continue;
        const uint8_t status = static_cast<uint8_t>(ack.status);
        if (status < static_cast<uint8_t>(
                         GameCommitStatusV2::Committed)
            || status > static_cast<uint8_t>(
                         GameCommitStatusV2::Unknown)
            || entry.nextRecord != ack.nextRecord
            || entry.canonicalGameDigest
                != ack.canonicalGameDigest) {
            lastFault_ = ReplayRecordingFaultV2::CommitAckConflict;
            return CommitAckResultV2::Conflict;
        }
        if (entry.resolved) {
            if (exactAck(entry.ack, ack))
                return CommitAckResultV2::ExactDuplicate;
            lastFault_ = ReplayRecordingFaultV2::CommitAckConflict;
            return CommitAckResultV2::Conflict;
        }
        entry.ack = ack;
        entry.resolved = true;
        if (lastFault_
                == ReplayRecordingFaultV2::CommitAckStale
            || lastFault_
                == ReplayRecordingFaultV2::CommitAckConflict) {
            lastFault_ = ReplayRecordingFaultV2::None;
        }
        return CommitAckResultV2::Applied;
    }
    lastFault_ = ReplayRecordingFaultV2::CommitAckStale;
    return CommitAckResultV2::Stale;
}

}
