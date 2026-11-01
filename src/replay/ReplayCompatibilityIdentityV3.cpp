#include "ReplayCompatibilityIdentityV3.h"

#include <cstring>
#include <utility>

namespace qreplay {
namespace {

void putU16(std::vector<uint8_t>& out, const uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8u));
}

void putU32(std::vector<uint8_t>& out, const uint32_t value) {
    for (unsigned index = 0u; index < 4u; ++index)
        out.push_back(static_cast<uint8_t>(value >> (index * 8u)));
}

void putU64(std::vector<uint8_t>& out, const uint64_t value) {
    for (unsigned index = 0u; index < 8u; ++index)
        out.push_back(static_cast<uint8_t>(value >> (index * 8u)));
}

void putBytes(std::vector<uint8_t>& out, const uint8_t* bytes,
              const size_t count) {
    out.insert(out.end(), bytes, bytes + count);
}

bool getU16(const uint8_t* const data, const size_t length, size_t& offset,
            uint16_t& out) {
    if (data == nullptr || offset > length || length - offset < 2u)
        return false;
    out = static_cast<uint16_t>(data[offset])
        | static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1u])
                                << 8u);
    offset += 2u;
    return true;
}

bool getU32(const uint8_t* const data, const size_t length, size_t& offset,
            uint32_t& out) {
    if (data == nullptr || offset > length || length - offset < 4u)
        return false;
    out = 0u;
    for (unsigned index = 0u; index < 4u; ++index)
        out |= static_cast<uint32_t>(data[offset + index]) << (index * 8u);
    offset += 4u;
    return true;
}

bool getU64(const uint8_t* const data, const size_t length, size_t& offset,
            uint64_t& out) {
    if (data == nullptr || offset > length || length - offset < 8u)
        return false;
    out = 0u;
    for (unsigned index = 0u; index < 8u; ++index)
        out |= static_cast<uint64_t>(data[offset + index]) << (index * 8u);
    offset += 8u;
    return true;
}

bool getBytes(const uint8_t* const data, const size_t length, size_t& offset,
              uint8_t* const out, const size_t count) {
    if ((data == nullptr || out == nullptr) || offset > length
        || length - offset < count) {
        return false;
    }
    std::memcpy(out, data + offset, count);
    offset += count;
    return true;
}

void putIdentity(std::vector<uint8_t>& out,
                 const qnetplay::compat::CompatibilityIdentity& identity) {
    using namespace qnetplay::compat;
    putU32(out, identity.schemaVersion);
    putU32(out, identity.protocolVersion);
    putBytes(out, reinterpret_cast<const uint8_t*>(identity.runtimeBuild.profileId),
             kCompatibilityProfileIdBytes);
    putBytes(out, identity.runtimeBuild.executableSha256,
             kCompatibilitySha256Bytes);
    putU64(out, identity.runtimeBuild.executableSize);
    putU32(out, identity.runtimeBuild.peTimestamp);
    putU32(out, identity.simulationContent.manifestSchemaVersion);
    putU32(out, identity.simulationContent.manifestFlags);
    putBytes(out, identity.simulationContent.simulationRootSha256,
             kCompatibilitySha256Bytes);
    putBytes(out, identity.simulationContent.validationRootSha256,
             kCompatibilitySha256Bytes);
    putU32(out, identity.simulationContent.entryCount);
    putU32(out, identity.simulationContent.requiredEntryCount);
    putU64(out, identity.stateSchemaId);
    putU64(out, identity.modSimulationId);
    putU64(out, identity.timingProfileId);
    putU32(out, identity.reserved);
}

