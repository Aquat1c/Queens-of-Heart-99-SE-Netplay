#pragma once

#include "replay/ReplayIpcWire.h"
#include "replay/ReplayCompatibilityIdentityV3.h"
#include "replay/ReplayTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qclient::replay {

struct ReplayRuntimeIdentityV2 {
    bool known = false;
    uint16_t containerReaderMajor = qreplay::kContainerReaderMajorV2;
    qreplay::SimulationContractV2 contract{};
    qnetplay::compat::CompatibilityIdentity compatibilityIdentity{};
    qreplay::LegacyContentRootsV3 legacyContentRoots{};
};

enum class ReplayCompatibilityStateV2 : uint8_t {
    Exact = 0,
    RuntimeUnknown,
    ContentMismatch,
    Unsupported,
    Corrupt,
};

enum class ReplayCompatibilityReasonV2 : uint16_t {
    None = 0,
    StructuralInvalid,
    RuntimeUnknown,
    ContainerVersion,
    Executable,
    NativeBuild,
    RuntimeSimulation,
    StateRegionSchema,
    StateHashProjection,
    StateHashAlgorithm,
    ContentManifest,
    BootstrapSchema,
    EventSchema,
    CharacterCatalog,
    TerminalResultSchema,
    ActionDomain,
    BootstrapKind,
    UnsupportedTopology,
    UnsupportedCpu,
    UnsupportedInputDomain,
    CharacterSelectorProof,
    StageIdentity,
    FpuContract,
    NativeDisplayState,
    CompleteCompatibilityIdentity,
};

struct ReplayCompatibilityDecisionV2 {
    ReplayCompatibilityStateV2 state =
        ReplayCompatibilityStateV2::Unsupported;
    ReplayCompatibilityReasonV2 reason =
        ReplayCompatibilityReasonV2::None;
    bool watchable = false;
    qreplay::ReplayVerificationModeV2 verificationMode =
        qreplay::ReplayVerificationModeV2::ByteExact;
    bool supersededStateSchema = false;
    std::string detail;
};

constexpr const char* kBehaviourVerifiedDetailSuffixV2 =
    " (rematch: behaviour-verified, not byte-exact)";

ReplayCompatibilityDecisionV2 evaluateReplayCompatibilityV2(
    const qreplay::ConfirmedGameStartV2& recorded,
    const ReplayRuntimeIdentityV2& runtime,
    const qreplay::ReplayCompatibilityIdentityEnvelopeV3* attested = nullptr);

enum class ReplaySourceClassificationV2 : uint8_t {
    SealedFile = 1,
    UnsealedValidatedPrefix = 2,
};

struct ReplaySourceFileIdentityV2 {
    uint64_t low = 0u;
    uint64_t high = 0u;

    bool valid() const { return low != 0u || high != 0u; }
};

struct ReplaySourceProofV2 {
    ReplaySourceClassificationV2 classification =
        ReplaySourceClassificationV2::SealedFile;
    ReplaySourceFileIdentityV2 fileIdentity{};
    bool structurallyValid = false;
    bool exactlyOneIndependentGame = false;
    bool independentStartVerified = false;
    bool completePostFrameBoundary = false;
    bool sourceIsPartFile = false;
    bool footerCompleteRenameFailure = false;
    bool hasGameEnd = false;
    bool hasFooter = false;
    bool footerVerified = false;
    bool sourceRangeDigestVerified = false;
    bool hasFileDigest = false;
    bool fileDigestVerified = false;
    uint16_t containerMajor = qreplay::kContainerVersionMajorV2;
    uint16_t containerMinor = qreplay::kContainerVersionMinorV2;
    uint16_t minimumReaderMajor = qreplay::kContainerReaderMajorV2;
    uint32_t featureFlags = 0u;
    uint64_t validatedSourceByteCount = 0u;
    std::array<uint8_t, 32> validatedSourceRangeDigest{};
    std::array<uint8_t, 32> fileDigest{};
};

struct ReplayPreflightInputV2 {
    ReplaySourceProofV2 source{};
    qreplay::ReplayCompatibilityIdentityEnvelopeV3 compatibilityIdentity{
        qreplay::ReplayCompatibilityIdentityStatusV3::LegacyUnattested,
        {}};
    qreplay::ConfirmedGameStartV2 start{};
    std::vector<qreplay::CanonicalTickBundleV2> ticks;
    std::vector<qreplay::ReplayPaletteV2> palettes;
    qreplay::ipc::v3::PlaybackEndpointPayload endpoint{};
};

