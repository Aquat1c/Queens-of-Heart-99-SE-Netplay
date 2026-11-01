#include "ReplayCompatibilityV2.h"

#include "replay/CharacterCatalog.h"
#include "replay/ConfirmedTimeline.h"
#include "replay/ReplaySimulationContractV2.h"
#include "replay/StageCatalog.h"

#include <algorithm>
#include <limits>
#include <new>

namespace qclient::replay {

namespace {

using namespace qreplay;
namespace wire3 = qreplay::ipc::v3;

bool zeroDigest(const std::array<uint8_t, 32>& digest) {
    return std::all_of(
        digest.begin(), digest.end(),
        [](uint8_t value) { return value == 0u; });
}

bool zeroNativeDisplay(const NativeDisplayStateV2& display) {
    for (uint32_t codePage : display.codePages)
        if (codePage != 0u) return false;
    for (const auto& name : display.names)
        for (char byte : name)
            if (byte != '\0') return false;
    return true;
}

ReplayCompatibilityDecisionV2 decision(
    ReplayCompatibilityStateV2 state,
    ReplayCompatibilityReasonV2 reason,
    const char* detail) {
    ReplayCompatibilityDecisionV2 result{};
    result.state = state;
    result.reason = reason;
    result.watchable = state == ReplayCompatibilityStateV2::Exact;
    result.detail = detail != nullptr ? detail : "";
    return result;
}

ReplayPreflightResultV2 preflightFailure(
    ReplayPreflightStatusV2 status,
    const char* detail,
    ParseStatus timeline = ParseStatus::Ok) {
    ReplayPreflightResultV2 result{};
    result.status = status;
    result.timelineStatus = timeline;
    result.detail = detail != nullptr ? detail : "";
    return result;
}

bool exactFirstShipTopology(const GameManifestV2& manifest) {
    if (manifest.bootstrapKind
            != BootstrapKindV2::MatchActiveDirectBootV1
        || manifest.vsRow != 1u
        || manifest.activeLaneMask != 0x03u
        || manifest.activeFighterMask != 0x03u
        || manifest.cpuMask != 0u
        || manifest.cpuContract != CpuContract::None
        || manifest.slotToSourceRoute
            != std::array<uint8_t, kFighterSlots>{{0u, 1u, 2u, 3u}}) {
        return false;
    }
    for (size_t lane = 0u; lane < kSourceLanes; ++lane) {
        if (manifest.lanes[lane].active != (lane < 2u))
            return false;
    }
    for (size_t slot = 0u; slot < kFighterSlots; ++slot) {
        const FighterSlotDescriptorV2& fighter = manifest.slots[slot];
        if (slot < 2u) {
            if (fighter.ownership != SlotOwnership::RecordedHuman
                || !fighter.selectorProofPresent) {
                return false;
            }
        } else if (fighter.ownership != SlotOwnership::Inactive) {
            return false;
        }
    }
    return true;
}

bool selectorProofsMatch(const GameManifestV2& manifest) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();
    if (manifest.contract.characterCatalogId != catalog.catalogId())
        return false;
    for (size_t slot = 0u; slot < 2u; ++slot) {
        const FighterSlotDescriptorV2& fighter = manifest.slots[slot];
        if (!catalog.isSelectable(fighter.identity.nativeCharacterId)
            || !catalog.verifySelectorProof(
                fighter.selectorProof,
                fighter.identity.nativeCharacterId)) {
            return false;
        }
    }
    return true;
}

uint32_t countEvents(
    const std::vector<CanonicalTickBundleV2>& ticks,
    bool& overflow) {
    uint64_t total = 0u;
    for (const CanonicalTickBundleV2& tick : ticks) {
        total += tick.preEvents.count;
        total += tick.postEvents.count;
    }
    overflow = total > std::numeric_limits<uint32_t>::max()
        || total > kMaxTimelineEventsV2;
    return overflow ? 0u : static_cast<uint32_t>(total);
}

bool sourceEnvelopeMatches(
    const ReplaySourceProofV2& source,
    const wire3::PlaybackEndpointPayload& endpoint,
    std::string& detail) {
    if (source.classification != ReplaySourceClassificationV2::SealedFile) {
        detail = "only finalized replay files may enter playback";
        return false;
    }
    if (!source.fileIdentity.valid()) {
        detail = "the selected source has no stable opaque file identity";
        return false;
    }
    if (!source.sourceRangeDigestVerified
        || source.validatedSourceByteCount == 0u
        || source.validatedSourceByteCount > kMaxFileBytes
        || zeroDigest(source.validatedSourceRangeDigest)) {
        detail = "the validated source byte range is not digest-bound";
        return false;
    }
    if (endpoint.validatedSourceByteCount
            != source.validatedSourceByteCount
        || endpoint.validatedSourceRangeDigest
            != source.validatedSourceRangeDigest) {
        detail = "the endpoint names a different source byte range";
        return false;
    }

    if ((source.sourceIsPartFile && !source.footerCompleteRenameFailure)
        || (!source.sourceIsPartFile && source.footerCompleteRenameFailure)
        || !source.hasGameEnd || !source.hasFooter || !source.footerVerified
        || !source.hasFileDigest || !source.fileDigestVerified
        || zeroDigest(source.fileDigest)
        || endpoint.kind != wire3::PlaybackEndpointKind::RecordedGameEnd
        || !endpoint.hasRecordedGameEnd) {
        detail = "a sealed replay requires a verified End, footer, and file "
                 "digest";
        return false;
    }
    return true;
}

ReplayCompatibilityDecisionV2 evaluateCompatibilityCoreV2(
    const qreplay::ConfirmedGameStartV2& recorded,
    const ReplayRuntimeIdentityV2& runtime,
    const qreplay::ReplayCompatibilityIdentityEnvelopeV3* attested) {
    using namespace qreplay;
    if (!isValidGameManifestV2(recorded.manifest)
        || !isValidDeterministicGameStartV2(recorded.start)) {
        return decision(
            ReplayCompatibilityStateV2::Corrupt,
            ReplayCompatibilityReasonV2::StructuralInvalid,
            "the recorded game start is structurally invalid");
    }
    if (!runtime.known) {
        return decision(
            ReplayCompatibilityStateV2::RuntimeUnknown,
            ReplayCompatibilityReasonV2::RuntimeUnknown,
            "no installed runtime identity is available");
    }
    if (runtime.containerReaderMajor != kContainerReaderMajorV2) {
        return decision(
            ReplayCompatibilityStateV2::Unsupported,
            ReplayCompatibilityReasonV2::ContainerVersion,
            "the runtime is not the exact Q99R-v2 reader");
    }
    if (!isValidSimulationContractV2(runtime.contract)) {
        return decision(
            ReplayCompatibilityStateV2::Unsupported,
            ReplayCompatibilityReasonV2::StructuralInvalid,
            "the installed runtime contract is incomplete");
    }

    const SimulationContractV2& file = recorded.manifest.contract;
    const SimulationContractV2& local = runtime.contract;
    struct ContractCheck {
        bool equal;
        ReplayCompatibilityStateV2 state;
        ReplayCompatibilityReasonV2 reason;
        const char* detail;
    };
    const ContractCheck checks[] = {
        { file.executableSha256 == local.executableSha256,
          ReplayCompatibilityStateV2::ContentMismatch,
          ReplayCompatibilityReasonV2::Executable,
          "the installed executable digest differs" },
        { file.nativeBuildIdentity == local.nativeBuildIdentity,
          ReplayCompatibilityStateV2::Unsupported,
          ReplayCompatibilityReasonV2::NativeBuild,
          "the native executable build identity differs" },
        { file.runtimeSimulationId == local.runtimeSimulationId,
          ReplayCompatibilityStateV2::Unsupported,
          ReplayCompatibilityReasonV2::RuntimeSimulation,
          "the replay runtime simulation identity differs" },
        { file.stateRegionSchemaFingerprint
              == local.stateRegionSchemaFingerprint,
          ReplayCompatibilityStateV2::Unsupported,
          ReplayCompatibilityReasonV2::StateRegionSchema,
          "the deterministic state-region schema differs" },
        { file.stateHashProjectionId == local.stateHashProjectionId,
          ReplayCompatibilityStateV2::Unsupported,
          ReplayCompatibilityReasonV2::StateHashProjection,
          "the replay state-hash projection differs" },
        { qreplay::replayStateHashAlgorithmSupportedV2(
              file.stateHashAlgorithmId),
          ReplayCompatibilityStateV2::Unsupported,
          ReplayCompatibilityReasonV2::StateHashAlgorithm,
          "the replay state-hash algorithm is not one this build can verify" },
        { file.bootstrapSchemaId == local.bootstrapSchemaId,
          ReplayCompatibilityStateV2::Unsupported,
          ReplayCompatibilityReasonV2::BootstrapSchema,
          "the deterministic bootstrap schema differs" },
        { file.eventSchemaId == local.eventSchemaId,
          ReplayCompatibilityStateV2::Unsupported,
          ReplayCompatibilityReasonV2::EventSchema,
          "the replay event schema differs" },
        { file.characterCatalogId == local.characterCatalogId,
          ReplayCompatibilityStateV2::ContentMismatch,
          ReplayCompatibilityReasonV2::CharacterCatalog,
          "the character catalog differs" },
        { file.terminalResultSchemaId == local.terminalResultSchemaId,
          ReplayCompatibilityStateV2::Unsupported,
          ReplayCompatibilityReasonV2::TerminalResultSchema,
          "the terminal-result schema differs" },
        { file.compactActionDomainVersion
              == local.compactActionDomainVersion,
          ReplayCompatibilityStateV2::Unsupported,
          ReplayCompatibilityReasonV2::ActionDomain,
          "the compact logical-action domain differs" },
    };
    for (const ContractCheck& check : checks) {
        if (!check.equal)
            return decision(check.state, check.reason, check.detail);
    }

    bool verifiedContentDrift = false;
    if (file.contentManifestRoot != local.contentManifestRoot) {
        const bool curatedContentIdentical = attested != nullptr
            && attested->attested()
            && evaluateReplayCompatibilityIdentityV3(
                   *attested, runtime.compatibilityIdentity,
                   &runtime.legacyContentRoots)
                   .accepted();
        if (!curatedContentIdentical) {
            return decision(
                ReplayCompatibilityStateV2::ContentMismatch,
                ReplayCompatibilityReasonV2::ContentManifest,
                "the installed simulation-relevant content differs");
        }
        verifiedContentDrift = true;
    }

    if (recorded.manifest.bootstrapKind
        != BootstrapKindV2::MatchActiveDirectBootV1) {
        return decision(
            ReplayCompatibilityStateV2::Unsupported,
            ReplayCompatibilityReasonV2::BootstrapKind,
            "the replay bootstrap kind is not implemented");
    }
    if (recorded.manifest.cpuMask != 0u
        || recorded.manifest.cpuContract != CpuContract::None) {
        return decision(
            ReplayCompatibilityStateV2::Unsupported,
            ReplayCompatibilityReasonV2::UnsupportedCpu,
            "CPU replay contracts are not qualified in the first release");
    }
    if (!exactFirstShipTopology(recorded.manifest)) {
        return decision(
            ReplayCompatibilityStateV2::Unsupported,
            ReplayCompatibilityReasonV2::UnsupportedTopology,
            "only row-1 two-human 1v1 with identity routing is qualified");
    }
    if (!selectorProofsMatch(recorded.manifest)) {
        return decision(
            ReplayCompatibilityStateV2::ContentMismatch,
            ReplayCompatibilityReasonV2::CharacterSelectorProof,
            "a selector proof does not resolve to its recorded character");
    }
    if (!StageCatalogV2::forSeBuild().validateIdentity(
            recorded.manifest.stage)) {
        return decision(
            ReplayCompatibilityStateV2::ContentMismatch,
            ReplayCompatibilityReasonV2::StageIdentity,
            "the stage identity is not in the verified SE stage catalog");
    }
    if (recorded.start.fpu.x87ControlWord != 0x027Fu
        || recorded.start.fpu.mxcsr != 0x1F80u) {
        return decision(
            ReplayCompatibilityStateV2::Unsupported,
            ReplayCompatibilityReasonV2::FpuContract,
            "only the qualified default x87/MXCSR contract is supported");
    }
    if (!zeroNativeDisplay(recorded.start.nativeDisplay)) {
        return decision(
            ReplayCompatibilityStateV2::Unsupported,
            ReplayCompatibilityReasonV2::NativeDisplayState,
            "native display-name state is not yet a proven replay field");
    }
    if (verifiedContentDrift) {
        return decision(
            ReplayCompatibilityStateV2::Exact,
            ReplayCompatibilityReasonV2::None,
            "exact verified-content match; unrelated local files in the "
            "game folder changed since this was recorded");
    }
    return decision(
        ReplayCompatibilityStateV2::Exact,
        ReplayCompatibilityReasonV2::None,
        "the replay exactly matches the installed row-1 1v1 runtime");
}

}

