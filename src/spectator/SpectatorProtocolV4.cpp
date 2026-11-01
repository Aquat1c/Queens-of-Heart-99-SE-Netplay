#include "SpectatorProtocolV4.h"

#include "../replay/ReplayIntegrity.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace qnetplay::spectator {
namespace {

constexpr size_t kPublicationHeaderBytesV4 = 16u;
constexpr size_t kChainStampBytesV4 = 32u;
constexpr size_t kSetEventPayloadBytesV4 =
    76u + qnetplay::spectator::kSetPaletteEntryBytes;
constexpr size_t kFragmentHeaderBytesV4 = 40u;
constexpr uint32_t kFragmentMagicV4 = 0x52463451u; // "Q4FR"
constexpr uint16_t kFragmentSchemaV4 = 1u;

bool hasValidConnectedDescriptorCore(
    const ConnectedSetDescriptorV4& descriptor);
uint64_t computeLegacyConnectedGenesisDigestV4(
    const ConnectedSetDescriptorV4& descriptor);
void mixCompatibilityIdentity(
    uint64_t& hash, const compat::CompatibilityIdentityWire& identity);

void putU8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}
void putU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}
void putU32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}
void putU64(std::vector<uint8_t>& out, uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

class Reader {
public:
    Reader(const uint8_t* bytes, size_t length)
        : bytes_(bytes), length_(length) {}

    bool u8(uint8_t& out) {
        if (offset_ >= length_) return false;
        out = bytes_[offset_++];
        return true;
    }
    bool u16(uint16_t& out) {
        if (length_ - offset_ < 2u) return false;
        out = static_cast<uint16_t>(bytes_[offset_])
            | static_cast<uint16_t>(bytes_[offset_ + 1u]) << 8;
        offset_ += 2u;
        return true;
    }
    bool u32(uint32_t& out) {
        if (length_ - offset_ < 4u) return false;
        out = 0u;
        for (unsigned shift = 0; shift != 32; shift += 8)
            out |= static_cast<uint32_t>(bytes_[offset_++]) << shift;
        return true;
    }
    bool u64(uint64_t& out) {
        if (length_ - offset_ < 8u) return false;
        out = 0u;
        for (unsigned shift = 0; shift != 64; shift += 8)
            out |= static_cast<uint64_t>(bytes_[offset_++]) << shift;
        return true;
    }
    bool span(size_t count, const uint8_t*& out) {
        if (count > length_ - offset_) return false;
        out = bytes_ + offset_;
        offset_ += count;
        return true;
    }
    size_t remaining() const { return length_ - offset_; }
    size_t offset() const { return offset_; }

private:
    const uint8_t* bytes_ = nullptr;
    size_t length_ = 0u;
    size_t offset_ = 0u;
};

// QSP4 is explicitly little-endian: never copy the packed CompatibilityIdentity object image into this stream.
void putCompatibilityIdentity(
    std::vector<uint8_t>& out,
    const compat::CompatibilityIdentityWire& identity) {
    putU32(out, identity.schemaVersion);
    putU32(out, identity.protocolVersion);
    out.insert(out.end(), identity.runtimeBuild.profileId,
               identity.runtimeBuild.profileId
                   + compat::kCompatibilityProfileIdBytes);
    out.insert(out.end(), identity.runtimeBuild.executableSha256,
               identity.runtimeBuild.executableSha256
                   + compat::kCompatibilitySha256Bytes);
    putU64(out, identity.runtimeBuild.executableSize);
    putU32(out, identity.runtimeBuild.peTimestamp);
    putU32(out, identity.simulationContent.manifestSchemaVersion);
    putU32(out, identity.simulationContent.manifestFlags);
    out.insert(out.end(), identity.simulationContent.simulationRootSha256,
               identity.simulationContent.simulationRootSha256
                   + compat::kCompatibilitySha256Bytes);
    out.insert(out.end(), identity.simulationContent.validationRootSha256,
               identity.simulationContent.validationRootSha256
                   + compat::kCompatibilitySha256Bytes);
    putU32(out, identity.simulationContent.entryCount);
    putU32(out, identity.simulationContent.requiredEntryCount);
    putU64(out, identity.stateSchemaId);
    putU64(out, identity.modSimulationId);
    putU64(out, identity.timingProfileId);
    putU32(out, identity.reserved);
}

bool getCompatibilityIdentity(
    Reader& reader, compat::CompatibilityIdentityWire& out) {
    compat::CompatibilityIdentityWire decoded{};
    const uint8_t* bytes = nullptr;
    if (!reader.u32(decoded.schemaVersion)
        || !reader.u32(decoded.protocolVersion)
        || !reader.span(compat::kCompatibilityProfileIdBytes, bytes)) {
        return false;
    }
    std::memcpy(decoded.runtimeBuild.profileId, bytes,
                compat::kCompatibilityProfileIdBytes);
    if (!reader.span(compat::kCompatibilitySha256Bytes, bytes)) return false;
    std::memcpy(decoded.runtimeBuild.executableSha256, bytes,
                compat::kCompatibilitySha256Bytes);
    if (!reader.u64(decoded.runtimeBuild.executableSize)
        || !reader.u32(decoded.runtimeBuild.peTimestamp)
        || !reader.u32(decoded.simulationContent.manifestSchemaVersion)
        || !reader.u32(decoded.simulationContent.manifestFlags)
        || !reader.span(compat::kCompatibilitySha256Bytes, bytes)) {
        return false;
    }
    std::memcpy(decoded.simulationContent.simulationRootSha256, bytes,
                compat::kCompatibilitySha256Bytes);
    if (!reader.span(compat::kCompatibilitySha256Bytes, bytes)) return false;
    std::memcpy(decoded.simulationContent.validationRootSha256, bytes,
                compat::kCompatibilitySha256Bytes);
    if (!reader.u32(decoded.simulationContent.entryCount)
        || !reader.u32(decoded.simulationContent.requiredEntryCount)
        || !reader.u64(decoded.stateSchemaId)
        || !reader.u64(decoded.modSimulationId)
        || !reader.u64(decoded.timingProfileId)
        || !reader.u32(decoded.reserved)
        || !compat::isValidCompatibilityIdentity(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool isZeroIdentity(const qreplay::GameIdentityV2& identity) {
    return identity.gameUuidLow == 0u && identity.gameUuidHigh == 0u
        && identity.gameGeneration == 0u;
}

void putIdentity(
    std::vector<uint8_t>& out, const qreplay::GameIdentityV2& identity) {
    putU64(out, identity.gameUuidLow);
    putU64(out, identity.gameUuidHigh);
    putU32(out, identity.gameGeneration);
}

bool getIdentity(Reader& reader, qreplay::GameIdentityV2& out) {
    qreplay::GameIdentityV2 decoded{};
    if (!reader.u64(decoded.gameUuidLow)
        || !reader.u64(decoded.gameUuidHigh)
        || !reader.u32(decoded.gameGeneration)) {
        return false;
    }
    out = decoded;
    return true;
}

void putChain(
    std::vector<uint8_t>& out, const ConnectedChainStampV4& chain) {
    putU64(out, chain.setStreamId);
    putU32(out, chain.itemSequence);
    putU32(out, chain.recordCursorBefore);
    putU64(out, chain.setDigestBefore);
    putU64(out, chain.setDigestAfter);
}

bool getChain(Reader& reader, ConnectedChainStampV4& out) {
    ConnectedChainStampV4 decoded{};
    if (!reader.u64(decoded.setStreamId)
        || !reader.u32(decoded.itemSequence)
        || !reader.u32(decoded.recordCursorBefore)
        || !reader.u64(decoded.setDigestBefore)
        || !reader.u64(decoded.setDigestAfter)) {
        return false;
    }
    out = decoded;
    return true;
}

bool putEventList(
    std::vector<uint8_t>& out, const qreplay::FixedEventListV2& list,
    qreplay::GameEventPhaseV2 requiredPhase) {
    if (list.count > qreplay::kMaxEventsPerTickV2) return false;
    putU8(out, list.count);
    putU8(out, 0u);
    putU16(out, 0u);
    for (size_t i = 0; i < list.count; ++i) {
        const qreplay::ConfirmedGameEventV2& event = list.events[i];
        if (event.phase != requiredPhase || event.replayFrame != 0u
            || event.eventSequence != 0u || event.rollingDigest != 0u
            || !qreplay::encodeConfirmedGameEventV2(event, out)) {
            return false;
        }
    }
    return true;
}

bool getEventList(
    Reader& reader, qreplay::GameEventPhaseV2 requiredPhase,
    qreplay::FixedEventListV2& out) {
    uint8_t count = 0u;
    uint8_t reserved8 = 0u;
    uint16_t reserved16 = 0u;
    if (!reader.u8(count) || !reader.u8(reserved8)
        || !reader.u16(reserved16)
        || count > qreplay::kMaxEventsPerTickV2
        || reserved8 != 0u || reserved16 != 0u) {
        return false;
    }
    qreplay::FixedEventListV2 decoded{};
    decoded.count = count;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* bytes = nullptr;
        if (!reader.span(qreplay::kConfirmedGameEventBytesV2, bytes)
            || qreplay::decodeConfirmedGameEventV2(
                   bytes, qreplay::kConfirmedGameEventBytesV2,
                   decoded.events[i]) != qreplay::ParseStatus::Ok
            || decoded.events[i].phase != requiredPhase
            || decoded.events[i].replayFrame != 0u
            || decoded.events[i].eventSequence != 0u
            || decoded.events[i].rollingDigest != 0u) {
            return false;
        }
    }
    out = decoded;
    return true;
}

bool encodeStartItem(
    const SpectatorPerGameStartV4& start, std::vector<uint8_t>& out) {
    if (!qreplay::isValidGameIdentityV2(start.game)
        || start.game != start.start.manifest.identity) {
        return false;
    }
    putChain(out, start.chain);
    putIdentity(out, start.game);
    return qreplay::encodeConfirmedGameStartV2(start.start, out);
}

bool decodeStartItem(
    const uint8_t* bytes, size_t length, SpectatorPerGameStartV4& out) {
    const size_t expected = kChainStampBytesV4
        + qreplay::kGameIdentityBytesV2
        + qreplay::kGameManifestBytesV2
        + qreplay::kDeterministicGameStartBytesV2;
    if (length != expected) return false;
    Reader reader(bytes, length);
    SpectatorPerGameStartV4 decoded{};
    const uint8_t* startBytes = nullptr;
    if (!getChain(reader, decoded.chain)
        || !getIdentity(reader, decoded.game)
        || !reader.span(
               qreplay::kGameManifestBytesV2
                   + qreplay::kDeterministicGameStartBytesV2,
               startBytes)
        || qreplay::decodeConfirmedGameStartV2(
               startBytes,
               qreplay::kGameManifestBytesV2
                   + qreplay::kDeterministicGameStartBytesV2,
               decoded.start) != qreplay::ParseStatus::Ok
        || decoded.game != decoded.start.manifest.identity
        || !qreplay::isValidGameIdentityV2(decoded.game)
        || reader.remaining() != 0u) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodeRecordItem(
    const ConnectedConfirmedRecordV4& record, std::vector<uint8_t>& out) {
    if (!isCanonicalConnectedRecordV4(record)) return false;
    putChain(out, record.chain);
    putU8(out, record.hasReplayGameIdentity ? 1u : 0u);
    putU8(out, 0u);
    putU16(out, 0u);
    putIdentity(out, record.game);
    putU32(out, record.sourceTimelineFrame);
    if (!putEventList(
            out, record.preEvents, qreplay::GameEventPhaseV2::PreFrame)
        || !qreplay::encodeConfirmedGameFrameCoreV2(record.frame, out)
        || !putEventList(
            out, record.postEvents,
            qreplay::GameEventPhaseV2::PostFrame)) {
        return false;
    }
    return true;
}

bool decodeRecordItem(
    const uint8_t* bytes, size_t length, ConnectedConfirmedRecordV4& out) {
    if (length < kChainStampBytesV4 + 4u
            + qreplay::kGameIdentityBytesV2 + 4u + 4u
            + qreplay::kConfirmedGameFrameCoreBytesV2 + 4u
        || length > kMaxConnectedPublicationBytesV4) {
        return false;
    }
    Reader reader(bytes, length);
    ConnectedConfirmedRecordV4 decoded{};
    uint8_t hasIdentity = 0u;
    uint8_t reserved8 = 0u;
    uint16_t reserved16 = 0u;
    const uint8_t* frameBytes = nullptr;
    if (!getChain(reader, decoded.chain)
        || !reader.u8(hasIdentity) || !reader.u8(reserved8)
        || !reader.u16(reserved16) || hasIdentity > 1u
        || reserved8 != 0u || reserved16 != 0u
        || !getIdentity(reader, decoded.game)
        || !reader.u32(decoded.sourceTimelineFrame)
        || !getEventList(
               reader, qreplay::GameEventPhaseV2::PreFrame,
               decoded.preEvents)
        || !reader.span(
               qreplay::kConfirmedGameFrameCoreBytesV2, frameBytes)
        || qreplay::decodeConfirmedGameFrameCoreV2(
               frameBytes, qreplay::kConfirmedGameFrameCoreBytesV2,
               qreplay::kFighterMaskV2, decoded.frame)
               != qreplay::ParseStatus::Ok
        || !getEventList(
               reader, qreplay::GameEventPhaseV2::PostFrame,
               decoded.postEvents)
        || reader.remaining() != 0u) {
        return false;
    }
    decoded.hasReplayGameIdentity = hasIdentity != 0u;
    if (!isCanonicalConnectedRecordV4(decoded)) return false;
    out = decoded;
    return true;
}

bool encodeSetEventPayload(
    const SetEventPayloadV4& event, std::vector<uint8_t>& out) {
    if (!isValidSetEventPayloadV4(event)) return false;
    const size_t initial = out.size();
    putU32(out, event.sourceTimelineFrame);
    putU16(out, static_cast<uint16_t>(event.kind));
    putU16(out, static_cast<uint16_t>(event.terminalReason));
    putU32(out, event.matchId);
    putU32(out, event.rosterEpoch);
    putU64(out, event.valueA);
    putU64(out, event.valueB);
    putU32(out, event.valueC);
    putU8(out, event.selectorProofMask);
    putU8(out, event.activeSlotMask);
    putU16(out, 0u);
    for (const SetFighterIdentity& fighter : event.roster) {
        putU8(out, fighter.nativeCharacterId);
        putU8(out, fighter.paletteIndex);
    }
    for (const SetSelectorProof& proof : event.selectorProof) {
        putU8(out, proof.cursorCell);
        putU8(out, proof.alternateFlag);
        putU8(out, proof.confirmState);
    }
    for (uint8_t route : event.slotToSourceRoute) putU8(out, route);
    for (uint8_t entry : event.paletteEntries) putU8(out, entry);
    putU32(out, 0u);
    putU64(out, event.stateProof);
    return out.size() - initial == kSetEventPayloadBytesV4;
}

bool decodeSetEventPayload(
    const uint8_t* bytes, size_t length, SetEventPayloadV4& out) {
    if (length != kSetEventPayloadBytesV4) return false;
    Reader reader(bytes, length);
    SetEventPayloadV4 decoded{};
    uint16_t kind = 0u;
    uint16_t reason = 0u;
    uint16_t reserved16 = 0u;
    uint32_t reserved32 = 0u;
    if (!reader.u32(decoded.sourceTimelineFrame)
        || !reader.u16(kind) || !reader.u16(reason)
        || !reader.u32(decoded.matchId)
        || !reader.u32(decoded.rosterEpoch)
        || !reader.u64(decoded.valueA) || !reader.u64(decoded.valueB)
        || !reader.u32(decoded.valueC)
        || !reader.u8(decoded.selectorProofMask)
        || !reader.u8(decoded.activeSlotMask)
        || !reader.u16(reserved16) || reserved16 != 0u) {
        return false;
    }
    for (SetFighterIdentity& fighter : decoded.roster) {
        if (!reader.u8(fighter.nativeCharacterId)
            || !reader.u8(fighter.paletteIndex)) {
            return false;
        }
    }
    for (SetSelectorProof& proof : decoded.selectorProof) {
        if (!reader.u8(proof.cursorCell)
            || !reader.u8(proof.alternateFlag)
            || !reader.u8(proof.confirmState)) {
            return false;
        }
    }
    for (uint8_t& route : decoded.slotToSourceRoute)
        if (!reader.u8(route)) return false;
    for (uint8_t& entry : decoded.paletteEntries)
        if (!reader.u8(entry)) return false;
    if (!reader.u32(reserved32) || reserved32 != 0u
        || !reader.u64(decoded.stateProof) || reader.remaining() != 0u
        || !isKnownSetEventKind(kind)
        || !isKnownSetSegmentTerminalReason(reason)) {
        return false;
    }
    decoded.kind = static_cast<SetEventKind>(kind);
    decoded.terminalReason =
        static_cast<SetSegmentTerminalReason>(reason);
    if (!isValidSetEventPayloadV4(decoded)) return false;
    out = decoded;
    return true;
}

constexpr size_t kPaletteItemPayloadBytesV4 =
    qreplay::kReplayPaletteBlockBytes;

bool encodePaletteItem(
    const ConnectedPaletteV4& palette, std::vector<uint8_t>& out) {
    if (palette.palette.side > 3u || palette.palette.variant > 3u
        || palette.palette.present > 1u || palette.palette.reserved != 0u) {
        return false;
    }
    putChain(out, palette.chain);
    std::vector<uint8_t> block;
    if (!qreplay::encodePaletteBlockV2(palette.palette, block)) return false;
    out.insert(out.end(), block.begin(), block.end());
    return true;
}

bool decodePaletteItem(
    const uint8_t* bytes, size_t length, ConnectedPaletteV4& out) {
    if (length != kChainStampBytesV4 + kPaletteItemPayloadBytesV4)
        return false;
    Reader reader(bytes, length);
    ConnectedPaletteV4 decoded{};
    const uint8_t* payload = nullptr;
    if (!getChain(reader, decoded.chain)
        || !reader.span(kPaletteItemPayloadBytesV4, payload)
        || qreplay::decodePaletteBlockV2(
               payload, kPaletteItemPayloadBytesV4, decoded.palette)
               != qreplay::ParseStatus::Ok
        || reader.remaining() != 0u) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodeSetEventItem(
    const ConnectedSetEventV4& event, std::vector<uint8_t>& out) {
    putChain(out, event.chain);
    return encodeSetEventPayload(event.event, out);
}

bool decodeSetEventItem(
    const uint8_t* bytes, size_t length, ConnectedSetEventV4& out) {
    if (length != kChainStampBytesV4 + kSetEventPayloadBytesV4)
        return false;
    Reader reader(bytes, length);
    ConnectedSetEventV4 decoded{};
    const uint8_t* payload = nullptr;
    if (!getChain(reader, decoded.chain)
        || !reader.span(kSetEventPayloadBytesV4, payload)
        || !decodeSetEventPayload(
               payload, kSetEventPayloadBytesV4, decoded.event)
        || reader.remaining() != 0u) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodeEndItem(
    const SpectatorPerGameEndV4& end, std::vector<uint8_t>& out) {
    if (!qreplay::isValidGameIdentityV2(end.game)
        || !qreplay::isValidConfirmedGameEndV2(end.end)) {
        return false;
    }
    putChain(out, end.chain);
    putIdentity(out, end.game);
    putU32(out, end.setNextRecordCursor);
    return qreplay::encodeConfirmedGameEndV2(end.end, out);
}

bool decodeEndItem(
    const uint8_t* bytes, size_t length, SpectatorPerGameEndV4& out) {
    const size_t expected = kChainStampBytesV4
        + qreplay::kGameIdentityBytesV2 + 4u
        + qreplay::kConfirmedGameEndBytesV2;
    if (length != expected) return false;
    Reader reader(bytes, length);
    SpectatorPerGameEndV4 decoded{};
    const uint8_t* endBytes = nullptr;
    if (!getChain(reader, decoded.chain)
        || !getIdentity(reader, decoded.game)
        || !reader.u32(decoded.setNextRecordCursor)
        || !reader.span(qreplay::kConfirmedGameEndBytesV2, endBytes)
        || qreplay::decodeConfirmedGameEndV2(
               endBytes, qreplay::kConfirmedGameEndBytesV2,
               decoded.end) != qreplay::ParseStatus::Ok
        || !qreplay::isValidGameIdentityV2(decoded.game)
        || reader.remaining() != 0u) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodeSealItem(
    const ConnectedSetSealV4& seal, std::vector<uint8_t>& out) {
    if (!isKnownConnectedSetTerminalReasonV4(
            static_cast<uint16_t>(seal.reason))) {
        return false;
    }
    putChain(out, seal.chain);
    putU16(out, static_cast<uint16_t>(seal.reason));
    putU16(out, 0u);
    putU32(out, seal.setNextRecordCursor);
    putU64(out, seal.postTailStateHash);
    return true;
}

bool decodeSealItem(
    const uint8_t* bytes, size_t length, ConnectedSetSealV4& out) {
    if (length != kChainStampBytesV4 + 16u) return false;
    Reader reader(bytes, length);
    ConnectedSetSealV4 decoded{};
    uint16_t reason = 0u;
    uint16_t reserved = 0u;
    if (!getChain(reader, decoded.chain)
        || !reader.u16(reason) || !reader.u16(reserved)
        || !reader.u32(decoded.setNextRecordCursor)
        || !reader.u64(decoded.postTailStateHash)
        || reader.remaining() != 0u || reserved != 0u
        || !isKnownConnectedSetTerminalReasonV4(reason)) {
        return false;
    }
    decoded.reason = static_cast<ConnectedSetTerminalReasonV4>(reason);
    out = decoded;
    return true;
}

bool encodePayload(
    const ConnectedPublicationV4& publication, std::vector<uint8_t>& out) {
    switch (publication.kind) {
        case ConnectedPublicationKindV4::BeginGameBundle:
            return encodeStartItem(publication.begin.start, out)
                && encodeRecordItem(publication.begin.frameZero, out);
        case ConnectedPublicationKindV4::ConfirmedRecord:
            return encodeRecordItem(publication.record, out);
        case ConnectedPublicationKindV4::SetEvent:
            return encodeSetEventItem(publication.setEvent, out);
        case ConnectedPublicationKindV4::Palette:
            return encodePaletteItem(publication.palette, out);
        case ConnectedPublicationKindV4::EndGameBundle:
            putU8(out, publication.end.containsFinalRecord ? 1u : 0u);
            putU8(out, 0u);
            putU16(out, 0u);
            if (publication.end.containsFinalRecord
                && !encodeRecordItem(publication.end.finalRecord, out)) {
                return false;
            }
            return encodeEndItem(publication.end.end, out);
        case ConnectedPublicationKindV4::SetSeal:
            return encodeSealItem(publication.seal, out);
    }
    return false;
}

bool decodePayload(
    ConnectedPublicationKindV4 kind, const uint8_t* bytes, size_t length,
    ConnectedPublicationV4& out) {
    ConnectedPublicationV4 decoded{};
    decoded.kind = kind;
    switch (kind) {
        case ConnectedPublicationKindV4::BeginGameBundle: {
            const size_t startBytes = kChainStampBytesV4
                + qreplay::kGameIdentityBytesV2
                + qreplay::kGameManifestBytesV2
                + qreplay::kDeterministicGameStartBytesV2;
            if (length <= startBytes
                || !decodeStartItem(
                       bytes, startBytes, decoded.begin.start)
                || !decodeRecordItem(
                       bytes + startBytes, length - startBytes,
                       decoded.begin.frameZero)) {
                return false;
            }
            break;
        }
        case ConnectedPublicationKindV4::ConfirmedRecord:
            if (!decodeRecordItem(bytes, length, decoded.record))
                return false;
            break;
        case ConnectedPublicationKindV4::SetEvent:
            if (!decodeSetEventItem(bytes, length, decoded.setEvent))
                return false;
            break;
        case ConnectedPublicationKindV4::Palette:
            if (!decodePaletteItem(bytes, length, decoded.palette))
                return false;
            break;
        case ConnectedPublicationKindV4::EndGameBundle: {
            Reader reader(bytes, length);
            uint8_t contains = 0u;
            uint8_t reserved8 = 0u;
            uint16_t reserved16 = 0u;
            if (!reader.u8(contains) || !reader.u8(reserved8)
                || !reader.u16(reserved16) || contains > 1u
                || reserved8 != 0u || reserved16 != 0u) {
                return false;
            }
            decoded.end.containsFinalRecord = contains != 0u;
            const size_t endBytes = kChainStampBytesV4
                + qreplay::kGameIdentityBytesV2 + 4u
                + qreplay::kConfirmedGameEndBytesV2;
            if (reader.remaining() < endBytes) return false;
            if (decoded.end.containsFinalRecord) {
                const size_t recordBytes = reader.remaining() - endBytes;
                const uint8_t* record = nullptr;
                if (!reader.span(recordBytes, record)
                    || !decodeRecordItem(
                           record, recordBytes,
                           decoded.end.finalRecord)) {
                    return false;
                }
            }
            const uint8_t* end = nullptr;
            if (!reader.span(endBytes, end)
                || !decodeEndItem(end, endBytes, decoded.end.end)
                || reader.remaining() != 0u) {
                return false;
            }
            break;
        }
        case ConnectedPublicationKindV4::SetSeal:
            if (!decodeSealItem(bytes, length, decoded.seal))
                return false;
            break;
    }
    out = decoded;
    return true;
}

void mixByte(uint64_t& hash, uint8_t value) {
    hash ^= value;
    hash *= 0x100000001B3ull;
}
void mixU16(uint64_t& hash, uint16_t value) {
    mixByte(hash, static_cast<uint8_t>(value));
    mixByte(hash, static_cast<uint8_t>(value >> 8));
}
void mixU32(uint64_t& hash, uint32_t value) {
    for (unsigned shift = 0u; shift != 32u; shift += 8u)
        mixByte(hash, static_cast<uint8_t>(value >> shift));
}
void mixU64(uint64_t& hash, uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        mixByte(hash, static_cast<uint8_t>(value >> shift));
}
uint64_t avalanche(uint64_t value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    value ^= value >> 31;
    return value;
}

bool hasValidConnectedDescriptorCore(
    const ConnectedSetDescriptorV4& descriptor) {
    return descriptor.setStreamId != 0u
        && isValidIdentity(descriptor.sourceIdentity)
        && descriptor.simulationContract != 0u
        && descriptor.initialDescriptorDigest != 0u;
}

uint64_t computeLegacyConnectedGenesisDigestV4(
    const ConnectedSetDescriptorV4& descriptor) {
    if (!hasValidConnectedDescriptorCore(descriptor)) return 0u;
    uint64_t hash = 0xCBF29CE484222325ull;
    mixU64(hash, 0x47454E2D51535034ull); // "GEN-QSP4"
    mixU16(hash, kSpectatorVersionV4);
    mixU64(hash, descriptor.setStreamId);
    mixU64(hash, descriptor.sourceIdentity.sessionId);
    mixU64(hash, descriptor.sourceIdentity.epoch);
    mixU64(hash, descriptor.simulationContract);
    mixU64(hash, descriptor.initialDescriptorDigest);
    return avalanche(hash);
}

void mixCompatibilityIdentity(
    uint64_t& hash, const compat::CompatibilityIdentityWire& identity) {
    mixU32(hash, identity.schemaVersion);
    mixU32(hash, identity.protocolVersion);
    for (char value : identity.runtimeBuild.profileId)
        mixByte(hash, static_cast<uint8_t>(value));
    for (uint8_t value : identity.runtimeBuild.executableSha256)
        mixByte(hash, value);
    mixU64(hash, identity.runtimeBuild.executableSize);
    mixU32(hash, identity.simulationContent.manifestSchemaVersion);
    mixU32(hash, identity.simulationContent.manifestFlags);
    for (uint8_t value : identity.simulationContent.simulationRootSha256)
        mixByte(hash, value);
    for (uint8_t value : identity.simulationContent.validationRootSha256)
        mixByte(hash, value);
    mixU32(hash, identity.simulationContent.entryCount);
    mixU32(hash, identity.simulationContent.requiredEntryCount);
    mixU64(hash, identity.stateSchemaId);
    mixU64(hash, identity.modSimulationId);
    mixU64(hash, identity.timingProfileId);
    mixU32(hash, identity.reserved);
}

template <typename T>
ConnectedChainStampV4& chainOf(T& item) {
    return item.chain;
}

bool encodeDigestProjection(
    ConnectedItemKindV4 kind, const SpectatorPerGameStartV4* start,
    const ConnectedConfirmedRecordV4* record,
    const ConnectedSetEventV4* event,
    const SpectatorPerGameEndV4* end,
    const ConnectedSetSealV4* seal,
    const ConnectedPaletteV4* palette, std::vector<uint8_t>& out) {
    out.clear();
    switch (kind) {
        case ConnectedItemKindV4::PerGameStart: {
            SpectatorPerGameStartV4 copy = *start;
            copy.chain.setDigestBefore = 0u;
            copy.chain.setDigestAfter = 0u;
            return encodeStartItem(copy, out);
        }
        case ConnectedItemKindV4::ConfirmedRecord: {
            ConnectedConfirmedRecordV4 copy = *record;
            copy.chain.setDigestBefore = 0u;
            copy.chain.setDigestAfter = 0u;
            return encodeRecordItem(copy, out);
        }
        case ConnectedItemKindV4::SetEvent: {
            ConnectedSetEventV4 copy = *event;
            copy.chain.setDigestBefore = 0u;
            copy.chain.setDigestAfter = 0u;
            return encodeSetEventItem(copy, out);
        }
        case ConnectedItemKindV4::Palette: {
            ConnectedPaletteV4 copy = *palette;
            copy.chain.setDigestBefore = 0u;
            copy.chain.setDigestAfter = 0u;
            return encodePaletteItem(copy, out);
        }
        case ConnectedItemKindV4::PerGameEnd: {
            SpectatorPerGameEndV4 copy = *end;
            copy.chain.setDigestBefore = 0u;
            copy.chain.setDigestAfter = 0u;
            return encodeEndItem(copy, out);
        }
        case ConnectedItemKindV4::SetSeal: {
            ConnectedSetSealV4 copy = *seal;
            copy.chain.setDigestBefore = 0u;
            copy.chain.setDigestAfter = 0u;
            return encodeSealItem(copy, out);
        }
    }
    return false;
}

uint64_t itemDigest(
    ConnectedItemKindV4 kind, uint64_t previous,
    const SpectatorPerGameStartV4* start,
    const ConnectedConfirmedRecordV4* record,
    const ConnectedSetEventV4* event,
    const SpectatorPerGameEndV4* end,
    const ConnectedSetSealV4* seal,
    const ConnectedPaletteV4* palette = nullptr) {
    std::vector<uint8_t> bytes;
    if (!encodeDigestProjection(
            kind, start, record, event, end, seal, palette, bytes)) {
        return 0u;
    }
    uint64_t hash = 0xCBF29CE484222325ull;
    mixU64(hash, 0x5139392D51535034ull); // "Q99-QSP4"
    mixU64(hash, previous);
    mixU16(hash, static_cast<uint16_t>(kind));
    for (uint8_t byte : bytes) mixByte(hash, byte);
    return avalanche(hash);
}

bool stampStart(
    SpectatorPerGameStartV4& start, uint64_t stream, uint32_t sequence,
    uint32_t cursor, uint64_t digest, uint64_t& after) {
    start.chain = ConnectedChainStampV4{
        stream, sequence, cursor, digest, 0u};
    after = itemDigest(
        ConnectedItemKindV4::PerGameStart, digest, &start, nullptr,
        nullptr, nullptr, nullptr);
    if (after == 0u) return false;
    start.chain.setDigestAfter = after;
    return true;
}

bool stampRecord(
    ConnectedConfirmedRecordV4& record, uint64_t stream,
    uint32_t sequence, uint32_t cursor, uint64_t digest,
    uint64_t& after) {
    record.chain = ConnectedChainStampV4{
        stream, sequence, cursor, digest, 0u};
    after = itemDigest(
        ConnectedItemKindV4::ConfirmedRecord, digest, nullptr, &record,
        nullptr, nullptr, nullptr);
    if (after == 0u) return false;
    record.chain.setDigestAfter = after;
    return true;
}

bool stampEvent(
    ConnectedSetEventV4& event, uint64_t stream, uint32_t sequence,
    uint32_t cursor, uint64_t digest, uint64_t& after) {
    event.chain = ConnectedChainStampV4{
        stream, sequence, cursor, digest, 0u};
    after = itemDigest(
        ConnectedItemKindV4::SetEvent, digest, nullptr, nullptr, &event,
        nullptr, nullptr);
    if (after == 0u) return false;
    event.chain.setDigestAfter = after;
    return true;
}

bool stampPalette(
    ConnectedPaletteV4& palette, uint64_t stream, uint32_t sequence,
    uint32_t cursor, uint64_t digest, uint64_t& after) {
    palette.chain = ConnectedChainStampV4{
        stream, sequence, cursor, digest, 0u};
    after = itemDigest(
        ConnectedItemKindV4::Palette, digest, nullptr, nullptr, nullptr,
        nullptr, nullptr, &palette);
    if (after == 0u) return false;
    palette.chain.setDigestAfter = after;
    return true;
}

bool stampEnd(
    SpectatorPerGameEndV4& end, uint64_t stream, uint32_t sequence,
    uint32_t cursor, uint64_t digest, uint64_t& after) {
    end.chain = ConnectedChainStampV4{
        stream, sequence, cursor, digest, 0u};
    after = itemDigest(
        ConnectedItemKindV4::PerGameEnd, digest, nullptr, nullptr, nullptr,
        &end, nullptr);
    if (after == 0u) return false;
    end.chain.setDigestAfter = after;
    return true;
}

bool stampSeal(
    ConnectedSetSealV4& seal, uint64_t stream, uint32_t sequence,
    uint32_t cursor, uint64_t digest, uint64_t& after) {
    seal.chain = ConnectedChainStampV4{
        stream, sequence, cursor, digest, 0u};
    after = itemDigest(
        ConnectedItemKindV4::SetSeal, digest, nullptr, nullptr, nullptr,
        nullptr, &seal);
    if (after == 0u) return false;
    seal.chain.setDigestAfter = after;
    return true;
}

bool containsGameResult(const ConnectedConfirmedRecordV4& record) {
    for (size_t i = 0; i < record.postEvents.count; ++i) {
        if (record.postEvents.events[i].kind
            == qreplay::GameEventKindV2::GameResult) {
            return true;
        }
    }
    return false;
}

}

bool isValidSetEventPayloadV4(const SetEventPayloadV4& event) {
    SetEvent legacy{};
    legacy.timelineFrame = event.sourceTimelineFrame;
    legacy.kind = event.kind;
    legacy.terminalReason = event.terminalReason;
    legacy.matchId = event.matchId;
    legacy.rosterEpoch = event.rosterEpoch;
    legacy.valueA = event.valueA;
    legacy.valueB = event.valueB;
    legacy.valueC = event.valueC;
    legacy.selectorProofMask = event.selectorProofMask;
    legacy.activeSlotMask = event.activeSlotMask;
    legacy.stateProof = event.stateProof;
    for (size_t i = 0; i < kSetPaletteEntryBytes; ++i)
        legacy.paletteEntries[i] = event.paletteEntries[i];
    for (size_t i = 0; i < kSetFighterSlots; ++i) {
        legacy.roster[i] = event.roster[i];
        legacy.selectorProof[i] = event.selectorProof[i];
        legacy.slotToSourceRoute[i] = event.slotToSourceRoute[i];
    }
    return isValidSetEvent(legacy);
}

bool isCanonicalConnectedRecordV4(
    const ConnectedConfirmedRecordV4& record) {
    if (record.hasReplayGameIdentity
            ? !qreplay::isValidGameIdentityV2(record.game)
            : !isZeroIdentity(record.game)
        || record.preEvents.count > qreplay::kMaxEventsPerTickV2
        || record.postEvents.count > qreplay::kMaxEventsPerTickV2
        || !qreplay::isValidConfirmedGameFrameCoreV2(
               record.frame, qreplay::kFighterMaskV2)) {
        return false;
    }
    for (size_t i = 0; i < record.preEvents.count; ++i) {
        const qreplay::ConfirmedGameEventV2& event =
            record.preEvents.events[i];
        if (!qreplay::isValidConfirmedGameEventV2(event)
            || event.phase != qreplay::GameEventPhaseV2::PreFrame
            || event.replayFrame != 0u || event.eventSequence != 0u
            || event.rollingDigest != 0u) {
            return false;
        }
    }
    for (size_t i = 0; i < record.postEvents.count; ++i) {
        const qreplay::ConfirmedGameEventV2& event =
            record.postEvents.events[i];
        if (!qreplay::isValidConfirmedGameEventV2(event)
            || event.phase != qreplay::GameEventPhaseV2::PostFrame
            || event.replayFrame != 0u || event.eventSequence != 0u
            || event.rollingDigest != 0u) {
            return false;
        }
    }
    return true;
}

bool encodeConnectedSetDescriptorV4(
    const ConnectedSetDescriptorV4& descriptor,
    std::vector<uint8_t>& out) {
    if (!descriptor.contentIdentityAttested
        || !compat::isValidCompatibilityIdentity(
               descriptor.compatibilityIdentity)
        || computeConnectedGenesisDigestV4(descriptor) == 0u) {
        return false;
    }
    std::vector<uint8_t> encoded;
    encoded.reserve(kConnectedDescriptorBytesV4);
    putU32(encoded, kConnectedDescriptorMagicV4);
    putU16(encoded, kConnectedDescriptorSchemaV4);
    putU16(encoded, 0u);
    putU64(encoded, descriptor.setStreamId);
    putU64(encoded, descriptor.sourceIdentity.sessionId);
    putU64(encoded, descriptor.sourceIdentity.epoch);
    putU64(encoded, descriptor.sourceIdentity.cookie.low);
    putU64(encoded, descriptor.sourceIdentity.cookie.high);
    putU64(encoded, descriptor.simulationContract);
    putU64(encoded, descriptor.initialDescriptorDigest);
    putCompatibilityIdentity(encoded, descriptor.compatibilityIdentity);
    putU32(
        encoded,
        qreplay::crc32c(encoded.data(), encoded.size()));
    if (encoded.size() != kConnectedDescriptorBytesV4)
        return false;
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ConnectedCodecErrorV4 decodeConnectedSetDescriptorV4(
    const uint8_t* bytes, size_t length,
    ConnectedSetDescriptorV4& out) {
    if (bytes == nullptr || length < kLegacyConnectedDescriptorBytesV4)
        return ConnectedCodecErrorV4::TooShort;
    if (length > kConnectedDescriptorBytesV4)
        return ConnectedCodecErrorV4::TrailingBytes;
    Reader reader(bytes, length);
    uint32_t magic = 0u;
    uint16_t schema = 0u;
    uint16_t reserved = 0u;
    uint32_t checksum = 0u;
    ConnectedSetDescriptorV4 decoded{};
    if (!reader.u32(magic) || !reader.u16(schema)
        || !reader.u16(reserved)) {
        return ConnectedCodecErrorV4::TooShort;
    }
    if (magic != kConnectedDescriptorMagicV4)
        return ConnectedCodecErrorV4::BadMagic;
    const size_t expectedLength =
        schema == kLegacyConnectedDescriptorSchemaV4
            ? kLegacyConnectedDescriptorBytesV4
            : schema == kConnectedDescriptorSchemaV4
                ? kConnectedDescriptorBytesV4 : 0u;
    if (expectedLength == 0u) return ConnectedCodecErrorV4::BadVersion;
    if (length < expectedLength) return ConnectedCodecErrorV4::TooShort;
    if (length > expectedLength) return ConnectedCodecErrorV4::TrailingBytes;
    if (!reader.u64(decoded.setStreamId)
        || !reader.u64(decoded.sourceIdentity.sessionId)
        || !reader.u64(decoded.sourceIdentity.epoch)
        || !reader.u64(decoded.sourceIdentity.cookie.low)
        || !reader.u64(decoded.sourceIdentity.cookie.high)
        || !reader.u64(decoded.simulationContract)
        || !reader.u64(decoded.initialDescriptorDigest)) {
        return ConnectedCodecErrorV4::TooShort;
    }
    if (schema == kConnectedDescriptorSchemaV4
        && !getCompatibilityIdentity(reader, decoded.compatibilityIdentity)) {
        return ConnectedCodecErrorV4::BadPayload;
    }
    if (!reader.u32(checksum)) return ConnectedCodecErrorV4::TooShort;
    if (reserved != 0u || reader.remaining() != 0u)
        return ConnectedCodecErrorV4::BadScalar;
    if (qreplay::crc32c(bytes, expectedLength - sizeof(uint32_t))
        != checksum) {
        return ConnectedCodecErrorV4::BadChecksum;
    }
    if (schema == kLegacyConnectedDescriptorSchemaV4) {
        decoded.contentIdentityAttested = false;
        if (computeLegacyConnectedGenesisDigestV4(decoded) == 0u)
            return ConnectedCodecErrorV4::BadPayload;
        out = decoded;
        return ConnectedCodecErrorV4::None;
    }
    decoded.contentIdentityAttested = true;
    if (computeConnectedGenesisDigestV4(decoded) == 0u)
        return ConnectedCodecErrorV4::BadPayload;
    std::vector<uint8_t> canonical;
    if (!encodeConnectedSetDescriptorV4(decoded, canonical)
        || canonical.size() != length
        || std::memcmp(canonical.data(), bytes, length) != 0) {
        return ConnectedCodecErrorV4::BadScalar;
    }
    out = decoded;
    return ConnectedCodecErrorV4::None;
}

bool encodeConnectedPublicationV4(
    const ConnectedPublicationV4& publication, std::vector<uint8_t>& out) {
    if (!isKnownConnectedPublicationKindV4(
            static_cast<uint16_t>(publication.kind))) {
        return false;
    }
    std::vector<uint8_t> payload;
    if (!encodePayload(publication, payload)
        || payload.empty()
        || payload.size()
            > kMaxConnectedPublicationBytesV4
                  - kPublicationHeaderBytesV4) {
        return false;
    }
    std::vector<uint8_t> encoded;
    encoded.reserve(kPublicationHeaderBytesV4 + payload.size());
    putU32(encoded, kConnectedPublicationMagicV4);
    putU16(encoded, kConnectedPublicationSchemaV4);
    putU16(encoded, static_cast<uint16_t>(publication.kind));
    putU32(encoded, static_cast<uint32_t>(payload.size()));
    putU32(encoded, qreplay::crc32c(payload.data(), payload.size()));
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    if (encoded.size() > kMaxConnectedPublicationBytesV4) return false;
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ConnectedCodecErrorV4 decodeConnectedPublicationV4(
    const uint8_t* bytes, size_t length, ConnectedPublicationV4& out) {
    if (bytes == nullptr || length < kPublicationHeaderBytesV4)
        return ConnectedCodecErrorV4::TooShort;
    if (length > kMaxConnectedPublicationBytesV4)
        return ConnectedCodecErrorV4::TooLarge;
    Reader reader(bytes, length);
    uint32_t magic = 0u;
    uint16_t version = 0u;
    uint16_t kindValue = 0u;
    uint32_t payloadBytes = 0u;
    uint32_t checksum = 0u;
    if (!reader.u32(magic) || !reader.u16(version)
        || !reader.u16(kindValue) || !reader.u32(payloadBytes)
        || !reader.u32(checksum)) {
        return ConnectedCodecErrorV4::TooShort;
    }
    if (magic != kConnectedPublicationMagicV4)
        return ConnectedCodecErrorV4::BadMagic;
    if (version != kConnectedPublicationSchemaV4)
        return ConnectedCodecErrorV4::BadVersion;
    if (!isKnownConnectedPublicationKindV4(kindValue))
        return ConnectedCodecErrorV4::BadKind;
    if (payloadBytes == 0u || payloadBytes != reader.remaining())
        return ConnectedCodecErrorV4::BadLength;
    const uint8_t* payload = nullptr;
    if (!reader.span(payloadBytes, payload))
        return ConnectedCodecErrorV4::BadLength;
    if (qreplay::crc32c(payload, payloadBytes) != checksum)
        return ConnectedCodecErrorV4::BadChecksum;
    ConnectedPublicationV4 decoded{};
    if (!decodePayload(
            static_cast<ConnectedPublicationKindV4>(kindValue),
            payload, payloadBytes, decoded)) {
        return ConnectedCodecErrorV4::BadPayload;
    }
    std::vector<uint8_t> canonical;
    if (!encodeConnectedPublicationV4(decoded, canonical)
        || canonical.size() != length
        || std::memcmp(canonical.data(), bytes, length) != 0) {
        return ConnectedCodecErrorV4::BadPayload;
    }
    out = decoded;
    return ConnectedCodecErrorV4::None;
}

bool encodeSpectatorResumeCursorV4(
    const SpectatorResumeCursorV4& cursor, std::vector<uint8_t>& out) {
    if (cursor.setStreamId == 0u || cursor.viewerGeneration == 0u)
        return false;
    putU64(out, cursor.setStreamId);
    putU32(out, cursor.nextItemSequence);
    putU32(out, cursor.nextRecordCursor);
    putU64(out, cursor.setDigestBeforeNext);
    putU32(out, cursor.viewerGeneration);
    putU32(out, 0u);
    return true;
}

ConnectedCodecErrorV4 decodeSpectatorResumeCursorV4(
    const uint8_t* bytes, size_t length, SpectatorResumeCursorV4& out) {
    if (bytes == nullptr || length < 32u)
        return ConnectedCodecErrorV4::TooShort;
    if (length != 32u) return ConnectedCodecErrorV4::BadLength;
    Reader reader(bytes, length);
    SpectatorResumeCursorV4 decoded{};
    uint32_t reserved = 0u;
    if (!reader.u64(decoded.setStreamId)
        || !reader.u32(decoded.nextItemSequence)
        || !reader.u32(decoded.nextRecordCursor)
        || !reader.u64(decoded.setDigestBeforeNext)
        || !reader.u32(decoded.viewerGeneration)
        || !reader.u32(reserved) || reserved != 0u
        || decoded.setStreamId == 0u
        || decoded.viewerGeneration == 0u) {
        return ConnectedCodecErrorV4::BadScalar;
    }
    out = decoded;
    return ConnectedCodecErrorV4::None;
}

bool encodeSpectatorAckV4(
    const SpectatorAckV4& ack, std::vector<uint8_t>& out) {
    if (ack.viewerId == 0u) return false;
    putU64(out, ack.viewerId);
    return encodeSpectatorResumeCursorV4(ack.committed, out);
}

ConnectedCodecErrorV4 decodeSpectatorAckV4(
    const uint8_t* bytes, size_t length, SpectatorAckV4& out) {
    if (bytes == nullptr || length < 40u)
        return ConnectedCodecErrorV4::TooShort;
    if (length != 40u) return ConnectedCodecErrorV4::BadLength;
    Reader reader(bytes, length);
    SpectatorAckV4 decoded{};
    const uint8_t* cursor = nullptr;
    if (!reader.u64(decoded.viewerId) || decoded.viewerId == 0u
        || !reader.span(32u, cursor)
        || decodeSpectatorResumeCursorV4(
               cursor, 32u, decoded.committed)
               != ConnectedCodecErrorV4::None) {
        return ConnectedCodecErrorV4::BadScalar;
    }
    out = decoded;
    return ConnectedCodecErrorV4::None;
}

bool encodeSpectatorSetSealAckV4(
    const SpectatorSetSealAckV4& ack, std::vector<uint8_t>& out) {
    if (ack.viewerId == 0u || ack.setStreamId == 0u
        || ack.viewerGeneration == 0u) {
        return false;
    }
    putU64(out, ack.viewerId);
    putU64(out, ack.setStreamId);
    putU32(out, ack.sealItemSequence);
    putU32(out, ack.setNextRecordCursor);
    putU64(out, ack.finalSetDigest);
    putU32(out, ack.viewerGeneration);
    putU32(out, 0u);
    return true;
}

ConnectedCodecErrorV4 decodeSpectatorSetSealAckV4(
    const uint8_t* bytes, size_t length, SpectatorSetSealAckV4& out) {
    if (bytes == nullptr || length < 40u)
        return ConnectedCodecErrorV4::TooShort;
    if (length != 40u) return ConnectedCodecErrorV4::BadLength;
    Reader reader(bytes, length);
    SpectatorSetSealAckV4 decoded{};
    uint32_t reserved = 0u;
    if (!reader.u64(decoded.viewerId)
        || !reader.u64(decoded.setStreamId)
        || !reader.u32(decoded.sealItemSequence)
        || !reader.u32(decoded.setNextRecordCursor)
        || !reader.u64(decoded.finalSetDigest)
        || !reader.u32(decoded.viewerGeneration)
        || !reader.u32(reserved) || reserved != 0u
        || decoded.viewerId == 0u || decoded.setStreamId == 0u
        || decoded.viewerGeneration == 0u) {
        return ConnectedCodecErrorV4::BadScalar;
    }
    out = decoded;
    return ConnectedCodecErrorV4::None;
}

bool fragmentConnectedPublicationV4(
    uint64_t setStreamId, uint32_t firstItemSequence,
    ConnectedPublicationKindV4 kind, const std::vector<uint8_t>& publication,
    std::vector<ConnectedPublicationFragmentV4>& out) {
    if (setStreamId == 0u
        || !isKnownConnectedPublicationKindV4(
               static_cast<uint16_t>(kind))
        || publication.empty()
        || publication.size() > kMaxConnectedPublicationBytesV4) {
        return false;
    }
    const size_t count =
        (publication.size() + kMaxConnectedFragmentBytesV4 - 1u)
        / kMaxConnectedFragmentBytesV4;
    if (count == 0u || count > kMaxConnectedFragmentsV4) return false;
    std::vector<ConnectedPublicationFragmentV4> fragments;
    fragments.reserve(count);
    const uint32_t checksum =
        qreplay::crc32c(publication.data(), publication.size());
    for (size_t index = 0; index < count; ++index) {
        const size_t offset = index * kMaxConnectedFragmentBytesV4;
        const size_t bytes = std::min(
            kMaxConnectedFragmentBytesV4, publication.size() - offset);
        ConnectedPublicationFragmentV4 fragment{};
        fragment.setStreamId = setStreamId;
        fragment.firstItemSequence = firstItemSequence;
        fragment.publicationKind = kind;
        fragment.fragmentIndex = static_cast<uint16_t>(index);
        fragment.fragmentCount = static_cast<uint16_t>(count);
        fragment.totalBytes = static_cast<uint32_t>(publication.size());
        fragment.byteOffset = static_cast<uint32_t>(offset);
        fragment.publicationCrc32c = checksum;
        fragment.bytes.assign(
            publication.begin() + static_cast<std::ptrdiff_t>(offset),
            publication.begin()
                + static_cast<std::ptrdiff_t>(offset + bytes));
        fragments.push_back(std::move(fragment));
    }
    out.swap(fragments);
    return true;
}

bool encodeConnectedFragmentV4(
    const ConnectedPublicationFragmentV4& fragment,
    std::vector<uint8_t>& out) {
    if (fragment.setStreamId == 0u
        || !isKnownConnectedPublicationKindV4(
               static_cast<uint16_t>(fragment.publicationKind))
        || fragment.fragmentCount == 0u
        || fragment.fragmentCount > kMaxConnectedFragmentsV4
        || fragment.fragmentIndex >= fragment.fragmentCount
        || fragment.totalBytes == 0u
        || fragment.totalBytes > kMaxConnectedPublicationBytesV4
        || fragment.bytes.empty()
        || fragment.bytes.size() > kMaxConnectedFragmentBytesV4
        || fragment.byteOffset
               != static_cast<uint32_t>(fragment.fragmentIndex)
                      * static_cast<uint32_t>(
                          kMaxConnectedFragmentBytesV4)
        || static_cast<uint64_t>(fragment.byteOffset)
                   + fragment.bytes.size()
               > fragment.totalBytes
        || (fragment.fragmentIndex + 1u < fragment.fragmentCount
            && fragment.bytes.size() != kMaxConnectedFragmentBytesV4)
        || (fragment.fragmentIndex + 1u == fragment.fragmentCount
            && static_cast<uint64_t>(fragment.byteOffset)
                       + fragment.bytes.size()
                   != fragment.totalBytes)) {
        return false;
    }
    putU32(out, kFragmentMagicV4);
    putU16(out, kFragmentSchemaV4);
    putU16(out, static_cast<uint16_t>(fragment.publicationKind));
    putU64(out, fragment.setStreamId);
    putU32(out, fragment.firstItemSequence);
    putU16(out, fragment.fragmentIndex);
    putU16(out, fragment.fragmentCount);
    putU32(out, fragment.totalBytes);
    putU32(out, fragment.byteOffset);
    putU32(out, fragment.publicationCrc32c);
    putU16(out, static_cast<uint16_t>(fragment.bytes.size()));
    putU16(out, 0u);
    out.insert(out.end(), fragment.bytes.begin(), fragment.bytes.end());
    return true;
}

ConnectedCodecErrorV4 decodeConnectedFragmentV4(
    const uint8_t* bytes, size_t length,
    ConnectedPublicationFragmentV4& out) {
    if (bytes == nullptr || length < kFragmentHeaderBytesV4)
        return ConnectedCodecErrorV4::TooShort;
    if (length > kFragmentHeaderBytesV4 + kMaxConnectedFragmentBytesV4)
        return ConnectedCodecErrorV4::TooLarge;
    Reader reader(bytes, length);
    uint32_t magic = 0u;
    uint16_t version = 0u;
    uint16_t kind = 0u;
    uint16_t payloadBytes = 0u;
    uint16_t reserved = 0u;
    ConnectedPublicationFragmentV4 decoded{};
    if (!reader.u32(magic) || !reader.u16(version)
        || !reader.u16(kind) || !reader.u64(decoded.setStreamId)
        || !reader.u32(decoded.firstItemSequence)
        || !reader.u16(decoded.fragmentIndex)
        || !reader.u16(decoded.fragmentCount)
        || !reader.u32(decoded.totalBytes)
        || !reader.u32(decoded.byteOffset)
        || !reader.u32(decoded.publicationCrc32c)
        || !reader.u16(payloadBytes) || !reader.u16(reserved)) {
        return ConnectedCodecErrorV4::TooShort;
    }
    if (magic != kFragmentMagicV4)
        return ConnectedCodecErrorV4::BadMagic;
    if (version != kFragmentSchemaV4)
        return ConnectedCodecErrorV4::BadVersion;
    if (!isKnownConnectedPublicationKindV4(kind))
        return ConnectedCodecErrorV4::BadKind;
    if (reserved != 0u || payloadBytes != reader.remaining())
        return ConnectedCodecErrorV4::BadLength;
    const uint8_t* payload = nullptr;
    if (!reader.span(payloadBytes, payload))
        return ConnectedCodecErrorV4::BadLength;
    decoded.publicationKind =
        static_cast<ConnectedPublicationKindV4>(kind);
    decoded.bytes.assign(payload, payload + payloadBytes);
    std::vector<uint8_t> canonical;
    if (!encodeConnectedFragmentV4(decoded, canonical)
        || canonical.size() != length
        || std::memcmp(canonical.data(), bytes, length) != 0) {
        return ConnectedCodecErrorV4::BadScalar;
    }
    out = std::move(decoded);
    return ConnectedCodecErrorV4::None;
}

uint64_t computeConnectedGenesisDigestV4(
    const ConnectedSetDescriptorV4& descriptor) {
    if (!hasValidConnectedDescriptorCore(descriptor)
        || !descriptor.contentIdentityAttested
        || !compat::isValidCompatibilityIdentity(
               descriptor.compatibilityIdentity)) {
        return 0u;
    }
    uint64_t hash = 0xCBF29CE484222325ull;
    mixU64(hash, 0x47454E2D51535034ull); // "GEN-QSP4"
    mixU16(hash, kSpectatorVersionV4);
    mixU16(hash, kConnectedDescriptorSchemaV4);
    mixU64(hash, descriptor.setStreamId);
    mixU64(hash, descriptor.sourceIdentity.sessionId);
    mixU64(hash, descriptor.sourceIdentity.epoch);
    mixU64(hash, descriptor.simulationContract);
    mixU64(hash, descriptor.initialDescriptorDigest);
    mixCompatibilityIdentity(hash, descriptor.compatibilityIdentity);
    return avalanche(hash);
}

bool connectedDescriptorMatchesCompatibilityIdentityV4(
    const ConnectedSetDescriptorV4& descriptor,
    const compat::CompatibilityIdentityWire& expectedIdentity) {
    return descriptor.contentIdentityAttested
        && compat::isValidCompatibilityIdentity(expectedIdentity)
        && compat::semanticallyEqual(
            descriptor.compatibilityIdentity, expectedIdentity)
        && computeConnectedGenesisDigestV4(descriptor) != 0u;
}

bool ConnectedChainBuilderV4::reset(
    const ConnectedSetDescriptorV4& descriptor) {
    const uint64_t genesis = computeConnectedGenesisDigestV4(descriptor);
    if (genesis == 0u) return false;
    setStreamId_ = descriptor.setStreamId;
    nextItemSequence_ = 0u;
    nextRecordCursor_ = 0u;
    digest_ = genesis;
    bound_ = true;
    sealed_ = false;
    gameOpen_ = false;
    openGame_ = qreplay::GameIdentityV2{};
    return true;
}

bool ConnectedChainBuilderV4::stamp(
    ConnectedPublicationV4& publication) {
    if (!bound_ || sealed_
        || !isKnownConnectedPublicationKindV4(
               static_cast<uint16_t>(publication.kind))) {
        return false;
    }

    ConnectedPublicationV4 staged = publication;
    uint32_t sequence = nextItemSequence_;
    uint32_t cursor = nextRecordCursor_;
    uint64_t digest = digest_;
    bool gameOpen = gameOpen_;
    qreplay::GameIdentityV2 openGame = openGame_;

    switch (staged.kind) {
        case ConnectedPublicationKindV4::BeginGameBundle:
            if (gameOpen
                || !qreplay::isValidGameIdentityV2(
                       staged.begin.start.game)
                || staged.begin.start.game
                       != staged.begin.start.start.manifest.identity
                || !staged.begin.frameZero.hasReplayGameIdentity
                || staged.begin.frameZero.game
                       != staged.begin.start.game
                || staged.begin.frameZero.frame.preStateHash
                       != staged.begin.start.start.start
                              .frameZeroPreStateHash) {
                return false;
            }
            if (!stampStart(
                    staged.begin.start, setStreamId_, sequence++, cursor,
                    digest, digest)
                || !stampRecord(
                    staged.begin.frameZero, setStreamId_, sequence++,
                    cursor, digest, digest)
                || cursor == (std::numeric_limits<uint32_t>::max)()) {
                return false;
            }
            ++cursor;
            gameOpen = true;
            openGame = staged.begin.start.game;
            break;

        case ConnectedPublicationKindV4::ConfirmedRecord:
            if ((gameOpen
                     ? (!staged.record.hasReplayGameIdentity
                        || staged.record.game != openGame)
                     : staged.record.hasReplayGameIdentity)
                || !stampRecord(
                    staged.record, setStreamId_, sequence++, cursor,
                    digest, digest)
                || cursor == (std::numeric_limits<uint32_t>::max)()) {
                return false;
            }
            ++cursor;
            break;

        case ConnectedPublicationKindV4::SetEvent:
            if (!stampEvent(
                    staged.setEvent, setStreamId_, sequence++, cursor,
                    digest, digest)) {
                return false;
            }
            break;

        case ConnectedPublicationKindV4::Palette:
            if (!stampPalette(
                    staged.palette, setStreamId_, sequence++, cursor,
                    digest, digest)) {
                return false;
            }
            break;

        case ConnectedPublicationKindV4::EndGameBundle:
            if (!gameOpen || staged.end.end.game != openGame
                || staged.end.end.setNextRecordCursor == 0u) {
                return false;
            }
            if (staged.end.containsFinalRecord) {
                if (!staged.end.finalRecord.hasReplayGameIdentity
                    || staged.end.finalRecord.game != openGame
                    || !stampRecord(
                        staged.end.finalRecord, setStreamId_, sequence++,
                        cursor, digest, digest)
                    || cursor
                        == (std::numeric_limits<uint32_t>::max)()) {
                    return false;
                }
                ++cursor;
            } else if (staged.end.end.end.reason
                       == qreplay::TerminalReason::Completed) {
                return false;
            }
            staged.end.end.setNextRecordCursor = cursor;
            if (!stampEnd(
                    staged.end.end, setStreamId_, sequence++, cursor,
                    digest, digest)) {
                return false;
            }
            gameOpen = false;
            openGame = qreplay::GameIdentityV2{};
            break;

        case ConnectedPublicationKindV4::SetSeal:
            if (gameOpen) return false;
            staged.seal.setNextRecordCursor = cursor;
            if (!stampSeal(
                    staged.seal, setStreamId_, sequence++, cursor,
                    digest, digest)) {
                return false;
            }
            sealed_ = true;
            break;
    }

    publication = staged;
    nextItemSequence_ = sequence;
    nextRecordCursor_ = cursor;
    digest_ = digest;
    gameOpen_ = gameOpen;
    openGame_ = openGame;
    return true;
}

bool rebaseConnectedRecordV4(
    const ConnectedConfirmedRecordV4& connected,
    uint32_t gameStartRecordCursor,
    uint32_t& nextEventSequence,
    uint64_t& rollingDigest,
    qreplay::CanonicalTickBundleV2& out) {
    if (!connected.hasReplayGameIdentity
        || connected.chain.recordCursorBefore < gameStartRecordCursor
        || !isCanonicalConnectedRecordV4(connected)) {
        return false;
    }
    const uint32_t replayFrame =
        connected.chain.recordCursorBefore - gameStartRecordCursor;
    qreplay::CanonicalTickBundleV2 tick{};
    tick.preEvents = connected.preEvents;
    tick.frame.sourceFrame = connected.sourceTimelineFrame;
    tick.frame.replayFrame = replayFrame;
    tick.frame.core = connected.frame;
    tick.postEvents = connected.postEvents;

    uint32_t stagedEvent = nextEventSequence;
    uint64_t stagedDigest = rollingDigest;
    auto prepareEvents = [&](qreplay::FixedEventListV2& list,
                             qreplay::GameEventPhaseV2 phase) {
        for (size_t i = 0; i < list.count; ++i) {
            qreplay::ConfirmedGameEventV2& event = list.events[i];
            if (stagedEvent
                == (std::numeric_limits<uint32_t>::max)()) {
                return false;
            }
            event.replayFrame = replayFrame;
            event.eventSequence = stagedEvent++;
            event.phase = phase;
            event.rollingDigest =
                qreplay::advanceReplayDigestV2(stagedDigest, event);
            if (qreplay::isCanonicalGameEventV2(event.kind))
                stagedDigest = event.rollingDigest;
            if (!qreplay::isValidConfirmedGameEventV2(event))
                return false;
        }
        return true;
    };
    if (!prepareEvents(
            tick.preEvents, qreplay::GameEventPhaseV2::PreFrame)) {
        return false;
    }
    tick.frame.rollingDigest =
        qreplay::advanceReplayDigestV2(stagedDigest, tick.frame);
    stagedDigest = tick.frame.rollingDigest;
    if (!prepareEvents(
            tick.postEvents, qreplay::GameEventPhaseV2::PostFrame)) {
        return false;
    }
    nextEventSequence = stagedEvent;
    rollingDigest = stagedDigest;
    out = tick;
    return true;
}

uint32_t firstItemSequenceV4(
    const ConnectedPublicationV4& publication) {
    switch (publication.kind) {
        case ConnectedPublicationKindV4::BeginGameBundle:
            return publication.begin.start.chain.itemSequence;
        case ConnectedPublicationKindV4::ConfirmedRecord:
            return publication.record.chain.itemSequence;
        case ConnectedPublicationKindV4::SetEvent:
            return publication.setEvent.chain.itemSequence;
        case ConnectedPublicationKindV4::Palette:
            return publication.palette.chain.itemSequence;
        case ConnectedPublicationKindV4::EndGameBundle:
            return publication.end.containsFinalRecord
                ? publication.end.finalRecord.chain.itemSequence
                : publication.end.end.chain.itemSequence;
        case ConnectedPublicationKindV4::SetSeal:
            return publication.seal.chain.itemSequence;
    }
    return 0u;
}

uint32_t nextItemSequenceV4(
    const ConnectedPublicationV4& publication) {
    switch (publication.kind) {
        case ConnectedPublicationKindV4::BeginGameBundle:
            return publication.begin.frameZero.chain.itemSequence + 1u;
        case ConnectedPublicationKindV4::ConfirmedRecord:
            return publication.record.chain.itemSequence + 1u;
        case ConnectedPublicationKindV4::SetEvent:
            return publication.setEvent.chain.itemSequence + 1u;
        case ConnectedPublicationKindV4::Palette:
            return publication.palette.chain.itemSequence + 1u;
        case ConnectedPublicationKindV4::EndGameBundle:
            return publication.end.end.chain.itemSequence + 1u;
        case ConnectedPublicationKindV4::SetSeal:
            return publication.seal.chain.itemSequence + 1u;
    }
    return 0u;
}

uint32_t firstRecordCursorV4(
    const ConnectedPublicationV4& publication) {
    switch (publication.kind) {
        case ConnectedPublicationKindV4::BeginGameBundle:
            return publication.begin.start.chain.recordCursorBefore;
        case ConnectedPublicationKindV4::ConfirmedRecord:
            return publication.record.chain.recordCursorBefore;
        case ConnectedPublicationKindV4::SetEvent:
            return publication.setEvent.chain.recordCursorBefore;
        case ConnectedPublicationKindV4::Palette:
            return publication.palette.chain.recordCursorBefore;
        case ConnectedPublicationKindV4::EndGameBundle:
            return publication.end.containsFinalRecord
                ? publication.end.finalRecord.chain.recordCursorBefore
                : publication.end.end.chain.recordCursorBefore;
        case ConnectedPublicationKindV4::SetSeal:
            return publication.seal.chain.recordCursorBefore;
    }
    return 0u;
}

uint32_t nextRecordCursorV4(
    const ConnectedPublicationV4& publication) {
    uint32_t cursor = firstRecordCursorV4(publication);
    switch (publication.kind) {
        case ConnectedPublicationKindV4::BeginGameBundle:
        case ConnectedPublicationKindV4::ConfirmedRecord:
            return cursor + 1u;
        case ConnectedPublicationKindV4::EndGameBundle:
            return cursor
                + (publication.end.containsFinalRecord ? 1u : 0u);
        case ConnectedPublicationKindV4::SetEvent:
        case ConnectedPublicationKindV4::SetSeal:
        case ConnectedPublicationKindV4::Palette:
            return cursor;
    }
    return cursor;
}

uint64_t firstDigestV4(const ConnectedPublicationV4& publication) {
    switch (publication.kind) {
        case ConnectedPublicationKindV4::BeginGameBundle:
            return publication.begin.start.chain.setDigestBefore;
        case ConnectedPublicationKindV4::ConfirmedRecord:
            return publication.record.chain.setDigestBefore;
        case ConnectedPublicationKindV4::SetEvent:
            return publication.setEvent.chain.setDigestBefore;
        case ConnectedPublicationKindV4::Palette:
            return publication.palette.chain.setDigestBefore;
        case ConnectedPublicationKindV4::EndGameBundle:
            return publication.end.containsFinalRecord
                ? publication.end.finalRecord.chain.setDigestBefore
                : publication.end.end.chain.setDigestBefore;
        case ConnectedPublicationKindV4::SetSeal:
            return publication.seal.chain.setDigestBefore;
    }
    return 0u;
}

uint64_t finalDigestV4(const ConnectedPublicationV4& publication) {
    switch (publication.kind) {
        case ConnectedPublicationKindV4::BeginGameBundle:
            return publication.begin.frameZero.chain.setDigestAfter;
        case ConnectedPublicationKindV4::ConfirmedRecord:
            return publication.record.chain.setDigestAfter;
        case ConnectedPublicationKindV4::SetEvent:
            return publication.setEvent.chain.setDigestAfter;
        case ConnectedPublicationKindV4::Palette:
            return publication.palette.chain.setDigestAfter;
        case ConnectedPublicationKindV4::EndGameBundle:
            return publication.end.end.chain.setDigestAfter;
        case ConnectedPublicationKindV4::SetSeal:
            return publication.seal.chain.setDigestAfter;
    }
    return 0u;
}

bool verifyConnectedPublicationChainV4(
    const ConnectedPublicationV4& publication, uint64_t setStreamId,
    uint32_t expectedItemSequence, uint32_t expectedRecordCursor,
    uint64_t expectedDigest, uint32_t& nextItemSequence,
    uint32_t& nextRecordCursor, uint64_t& nextDigest) {
    if (setStreamId == 0u
        || firstItemSequenceV4(publication) != expectedItemSequence
        || firstRecordCursorV4(publication) != expectedRecordCursor
        || firstDigestV4(publication) != expectedDigest) {
        return false;
    }
    ConnectedPublicationV4 expected = publication;
    uint32_t sequence = expectedItemSequence;
    uint32_t cursor = expectedRecordCursor;
    uint64_t digest = expectedDigest;
    switch (expected.kind) {
        case ConnectedPublicationKindV4::BeginGameBundle:
            if (!stampStart(
                    expected.begin.start, setStreamId, sequence++, cursor,
                    digest, digest)
                || !stampRecord(
                    expected.begin.frameZero, setStreamId, sequence++,
                    cursor, digest, digest)
                || cursor == (std::numeric_limits<uint32_t>::max)()) {
                return false;
            }
            ++cursor;
            break;
        case ConnectedPublicationKindV4::ConfirmedRecord:
            if (!stampRecord(
                    expected.record, setStreamId, sequence++, cursor,
                    digest, digest)
                || cursor == (std::numeric_limits<uint32_t>::max)()) {
                return false;
            }
            ++cursor;
            break;
        case ConnectedPublicationKindV4::SetEvent:
            if (!stampEvent(
                    expected.setEvent, setStreamId, sequence++, cursor,
                    digest, digest)) {
                return false;
            }
            break;
        case ConnectedPublicationKindV4::EndGameBundle:
            if (expected.end.containsFinalRecord) {
                if (!stampRecord(
                        expected.end.finalRecord, setStreamId, sequence++,
                        cursor, digest, digest)
                    || cursor
                        == (std::numeric_limits<uint32_t>::max)()) {
                    return false;
                }
                ++cursor;
            }
            if (!stampEnd(
                    expected.end.end, setStreamId, sequence++, cursor,
                    digest, digest)) {
                return false;
            }
            break;
        case ConnectedPublicationKindV4::SetSeal:
            if (!stampSeal(
                    expected.seal, setStreamId, sequence++, cursor,
                    digest, digest)) {
                return false;
            }
            break;
        case ConnectedPublicationKindV4::Palette:
            if (!stampPalette(
                    expected.palette, setStreamId, sequence++, cursor,
                    digest, digest)) {
                return false;
            }
            break;
    }
    if (!(expected == publication)) return false;
    nextItemSequence = sequence;
    nextRecordCursor = cursor;
    nextDigest = digest;
    return true;
}

}
