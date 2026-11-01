#include "SpectatorReplaySaveAdapter.h"

#include "../../replay/ReplayCodec.h"
#include "../../replay/ReplayIntegrity.h"

#include <limits>

namespace qclient::spectator {
namespace qsp = qnetplay::spectator;

namespace {

bool appendCanonicalTick(
    qreplay::CanonicalGameDigestV2& canonical,
    const qreplay::CanonicalTickBundleV2& tick) {
    for (size_t i = 0; i < tick.preEvents.count; ++i)
        if (!canonical.append(tick.preEvents.events[i])) return false;
    if (!canonical.append(tick.frame)) return false;
    for (size_t i = 0; i < tick.postEvents.count; ++i)
        if (!canonical.append(tick.postEvents.events[i])) return false;
    return true;
}

bool digestIsZero(const std::array<uint8_t, 32>& digest) {
    for (uint8_t byte : digest)
        if (byte != 0u) return false;
    return true;
}

qreplay::ConfirmedGameStartV2 spectatorStart(
    const qreplay::ConfirmedGameStartV2& source) {
    qreplay::ConfirmedGameStartV2 copy = source;
    copy.manifest.display.sourceKind =
        qreplay::ReplaySourceKindV2::SpectatorSave;
    copy.manifest.display.routeLabel = qreplay::ReplayTextV2{};
    return copy;
}

}

SpectatorReplaySaveAdapterV4::SpectatorReplaySaveAdapterV4(
    SpectatorReplaySaveSinkV4* sink,
    const SpectatorReplaySaveConfigV4& config)
    : sink_(sink), config_(config) {}

bool SpectatorReplaySaveAdapterV4::attach(
    const qsp::SpectatorConnectedArchiveV4* archive) {
    if (candidate_.active && sink_ != nullptr)
        sink_->abort(SpectatorReplaySaveFailureV4::StreamFailed);
    candidate_ = Candidate{};
    skipFailedGame_ = false;
    archive_ = nullptr;
    readItemSequence_ = 0u;
    state_ = SpectatorReplaySaveState::Disabled;
    enabled_ = false;

    if (sink_ == nullptr || archive == nullptr || !archive->bound()
        || config_.maxPendingItems == 0u
        || config_.maxPendingItems
            > qsp::kMaxConnectedUnackedItemsV4
        || config_.maxPendingBytes == 0u
        || config_.maxPendingBytes
            > qsp::kMaxConnectedUnackedBytesV4
        || config_.maxOutcomes == 0u
        || config_.maxOutcomes > qreplay::kMaxMatchesPerSet) {
        return false;
    }
    archive_ = archive;
    return true;
}

bool SpectatorReplaySaveAdapterV4::alreadyFinished(
    const qreplay::GameIdentityV2& game) const {
    for (const SpectatorReplaySaveOutcomeV4& outcome : outcomes_)
        if (outcome.game == game) return true;
    return false;
}

bool SpectatorReplaySaveAdapterV4::selectRetainedRange() {
    if (archive_ == nullptr || !enabled_) return false;
    for (const qsp::ConnectedGameRangeV4& range : archive_->ranges()) {
        if (alreadyFinished(range.game)) continue;
        qsp::ConnectedPublicationV4 publication{};
        if (!archive_->publicationAt(
                range.startItemSequence, publication)
            || publication.kind
                != qsp::ConnectedPublicationKindV4::BeginGameBundle
            || publication.begin.start.game != range.game) {
            continue;
        }
        readItemSequence_ = range.startItemSequence;
        state_ = readItemSequence_ < archive_->nextItemSequence()
            ? SpectatorReplaySaveState::BackfillingRetainedRange
            : SpectatorReplaySaveState::CapturingLiveTail;
        return true;
    }
    readItemSequence_ = archive_->nextItemSequence();
    state_ = SpectatorReplaySaveState::WaitingForPerGameStart;
    return false;
}

void SpectatorReplaySaveAdapterV4::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled) {
        if (candidate_.active && sink_ != nullptr) {
            sink_->abort(SpectatorReplaySaveFailureV4::UserDisabled);
            SpectatorReplaySaveOutcomeV4 outcome{};
            outcome.game = candidate_.game;
            outcome.kind =
                SpectatorReplaySaveOutcomeKindV4::IncompleteNoSave;
            outcome.failure =
                SpectatorReplaySaveFailureV4::UserDisabled;
            outcome.recordCount =
                candidate_.nextRecordCursor
                - candidate_.startRecordCursor;
            publishOutcome(outcome);
        }
        candidate_ = Candidate{};
        skipFailedGame_ = false;
        if (archive_ != nullptr)
            readItemSequence_ = archive_->nextItemSequence();
        state_ = SpectatorReplaySaveState::Disabled;
        return;
    }
    if (archive_ == nullptr || archive_->failed()) {
        state_ = SpectatorReplaySaveState::FailedCurrentGame;
        return;
    }
    selectRetainedRange();
}