ReplayCompatibilityDecisionV2 evaluateReplayCompatibilityV2(
    const qreplay::ConfirmedGameStartV2& recorded,
    const ReplayRuntimeIdentityV2& runtime,
    const qreplay::ReplayCompatibilityIdentityEnvelopeV3* attested) {
    ReplayCompatibilityDecisionV2 result =
        evaluateCompatibilityCoreV2(recorded, runtime, attested);
    result.verificationMode = qreplay::replayVerificationModeForV2(
        recorded.manifest.identity);
    if (attested != nullptr
        && qnetplay::compat::isSupersededStateSchema(
               attested->identity.stateSchemaId)) {
        result.supersededStateSchema = true;
        result.verificationMode =
            qreplay::ReplayVerificationModeV2::BehaviourVerified;
    }
    if (result.watchable
        && result.verificationMode
            == qreplay::ReplayVerificationModeV2::BehaviourVerified) {
        result.detail += kBehaviourVerifiedDetailSuffixV2;
    }
    return result;
}

const char* replayPreflightStatusTextV2(
    ReplayPreflightStatusV2 status) {
    switch (status) {
        case ReplayPreflightStatusV2::Ready: return "ready";
        case ReplayPreflightStatusV2::StructuralInvalid:
            return "structural invalid";
        case ReplayPreflightStatusV2::SourceClassificationMismatch:
            return "source classification mismatch";
        case ReplayPreflightStatusV2::SourceIdentityMissing:
            return "source identity missing";
        case ReplayPreflightStatusV2::SourceRangeInvalid:
            return "source range invalid";
        case ReplayPreflightStatusV2::FileDigestInvalid:
            return "file digest invalid";
        case ReplayPreflightStatusV2::EmptyTimeline:
            return "empty timeline";
        case ReplayPreflightStatusV2::PrebufferLimitExceeded:
            return "prebuffer limit exceeded";
        case ReplayPreflightStatusV2::TimelineInvalid:
            return "timeline invalid";
        case ReplayPreflightStatusV2::EndpointMismatch:
            return "endpoint mismatch";
        case ReplayPreflightStatusV2::CompatibilityRejected:
            return "compatibility rejected";
        case ReplayPreflightStatusV2::CompatibilityIdentityUnattested:
            return "complete compatibility identity unattested";
        case ReplayPreflightStatusV2::CompatibilityIdentityMismatch:
            return "complete compatibility identity mismatch";
        case ReplayPreflightStatusV2::AllocationFailure:
            return "allocation failure";
    }
    return "unknown";
}