bool getIdentity(const uint8_t* const data, const size_t length,
                 size_t& offset,
                 qnetplay::compat::CompatibilityIdentity& out) {
    using namespace qnetplay::compat;
    CompatibilityIdentity decoded{};
    if (!getU32(data, length, offset, decoded.schemaVersion)
        || !getU32(data, length, offset, decoded.protocolVersion)
        || !getBytes(data, length, offset,
                     reinterpret_cast<uint8_t*>(decoded.runtimeBuild.profileId),
                     kCompatibilityProfileIdBytes)
        || !getBytes(data, length, offset,
                     decoded.runtimeBuild.executableSha256,
                     kCompatibilitySha256Bytes)
        || !getU64(data, length, offset, decoded.runtimeBuild.executableSize)
        || !getU32(data, length, offset, decoded.runtimeBuild.peTimestamp)
        || !getU32(data, length, offset,
                    decoded.simulationContent.manifestSchemaVersion)
        || !getU32(data, length, offset,
                    decoded.simulationContent.manifestFlags)
        || !getBytes(data, length, offset,
                     decoded.simulationContent.simulationRootSha256,
                     kCompatibilitySha256Bytes)
        || !getBytes(data, length, offset,
                     decoded.simulationContent.validationRootSha256,
                     kCompatibilitySha256Bytes)
        || !getU32(data, length, offset,
                    decoded.simulationContent.entryCount)
        || !getU32(data, length, offset,
                    decoded.simulationContent.requiredEntryCount)
        || !getU64(data, length, offset, decoded.stateSchemaId)
        || !getU64(data, length, offset, decoded.modSimulationId)
        || !getU64(data, length, offset, decoded.timingProfileId)
        || !getU32(data, length, offset, decoded.reserved)) {
        return false;
    }
    out = decoded;
    return true;
}

}

ReplayCompatibilityIdentityEnvelopeV3
makeLegacyUnattestedReplayCompatibilityIdentityV3() {
    ReplayCompatibilityIdentityEnvelopeV3 legacy{};
    legacy.status = ReplayCompatibilityIdentityStatusV3::LegacyUnattested;
    return legacy;
}

bool validateReplayCompatibilityIdentityEnvelopeV3(
    const ReplayCompatibilityIdentityEnvelopeV3& envelope,
    std::string& error) {
    error.clear();
    if (envelope.status == ReplayCompatibilityIdentityStatusV3::LegacyUnattested) {
        error = "legacy replay has no complete compatibility identity attestation";
        return false;
    }
    if (envelope.status != ReplayCompatibilityIdentityStatusV3::Attested) {
        error = "replay compatibility identity envelope is not attested";
        return false;
    }
    if (!qnetplay::compat::isValidCompatibilityIdentity(envelope.identity)) {
        error = "replay compatibility identity is incomplete or invalid";
        return false;
    }
    return true;
}

bool encodeReplayCompatibilityIdentityEnvelopeV3(
    const ReplayCompatibilityIdentityEnvelopeV3& envelope,
    std::vector<uint8_t>& out, std::string& error) {
    if (!validateReplayCompatibilityIdentityEnvelopeV3(envelope, error))
        return false;

    std::vector<uint8_t> encoded;
    encoded.reserve(kReplayCompatibilityIdentityEnvelopeBytesV3);
    putU32(encoded, kReplayCompatibilityIdentityMagicV3);
    putU16(encoded, kReplayCompatibilityIdentityEnvelopeVersionV3);
    putU16(encoded, 0u);
    putU32(encoded, static_cast<uint32_t>(
        qnetplay::compat::kCompatibilityIdentityWireBytes));
    putIdentity(encoded, envelope.identity);
    if (encoded.size() != kReplayCompatibilityIdentityEnvelopeBytesV3) {
        error = "replay compatibility identity encoder produced an unexpected size";
        return false;
    }
    out = std::move(encoded);
    error.clear();
    return true;
}