void SpectatorReplaySaveAdapterV4::publishOutcome(
    const SpectatorReplaySaveOutcomeV4& outcome) {
    if (outcomes_.size() < config_.maxOutcomes)
        outcomes_.push_back(outcome);
}

void SpectatorReplaySaveAdapterV4::failCurrent(
    SpectatorReplaySaveFailureV4 failure,
    SpectatorReplaySaveOutcomeKindV4 kind) {
    if (candidate_.active) {
        sink_->abort(failure);
        SpectatorReplaySaveOutcomeV4 outcome{};
        outcome.game = candidate_.game;
        outcome.kind = kind;
        outcome.failure = failure;
        outcome.recordCount =
            candidate_.nextRecordCursor - candidate_.startRecordCursor;
        publishOutcome(outcome);
        skipFailedGame_ = true;
    }
    candidate_ = Candidate{};
    state_ = SpectatorReplaySaveState::FailedCurrentGame;
}

bool SpectatorReplaySaveAdapterV4::appendRecord(
    const qsp::ConnectedConfirmedRecordV4& record) {
    if (!candidate_.active || !record.hasReplayGameIdentity
        || record.game != candidate_.game
        || record.chain.recordCursorBefore
               != candidate_.nextRecordCursor) {
        return false;
    }
    qreplay::CanonicalTickBundleV2 tick{};
    uint32_t eventSequence = candidate_.nextEventSequence;
    uint64_t rollingDigest = candidate_.rollingDigest;
    if (!qsp::rebaseConnectedRecordV4(
            record, candidate_.startRecordCursor, eventSequence,
            rollingDigest, tick)
        || !appendCanonicalTick(candidate_.canonical, tick)) {
        return false;
    }
    if (!sink_->append(tick)) {
        failCurrent(
            SpectatorReplaySaveFailureV4::SinkAppendFailed);
        return false;
    }
    candidate_.nextEventSequence = eventSequence;
    candidate_.rollingDigest = rollingDigest;
    candidate_.nextRecordCursor =
        record.chain.recordCursorBefore + 1u;
    return true;
}

bool SpectatorReplaySaveAdapterV4::beginCandidate(
    const qsp::ConnectedBeginGameBundleV4& begin) {
    if (candidate_.active
        || begin.start.game != begin.start.start.manifest.identity
        || !begin.frameZero.hasReplayGameIdentity
        || begin.frameZero.game != begin.start.game
        || begin.frameZero.chain.recordCursorBefore
               != begin.start.chain.recordCursorBefore) {
        return false;
    }
    qreplay::ConfirmedGameStartV2 start =
        spectatorStart(begin.start.start);
    if (start.start.initialRollingDigest
            != qreplay::initialReplayDigestV2(start)
        || !sink_->begin(start)) {
        SpectatorReplaySaveOutcomeV4 outcome{};
        outcome.game = begin.start.game;
        outcome.kind = SpectatorReplaySaveOutcomeKindV4::Failed;
        outcome.failure =
            SpectatorReplaySaveFailureV4::SinkBeginFailed;
        publishOutcome(outcome);
        skipFailedGame_ = true;
        state_ = SpectatorReplaySaveState::FailedCurrentGame;
        return false;
    }
    candidate_ = Candidate{};
    candidate_.active = true;
    candidate_.game = begin.start.game;
    candidate_.startRecordCursor =
        begin.start.chain.recordCursorBefore;
    candidate_.nextRecordCursor =
        candidate_.startRecordCursor;
    candidate_.rollingDigest = start.start.initialRollingDigest;
    if (!candidate_.canonical.begin(start)
        || !appendRecord(begin.frameZero)) {
        if (candidate_.active)
            failCurrent(
                SpectatorReplaySaveFailureV4::DigestMismatch);
        return false;
    }
    state_ = readItemSequence_ < archive_->nextItemSequence()
        ? SpectatorReplaySaveState::BackfillingRetainedRange
        : SpectatorReplaySaveState::CapturingLiveTail;
    return true;
}

bool SpectatorReplaySaveAdapterV4::finishCandidate(
    const qsp::ConnectedEndGameBundleV4& bundle) {
    if (!candidate_.active || bundle.end.game != candidate_.game)
        return false;
    if (bundle.containsFinalRecord
        && !appendRecord(bundle.finalRecord)) {
        return false;
    }
    const qreplay::ConfirmedGameEndV2& end = bundle.end.end;
    const uint32_t recordCount =
        candidate_.nextRecordCursor - candidate_.startRecordCursor;
    if (bundle.end.setNextRecordCursor
            != candidate_.nextRecordCursor
        || end.nextRecord != recordCount
        || end.finalRollingDigest != candidate_.rollingDigest
        || digestIsZero(end.canonicalGameDigest)) {
        return false;
    }
    state_ = SpectatorReplaySaveState::Finalizing;
    std::array<uint8_t, 32> computed{};
    if (!candidate_.canonical.finish(end, computed)
        || computed != end.canonicalGameDigest) {
        return false;
    }
    if (!sink_->finalize(end)) {
        failCurrent(
            SpectatorReplaySaveFailureV4::SinkFinalizeFailed);
        return false;
    }
    SpectatorReplaySaveOutcomeV4 outcome{};
    outcome.game = candidate_.game;
    outcome.kind = SpectatorReplaySaveOutcomeKindV4::Saved;
    outcome.failure = SpectatorReplaySaveFailureV4::None;
    outcome.recordCount = recordCount;
    outcome.canonicalGameDigest = computed;
    publishOutcome(outcome);
    candidate_ = Candidate{};
    skipFailedGame_ = false;
    state_ = SpectatorReplaySaveState::WaitingForPerGameStart;
    return true;
}