ReplayPreflightResultV2 prepareReplayV2(
    const ReplayPreflightInputV2& input,
    const ReplayRuntimeIdentityV2& runtime,
    std::unique_ptr<PreparedReplayV2>& out) {
    using namespace qreplay;
    namespace wire3 = qreplay::ipc::v3;

    if (!input.source.structurallyValid
        || !input.source.exactlyOneIndependentGame
        || !input.source.independentStartVerified
        || !input.source.completePostFrameBoundary
        || input.source.containerMajor != kContainerVersionMajorV2
        || input.source.containerMinor != kContainerVersionMinorV2
        || input.source.minimumReaderMajor != kContainerReaderMajorV2
        || (input.source.featureFlags & ~kFeatureRequiredMaskV2) != 0u) {
        return preflightFailure(
            ReplayPreflightStatusV2::StructuralInvalid,
            "the parser did not prove one independent Q99R-v2 game");
    }
    if (!input.source.fileIdentity.valid()) {
        return preflightFailure(
            ReplayPreflightStatusV2::SourceIdentityMissing,
            "the selected source has no stable opaque identity");
    }
    if (input.source.featureFlags != kFeatureCompatibilityIdentityV3) {
        ReplayPreflightResultV2 result = preflightFailure(
            ReplayPreflightStatusV2::CompatibilityRejected,
            "the identity-attested replay profile supports no team, CPU, or "
            "checkpoint feature bit");
        result.compatibility = decision(
            ReplayCompatibilityStateV2::Unsupported,
            ReplayCompatibilityReasonV2::UnsupportedTopology,
            result.detail.c_str());
        return result;
    }
    const qreplay::ReplayCompatibilityIdentityDecisionV3 identityDecision =
        qreplay::evaluateReplayCompatibilityIdentityV3(
            input.compatibilityIdentity, runtime.compatibilityIdentity,
            &runtime.legacyContentRoots);
    if (!identityDecision.accepted()) {
        const bool legacy = identityDecision.status
            == qreplay::ReplayCompatibilityIdentityDecisionStatusV3::
                LegacyUnattested;
        const qnetplay::compat::IdentityMismatch mismatch =
            identityDecision.mismatch;
        const std::string detail = legacy
            ? std::string(
                "this replay was recorded before recordings carried a "
                "compatibility record, so there is no way to tell whether "
                "this build would reproduce it faithfully")
            : (mismatch == qnetplay::compat::IdentityMismatch::None
                ? std::string(
                    qreplay::replayCompatibilityIdentityDecisionStatusTextV3(
                        identityDecision.status))
                : std::string("this replay cannot be played here: ")
                    + qnetplay::compat::identityMismatchPlayerText(mismatch)
                    + ".");
        ReplayPreflightResultV2 result = preflightFailure(
            legacy
                ? ReplayPreflightStatusV2::CompatibilityIdentityUnattested
                : ReplayPreflightStatusV2::CompatibilityIdentityMismatch,
            detail.c_str());
        result.technicalDetail =
            std::string("identity decision=")
            + qreplay::replayCompatibilityIdentityDecisionStatusTextV3(
                identityDecision.status)
            + " mismatch="
            + qnetplay::compat::identityMismatchReason(mismatch);
        result.compatibility = decision(
            identityDecision.status
                    == qreplay::ReplayCompatibilityIdentityDecisionStatusV3::
                        IdentityMismatch
                ? ReplayCompatibilityStateV2::ContentMismatch
                : ReplayCompatibilityStateV2::Unsupported,
            ReplayCompatibilityReasonV2::CompleteCompatibilityIdentity,
            detail.c_str());
        return result;
    }
    std::string sourceDetail;
    if (!sourceEnvelopeMatches(input.source, input.endpoint, sourceDetail)) {
        const ReplayPreflightStatusV2 status =
            !input.source.sourceRangeDigestVerified
                || input.source.validatedSourceByteCount == 0u
                || zeroDigest(input.source.validatedSourceRangeDigest)
            ? ReplayPreflightStatusV2::SourceRangeInvalid
            : (input.source.classification
                    == ReplaySourceClassificationV2::SealedFile
                && (!input.source.hasFileDigest
                    || !input.source.fileDigestVerified
                    || zeroDigest(input.source.fileDigest))
                ? ReplayPreflightStatusV2::FileDigestInvalid
                : ReplayPreflightStatusV2::
                    SourceClassificationMismatch);
        return preflightFailure(status, sourceDetail.c_str());
    }
    if (input.ticks.empty()) {
        return preflightFailure(
            ReplayPreflightStatusV2::EmptyTimeline,
            "an independently bootable replay must contain frame zero");
    }
    if (input.ticks.size() > kMaxPlaybackPrebufferFramesV2
        || input.ticks.size() > std::numeric_limits<uint32_t>::max()) {
        return preflightFailure(
            ReplayPreflightStatusV2::PrebufferLimitExceeded,
            "the replay exceeds the 300000-frame full-prebuffer policy");
    }

    bool eventOverflow = false;
    const uint32_t eventCount = countEvents(input.ticks, eventOverflow);
    if (eventOverflow) {
        return preflightFailure(
            ReplayPreflightStatusV2::PrebufferLimitExceeded,
            "the replay event count exceeds the bounded v2 policy");
    }
    for (const CanonicalTickBundleV2& tick : input.ticks) {
        if (tick.frame.core.inputDomain
            != InputDomain::LogicalSourceV1) {
            ReplayPreflightResultV2 result = preflightFailure(
                ReplayPreflightStatusV2::CompatibilityRejected,
                "only recorded logical QOH source actions are qualified");
            result.compatibility = decision(
                ReplayCompatibilityStateV2::Unsupported,
                ReplayCompatibilityReasonV2::UnsupportedInputDomain,
                result.detail.c_str());
            result.frameCount =
                static_cast<uint32_t>(input.ticks.size());
            result.eventCount = eventCount;
            return result;
        }
    }

    ConfirmedTimelineValidatorV2 validator;
    ParseStatus timelineStatus = ParseStatus::Ok;
    if (!validator.begin(input.start, timelineStatus)) {
        return preflightFailure(
            ReplayPreflightStatusV2::TimelineInvalid,
            "the independent deterministic start failed validation",
            timelineStatus);
    }
    for (const CanonicalTickBundleV2& tick : input.ticks) {
        if (!validator.append(tick, timelineStatus)) {
            return preflightFailure(
                ReplayPreflightStatusV2::TimelineInvalid,
                confirmedTimelineFaultTextV2(validator.lastFault()),
                timelineStatus);
        }
    }
    const bool sealed = input.source.classification
        == ReplaySourceClassificationV2::SealedFile;
    if (sealed
        && !validator.seal(
            input.endpoint.recordedGameEnd, timelineStatus)) {
        return preflightFailure(
            ReplayPreflightStatusV2::TimelineInvalid,
            confirmedTimelineFaultTextV2(validator.lastFault()),
            timelineStatus);
    }

    const uint32_t frameCount =
        static_cast<uint32_t>(input.ticks.size());
    const CanonicalTickBundleV2& tail = input.ticks.back();
    if (input.endpoint.nextRecord != frameCount
        || input.endpoint.postTailStateHash
            != tail.frame.core.postStateHash
        || input.endpoint.finalRollingDigest
            != validator.rollingDigest()
        || (sealed && !validator.sealed())
        || (!sealed && validator.sealed())) {
        return preflightFailure(
            ReplayPreflightStatusV2::EndpointMismatch,
            "the finite endpoint does not exactly describe the validated "
            "timeline tail");
    }
    std::vector<uint8_t> endpointBytes;
    if (!wire3::encodePlaybackEndpoint(input.endpoint, endpointBytes)) {
        return preflightFailure(
            ReplayPreflightStatusV2::EndpointMismatch,
            "the finite endpoint is not a canonical wire-v3 endpoint");
    }

    ReplayCompatibilityDecisionV2 compatibility =
        evaluateReplayCompatibilityV2(input.start, runtime,
                                      &input.compatibilityIdentity);
    if (!compatibility.watchable) {
        ReplayPreflightResultV2 result = preflightFailure(
            ReplayPreflightStatusV2::CompatibilityRejected,
            compatibility.detail.c_str());
        result.compatibility = compatibility;
        result.frameCount = frameCount;
        result.eventCount = eventCount;
        return result;
    }

    std::unique_ptr<PreparedReplayV2> prepared(
        new (std::nothrow) PreparedReplayV2());
    if (!prepared) {
        return preflightFailure(
            ReplayPreflightStatusV2::AllocationFailure,
            "could not allocate the prepared replay descriptor");
    }
    std::unique_ptr<CanonicalTickBundleV2[]> exact(
        new (std::nothrow) CanonicalTickBundleV2[frameCount]);
    if (!exact) {
        return preflightFailure(
            ReplayPreflightStatusV2::AllocationFailure,
            "could not allocate the exact-size replay prebuffer");
    }
    for (uint32_t i = 0u; i < frameCount; ++i)
        exact[i] = input.ticks[i];

    wire3::PlaybackSessionInfoPayload info{};
    info.containerMajor = input.source.containerMajor;
    info.containerMinor = input.source.containerMinor;
    info.minimumReaderMajor = input.source.minimumReaderMajor;
    info.featureFlags = input.source.featureFlags;
    info.frameCount = frameCount;
    info.eventCount = eventCount;
    info.activeLaneMask = input.start.manifest.activeLaneMask;
    info.activeFighterMask = input.start.manifest.activeFighterMask;
    info.hasFileDigest = sealed;
    info.hasCanonicalGameDigest = sealed;
    if (sealed) {
        info.fileDigest = input.source.fileDigest;
        info.canonicalGameDigest =
            input.endpoint.recordedGameEnd.canonicalGameDigest;
    }
    info.validatedSourceByteCount =
        input.source.validatedSourceByteCount;
    info.validatedSourceRangeDigest =
        input.source.validatedSourceRangeDigest;
    info.recordedContract = input.start.manifest.contract;
    info.endpoint = input.endpoint;
    std::vector<uint8_t> sessionBytes;
    if (!wire3::encodePlaybackSessionInfo(info, sessionBytes)) {
        return preflightFailure(
            ReplayPreflightStatusV2::EndpointMismatch,
            "the prepared session envelope is not canonical wire-v3 data");
    }

    prepared->source_ = input.source;
    prepared->supersededStateSchema_ = compatibility.supersededStateSchema;
    prepared->runtime_ = runtime;
    prepared->compatibilityIdentity_ = input.compatibilityIdentity;
    prepared->start_ = input.start;
    prepared->palettes_ = input.palettes;
    prepared->ticks_ = static_cast<
        std::unique_ptr<CanonicalTickBundleV2[]>&&>(exact);
    prepared->frameCount_ = frameCount;
    prepared->eventCount_ = eventCount;
    prepared->endpoint_ = input.endpoint;
    prepared->sessionInfo_ = info;

    ReplayPreflightResultV2 result{};
    result.status = ReplayPreflightStatusV2::Ready;
    result.compatibility = compatibility;
    result.timelineStatus = ParseStatus::Ok;
    result.frameCount = frameCount;
    result.eventCount = eventCount;
    result.detail = sealed
        ? "sealed replay fully validated and prebuffered"
        : "unsealed source prefix fully validated and prebuffered";
    out = static_cast<std::unique_ptr<PreparedReplayV2>&&>(prepared);
    return result;
}

}