ReplayCompatibilityIdentityDecodeStatusV3
decodeReplayCompatibilityIdentityEnvelopeV3(
    const uint8_t* const bytes, const size_t length,
    ReplayCompatibilityIdentityEnvelopeV3& out, std::string& error) {
    out = ReplayCompatibilityIdentityEnvelopeV3{};
    error.clear();
    if (bytes == nullptr) {
        error = "replay compatibility identity envelope has no input bytes";
        return ReplayCompatibilityIdentityDecodeStatusV3::NullInput;
    }
    if (length != kReplayCompatibilityIdentityEnvelopeBytesV3) {
        error = "replay compatibility identity envelope has the wrong length";
        return ReplayCompatibilityIdentityDecodeStatusV3::WrongLength;
    }

    size_t offset = 0u;
    uint32_t magic = 0u;
    uint16_t version = 0u;
    uint16_t reserved = 0u;
    uint32_t payloadBytes = 0u;
    ReplayCompatibilityIdentityEnvelopeV3 decoded{};
    if (!getU32(bytes, length, offset, magic)
        || !getU16(bytes, length, offset, version)
        || !getU16(bytes, length, offset, reserved)
        || !getU32(bytes, length, offset, payloadBytes)
        || !getIdentity(bytes, length, offset, decoded.identity)
        || offset != length) {
        error = "replay compatibility identity envelope is truncated";
        return ReplayCompatibilityIdentityDecodeStatusV3::WrongLength;
    }
    if (magic != kReplayCompatibilityIdentityMagicV3) {
        error = "replay compatibility identity envelope magic is not RCI3";
        return ReplayCompatibilityIdentityDecodeStatusV3::BadMagic;
    }
    if (version != kReplayCompatibilityIdentityEnvelopeVersionV3) {
        error = "replay compatibility identity envelope version is unsupported";
        return ReplayCompatibilityIdentityDecodeStatusV3::UnsupportedVersion;
    }
    if (reserved != 0u
        || payloadBytes != qnetplay::compat::kCompatibilityIdentityWireBytes) {
        error = "replay compatibility identity envelope has unsupported reserved fields";
        return ReplayCompatibilityIdentityDecodeStatusV3::NonzeroReserved;
    }
    decoded.status = ReplayCompatibilityIdentityStatusV3::Attested;
    if (!validateReplayCompatibilityIdentityEnvelopeV3(decoded, error)) {
        return ReplayCompatibilityIdentityDecodeStatusV3::InvalidIdentity;
    }
    out = decoded;
    return ReplayCompatibilityIdentityDecodeStatusV3::Ok;
}

ReplayCompatibilityIdentityDecisionV3
evaluateReplayCompatibilityIdentityV3(
    const ReplayCompatibilityIdentityEnvelopeV3& recorded,
    const qnetplay::compat::CompatibilityIdentity& runtime,
    const LegacyContentRootsV3* const legacyRoots) {
    ReplayCompatibilityIdentityDecisionV3 decision{};
    if (recorded.legacyUnattested()) {
        decision.status =
            ReplayCompatibilityIdentityDecisionStatusV3::LegacyUnattested;
        return decision;
    }
    if (!recorded.attested()
        || !qnetplay::compat::isValidCompatibilityIdentity(recorded.identity)) {
        decision.status = ReplayCompatibilityIdentityDecisionStatusV3::
            RecordedIdentityInvalid;
        return decision;
    }
    if (!qnetplay::compat::isValidCompatibilityIdentity(runtime)) {
        decision.status = ReplayCompatibilityIdentityDecisionStatusV3::
            RuntimeIdentityInvalid;
        decision.mismatch = qnetplay::compat::IdentityMismatch::
            LocalIdentityInvalid;
        return decision;
    }
    qnetplay::compat::CompatibilityIdentity runtimeForReplay = runtime;
    runtimeForReplay.protocolVersion = recorded.identity.protocolVersion;
    decision.mismatch = qnetplay::compat::compatibilityMismatch(
        runtimeForReplay, recorded.identity);
    if (decision.mismatch == qnetplay::compat::IdentityMismatch::None) {
        decision.status = ReplayCompatibilityIdentityDecisionStatusV3::Exact;
        return decision;
    }
    const bool anySupersededSchema =
        qnetplay::compat::isSupersededStateSchema(
            recorded.identity.stateSchemaId);
    const bool haveLegacyRoots =
        legacyRoots != nullptr && legacyRoots->valid;
    const bool recordedSchemaIsSuperseded =
        haveLegacyRoots
        && recorded.identity.simulationContent.manifestSchemaVersion
            == legacyRoots->manifestSchemaVersion;

    const int rootAttempts = haveLegacyRoots ? 2 : 1;
    for (int rootAttempt = 0; rootAttempt < rootAttempts; ++rootAttempt) {
        for (int useRecordedSchema = 0; useRecordedSchema < 2;
             ++useRecordedSchema) {
            if (useRecordedSchema == 1 && !anySupersededSchema) continue;
            qnetplay::compat::CompatibilityIdentity probe = runtimeForReplay;
            if (rootAttempt == 1) {
                std::memcpy(probe.simulationContent.simulationRootSha256,
                            legacyRoots->simulationRootSha256,
                            qnetplay::compat::kCompatibilitySha256Bytes);
                std::memcpy(probe.simulationContent.validationRootSha256,
                            legacyRoots->validationRootSha256,
                            qnetplay::compat::kCompatibilitySha256Bytes);
            }
            if (recordedSchemaIsSuperseded) {
                probe.simulationContent.manifestSchemaVersion =
                    recorded.identity.simulationContent.manifestSchemaVersion;
            }
            if (useRecordedSchema == 1) {
                probe.stateSchemaId = recorded.identity.stateSchemaId;
            }
            if (probe.simulationContent.manifestSchemaVersion
                    == runtimeForReplay.simulationContent.manifestSchemaVersion
                && rootAttempt == 0 && useRecordedSchema == 0) {
                continue;
            }
            if (qnetplay::compat::compatibilityMismatch(probe,
                                                        recorded.identity)
                != qnetplay::compat::IdentityMismatch::None) {
                continue;
            }
            decision.status = useRecordedSchema == 1
                ? ReplayCompatibilityIdentityDecisionStatusV3::
                      SupersededStateSchema
                : ReplayCompatibilityIdentityDecisionStatusV3::
                      LegacyContentRootMembership;
            return decision;
        }
    }
    decision.status =
        ReplayCompatibilityIdentityDecisionStatusV3::IdentityMismatch;
    return decision;
}