enum class ReplayPreflightStatusV2 : uint16_t {
    Ready = 0,
    StructuralInvalid,
    SourceClassificationMismatch,
    SourceIdentityMissing,
    SourceRangeInvalid,
    FileDigestInvalid,
    EmptyTimeline,
    PrebufferLimitExceeded,
    TimelineInvalid,
    EndpointMismatch,
    CompatibilityRejected,
    CompatibilityIdentityUnattested,
    CompatibilityIdentityMismatch,
    AllocationFailure,
};

const char* replayPreflightStatusTextV2(ReplayPreflightStatusV2 status);

struct ReplayPreflightResultV2 {
    ReplayPreflightStatusV2 status =
        ReplayPreflightStatusV2::StructuralInvalid;
    ReplayCompatibilityDecisionV2 compatibility{};
    qreplay::ParseStatus timelineStatus = qreplay::ParseStatus::Ok;
    uint32_t frameCount = 0u;
    uint32_t eventCount = 0u;
    std::string detail;
    std::string technicalDetail;

    bool ok() const { return status == ReplayPreflightStatusV2::Ready; }
};

class PreparedReplayV2 {
public:
    PreparedReplayV2() = default;
    ~PreparedReplayV2() = default;
    PreparedReplayV2(PreparedReplayV2&&) noexcept = default;
    PreparedReplayV2& operator=(PreparedReplayV2&&) noexcept = default;
    PreparedReplayV2(const PreparedReplayV2&) = delete;
    PreparedReplayV2& operator=(const PreparedReplayV2&) = delete;

    const qreplay::ConfirmedGameStartV2& start() const { return start_; }
    const qreplay::CanonicalTickBundleV2* ticks() const {
        return ticks_.get();
    }
    const qreplay::CanonicalTickBundleV2& tick(size_t index) const {
        return ticks_[index];
    }
    const std::vector<qreplay::ReplayPaletteV2>& palettes() const {
        return palettes_;
    }
    uint32_t frameCount() const { return frameCount_; }
    uint32_t eventCount() const { return eventCount_; }
    const qreplay::ipc::v3::PlaybackEndpointPayload& endpoint() const {
        return endpoint_;
    }
    const qreplay::ipc::v3::PlaybackSessionInfoPayload& sessionInfo() const {
        return sessionInfo_;
    }
    const ReplaySourceProofV2& source() const { return source_; }
    const ReplayRuntimeIdentityV2& runtime() const { return runtime_; }
    const qreplay::ReplayCompatibilityIdentityEnvelopeV3&
    recordedCompatibilityIdentity() const {
        return compatibilityIdentity_;
    }
    qreplay::ReplayVerificationModeV2 verificationMode() const {
        return supersededStateSchema_
            ? qreplay::ReplayVerificationModeV2::BehaviourVerified
            : qreplay::replayVerificationModeForV2(
                  start_.manifest.identity);
    }
    bool supersededStateSchema() const { return supersededStateSchema_; }

private:
    friend ReplayPreflightResultV2 prepareReplayV2(
        const ReplayPreflightInputV2&,
        const ReplayRuntimeIdentityV2&,
        std::unique_ptr<PreparedReplayV2>&);

    bool supersededStateSchema_ = false;
    ReplaySourceProofV2 source_{};
    ReplayRuntimeIdentityV2 runtime_{};
    qreplay::ReplayCompatibilityIdentityEnvelopeV3 compatibilityIdentity_{};
    qreplay::ConfirmedGameStartV2 start_{};
    std::vector<qreplay::ReplayPaletteV2> palettes_{};
    std::unique_ptr<qreplay::CanonicalTickBundleV2[]> ticks_;
    uint32_t frameCount_ = 0u;
    uint32_t eventCount_ = 0u;
    qreplay::ipc::v3::PlaybackEndpointPayload endpoint_{};
    qreplay::ipc::v3::PlaybackSessionInfoPayload sessionInfo_{};
};

ReplayPreflightResultV2 prepareReplayV2(
    const ReplayPreflightInputV2& input,
    const ReplayRuntimeIdentityV2& runtime,
    std::unique_ptr<PreparedReplayV2>& out);

}
