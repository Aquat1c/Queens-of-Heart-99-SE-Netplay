#pragma once

#include "compat/CompatibilityIdentity.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qreplay {

// Bytes on disk are ASCII "RCI3" in this little-endian u32 representation.
constexpr uint32_t kReplayCompatibilityIdentityMagicV3 = 0x33494352u;
constexpr uint16_t kReplayCompatibilityIdentityEnvelopeVersionV3 = 3u;
constexpr size_t kReplayCompatibilityIdentityHeaderBytesV3 = 12u;
constexpr size_t kReplayCompatibilityIdentityEnvelopeBytesV3 =
    kReplayCompatibilityIdentityHeaderBytesV3
    + qnetplay::compat::kCompatibilityIdentityWireBytes;

enum class ReplayCompatibilityIdentityStatusV3 : uint8_t {
    Invalid = 0u,
    Attested = 1u,
    LegacyUnattested = 2u,
};

struct ReplayCompatibilityIdentityEnvelopeV3 {
    ReplayCompatibilityIdentityStatusV3 status =
        ReplayCompatibilityIdentityStatusV3::Invalid;
    qnetplay::compat::CompatibilityIdentity identity{};

    bool attested() const {
        return status == ReplayCompatibilityIdentityStatusV3::Attested;
    }
    bool legacyUnattested() const {
        return status
            == ReplayCompatibilityIdentityStatusV3::LegacyUnattested;
    }
};

enum class ReplayCompatibilityIdentityDecodeStatusV3 : uint8_t {
    Ok = 0u,
    NullInput,
    WrongLength,
    BadMagic,
    UnsupportedVersion,
    NonzeroReserved,
    InvalidIdentity,
};

enum class ReplayCompatibilityIdentityDecisionStatusV3 : uint8_t {
    Exact = 0u,
    LegacyUnattested,
    RecordedIdentityInvalid,
    RuntimeIdentityInvalid,
    IdentityMismatch,
    SupersededStateSchema,
    LegacyContentRootMembership,
};

struct ReplayCompatibilityIdentityDecisionV3 {
    ReplayCompatibilityIdentityDecisionStatusV3 status =
        ReplayCompatibilityIdentityDecisionStatusV3::RecordedIdentityInvalid;
    qnetplay::compat::IdentityMismatch mismatch =
        qnetplay::compat::IdentityMismatch::None;

    bool accepted() const {
        return status == ReplayCompatibilityIdentityDecisionStatusV3::Exact
            || status == ReplayCompatibilityIdentityDecisionStatusV3::
                             SupersededStateSchema
            || status == ReplayCompatibilityIdentityDecisionStatusV3::
                             LegacyContentRootMembership;
    }

    bool stateProofsAreBinding() const {
        return status == ReplayCompatibilityIdentityDecisionStatusV3::Exact
            || status == ReplayCompatibilityIdentityDecisionStatusV3::
                             LegacyContentRootMembership;
    }
};

ReplayCompatibilityIdentityEnvelopeV3
makeLegacyUnattestedReplayCompatibilityIdentityV3();

bool validateReplayCompatibilityIdentityEnvelopeV3(
    const ReplayCompatibilityIdentityEnvelopeV3& envelope,
    std::string& error);

bool encodeReplayCompatibilityIdentityEnvelopeV3(
    const ReplayCompatibilityIdentityEnvelopeV3& envelope,
    std::vector<uint8_t>& out, std::string& error);

ReplayCompatibilityIdentityDecodeStatusV3
decodeReplayCompatibilityIdentityEnvelopeV3(
    const uint8_t* bytes, size_t length,
    ReplayCompatibilityIdentityEnvelopeV3& out, std::string& error);

struct LegacyContentRootsV3 {
    uint8_t simulationRootSha256[qnetplay::compat::kCompatibilitySha256Bytes]{};
    uint8_t validationRootSha256[qnetplay::compat::kCompatibilitySha256Bytes]{};
    uint32_t manifestSchemaVersion = 0u;
    bool valid = false;
};

ReplayCompatibilityIdentityDecisionV3
evaluateReplayCompatibilityIdentityV3(
    const ReplayCompatibilityIdentityEnvelopeV3& recorded,
    const qnetplay::compat::CompatibilityIdentity& runtime,
    const LegacyContentRootsV3* legacyRoots = nullptr);

const char* replayCompatibilityIdentityDecodeStatusTextV3(
    ReplayCompatibilityIdentityDecodeStatusV3 status);
const char* replayCompatibilityIdentityDecisionStatusTextV3(
    ReplayCompatibilityIdentityDecisionStatusV3 status);

}