const char* replayCompatibilityIdentityDecodeStatusTextV3(
    const ReplayCompatibilityIdentityDecodeStatusV3 status) {
    switch (status) {
        case ReplayCompatibilityIdentityDecodeStatusV3::Ok:
            return "attested V3 identity decoded";
        case ReplayCompatibilityIdentityDecodeStatusV3::NullInput:
            return "no identity bytes supplied";
        case ReplayCompatibilityIdentityDecodeStatusV3::WrongLength:
            return "identity envelope length is invalid";
        case ReplayCompatibilityIdentityDecodeStatusV3::BadMagic:
            return "identity envelope magic is invalid";
        case ReplayCompatibilityIdentityDecodeStatusV3::UnsupportedVersion:
            return "identity envelope version is unsupported";
        case ReplayCompatibilityIdentityDecodeStatusV3::NonzeroReserved:
            return "identity envelope reserved fields are nonzero";
        case ReplayCompatibilityIdentityDecodeStatusV3::InvalidIdentity:
            return "identity payload is invalid";
    }
    return "unknown identity decode status";
}

const char* replayCompatibilityIdentityDecisionStatusTextV3(
    const ReplayCompatibilityIdentityDecisionStatusV3 status) {
    switch (status) {
        case ReplayCompatibilityIdentityDecisionStatusV3::Exact:
            return "complete compatibility identity matches";
        case ReplayCompatibilityIdentityDecisionStatusV3::LegacyUnattested:
            return "legacy replay has no complete compatibility identity attestation";
        case ReplayCompatibilityIdentityDecisionStatusV3::
            RecordedIdentityInvalid:
            return "recorded compatibility identity is invalid";
        case ReplayCompatibilityIdentityDecisionStatusV3::
            RuntimeIdentityInvalid:
            return "runtime compatibility identity is invalid";
        case ReplayCompatibilityIdentityDecisionStatusV3::IdentityMismatch:
            return "recorded and runtime compatibility identities differ";
        case ReplayCompatibilityIdentityDecisionStatusV3::SupersededStateSchema:
            return "identity matches under a superseded state schema; "
                   "state proofs are measured but not enforced";
        case ReplayCompatibilityIdentityDecisionStatusV3::
            LegacyContentRootMembership:
            return "identity matches this installation under the pre-.chr "
                   "content-root membership the recording was made with";
    }
    return "unknown identity compatibility decision";
}

}
