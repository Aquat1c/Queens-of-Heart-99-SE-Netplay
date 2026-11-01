#include "ReplayPlaybackSessionV2.h"
#include "replay/ReplaySimulationContractV2.h"

#include "StageCatalog.h"

#include <algorithm>
#include <limits>

namespace qreplay {
namespace {

bool allZero(const std::array<uint8_t, 32>& value) {
    return std::all_of(
        value.begin(), value.end(), [](uint8_t byte) {
            return byte == 0u;
        });
}

uint64_t fnvU32(uint64_t hash, uint32_t value) {
    constexpr uint64_t kPrime = 1099511628211ull;
    for (unsigned shift = 0u; shift != 32u; shift += 8u) {
        hash ^= static_cast<uint8_t>(value >> shift);
        hash *= kPrime;
    }
    return hash;
}

uint64_t lifecycleProof(const GameLifecycleV2& lifecycle) {
    uint64_t hash = 1469598103934665603ull;
    hash = fnvU32(hash, lifecycle.scene);
    hash = fnvU32(hash, lifecycle.requestedScene);
    hash = fnvU32(
        hash, static_cast<uint32_t>(lifecycle.matchSub));
    hash = fnvU32(hash, lifecycle.sceneThreeState);
    hash = fnvU32(hash, lifecycle.roundState);
    hash = fnvU32(hash, lifecycle.roundCounter);
    hash = fnvU32(hash, lifecycle.resultCounter);
    hash = fnvU32(
        hash, static_cast<uint32_t>(lifecycle.winnerSlot));
    hash = fnvU32(
        hash,
        static_cast<uint32_t>(lifecycle.roundTallies[0])
            | (static_cast<uint32_t>(lifecycle.roundTallies[1]) << 8));
    hash = fnvU32(hash, lifecycle.routeEpoch);
    hash = fnvU32(hash, lifecycle.rosterEpoch);
    hash = fnvU32(hash, lifecycle.layoutGeneration);
    return hash;
}

uint64_t contractProof(const SimulationContractV2& contract) {
    uint64_t hash = 1469598103934665603ull;
    for (uint8_t byte : contract.executableSha256)
        hash = fnvU32(hash, byte);
    hash = fnvU32(
        hash, static_cast<uint32_t>(contract.nativeBuildIdentity));
    hash = fnvU32(
        hash, static_cast<uint32_t>(contract.nativeBuildIdentity >> 32));
    hash = fnvU32(
        hash, static_cast<uint32_t>(contract.runtimeSimulationId));
    hash = fnvU32(
        hash, static_cast<uint32_t>(contract.runtimeSimulationId >> 32));
    hash = fnvU32(
        hash,
        static_cast<uint32_t>(
            contract.stateRegionSchemaFingerprint));
    hash = fnvU32(
        hash,
        static_cast<uint32_t>(
            contract.stateRegionSchemaFingerprint >> 32));
    hash = fnvU32(
        hash, static_cast<uint32_t>(contract.bootstrapSchemaId));
    hash = fnvU32(
        hash, static_cast<uint32_t>(contract.eventSchemaId));
    hash = fnvU32(
        hash, static_cast<uint32_t>(contract.characterCatalogId));
    hash = fnvU32(hash, contract.compactActionDomainVersion);
    return hash;
}

uint64_t saturatedAdd(uint64_t a, uint64_t b) {
    if (b > std::numeric_limits<uint64_t>::max() - a)
        return std::numeric_limits<uint64_t>::max();
    return a + b;
}

}

ReplayPlaybackSessionV2::ReplayPlaybackSessionV2(
    ConfirmedTimelineBufferV2&& timeline,
    const ReplayPlaybackBindingV2& binding)
    : timeline_(static_cast<ConfirmedTimelineBufferV2&&>(timeline)),
      binding_(binding) {
    mode_ = replayVerificationModeForV2(
        timeline_.start().manifest.identity);
    if (binding_.supersededStateSchema)
        mode_ = ReplayVerificationModeV2::BehaviourVerified;
    if (!validateBinding()) return;
    phase_ = ReplayPlaybackPhaseV2::Bound;
}

bool ReplayPlaybackSessionV2::validateBinding() {
    if (!timeline_.validCapacity() || !timeline_.started()
        || timeline_.capacity() != timeline_.size()
        || timeline_.size() == 0u
        || timeline_.size() > kMaxPlaybackPrebufferFramesV2
        || timeline_.validator().failed()
        || timeline_.validator().nextRecord() != timeline_.size()) {
        finishTyped(
            ipc::v3::PlaybackResultReason::TerminalMismatch,
            ReplayPlaybackFaultV2::InvalidPrebuffer,
            ReplayPlaybackProofRegionV2::Prebuffer,
            kNoReplayMismatchFrameV2,
            static_cast<uint64_t>(timeline_.capacity()),
            static_cast<uint64_t>(timeline_.size()));
        return false;
    }

    if (!replayContractAgreesForPlaybackV2(
            timeline_.start().manifest.contract, binding_.runtimeContract)) {
        finishTyped(
            ipc::v3::PlaybackResultReason::Unsupported,
            ReplayPlaybackFaultV2::RuntimeContractMismatch,
            ReplayPlaybackProofRegionV2::RuntimeContract,
            kNoReplayMismatchFrameV2,
            contractProof(timeline_.start().manifest.contract),
            contractProof(binding_.runtimeContract));
        return false;
    }
    const StageIdentityV2& stage = timeline_.start().manifest.stage;
    const StageCatalogV2& stages = StageCatalogV2::forSeBuild();
    if (!stages.validateIdentity(stage)) {
        const StageCatalogEntryV2* expected =
            stages.findByNativeStageId(stage.nativeStageId);
        finishTyped(
            ipc::v3::PlaybackResultReason::Unsupported,
            ReplayPlaybackFaultV2::StageIdentityMismatch,
            ReplayPlaybackProofRegionV2::StageIdentity,
            kNoReplayMismatchFrameV2,
            expected == nullptr ? 0u : expected->catalogEntryId,
            stage.catalogEntryId);
        return false;
    }

    const ipc::v3::PlaybackEndpointPayload& endpoint =
        binding_.endpoint;
    const bool sealedKind = endpoint.kind
        == ipc::v3::PlaybackEndpointKind::RecordedGameEnd;
    const bool prefixKind = endpoint.kind
        == ipc::v3::PlaybackEndpointKind::UnsealedValidatedPrefix;
    const CanonicalTickBundleV2* tail = timeline_.tick(
        static_cast<uint32_t>(timeline_.size() - 1u));
    bool exact = (sealedKind || prefixKind)
        && endpoint.nextRecord == timeline_.size()
        && endpoint.nextRecord != 0u
        && endpoint.validatedSourceByteCount != 0u
        && !allZero(endpoint.validatedSourceRangeDigest)
        && tail != nullptr
        && endpoint.postTailStateHash
            == tail->frame.core.postStateHash
        && endpoint.finalRollingDigest
            == timeline_.validator().rollingDigest();

    if (sealedKind) {
        exact = exact && endpoint.hasRecordedGameEnd
            && timeline_.sealed()
            && endpoint.recordedGameEnd == timeline_.end()
            && binding_.hasFileDigest
            && !allZero(binding_.fileDigest);
    } else {
        exact = exact && !endpoint.hasRecordedGameEnd
            && !timeline_.sealed()
            && !binding_.hasFileDigest
            && allZero(binding_.fileDigest);
    }

    if (!exact) {
        finishTyped(
            ipc::v3::PlaybackResultReason::TerminalMismatch,
            ReplayPlaybackFaultV2::EndpointMismatch,
            ReplayPlaybackProofRegionV2::Endpoint,
            kNoReplayMismatchFrameV2,
            static_cast<uint64_t>(timeline_.size()),
            endpoint.nextRecord);
        return false;
    }
    return true;
}

bool ReplayPlaybackSessionV2::restart() {
    if (!transportMayHold()) return false;
    phase_ = ReplayPlaybackPhaseV2::Bound;
    nextRecord_ = 0u;
    inFlightReplayFrame_ = kNoReplayMismatchFrameV2;
    latchedActiveLaneMask_ = 0u;
    latchedActions_.fill(0u);
    finalPresentationSerial_ = 0u;
    divergenceCount_ = 0u;
    firstDivergenceFrame_ = kNoReplayMismatchFrameV2;
    firstDivergenceRegion_ = ReplayPlaybackProofRegionV2::None;
    firstDivergenceExpected_ = firstDivergenceActual_ = 0u;
    return true;
}

bool ReplayPlaybackSessionV2::captureCheckpoint(Checkpoint& checkpoint) const {
    if (!transportMayHold() || nextRecord_ >= binding_.endpoint.nextRecord
        || timeline_.tick(nextRecord_) == nullptr)
        return false;
    checkpoint.owner_ = this;
    checkpoint.timelineIdentity_ = timeline_.tick(0u);
    checkpoint.nextRecord_ = nextRecord_;
    checkpoint.divergenceCount_ = divergenceCount_;
    checkpoint.firstDivergenceFrame_ = firstDivergenceFrame_;
    checkpoint.firstDivergenceRegion_ = firstDivergenceRegion_;
    checkpoint.firstDivergenceExpected_ = firstDivergenceExpected_;
    checkpoint.firstDivergenceActual_ = firstDivergenceActual_;
    return true;
}

bool ReplayPlaybackSessionV2::restoreCheckpoint(const Checkpoint& checkpoint) {
    if (!transportMayHold() || checkpoint.owner_ != this
        || checkpoint.timelineIdentity_ != timeline_.tick(0u)
        || checkpoint.nextRecord_ >= binding_.endpoint.nextRecord
        || timeline_.tick(checkpoint.nextRecord_) == nullptr)
        return false;
    nextRecord_ = checkpoint.nextRecord_;
    divergenceCount_ = checkpoint.divergenceCount_;
    firstDivergenceFrame_ = checkpoint.firstDivergenceFrame_;
    firstDivergenceRegion_ = checkpoint.firstDivergenceRegion_;
    firstDivergenceExpected_ = checkpoint.firstDivergenceExpected_;
    firstDivergenceActual_ = checkpoint.firstDivergenceActual_;
    inFlightReplayFrame_ = kNoReplayMismatchFrameV2;
    latchedActiveLaneMask_ = 0u;
    latchedActions_.fill(0u);
    finalPresentationSerial_ = 0u;
    return true;
}

bool ReplayPlaybackSessionV2::completeBootstrap(
    const ReplayBootstrapObservationV2& observed) {
    if (phase_ != ReplayPlaybackPhaseV2::Bound) return false;
    const DeterministicGameStartV2& expected = timeline_.start().start;
    const CanonicalTickBundleV2* zero = timeline_.tick(0u);
    if (zero == nullptr) {
        finishTyped(
            ipc::v3::PlaybackResultReason::BootstrapMismatch,
            ReplayPlaybackFaultV2::InvalidPrebuffer,
            ReplayPlaybackProofRegionV2::Prebuffer,
            0u, 1u, 0u);
        return false;
    }
    if (observed.initialPhase != expected.initialPhase) {
        finishTyped(
            ipc::v3::PlaybackResultReason::BootstrapMismatch,
            ReplayPlaybackFaultV2::BootstrapPhaseMismatch,
            ReplayPlaybackProofRegionV2::BootstrapPhase,
            0u, expected.initialPhase, observed.initialPhase);
        return false;
    }
    if (byteExactStateProof()
        && observed.bootstrapFingerprint
            != expected.bootstrapFingerprint) {
        noteDivergence(ReplayPlaybackProofRegionV2::BootstrapFingerprint,
                       0u, expected.bootstrapFingerprint,
                       observed.bootstrapFingerprint);
    }
    if (expected.frameZeroPreStateHash
        != zero->frame.core.preStateHash) {
        finishTyped(
            ipc::v3::PlaybackResultReason::BootstrapMismatch,
            ReplayPlaybackFaultV2::BootstrapPreHashMismatch,
            ReplayPlaybackProofRegionV2::BootstrapPreState,
            0u, expected.frameZeroPreStateHash,
            zero->frame.core.preStateHash);
        return false;
    }
    if (byteExactStateProof()
        && observed.frameZeroPreStateHash
            != expected.frameZeroPreStateHash) {
        noteDivergence(ReplayPlaybackProofRegionV2::BootstrapPreState, 0u,
                       expected.frameZeroPreStateHash,
                       observed.frameZeroPreStateHash);
    }
    if (observed.frameZeroLifecycle != zero->frame.core.before) {
        finishTyped(
            ipc::v3::PlaybackResultReason::BootstrapMismatch,
            ReplayPlaybackFaultV2::BootstrapLifecycleMismatch,
            ReplayPlaybackProofRegionV2::BootstrapLifecycle,
            0u, lifecycleProof(zero->frame.core.before),
            lifecycleProof(observed.frameZeroLifecycle));
        return false;
    }
    phase_ = ReplayPlaybackPhaseV2::Ready;
    return true;
}

const CanonicalTickBundleV2* ReplayPlaybackSessionV2::peek() const {
    if (phase_ != ReplayPlaybackPhaseV2::Ready) return nullptr;
    return timeline_.tick(nextRecord_);
}

bool ReplayPlaybackSessionV2::transportMayHold() const {
    return phase_ == ReplayPlaybackPhaseV2::Ready && !hasResult();
}

uint64_t ReplayPlaybackSessionV2::currentRollingDigest() const {
    if (nextRecord_ == 0u) return 0u;
    const CanonicalTickBundleV2* tick = timeline_.tick(nextRecord_ - 1u);
    if (tick == nullptr) return 0u;
    if (tick->postEvents.count != 0u) {
        return tick->postEvents.events[
            tick->postEvents.count - 1u].rollingDigest;
    }
    return tick->frame.rollingDigest;
}

bool ReplayPlaybackSessionV2::isFinalRecord(
    uint32_t replayFrame) const {
    return replayFrame + 1u == binding_.endpoint.nextRecord;
}

bool ReplayPlaybackSessionV2::beginFrame(
    const ReplayFrameBeginObservationV2& observed) {
    if (phase_ != ReplayPlaybackPhaseV2::Ready) return false;
    const CanonicalTickBundleV2* tick = timeline_.tick(nextRecord_);
    if (tick == nullptr) {
        finishTyped(
            ipc::v3::PlaybackResultReason::TerminalMismatch,
            ReplayPlaybackFaultV2::EndpointMismatch,
            ReplayPlaybackProofRegionV2::Endpoint,
            nextRecord_, binding_.endpoint.nextRecord, nextRecord_);
        return false;
    }
    const ConfirmedGameFrameV2& frame = tick->frame;
    if (observed.replayFrame != frame.replayFrame) {
        finishTyped(
            ipc::v3::PlaybackResultReason::FramePreHashMismatch,
            ReplayPlaybackFaultV2::FrameNumberMismatch,
            ReplayPlaybackProofRegionV2::FrameNumber,
            frame.replayFrame, frame.replayFrame, observed.replayFrame);
        return false;
    }
    if (frame.core.inputDomain != InputDomain::LogicalSourceV1) {
        finishTyped(
            ipc::v3::PlaybackResultReason::Unsupported,
            ReplayPlaybackFaultV2::UnsupportedInputDomain,
            ReplayPlaybackProofRegionV2::InputDomain,
            frame.replayFrame,
            static_cast<uint8_t>(InputDomain::LogicalSourceV1),
            static_cast<uint8_t>(frame.core.inputDomain));
        return false;
    }
    if (byteExactStateProof()
        && observed.preStateHash != frame.core.preStateHash) {
        noteDivergence(ReplayPlaybackProofRegionV2::FramePreState,
                       frame.replayFrame, frame.core.preStateHash,
                       observed.preStateHash);
    }
    if (observed.lifecycleBefore != frame.core.before) {
        noteDivergence(ReplayPlaybackProofRegionV2::FrameLifecycleBefore,
                       frame.replayFrame, lifecycleProof(frame.core.before),
                       lifecycleProof(observed.lifecycleBefore));
    }
    if (observed.activeLaneMask != frame.core.activeLaneMask
        || frame.core.activeLaneMask
            != timeline_.start().manifest.activeLaneMask) {
        finishTyped(
            ipc::v3::PlaybackResultReason::FramePreHashMismatch,
            ReplayPlaybackFaultV2::FrameLaneMaskMismatch,
            ReplayPlaybackProofRegionV2::FrameLaneMask,
            frame.replayFrame, frame.core.activeLaneMask,
            observed.activeLaneMask);
        return false;
    }

    const bool final = isFinalRecord(frame.replayFrame);
    if (final && !binding_.headless
        && observed.finalPresentationSerial == 0u) {
        finishTyped(
            ipc::v3::PlaybackResultReason::PresentationAckTimeout,
            ReplayPlaybackFaultV2::PresentationSerialMissing,
            ReplayPlaybackProofRegionV2::PresentationSerial,
            frame.replayFrame, 1u, 0u);
        return false;
    }

    inFlightReplayFrame_ = frame.replayFrame;
    latchedActiveLaneMask_ = frame.core.activeLaneMask;
    latchedActions_ = frame.core.actions;
    finalPresentationSerial_ = final
        ? observed.finalPresentationSerial : 0u;
    ++nextRecord_;
    phase_ = final
        ? ReplayPlaybackPhaseV2::TerminalArmed
        : ReplayPlaybackPhaseV2::FrameInFlight;
    return true;
}

bool ReplayPlaybackSessionV2::commitFrame(
    const ReplayFrameCommitObservationV2& observed) {
    if (phase_ != ReplayPlaybackPhaseV2::FrameInFlight
        && phase_ != ReplayPlaybackPhaseV2::TerminalArmed) {
        return false;
    }
    const bool final =
        phase_ == ReplayPlaybackPhaseV2::TerminalArmed;
    const CanonicalTickBundleV2* tick =
        timeline_.tick(inFlightReplayFrame_);
    if (tick == nullptr) {
        finishTyped(
            ipc::v3::PlaybackResultReason::InternalFailure,
            ReplayPlaybackFaultV2::InvalidTransition,
            ReplayPlaybackProofRegionV2::StateTransition,
            inFlightReplayFrame_, 1u, 0u);
        return false;
    }
    const ConfirmedGameFrameV2& frame = tick->frame;
    if (observed.replayFrame != frame.replayFrame) {
        finishTyped(
            ipc::v3::PlaybackResultReason::FramePostHashMismatch,
            ReplayPlaybackFaultV2::FrameNumberMismatch,
            ReplayPlaybackProofRegionV2::FrameNumber,
            frame.replayFrame, frame.replayFrame, observed.replayFrame);
        return false;
    }
    if (byteExactStateProof()
        && observed.postStateHash != frame.core.postStateHash) {
        noteDivergence(ReplayPlaybackProofRegionV2::FramePostState,
                       frame.replayFrame, frame.core.postStateHash,
                       observed.postStateHash);
    }
    if (observed.lifecycleAfter != frame.core.after) {
        noteDivergence(ReplayPlaybackProofRegionV2::FrameLifecycleAfter,
                       frame.replayFrame, lifecycleProof(frame.core.after),
                       lifecycleProof(observed.lifecycleAfter));
    }
    if (observed.dispatcherResult != frame.core.dispatcherResult) {
        noteDivergence(ReplayPlaybackProofRegionV2::DispatcherResult,
                       frame.replayFrame,
                       static_cast<uint64_t>(
                           static_cast<int64_t>(frame.core.dispatcherResult)),
                       static_cast<uint64_t>(
                           static_cast<int64_t>(observed.dispatcherResult)));
    }

    inFlightReplayFrame_ = kNoReplayMismatchFrameV2;
    latchedActiveLaneMask_ = 0u;
    latchedActions_.fill(0u);
    if (!final) {
        phase_ = ReplayPlaybackPhaseV2::Ready;
        return true;
    }
    if (binding_.headless) {
        finishVerified();
    } else {
        phase_ =
            ReplayPlaybackPhaseV2::AwaitingFinalPresentation;
    }
    return true;
}

ReplayPresentationAckV2
ReplayPlaybackSessionV2::acknowledgeFinalPresentation(
    uint64_t presentationSerial) {
    if (phase_ == ReplayPlaybackPhaseV2::ResultReady
        || phase_ == ReplayPlaybackPhaseV2::Faulted) {
        return ReplayPresentationAckV2::AlreadyCompleted;
    }
    if (phase_
        != ReplayPlaybackPhaseV2::AwaitingFinalPresentation) {
        return ReplayPresentationAckV2::NotWaiting;
    }
    if (presentationSerial == 0u
        || presentationSerial != finalPresentationSerial_) {
        return ReplayPresentationAckV2::WrongSerial;
    }
    finishVerified();
    return ReplayPresentationAckV2::Accepted;
}

bool ReplayPlaybackSessionV2::finalPresentationTimedOut() {
    if (phase_
        != ReplayPlaybackPhaseV2::AwaitingFinalPresentation) {
        return false;
    }
    finishTyped(
        ipc::v3::PlaybackResultReason::PresentationAckTimeout,
        ReplayPlaybackFaultV2::PresentationAckTimeout,
        ReplayPlaybackProofRegionV2::PresentationSerial,
        binding_.endpoint.nextRecord - 1u,
        finalPresentationSerial_, 0u);
    return true;
}

bool ReplayPlaybackSessionV2::noteDeviceBoundaryCall(
    uint64_t count) {
    if (count == 0u || hasResult()) return false;
    deviceCallCount_ = saturatedAdd(deviceCallCount_, count);
    const uint32_t frame = inFlightReplayFrame_
        == kNoReplayMismatchFrameV2 ? nextRecord_
                                   : inFlightReplayFrame_;
    finishTyped(
        ipc::v3::PlaybackResultReason::DeviceIsolationViolation,
        ReplayPlaybackFaultV2::DeviceIsolationViolation,
        ReplayPlaybackProofRegionV2::DeviceBoundary,
        frame, 0u, deviceCallCount_);
    return true;
}

bool ReplayPlaybackSessionV2::requestUserExit() {
    if (hasResult()) return false;
    finishTyped(
        ipc::v3::PlaybackResultReason::UserExit,
        ReplayPlaybackFaultV2::None,
        ReplayPlaybackProofRegionV2::None,
        kNoReplayMismatchFrameV2, 0u, 0u);
    return true;
}

bool ReplayPlaybackSessionV2::reportBrokerFailure() {
    if (hasResult()) return false;
    finishTyped(
        ipc::v3::PlaybackResultReason::BrokerFailure,
        ReplayPlaybackFaultV2::BrokerFailure,
        ReplayPlaybackProofRegionV2::None,
        kNoReplayMismatchFrameV2, 0u, 0u);
    return true;
}

void ReplayPlaybackSessionV2::populateResultBase() {
    result_ = ipc::v3::PlaybackResultPayload{};
    result_.endpointKind = binding_.endpoint.kind;
    result_.headless = binding_.headless;
    result_.hasFileDigest = binding_.hasFileDigest;
    if (binding_.hasFileDigest)
        result_.fileDigest = binding_.fileDigest;
    result_.validatedSourceByteCount =
        binding_.endpoint.validatedSourceByteCount;
    result_.validatedSourceRangeDigest =
        binding_.endpoint.validatedSourceRangeDigest;
    result_.runtimeContract = binding_.runtimeContract;
    result_.nextRecordConsumed = nextRecord_;
    result_.finalRollingDigest =
        binding_.endpoint.finalRollingDigest;
    result_.postTailStateHash =
        binding_.endpoint.postTailStateHash;
    result_.deviceCallCount = deviceCallCount_;

    if (binding_.endpoint.hasRecordedGameEnd) {
        result_.hasCanonicalGameDigest = true;
        result_.canonicalGameDigest =
            binding_.endpoint.recordedGameEnd.canonicalGameDigest;
        result_.hasRecordedTerminal = true;
        result_.recordedTerminal = static_cast<uint16_t>(
            binding_.endpoint.recordedGameEnd.reason);
    }
}

void ReplayPlaybackSessionV2::noteDivergence(
    const ReplayPlaybackProofRegionV2 region, const uint32_t replayFrame,
    const uint64_t expected, const uint64_t actual) {
    ++divergenceCount_;
    if (firstDivergenceFrame_ != kNoReplayMismatchFrameV2) return;
    firstDivergenceFrame_ = replayFrame;
    firstDivergenceRegion_ = region;
    firstDivergenceExpected_ = expected;
    firstDivergenceActual_ = actual;
}

void ReplayPlaybackSessionV2::finishVerified() {
    if (divergenceCount_ != 0u) {
        finishTyped(
            ipc::v3::PlaybackResultReason::CompletedWithDivergence,
            ReplayPlaybackFaultV2::None, firstDivergenceRegion_,
            firstDivergenceFrame_, firstDivergenceExpected_,
            firstDivergenceActual_);
        return;
    }
    const ipc::v3::PlaybackResultReason reason =
        !byteExactStateProof()
        ? ipc::v3::PlaybackResultReason::VerifiedBehaviourOnly
        : (binding_.endpoint.kind
                == ipc::v3::PlaybackEndpointKind::
                    UnsealedValidatedPrefix
            ? ipc::v3::PlaybackResultReason::VerifiedUnsealedPrefix
            : (binding_.endpoint.recordedGameEnd.reason
                    == TerminalReason::Completed
                ? ipc::v3::PlaybackResultReason::VerifiedCompleted
                : ipc::v3::PlaybackResultReason::VerifiedAbnormalEnd));
    finishTyped(
        reason, ReplayPlaybackFaultV2::None,
        ReplayPlaybackProofRegionV2::None,
        kNoReplayMismatchFrameV2, 0u, 0u);
}

void ReplayPlaybackSessionV2::finishTyped(
    ipc::v3::PlaybackResultReason reason,
    ReplayPlaybackFaultV2 fault,
    ReplayPlaybackProofRegionV2 region,
    uint32_t mismatchFrame,
    uint64_t expected,
    uint64_t actual) {
    if (hasResult()) return;
    populateResultBase();
    result_.reason = reason;
    result_.mismatchFrame = mismatchFrame;
    result_.mismatchRegion = static_cast<uint32_t>(region);
    result_.expectedValue = expected;
    result_.actualValue = actual;
    fault_ = fault;
    exitState_ = ReplayPlaybackExitStateV2::ResultPending;
    phase_ = fault == ReplayPlaybackFaultV2::None
        ? ReplayPlaybackPhaseV2::ResultReady
        : ReplayPlaybackPhaseV2::Faulted;
}

bool ReplayPlaybackSessionV2::acknowledgeResultDelivery() {
    if (exitState_ != ReplayPlaybackExitStateV2::ResultPending)
        return false;
    exitState_ = ReplayPlaybackExitStateV2::ResultAcknowledged;
    return true;
}

bool ReplayPlaybackSessionV2::requestGracefulExit() {
    if (exitState_
        != ReplayPlaybackExitStateV2::ResultAcknowledged) {
        return false;
    }
    exitState_ = ReplayPlaybackExitStateV2::ExitRequested;
    return true;
}

}