size_t SpectatorReplaySaveAdapterV4::pump(size_t maxPublications) {
    if (!enabled_ || archive_ == nullptr || maxPublications == 0u)
        return 0u;
    if (archive_->failed()) {
        notifyStreamFailure();
        return 0u;
    }

    const uint32_t frontier = archive_->nextItemSequence();
    if (readItemSequence_ > frontier) {
        failCurrent(SpectatorReplaySaveFailureV4::RangeConflict);
        return 0u;
    }
    uint64_t pendingBytes = 0u;
    if (!archive_->byteDistance(
            readItemSequence_, frontier, pendingBytes)
        || frontier - readItemSequence_ > config_.maxPendingItems
        || pendingBytes > config_.maxPendingBytes) {
        if (candidate_.active)
            failCurrent(
                SpectatorReplaySaveFailureV4::WorkerLiability);
        else
            state_ = SpectatorReplaySaveState::FailedCurrentGame;
        readItemSequence_ = frontier;
        return 0u;
    }

    size_t processed = 0u;
    while (processed < maxPublications
           && readItemSequence_ < archive_->nextItemSequence()) {
        qsp::ConnectedPublicationV4 publication{};
        if (!archive_->publicationAt(
                readItemSequence_, publication)) {
            failCurrent(SpectatorReplaySaveFailureV4::MissingPrefix);
            readItemSequence_ = archive_->nextItemSequence();
            break;
        }
        const uint32_t next =
            qsp::nextItemSequenceV4(publication);

        if (skipFailedGame_) {
            if (publication.kind
                == qsp::ConnectedPublicationKindV4::EndGameBundle) {
                skipFailedGame_ = false;
                state_ =
                    SpectatorReplaySaveState::WaitingForPerGameStart;
            } else if (publication.kind
                       == qsp::ConnectedPublicationKindV4::SetSeal) {
                skipFailedGame_ = false;
                state_ =
                    SpectatorReplaySaveState::WaitingForPerGameStart;
            }
            readItemSequence_ = next;
            ++processed;
            continue;
        }

        bool ok = true;
        switch (publication.kind) {
            case qsp::ConnectedPublicationKindV4::BeginGameBundle:
                if (alreadyFinished(publication.begin.start.game)) {
                    skipFailedGame_ = true;
                } else {
                    ok = beginCandidate(publication.begin);
                }
                break;
            case qsp::ConnectedPublicationKindV4::ConfirmedRecord:
                if (candidate_.active)
                    ok = appendRecord(publication.record);
                break;
            case qsp::ConnectedPublicationKindV4::SetEvent:
                break;
            case qsp::ConnectedPublicationKindV4::Palette:
                if (candidate_.active) {
                    (void)sink_->palette(publication.palette.palette);
                }
                break;
            case qsp::ConnectedPublicationKindV4::EndGameBundle:
                if (candidate_.active)
                    ok = finishCandidate(publication.end);
                break;
            case qsp::ConnectedPublicationKindV4::SetSeal:
                if (candidate_.active) {
                    failCurrent(
                        SpectatorReplaySaveFailureV4::
                            SetSealedBeforeGameEnd,
                        SpectatorReplaySaveOutcomeKindV4::
                            IncompleteNoSave);
                    ok = false;
                }
                break;
        }
        if (!ok && candidate_.active)
            failCurrent(SpectatorReplaySaveFailureV4::DigestMismatch);
        readItemSequence_ = next;
        ++processed;
    }
    if (candidate_.active
        && readItemSequence_ == archive_->nextItemSequence()) {
        state_ = SpectatorReplaySaveState::CapturingLiveTail;
    } else if (!candidate_.active && !skipFailedGame_
               && state_ != SpectatorReplaySaveState::Disabled
               && state_ != SpectatorReplaySaveState::FailedCurrentGame) {
        state_ = SpectatorReplaySaveState::WaitingForPerGameStart;
    }
    return processed;
}

void SpectatorReplaySaveAdapterV4::notifyStreamFailure() {
    if (candidate_.active)
        failCurrent(
            SpectatorReplaySaveFailureV4::StreamFailed,
            SpectatorReplaySaveOutcomeKindV4::IncompleteNoSave);
    else
        state_ = SpectatorReplaySaveState::FailedCurrentGame;
    if (archive_ != nullptr)
        readItemSequence_ = archive_->nextItemSequence();
}

}
