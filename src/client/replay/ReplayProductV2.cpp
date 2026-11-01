#include "ReplayProductV2.h"

#include <algorithm>

namespace qclient::replay {
namespace {

bool zeroDigest(const std::array<uint8_t, 32>& digest) {
    return std::all_of(
        digest.begin(), digest.end(),
        [](const uint8_t byte) { return byte == 0u; });
}

}

bool makeReplayPreflightInputV2(
    const ParsedReplayV2& parsed,
    ReplayPreflightInputV2& out,
    std::string& detail) {
    namespace wire3 = qreplay::ipc::v3;

    const bool sealed = parsed.sealed();
    if (!sealed) {
        detail = "the selected replay is incomplete; only finalized files can "
                 "be played";
        return false;
    }
    if (!parsed.structurallyVerified || !parsed.hasManifest
        || !parsed.hasStart || !parsed.hasFrameZero
        || parsed.frameCount == 0u
        || parsed.ticks.size() != parsed.frameCount
        || parsed.validatedSourceBytes == 0u
        || zeroDigest(parsed.validatedSourceRangeDigest)) {
        detail =
            "the parser did not retain one complete, digest-bound game "
            "timeline";
        return false;
    }
    if (!parsed.hasCompatibilityIdentity
        || !parsed.compatibilityIdentity.attested()) {
        detail =
            "this legacy replay has no complete simulation compatibility "
            "identity attestation and cannot be injected";
        return false;
    }
    if (!parsed.fileIdentity.known
        || (parsed.fileIdentity.volumeId == 0u
            && parsed.fileIdentity.fileId == 0u)) {
        detail = "the selected replay has no stable file identity";
        return false;
    }
    const qreplay::GameIdentityV2& identity =
        parsed.start.manifest.identity;
    if (!qreplay::isValidGameIdentityV2(identity)
        || identity.gameUuidLow != parsed.prefix.gameUuidLow
        || identity.gameUuidHigh != parsed.prefix.gameUuidHigh) {
        detail =
            "the replay prefix and independent game start name different "
            "games";
        return false;
    }
    if (!parsed.hasEnd || !parsed.hasFooter || !parsed.fileDigestVerified
        || zeroDigest(parsed.footer.fileDigest)) {
        detail =
            "a sealed replay requires a verified End, footer, and file "
            "digest";
        return false;
    }

    ReplayPreflightInputV2 candidate{};
    candidate.start = parsed.start;
    candidate.compatibilityIdentity = parsed.compatibilityIdentity;
    candidate.ticks = parsed.ticks;
    candidate.palettes = parsed.palettes;

    ReplaySourceProofV2& source = candidate.source;
    source.classification = ReplaySourceClassificationV2::SealedFile;
    source.fileIdentity.low = parsed.fileIdentity.volumeId;
    source.fileIdentity.high = parsed.fileIdentity.fileId;
    source.structurallyValid = parsed.structurallyVerified;
    source.exactlyOneIndependentGame = true;
    source.independentStartVerified =
        parsed.hasStart && parsed.hasFrameZero;
    source.completePostFrameBoundary = true;
    source.sourceIsPartFile = parsed.classification
        == ReplayClassificationV2::FooterCompleteRenameFailure;
    source.footerCompleteRenameFailure =
        parsed.classification
        == ReplayClassificationV2::FooterCompleteRenameFailure;
    source.hasGameEnd = parsed.hasEnd;
    source.hasFooter = parsed.hasFooter;
    source.footerVerified = parsed.structurallyVerified;
    source.sourceRangeDigestVerified = true;
    source.hasFileDigest = true;
    source.fileDigestVerified = parsed.fileDigestVerified;
    source.containerMajor = parsed.prefix.containerMajor;
    source.containerMinor = parsed.prefix.containerMinor;
    source.minimumReaderMajor = parsed.prefix.minimumReaderMajor;
    source.featureFlags = parsed.prefix.featureFlags;
    source.validatedSourceByteCount = parsed.validatedSourceBytes;
    source.validatedSourceRangeDigest =
        parsed.validatedSourceRangeDigest;
    source.fileDigest = parsed.footer.fileDigest;

    wire3::PlaybackEndpointPayload& endpoint = candidate.endpoint;
    endpoint.kind = wire3::PlaybackEndpointKind::RecordedGameEnd;
    endpoint.hasRecordedGameEnd = true;
    endpoint.nextRecord = parsed.frameCount;
    endpoint.postTailStateHash = parsed.lastPostStateHash;
    endpoint.finalRollingDigest = parsed.finalRollingDigest;
    endpoint.validatedSourceByteCount = parsed.validatedSourceBytes;
    endpoint.validatedSourceRangeDigest =
        parsed.validatedSourceRangeDigest;
    endpoint.recordedGameEnd = parsed.end;

    out = std::move(candidate);
    detail.clear();
    return true;
}

}
