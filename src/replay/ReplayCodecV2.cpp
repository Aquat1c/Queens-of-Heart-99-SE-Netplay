#include "ReplayCodec.h"

#include "ReplayIntegrity.h"

#include <cstring>
#include <limits>

namespace qreplay {
namespace {

constexpr uint8_t kLaneMaskV2 = 0x0Fu;
constexpr uint8_t kKnownDisplayFlagsV2 =
    DisplayMetadataNetworkV2
    | DisplayMetadataSeriesV2
    | DisplayMetadataPreviousGameDigestV2;
constexpr int32_t kMaxTimezoneOffsetMinutesV2 = 1440;
constexpr uint32_t kMaxLossPermilleV2 = 1000u;
constexpr uint16_t kChunkSchemaVersionV2 = 2u;
constexpr size_t kMaxTimelineBundleBytesV2 = 512u * 1024u;

static_assert(kPrefixBytes == 48u, "v2 prefix reuses the 48-byte envelope");
static_assert(kChunkHeaderBytes == 48u,
              "v2 chunk header reuses the 48-byte envelope");
static_assert(kSimulationContractBytesV2 == 144u, "v2 contract layout");
static_assert(kGameIdentityBytesV2 == 20u, "v2 game identity layout");
static_assert(kReplayTextBytesV2 == 65u, "v2 bounded text layout");
static_assert(kSourceLaneDescriptorBytesV2 == 66u,
              "v2 source lane layout");
static_assert(kFighterSlotDescriptorBytesV2 == 8u,
              "v2 fighter slot layout");
static_assert(kStageIdentityBytesV2 == 16u, "v2 stage layout");
static_assert(kDisplayMetadataBytesV2 == 216u, "v2 display layout");
static_assert(kGameManifestBytesV2 == 716u, "v2 manifest layout");
static_assert(kDeterministicGameStartBytesV2 == 208u, "v2 start layout");
static_assert(kGameLifecycleBytesV2 == 48u, "v2 lifecycle layout");
static_assert(kConfirmedGameFrameCoreBytesV2 == 128u, "v2 frame core");
static_assert(kGameFrameRecordBytesV2 == 136u, "v2 file frame");
static_assert(kConfirmedGameFrameTransportBytesV2 == 144u,
              "v2 transport frame");
static_assert(kConfirmedGameEventBytesV2 == 64u, "v2 event");
static_assert(kConfirmedGameEndBytesV2 == 64u, "v2 terminal");
static_assert(kRoundIndexEntryBytesV2 == 40u, "v2 round index");
static_assert(kFooterFixedBytesV2 == 128u, "v2 footer");

uint32_t asUnsigned32V2(int32_t value) {
    return value >= 0
        ? static_cast<uint32_t>(value)
        : 0xFFFFFFFFu - static_cast<uint32_t>(-(value + 1));
}

int32_t asSigned32V2(uint32_t value) {
    return value <= 0x7FFFFFFFu
        ? static_cast<int32_t>(value)
        : -static_cast<int32_t>(0xFFFFFFFFu - value) - 1;
}

bool allZero(const uint8_t* bytes, size_t count) {
    if (bytes == nullptr && count != 0u) return false;
    for (size_t i = 0; i < count; ++i)
        if (bytes[i] != 0u) return false;
    return true;
}

bool validIdentityV2(const FighterIdentityV2& identity) {
    return identity.paletteIndex <= kMaxPaletteIndex
        && (identity.nativeCharacterId <= kMaxSelectableCharacterId
            || identity.nativeCharacterId == kInactiveCharacterId);
}

bool inactiveIdentityV2(const FighterIdentityV2& identity) {
    return identity.nativeCharacterId == kInactiveCharacterId
        && identity.paletteIndex == 0u;
}

bool validSelectorV2(const SelectorProofV2& proof) {
    return proof.cursorCell < kSelectorCells
        && proof.alternateFlag <= 1u && proof.confirmState <= 4u;
}

bool zeroSelectorV2(const SelectorProofV2& proof) {
    return proof.cursorCell == 0u && proof.alternateFlag == 0u
        && proof.confirmState == 0u;
}

bool validNativeDisplayNameV2(
    const std::array<char, kNativeDisplayNameBytesV2>& name) {
    size_t length = 0u;
    while (length < name.size() && name[length] != '\0') ++length;
    if (length >= name.size()
        || length > kNativeDisplayNamePayloadBytesV2) {
        return false;
    }
    for (size_t i = length; i < name.size(); ++i)
        if (name[i] != '\0') return false;
    for (size_t i = 0u; i < length; ++i) {
        const uint8_t byte = static_cast<uint8_t>(name[i]);
        if (byte < 0x20u || byte == 0x7Fu) return false;
    }
    return true;
}

void putReplayText(std::vector<uint8_t>& out, const ReplayTextV2& text) {
    putU8(out, text.bytes);
    putBytes(
        out, reinterpret_cast<const uint8_t*>(text.data.data()),
        text.data.size());
}

bool getReplayText(
    const uint8_t* data, size_t len, size_t& off, ReplayTextV2& out) {
    ReplayTextV2 decoded{};
    if (!getU8(data, len, off, decoded.bytes)
        || !getBytes(
            data, len, off,
            reinterpret_cast<uint8_t*>(decoded.data.data()),
            decoded.data.size())) {
        return false;
    }
    if (!isValidReplayTextV2(decoded)) return false;
    out = decoded;
    return true;
}

void putGameIdentity(
    std::vector<uint8_t>& out, const GameIdentityV2& identity) {
    putU64(out, identity.gameUuidLow);
    putU64(out, identity.gameUuidHigh);
    putU32(out, identity.gameGeneration);
}

bool getGameIdentity(
    const uint8_t* data, size_t len, size_t& off, GameIdentityV2& out) {
    GameIdentityV2 decoded{};
    if (!getU64(data, len, off, decoded.gameUuidLow)
        || !getU64(data, len, off, decoded.gameUuidHigh)
        || !getU32(data, len, off, decoded.gameGeneration)) {
        return false;
    }
    out = decoded;
    return true;
}

void putSimulationContractV2(
    std::vector<uint8_t>& out, const SimulationContractV2& contract) {
    putBytes(
        out, contract.executableSha256.data(),
        contract.executableSha256.size());
    putU64(out, contract.nativeBuildIdentity);
    putU64(out, contract.runtimeSimulationId);
    putU64(out, contract.stateRegionSchemaFingerprint);
    putU64(out, contract.stateHashProjectionId);
    putU64(out, contract.stateHashAlgorithmId);
    putBytes(
        out, contract.contentManifestRoot.data(),
        contract.contentManifestRoot.size());
    putU64(out, contract.bootstrapSchemaId);
    putU64(out, contract.eventSchemaId);
    putU64(out, contract.characterCatalogId);
    putU64(out, contract.terminalResultSchemaId);
    putU32(out, contract.compactActionDomainVersion);
    putU32(out, 0u);
}

ParseStatus getSimulationContractV2(
    const uint8_t* data, size_t len, size_t& off,
    SimulationContractV2& out) {
    SimulationContractV2 decoded{};
    uint32_t reserved = 0u;
    if (!getBytes(
            data, len, off, decoded.executableSha256.data(),
            decoded.executableSha256.size())
        || !getU64(data, len, off, decoded.nativeBuildIdentity)
        || !getU64(data, len, off, decoded.runtimeSimulationId)
        || !getU64(
            data, len, off, decoded.stateRegionSchemaFingerprint)
        || !getU64(data, len, off, decoded.stateHashProjectionId)
        || !getU64(data, len, off, decoded.stateHashAlgorithmId)
        || !getBytes(
            data, len, off, decoded.contentManifestRoot.data(),
            decoded.contentManifestRoot.size())
        || !getU64(data, len, off, decoded.bootstrapSchemaId)
        || !getU64(data, len, off, decoded.eventSchemaId)
        || !getU64(data, len, off, decoded.characterCatalogId)
        || !getU64(data, len, off, decoded.terminalResultSchemaId)
        || !getU32(data, len, off, decoded.compactActionDomainVersion)
        || !getU32(data, len, off, reserved)) {
        return ParseStatus::Truncated;
    }
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    out = decoded;
    return ParseStatus::Ok;
}

void putFighterSlotV2(
    std::vector<uint8_t>& out, const FighterSlotDescriptorV2& slot) {
    putU8(out, static_cast<uint8_t>(slot.ownership));
    putU8(out, slot.side);
    putU8(out, slot.identity.nativeCharacterId);
    putU8(out, slot.identity.paletteIndex);
    putU8(out, slot.selectorProofPresent ? 1u : 0u);
    putU8(out, slot.selectorProof.cursorCell);
    putU8(out, slot.selectorProof.alternateFlag);
    putU8(out, slot.selectorProof.confirmState);
}

ParseStatus getFighterSlotV2(
    const uint8_t* data, size_t len, size_t& off,
    FighterSlotDescriptorV2& out) {
    FighterSlotDescriptorV2 decoded{};
    uint8_t ownership = 0u;
    uint8_t proof = 0u;
    if (!getU8(data, len, off, ownership)
        || !getU8(data, len, off, decoded.side)
        || !getU8(data, len, off, decoded.identity.nativeCharacterId)
        || !getU8(data, len, off, decoded.identity.paletteIndex)
        || !getU8(data, len, off, proof)
        || !getU8(data, len, off, decoded.selectorProof.cursorCell)
        || !getU8(data, len, off, decoded.selectorProof.alternateFlag)
        || !getU8(data, len, off, decoded.selectorProof.confirmState)) {
        return ParseStatus::Truncated;
    }
    if (!isKnownSlotOwnership(ownership) || proof > 1u)
        return ParseStatus::BadLaneShape;
    decoded.ownership = static_cast<SlotOwnership>(ownership);
    decoded.selectorProofPresent = proof != 0u;
    if (decoded.side > 1u || !validIdentityV2(decoded.identity)
        || (decoded.selectorProofPresent
                ? !validSelectorV2(decoded.selectorProof)
                : !zeroSelectorV2(decoded.selectorProof))) {
        return ParseStatus::BadLaneShape;
    }
    out = decoded;
    return ParseStatus::Ok;
}

void putLifecycleV2(
    std::vector<uint8_t>& out, const GameLifecycleV2& lifecycle) {
    putU32(out, lifecycle.scene);
    putU32(out, lifecycle.requestedScene);
    putI32(out, lifecycle.matchSub);
    putU32(out, lifecycle.sceneThreeState);
    putU32(out, lifecycle.roundState);
    putU32(out, lifecycle.roundCounter);
    putU32(out, lifecycle.resultCounter);
    putI32(out, lifecycle.winnerSlot);
    putU8(out, lifecycle.roundTallies[0]);
    putU8(out, lifecycle.roundTallies[1]);
    putU16(out, 0u);
    putU32(out, lifecycle.routeEpoch);
    putU32(out, lifecycle.rosterEpoch);
    putU32(out, lifecycle.layoutGeneration);
}

ParseStatus getLifecycleV2(
    const uint8_t* data, size_t len, size_t& off, GameLifecycleV2& out) {
    GameLifecycleV2 decoded{};
    uint16_t reserved = 0u;
    if (!getU32(data, len, off, decoded.scene)
        || !getU32(data, len, off, decoded.requestedScene)
        || !getI32(data, len, off, decoded.matchSub)
        || !getU32(data, len, off, decoded.sceneThreeState)
        || !getU32(data, len, off, decoded.roundState)
        || !getU32(data, len, off, decoded.roundCounter)
        || !getU32(data, len, off, decoded.resultCounter)
        || !getI32(data, len, off, decoded.winnerSlot)
        || !getU8(data, len, off, decoded.roundTallies[0])
        || !getU8(data, len, off, decoded.roundTallies[1])
        || !getU16(data, len, off, reserved)
        || !getU32(data, len, off, decoded.routeEpoch)
        || !getU32(data, len, off, decoded.rosterEpoch)
        || !getU32(data, len, off, decoded.layoutGeneration)) {
        return ParseStatus::Truncated;
    }
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (decoded.winnerSlot < -1
        || decoded.winnerSlot >= static_cast<int32_t>(kFighterSlots)) {
        return ParseStatus::BadEventPayload;
    }
    out = decoded;
    return ParseStatus::Ok;
}

bool eventPayloadZerosExcept(
    const ConfirmedGameEventV2& e, bool round, bool delay, bool network) {
    if (!round
        && (e.transitionSiteId != 0u || e.roundOrdinal != 0u
            || e.winningSideMask != 0u || e.roundTallies[0] != 0u
            || e.roundTallies[1] != 0u || e.winnerSlot != -1)) {
        return false;
    }
    if (!delay
        && (e.playerOneDelay != 0u || e.playerTwoDelay != 0u
            || e.rollbackWindow != 0u)) {
        return false;
    }
    if (!network
        && (e.averageRttMicros != 0u || e.jitterMicros != 0u
            || e.lossPermille != 0u)) {
        return false;
    }
    return true;
}

void putEventV2(
    std::vector<uint8_t>& out, const ConfirmedGameEventV2& event) {
    putU32(out, event.replayFrame);
    putU32(out, event.eventSequence);
    putU16(out, static_cast<uint16_t>(event.kind));
    putU8(out, static_cast<uint8_t>(event.phase));
    putU8(out, 0u);
    putU32(out, event.transitionSiteId);
    putU8(out, event.roundOrdinal);
    putU8(out, event.winningSideMask);
    putU8(out, event.roundTallies[0]);
    putU8(out, event.roundTallies[1]);
    putI32(out, event.winnerSlot);
    putU16(out, event.playerOneDelay);
    putU16(out, event.playerTwoDelay);
    putU16(out, event.rollbackWindow);
    putU16(out, 0u);
    putU32(out, event.averageRttMicros);
    putU32(out, event.jitterMicros);
    putU32(out, event.lossPermille);
    putU64(out, event.stateProof);
    putU64(out, event.rollingDigest);
    putU32(out, 0u);
}

ParseStatus getEventV2(
    const uint8_t* data, size_t len, size_t& off,
    ConfirmedGameEventV2& out) {
    ConfirmedGameEventV2 decoded{};
    uint16_t kind = 0u;
    uint8_t phase = 0u;
    uint8_t reserved8 = 0u;
    uint16_t reserved16 = 0u;
    uint32_t reserved32 = 0u;
    if (!getU32(data, len, off, decoded.replayFrame)
        || !getU32(data, len, off, decoded.eventSequence)
        || !getU16(data, len, off, kind)
        || !getU8(data, len, off, phase)
        || !getU8(data, len, off, reserved8)
        || !getU32(data, len, off, decoded.transitionSiteId)
        || !getU8(data, len, off, decoded.roundOrdinal)
        || !getU8(data, len, off, decoded.winningSideMask)
        || !getU8(data, len, off, decoded.roundTallies[0])
        || !getU8(data, len, off, decoded.roundTallies[1])
        || !getI32(data, len, off, decoded.winnerSlot)
        || !getU16(data, len, off, decoded.playerOneDelay)
        || !getU16(data, len, off, decoded.playerTwoDelay)
        || !getU16(data, len, off, decoded.rollbackWindow)
        || !getU16(data, len, off, reserved16)
        || !getU32(data, len, off, decoded.averageRttMicros)
        || !getU32(data, len, off, decoded.jitterMicros)
        || !getU32(data, len, off, decoded.lossPermille)
        || !getU64(data, len, off, decoded.stateProof)
        || !getU64(data, len, off, decoded.rollingDigest)
        || !getU32(data, len, off, reserved32)) {
        return ParseStatus::Truncated;
    }
    if (reserved8 != 0u || reserved16 != 0u || reserved32 != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (!isKnownGameEventKindV2(kind)
        || !isKnownGameEventPhaseV2(phase)) {
        return ParseStatus::BadEventPayload;
    }
    decoded.kind = static_cast<GameEventKindV2>(kind);
    decoded.phase = static_cast<GameEventPhaseV2>(phase);
    if (!isValidConfirmedGameEventV2(decoded))
        return ParseStatus::BadEventPayload;
    out = decoded;
    return ParseStatus::Ok;
}

bool validRoundIndexV2(const RoundIndexEntryV2& round) {
    return round.roundOrdinal != 0u
        && (round.winningSideMask & ~kSideMaskV2) == 0u
        && round.firstRecord < round.nextRecord
        && round.nextRecord <= kMaxTimelineFrames
        && round.firstEventSequence <= round.nextEventSequence
        && round.firstChunkOffset < round.lastChunkOffset
        && round.lastChunkOffset <= kMaxFileBytes;
}

void putRoundIndexV2(
    std::vector<uint8_t>& out, const RoundIndexEntryV2& round) {
    putU8(out, round.roundOrdinal);
    putU8(out, round.winningSideMask);
    putU8(out, round.roundTallies[0]);
    putU8(out, round.roundTallies[1]);
    putU32(out, round.firstRecord);
    putU32(out, round.nextRecord);
    putU32(out, round.firstEventSequence);
    putU32(out, round.nextEventSequence);
    putU64(out, round.firstChunkOffset);
    putU64(out, round.lastChunkOffset);
    putU32(out, 0u);
}

ParseStatus getRoundIndexV2(
    const uint8_t* data, size_t len, size_t& off,
    RoundIndexEntryV2& out) {
    RoundIndexEntryV2 decoded{};
    uint32_t reserved = 0u;
    if (!getU8(data, len, off, decoded.roundOrdinal)
        || !getU8(data, len, off, decoded.winningSideMask)
        || !getU8(data, len, off, decoded.roundTallies[0])
        || !getU8(data, len, off, decoded.roundTallies[1])
        || !getU32(data, len, off, decoded.firstRecord)
        || !getU32(data, len, off, decoded.nextRecord)
        || !getU32(data, len, off, decoded.firstEventSequence)
        || !getU32(data, len, off, decoded.nextEventSequence)
        || !getU64(data, len, off, decoded.firstChunkOffset)
        || !getU64(data, len, off, decoded.lastChunkOffset)
        || !getU32(data, len, off, reserved)) {
        return ParseStatus::Truncated;
    }
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (!validRoundIndexV2(decoded))
        return ParseStatus::BadMatchIndex;
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeFooterIndexV2Impl(
    const FooterIndexV2& footer, bool zeroFileDigest,
    std::vector<uint8_t>& out) {
    if (footer.chunkCount == 0u
        || footer.chunkCount > kMaxChunksPerFile
        || footer.frameCount == 0u
        || footer.frameCount > kMaxTimelineFrames
        || footer.eventCount > kMaxTimelineEventsV2
        || allZero(
            footer.canonicalGameDigest.data(),
            footer.canonicalGameDigest.size())
        || (!zeroFileDigest
            && allZero(
                footer.fileDigest.data(), footer.fileDigest.size()))
        || footer.rounds.size() > kMaxEventsPerBlock
        || footer.manifestOffset < kPrefixBytes
        || footer.manifestOffset >= footer.startOffset
        || footer.startOffset >= footer.firstInputOffset
        || footer.firstInputOffset >= footer.gameEndOffset
        || footer.gameEndOffset > kMaxFileBytes) {
        return false;
    }
    for (size_t i = 0u; i < footer.rounds.size(); ++i) {
        if (!validRoundIndexV2(footer.rounds[i])) return false;
        if (i != 0u) {
            const RoundIndexEntryV2& prev = footer.rounds[i - 1u];
            const RoundIndexEntryV2& cur = footer.rounds[i];
            if (cur.roundOrdinal <= prev.roundOrdinal
                || cur.firstRecord < prev.nextRecord
                || cur.firstEventSequence < prev.nextEventSequence
                || cur.firstChunkOffset < prev.lastChunkOffset) {
                return false;
            }
        }
    }
    size_t roundBytes = 0u;
    size_t total = 0u;
    if (!mulChecked(
            footer.rounds.size(), kRoundIndexEntryBytesV2, roundBytes)
        || !addChecked(kFooterFixedBytesV2, roundBytes, total)
        || total > kMaxChunkPayloadBytes) {
        return false;
    }
    std::vector<uint8_t> encoded;
    encoded.reserve(total);
    putU32(encoded, footer.chunkCount);
    putU32(encoded, footer.frameCount);
    putU32(encoded, footer.eventCount);
    putU32(encoded, static_cast<uint32_t>(footer.rounds.size()));
    putU64(encoded, footer.manifestOffset);
    putU64(encoded, footer.startOffset);
    putU64(encoded, footer.firstInputOffset);
    putU64(encoded, footer.gameEndOffset);
    putU64(encoded, footer.finalRollingDigest);
    putBytes(
        encoded, footer.canonicalGameDigest.data(),
        footer.canonicalGameDigest.size());
    putU64(encoded, 0u);
    for (const RoundIndexEntryV2& round : footer.rounds)
        putRoundIndexV2(encoded, round);
    if (zeroFileDigest) {
        std::array<uint8_t, 32> zero{};
        putBytes(encoded, zero.data(), zero.size());
    } else {
        putBytes(
            encoded, footer.fileDigest.data(), footer.fileDigest.size());
    }
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

}

bool setReplayTextV2(ReplayTextV2& out, const std::string& text) {
    if (text.size() > kFixedReplayTextBytesV2
        || !isCanonicalReplayText(text)) {
        return false;
    }
    ReplayTextV2 encoded{};
    encoded.bytes = static_cast<uint8_t>(text.size());
    if (!text.empty())
        std::memcpy(encoded.data.data(), text.data(), text.size());
    out = encoded;
    return true;
}

std::string replayTextV2(const ReplayTextV2& text) {
    if (!isValidReplayTextV2(text)) return std::string();
    return std::string(text.data.data(), text.data.data() + text.bytes);
}

bool isValidReplayTextV2(const ReplayTextV2& text) {
    if (text.bytes > text.data.size()) return false;
    if (!isCanonicalReplayText(
            reinterpret_cast<const uint8_t*>(text.data.data()),
            text.bytes)) {
        return false;
    }
    for (size_t i = text.bytes; i < text.data.size(); ++i)
        if (text.data[i] != '\0') return false;
    return true;
}

bool isValidGameIdentityV2(const GameIdentityV2& identity) {
    return identity.gameGeneration != 0u
        && (identity.gameUuidLow != 0u || identity.gameUuidHigh != 0u);
}

bool isValidSimulationContractV2(
    const SimulationContractV2& contract) {
    return !allZero(
               contract.executableSha256.data(),
               contract.executableSha256.size())
        && contract.nativeBuildIdentity != 0u
        && contract.runtimeSimulationId != 0u
        && contract.stateRegionSchemaFingerprint != 0u
        && contract.stateHashProjectionId != 0u
        && contract.stateHashAlgorithmId != 0u
        && !allZero(
               contract.contentManifestRoot.data(),
               contract.contentManifestRoot.size())
        && contract.bootstrapSchemaId != 0u
        && contract.eventSchemaId != 0u
        && contract.characterCatalogId != 0u
        && contract.terminalResultSchemaId != 0u
        && contract.compactActionDomainVersion != 0u;
}

bool isValidGameManifestV2(const GameManifestV2& manifest) {
    if (!isValidGameIdentityV2(manifest.identity)
        || !isValidSimulationContractV2(manifest.contract)
        || !isKnownBootstrapKindV2(
            static_cast<uint8_t>(manifest.bootstrapKind))
        || !isKnownCpuContract(static_cast<uint8_t>(manifest.cpuContract))
        || manifest.vsRow == 0u || manifest.vsRow > 5u
        || manifest.activeLaneMask == 0u
        || manifest.activeFighterMask == 0u
        || (manifest.activeLaneMask & ~kLaneMaskV2) != 0u
        || (manifest.activeFighterMask & ~kFighterMaskV2) != 0u
        || (manifest.cpuMask & ~manifest.activeFighterMask) != 0u) {
        return false;
    }

    uint8_t observedCpuMask = 0u;
    for (size_t lane = 0u; lane < kSourceLanes; ++lane) {
        const bool active =
            (manifest.activeLaneMask & (1u << lane)) != 0u;
        if (manifest.lanes[lane].active != active
            || !isValidReplayTextV2(manifest.lanes[lane].name)
            || (!active && manifest.lanes[lane].name.bytes != 0u)) {
            return false;
        }
    }
    for (size_t slot = 0u; slot < kFighterSlots; ++slot) {
        const FighterSlotDescriptorV2& desc = manifest.slots[slot];
        const bool active =
            (manifest.activeFighterMask & (1u << slot)) != 0u;
        if (manifest.slotToSourceRoute[slot] >= kSourceLanes
            || desc.side != static_cast<uint8_t>(slot & 1u)
            || !isKnownSlotOwnership(static_cast<uint8_t>(desc.ownership))
            || !validIdentityV2(desc.identity)) {
            return false;
        }
        if (!active) {
            if (desc.ownership != SlotOwnership::Inactive
                || !inactiveIdentityV2(desc.identity)
                || desc.selectorProofPresent
                || !zeroSelectorV2(desc.selectorProof)) {
                return false;
            }
        } else {
            if (desc.ownership == SlotOwnership::Inactive
                || inactiveIdentityV2(desc.identity)
                || (desc.selectorProofPresent
                        ? !validSelectorV2(desc.selectorProof)
                        : !zeroSelectorV2(desc.selectorProof))) {
                return false;
            }
            if (desc.ownership == SlotOwnership::RecordedHuman
                && (manifest.activeLaneMask
                    & (1u << manifest.slotToSourceRoute[slot])) == 0u) {
                return false;
            }
            if (desc.ownership == SlotOwnership::NativeCpu)
                observedCpuMask |= static_cast<uint8_t>(1u << slot);
        }
    }
    if (observedCpuMask != manifest.cpuMask) return false;
    if ((manifest.cpuMask == 0u)
        != (manifest.cpuContract == CpuContract::None)) {
        return false;
    }
    if (!manifest.stage.verified
        || manifest.stage.nativeStageId < kMinNativeStageIdV2
        || manifest.stage.nativeStageId > kMaxNativeStageIdV2
        || manifest.stage.catalogEntryId == 0u) {
        return false;
    }

    const DisplayMetadataV2& display = manifest.display;
    if (!isKnownReplaySourceKindV2(
            static_cast<uint8_t>(display.sourceKind))
        || (display.flags & ~kKnownDisplayFlagsV2) != 0u
        || display.recordedUnixSeconds < 0
        || display.timezoneOffsetMinutes < -kMaxTimezoneOffsetMinutesV2
        || display.timezoneOffsetMinutes > kMaxTimezoneOffsetMinutesV2
        || display.lossPermille > kMaxLossPermilleV2
        || !isValidReplayTextV2(display.title)
        || !isValidReplayTextV2(display.routeLabel)) {
        return false;
    }
    const bool network =
        (display.flags & DisplayMetadataNetworkV2) != 0u;
    if (!network
        && (display.playerOneDelay != 0u
            || display.playerTwoDelay != 0u
            || display.rollbackWindow != 0u
            || display.averageRttMicros != 0u
            || display.jitterMicros != 0u
            || display.lossPermille != 0u
            || display.routeLabel.bytes != 0u)) {
        return false;
    }
    const bool series = (display.flags & DisplayMetadataSeriesV2) != 0u;
    const bool previous =
        (display.flags & DisplayMetadataPreviousGameDigestV2) != 0u;
    if (!series
        && (display.seriesUuidLow != 0u || display.seriesUuidHigh != 0u
            || display.gameOrdinalInSeries != 0u || previous)) {
        return false;
    }
    if (series
        && display.seriesUuidLow == 0u && display.seriesUuidHigh == 0u) {
        return false;
    }
    if (!previous
        && !allZero(
            display.previousGameDigest.data(),
            display.previousGameDigest.size())) {
        return false;
    }
    return true;
}

bool isValidDeterministicGameStartV2(
    const DeterministicGameStartV2& start) {
    if (start.fpu.x87ControlWord == 0u || start.fpu.mxcsr == 0u)
        return false;
    for (const auto& name : start.nativeDisplay.names)
        if (!validNativeDisplayNameV2(name)) return false;
    return true;
}

bool isValidConfirmedGameFrameCoreV2(
    const ConfirmedGameFrameCoreV2& frame, uint8_t allowedLaneMask) {
    if (!isKnownInputDomain(static_cast<uint8_t>(frame.inputDomain))
        || (allowedLaneMask & ~kLaneMaskV2) != 0u
        || frame.activeLaneMask == 0u
        || (frame.activeLaneMask & ~allowedLaneMask) != 0u) {
        return false;
    }
    for (size_t lane = 0u; lane < kSourceLanes; ++lane) {
        const bool active =
            (frame.activeLaneMask & (1u << lane)) != 0u;
        if (active) {
            if (!isValidActionMask(frame.actions[lane])) return false;
        } else if (frame.actions[lane] != 0u) {
            return false;
        }
    }
    return frame.before.winnerSlot >= -1
        && frame.before.winnerSlot < static_cast<int32_t>(kFighterSlots)
        && frame.after.winnerSlot >= -1
        && frame.after.winnerSlot < static_cast<int32_t>(kFighterSlots);
}

bool isValidConfirmedGameEventV2(const ConfirmedGameEventV2& event) {
    if (!isKnownGameEventKindV2(static_cast<uint16_t>(event.kind))
        || !isKnownGameEventPhaseV2(static_cast<uint8_t>(event.phase))
        || event.replayFrame >= kMaxTimelineFrames
        || (event.winningSideMask & ~kFighterMaskV2) != 0u
        || event.winnerSlot < -1
        || event.winnerSlot >= static_cast<int32_t>(kFighterSlots)
        || event.lossPermille > kMaxLossPermilleV2) {
        return false;
    }
    switch (event.kind) {
        case GameEventKindV2::GameStarted:
            return event.phase == GameEventPhaseV2::PreFrame
                && event.transitionSiteId == 0u
                && event.roundOrdinal == 0u
                && eventPayloadZerosExcept(event, true, false, false);
        case GameEventKindV2::RoundStarted:
            return event.phase == GameEventPhaseV2::PreFrame
                && event.transitionSiteId != 0u
                && event.roundOrdinal != 0u
                && event.winningSideMask == 0u
                && event.roundTallies[0] == 0u
                && event.roundTallies[1] == 0u && event.winnerSlot == -1
                && eventPayloadZerosExcept(event, true, false, false);
        case GameEventKindV2::RoundEnded:
        case GameEventKindV2::GameResult:
            return event.phase == GameEventPhaseV2::PostFrame
                && event.transitionSiteId != 0u
                && event.roundOrdinal != 0u
                && (event.winningSideMask & ~kSideMaskV2) == 0u
                && ((event.winningSideMask == 0u
                        && event.winnerSlot == -1)
                    || (event.winningSideMask != 0u
                        && event.winnerSlot >= 0
                        && (event.winningSideMask
                            & (1u << static_cast<uint32_t>(
                                event.winnerSlot & 1))) != 0u))
                && eventPayloadZerosExcept(event, true, false, false);
        case GameEventKindV2::DelayChanged:
            return event.phase == GameEventPhaseV2::PostFrame
                && eventPayloadZerosExcept(event, false, true, false);
        case GameEventKindV2::NetworkSample:
            return event.phase == GameEventPhaseV2::PostFrame
                && eventPayloadZerosExcept(event, false, false, true);
    }
    return false;
}

bool isValidConfirmedGameEndV2(const ConfirmedGameEndV2& end) {
    if (!isKnownTerminalReason(static_cast<uint16_t>(end.reason))
        || end.nextRecord == 0u || end.nextRecord > kMaxTimelineFrames
        || (end.winningSideMask & ~kSideMaskV2) != 0u
        || end.winnerSlot < -1
        || end.winnerSlot >= static_cast<int32_t>(kFighterSlots)) {
        return false;
    }
    if (end.winningSideMask == 0u && end.winnerSlot != -1) return false;
    if (end.winningSideMask != 0u && end.winnerSlot < 0) return false;
    if (end.winnerSlot >= 0
        && (end.winningSideMask
            & (1u << static_cast<uint32_t>(
                end.winnerSlot & 1))) == 0u) {
        return false;
    }
    return true;
}

void encodePrefixV2(
    const ReplayPrefixV2& prefix, std::vector<uint8_t>& out) {
    std::vector<uint8_t> encoded;
    encoded.reserve(kPrefixBytes);
    putU32(encoded, kReplayMagic);
    putU16(encoded, kContainerVersionMajorV2);
    putU16(encoded, kContainerVersionMinorV2);
    putU16(encoded, kContainerReaderMajorV2);
    putU16(encoded, static_cast<uint16_t>(kPrefixBytes));
    putU32(encoded, kPrefixEndianMarker);
    putU32(encoded, prefix.featureFlags);
    putU64(encoded, prefix.gameUuidLow);
    putU64(encoded, prefix.gameUuidHigh);
    putU64(encoded, 0u);
    putU32(encoded, 0u);
    const uint32_t crc = crc32c(encoded.data(), encoded.size());
    for (unsigned i = 0u; i < 4u; ++i)
        encoded[kPrefixBytes - 4u + i] =
            static_cast<uint8_t>(crc >> (i * 8u));
    out.insert(out.end(), encoded.begin(), encoded.end());
}

ParseStatus decodePrefixV2(
    const uint8_t* data, size_t len, ReplayPrefixV2& out) {
    if (data == nullptr || len < kPrefixBytes) return ParseStatus::Truncated;
    uint8_t scratch[kPrefixBytes];
    std::memcpy(scratch, data, kPrefixBytes);
    size_t crcOff = kPrefixBytes - 4u;
    uint32_t storedCrc = 0u;
    if (!getU32(data, len, crcOff, storedCrc))
        return ParseStatus::Truncated;
    for (unsigned i = 0u; i < 4u; ++i)
        scratch[kPrefixBytes - 4u + i] = 0u;
    if (crc32c(scratch, sizeof(scratch)) != storedCrc)
        return ParseStatus::BadPrefixCrc;

    size_t off = 0u;
    uint32_t magic = 0u;
    ReplayPrefixV2 decoded{};
    uint16_t prefixBytes = 0u;
    uint32_t endian = 0u;
    uint64_t reserved = 0u;
    if (!getU32(data, len, off, magic)
        || !getU16(data, len, off, decoded.containerMajor)
        || !getU16(data, len, off, decoded.containerMinor)
        || !getU16(data, len, off, decoded.minimumReaderMajor)
        || !getU16(data, len, off, prefixBytes)
        || !getU32(data, len, off, endian)
        || !getU32(data, len, off, decoded.featureFlags)
        || !getU64(data, len, off, decoded.gameUuidLow)
        || !getU64(data, len, off, decoded.gameUuidHigh)
        || !getU64(data, len, off, reserved)) {
        return ParseStatus::Truncated;
    }
    if (magic != kReplayMagic) return ParseStatus::BadMagic;
    if (decoded.containerMajor != kContainerVersionMajorV2
        || decoded.containerMinor != kContainerVersionMinorV2
        || decoded.minimumReaderMajor != kContainerReaderMajorV2
        || prefixBytes != kPrefixBytes || endian != kPrefixEndianMarker
        || reserved != 0u) {
        return ParseStatus::UnsupportedContainerVersion;
    }
    if ((decoded.featureFlags & ~kFeatureRequiredMaskV2) != 0u)
        return ParseStatus::UnsupportedFeature;
    if (decoded.gameUuidLow == 0u && decoded.gameUuidHigh == 0u)
        return ParseStatus::BadMatchIndex;
    decoded.prefixBytes = prefixBytes;
    out = decoded;
    return ParseStatus::Ok;
}

void encodeChunkHeaderV2(
    const ChunkHeaderV2& header, std::vector<uint8_t>& out) {
    std::vector<uint8_t> encoded;
    encoded.reserve(kChunkHeaderBytes);
    putU32(encoded, kChunkSyncWord);
    putU16(encoded, static_cast<uint16_t>(header.kind));
    putU16(encoded, header.schemaVersion);
    putU8(encoded, header.critical);
    putU8(encoded, static_cast<uint8_t>(header.codec));
    putU16(encoded, 0u);
    putU32(encoded, header.sequence);
    putU32(encoded, header.gameGeneration);
    putU32(encoded, header.firstRecord);
    putU32(encoded, header.nextRecord);
    putU32(encoded, header.recordCount);
    putU32(encoded, header.payloadBytes);
    putU32(encoded, header.decodedBytes);
    putU32(encoded, header.payloadCrc32c);
    putU32(encoded, 0u);
    const uint32_t crc = crc32c(encoded.data(), encoded.size());
    for (unsigned i = 0u; i < 4u; ++i)
        encoded[kChunkHeaderBytes - 4u + i] =
            static_cast<uint8_t>(crc >> (i * 8u));
    out.insert(out.end(), encoded.begin(), encoded.end());
}

ParseStatus decodeChunkHeaderV2(
    const uint8_t* data, size_t len, ChunkHeaderV2& out) {
    if (data == nullptr || len < kChunkHeaderBytes)
        return ParseStatus::Truncated;
    uint8_t scratch[kChunkHeaderBytes];
    std::memcpy(scratch, data, kChunkHeaderBytes);
    size_t crcOff = kChunkHeaderBytes - 4u;
    uint32_t storedCrc = 0u;
    if (!getU32(data, len, crcOff, storedCrc))
        return ParseStatus::Truncated;
    for (unsigned i = 0u; i < 4u; ++i)
        scratch[kChunkHeaderBytes - 4u + i] = 0u;
    if (crc32c(scratch, sizeof(scratch)) != storedCrc)
        return ParseStatus::BadChunkHeaderCrc;

    size_t off = 0u;
    uint32_t sync = 0u;
    uint16_t kind = 0u;
    uint8_t codec = 0u;
    uint16_t reserved = 0u;
    ChunkHeaderV2 decoded{};
    if (!getU32(data, len, off, sync)
        || !getU16(data, len, off, kind)
        || !getU16(data, len, off, decoded.schemaVersion)
        || !getU8(data, len, off, decoded.critical)
        || !getU8(data, len, off, codec)
        || !getU16(data, len, off, reserved)
        || !getU32(data, len, off, decoded.sequence)
        || !getU32(data, len, off, decoded.gameGeneration)
        || !getU32(data, len, off, decoded.firstRecord)
        || !getU32(data, len, off, decoded.nextRecord)
        || !getU32(data, len, off, decoded.recordCount)
        || !getU32(data, len, off, decoded.payloadBytes)
        || !getU32(data, len, off, decoded.decodedBytes)
        || !getU32(data, len, off, decoded.payloadCrc32c)) {
        return ParseStatus::Truncated;
    }
    decoded.headerCrc32c = storedCrc;
    if (sync != kChunkSyncWord) return ParseStatus::BadSyncWord;
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (!isKnownChunkKindV2(kind)
        || decoded.schemaVersion != kChunkSchemaVersionV2) {
        return ParseStatus::UnknownCriticalChunk;
    }
    decoded.kind = static_cast<ChunkKindV2>(kind);
    if (decoded.critical > 1u
        || (decoded.critical != 0u)
            != isCriticalChunkKindV2(decoded.kind)) {
        return ParseStatus::UnknownCriticalChunk;
    }
    if (!isKnownChunkCodec(codec)) return ParseStatus::UnknownChunkCodec;
    decoded.codec = static_cast<ChunkCodec>(codec);
    if (decoded.sequence >= kMaxChunksPerFile
        || decoded.gameGeneration == 0u
        || decoded.firstRecord > decoded.nextRecord
        || decoded.nextRecord > kMaxTimelineFrames
        || decoded.payloadBytes > kMaxChunkPayloadBytes
        || decoded.decodedBytes > kMaxCheckpointDecodedBytes
        || (decoded.codec == ChunkCodec::Raw
            && decoded.payloadBytes != decoded.decodedBytes)) {
        return ParseStatus::LimitExceeded;
    }
    if (decoded.kind == ChunkKindV2::InputBlock) {
        if (decoded.recordCount == 0u
            || decoded.recordCount > kMaxRecordsPerInputBlock
            || decoded.nextRecord - decoded.firstRecord
                != decoded.recordCount) {
            return ParseStatus::FrameGap;
        }
    } else if (decoded.kind == ChunkKindV2::EventBlock
               && (decoded.recordCount == 0u
                   || decoded.recordCount > kMaxEventsPerBlock)) {
        return ParseStatus::LimitExceeded;
    }
    out = decoded;
    return ParseStatus::Ok;
}

bool encodePaletteBlockV2(
    const ReplayPaletteV2& palette, std::vector<uint8_t>& out) {
    if (palette.side > 3u || palette.variant > 3u || palette.present > 1u
        || palette.reserved != 0u) {
        return false;
    }
    std::vector<uint8_t> encoded;
    encoded.reserve(kReplayPaletteBlockBytes);
    putU8(encoded, palette.side);
    putU8(encoded, palette.variant);
    putU8(encoded, palette.present);
    putU8(encoded, 0u);
    encoded.insert(encoded.end(), palette.table,
                   palette.table + kReplayPaletteTableBytes);
    out.swap(encoded);
    return true;
}

ParseStatus decodePaletteBlockV2(
    const uint8_t* data, size_t len, ReplayPaletteV2& out) {
    if (data == nullptr || len != kReplayPaletteBlockBytes)
        return ParseStatus::Truncated;
    ReplayPaletteV2 decoded{};
    decoded.side = data[0];
    decoded.variant = data[1];
    decoded.present = data[2];
    decoded.reserved = data[3];
    if (decoded.side > 3u || decoded.variant > 3u || decoded.present > 1u
        || decoded.reserved != 0u) {
        return ParseStatus::BadPalettePayload;
    }
    std::memcpy(decoded.table, data + 4, kReplayPaletteTableBytes);
    // Entry 0 of every variant is the game's transparent key and must be black.
    if (decoded.present != 0u) {
        for (size_t variant = 0; variant < 4u; ++variant) {
            const uint8_t* key = decoded.table + variant * 64u;
            if (key[0] != 0u || key[1] != 0u || key[2] != 0u)
                return ParseStatus::BadPalettePayload;
        }
    }
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeGameManifestV2(
    const GameManifestV2& manifest, std::vector<uint8_t>& out) {
    if (!isValidGameManifestV2(manifest)) return false;
    std::vector<uint8_t> encoded;
    encoded.reserve(kGameManifestBytesV2);
    putGameIdentity(encoded, manifest.identity);
    putSimulationContractV2(encoded, manifest.contract);
    putU8(encoded, static_cast<uint8_t>(manifest.bootstrapKind));
    putU8(encoded, manifest.vsRow);
    putU8(encoded, manifest.activeLaneMask);
    putU8(encoded, manifest.activeFighterMask);
    putU8(encoded, manifest.cpuMask);
    putU8(encoded, static_cast<uint8_t>(manifest.cpuContract));
    putU16(encoded, 0u);
    for (uint8_t route : manifest.slotToSourceRoute) putU8(encoded, route);
    for (const SourceLaneDescriptorV2& lane : manifest.lanes) {
        putU8(encoded, lane.active ? 1u : 0u);
        putReplayText(encoded, lane.name);
    }
    for (const FighterSlotDescriptorV2& slot : manifest.slots)
        putFighterSlotV2(encoded, slot);
    putU32(encoded, manifest.routeEpoch);
    putU32(encoded, manifest.rosterEpoch);
    putU32(encoded, manifest.layoutGeneration);
    putU32(encoded, manifest.stage.nativeStageId);
    putU8(encoded, manifest.stage.verified ? 1u : 0u);
    putU8(encoded, 0u);
    putU8(encoded, 0u);
    putU8(encoded, 0u);
    putU64(encoded, manifest.stage.catalogEntryId);
    const DisplayMetadataV2& display = manifest.display;
    putI64(encoded, display.recordedUnixSeconds);
    putI32(encoded, display.timezoneOffsetMinutes);
    putU8(encoded, static_cast<uint8_t>(display.sourceKind));
    putU8(encoded, display.flags);
    putU16(encoded, display.playerOneDelay);
    putU16(encoded, display.playerTwoDelay);
    putU16(encoded, display.rollbackWindow);
    putU32(encoded, display.averageRttMicros);
    putU32(encoded, display.jitterMicros);
    putU32(encoded, display.lossPermille);
    putReplayText(encoded, display.title);
    putReplayText(encoded, display.routeLabel);
    putU64(encoded, display.seriesUuidLow);
    putU64(encoded, display.seriesUuidHigh);
    putU32(encoded, display.gameOrdinalInSeries);
    putBytes(
        encoded, display.previousGameDigest.data(),
        display.previousGameDigest.size());
    putU16(encoded, 0u);
    if (encoded.size() != kGameManifestBytesV2) return false;
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeGameManifestV2(
    const uint8_t* data, size_t len, GameManifestV2& out) {
    if (data == nullptr || len < kGameManifestBytesV2)
        return ParseStatus::Truncated;
    if (len > kGameManifestBytesV2) return ParseStatus::LimitExceeded;
    GameManifestV2 decoded{};
    size_t off = 0u;
    if (!getGameIdentity(data, len, off, decoded.identity))
        return ParseStatus::Truncated;
    ParseStatus status =
        getSimulationContractV2(data, len, off, decoded.contract);
    if (status != ParseStatus::Ok) return status;
    uint8_t bootstrap = 0u;
    uint8_t cpuContract = 0u;
    uint16_t reserved16 = 0u;
    if (!getU8(data, len, off, bootstrap)
        || !getU8(data, len, off, decoded.vsRow)
        || !getU8(data, len, off, decoded.activeLaneMask)
        || !getU8(data, len, off, decoded.activeFighterMask)
        || !getU8(data, len, off, decoded.cpuMask)
        || !getU8(data, len, off, cpuContract)
        || !getU16(data, len, off, reserved16)) {
        return ParseStatus::Truncated;
    }
    if (reserved16 != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (!isKnownBootstrapKindV2(bootstrap)
        || !isKnownCpuContract(cpuContract)) {
        return ParseStatus::UnsupportedFeature;
    }
    decoded.bootstrapKind = static_cast<BootstrapKindV2>(bootstrap);
    decoded.cpuContract = static_cast<CpuContract>(cpuContract);
    for (uint8_t& route : decoded.slotToSourceRoute)
        if (!getU8(data, len, off, route)) return ParseStatus::Truncated;
    for (SourceLaneDescriptorV2& lane : decoded.lanes) {
        uint8_t active = 0u;
        if (!getU8(data, len, off, active))
            return ParseStatus::Truncated;
        if (active > 1u) return ParseStatus::BadLaneShape;
        lane.active = active != 0u;
        if (!getReplayText(data, len, off, lane.name))
            return ParseStatus::BadUtf8;
    }
    for (FighterSlotDescriptorV2& slot : decoded.slots) {
        status = getFighterSlotV2(data, len, off, slot);
        if (status != ParseStatus::Ok) return status;
    }
    uint8_t stageVerified = 0u;
    uint8_t stageReserved[3]{};
    DisplayMetadataV2& display = decoded.display;
    uint8_t sourceKind = 0u;
    if (!getU32(data, len, off, decoded.routeEpoch)
        || !getU32(data, len, off, decoded.rosterEpoch)
        || !getU32(data, len, off, decoded.layoutGeneration)
        || !getU32(data, len, off, decoded.stage.nativeStageId)
        || !getU8(data, len, off, stageVerified)
        || !getBytes(data, len, off, stageReserved, sizeof(stageReserved))
        || !getU64(data, len, off, decoded.stage.catalogEntryId)
        || !getI64(data, len, off, display.recordedUnixSeconds)
        || !getI32(data, len, off, display.timezoneOffsetMinutes)
        || !getU8(data, len, off, sourceKind)
        || !getU8(data, len, off, display.flags)
        || !getU16(data, len, off, display.playerOneDelay)
        || !getU16(data, len, off, display.playerTwoDelay)
        || !getU16(data, len, off, display.rollbackWindow)
        || !getU32(data, len, off, display.averageRttMicros)
        || !getU32(data, len, off, display.jitterMicros)
        || !getU32(data, len, off, display.lossPermille)) {
        return ParseStatus::Truncated;
    }
    if (stageVerified > 1u || !allZero(stageReserved, sizeof(stageReserved)))
        return ParseStatus::UnsupportedContainerVersion;
    if (!isKnownReplaySourceKindV2(sourceKind))
        return ParseStatus::UnsupportedFeature;
    decoded.stage.verified = stageVerified != 0u;
    display.sourceKind = static_cast<ReplaySourceKindV2>(sourceKind);
    if (!getReplayText(data, len, off, display.title)
        || !getReplayText(data, len, off, display.routeLabel)
        || !getU64(data, len, off, display.seriesUuidLow)
        || !getU64(data, len, off, display.seriesUuidHigh)
        || !getU32(data, len, off, display.gameOrdinalInSeries)
        || !getBytes(
            data, len, off, display.previousGameDigest.data(),
            display.previousGameDigest.size())
        || !getU16(data, len, off, reserved16)) {
        return ParseStatus::Truncated;
    }
    if (reserved16 != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (off != len) return ParseStatus::LimitExceeded;
    if (!isValidGameManifestV2(decoded))
        return ParseStatus::BadLaneShape;
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeDeterministicGameStartV2(
    const DeterministicGameStartV2& start, std::vector<uint8_t>& out) {
    if (!isValidDeterministicGameStartV2(start)) return false;
    std::vector<uint8_t> encoded;
    encoded.reserve(kDeterministicGameStartBytesV2);
    putU32(encoded, start.configuration.timerMode);
    putU32(encoded, start.configuration.aiDifficulty);
    putU32(encoded, start.configuration.lifeMultiplier);
    putU32(encoded, start.configuration.roundsToWinMinusOne);
    putU32(encoded, start.rng.gameplay);
    putU32(encoded, start.rng.cosmetic);
    putU32(encoded, start.rng.selector);
    putU16(encoded, start.fpu.x87ControlWord);
    putU16(encoded, 0u);
    putU32(encoded, start.fpu.mxcsr);
    for (uint32_t codePage : start.nativeDisplay.codePages)
        putU32(encoded, codePage);
    for (const auto& name : start.nativeDisplay.names) {
        putBytes(
            encoded, reinterpret_cast<const uint8_t*>(name.data()),
            name.size());
    }
    putU32(encoded, start.initialPhase);
    putU64(encoded, start.bootstrapFingerprint);
    putU64(encoded, start.frameZeroPreStateHash);
    putU64(encoded, start.initialRollingDigest);
    if (encoded.size() != kDeterministicGameStartBytesV2) return false;
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeDeterministicGameStartV2(
    const uint8_t* data, size_t len, DeterministicGameStartV2& out) {
    if (data == nullptr || len < kDeterministicGameStartBytesV2)
        return ParseStatus::Truncated;
    if (len > kDeterministicGameStartBytesV2)
        return ParseStatus::LimitExceeded;
    DeterministicGameStartV2 decoded{};
    size_t off = 0u;
    uint16_t reserved = 0u;
    if (!getU32(data, len, off, decoded.configuration.timerMode)
        || !getU32(data, len, off, decoded.configuration.aiDifficulty)
        || !getU32(data, len, off, decoded.configuration.lifeMultiplier)
        || !getU32(
            data, len, off, decoded.configuration.roundsToWinMinusOne)
        || !getU32(data, len, off, decoded.rng.gameplay)
        || !getU32(data, len, off, decoded.rng.cosmetic)
        || !getU32(data, len, off, decoded.rng.selector)
        || !getU16(data, len, off, decoded.fpu.x87ControlWord)
        || !getU16(data, len, off, reserved)
        || !getU32(data, len, off, decoded.fpu.mxcsr)) {
        return ParseStatus::Truncated;
    }
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    for (uint32_t& codePage : decoded.nativeDisplay.codePages)
        if (!getU32(data, len, off, codePage)) return ParseStatus::Truncated;
    for (auto& name : decoded.nativeDisplay.names) {
        if (!getBytes(
                data, len, off, reinterpret_cast<uint8_t*>(name.data()),
                name.size())) {
            return ParseStatus::Truncated;
        }
    }
    if (!getU32(data, len, off, decoded.initialPhase)
        || !getU64(data, len, off, decoded.bootstrapFingerprint)
        || !getU64(data, len, off, decoded.frameZeroPreStateHash)
        || !getU64(data, len, off, decoded.initialRollingDigest)) {
        return ParseStatus::Truncated;
    }
    if (!isValidDeterministicGameStartV2(decoded))
        return ParseStatus::BadLaneShape;
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeConfirmedGameFrameCoreV2(
    const ConfirmedGameFrameCoreV2& frame, std::vector<uint8_t>& out) {
    if (!isValidConfirmedGameFrameCoreV2(frame, frame.activeLaneMask))
        return false;
    std::vector<uint8_t> encoded;
    encoded.reserve(kConfirmedGameFrameCoreBytesV2);
    putU8(encoded, static_cast<uint8_t>(frame.inputDomain));
    putU8(encoded, frame.activeLaneMask);
    for (uint16_t action : frame.actions) putU16(encoded, action);
    putU16(encoded, 0u);
    putLifecycleV2(encoded, frame.before);
    putLifecycleV2(encoded, frame.after);
    putI32(encoded, frame.dispatcherResult);
    putU64(encoded, frame.preStateHash);
    putU64(encoded, frame.postStateHash);
    if (encoded.size() != kConfirmedGameFrameCoreBytesV2) return false;
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeConfirmedGameFrameCoreV2(
    const uint8_t* data, size_t len, uint8_t allowedLaneMask,
    ConfirmedGameFrameCoreV2& out) {
    if (data == nullptr || len < kConfirmedGameFrameCoreBytesV2)
        return ParseStatus::Truncated;
    ConfirmedGameFrameCoreV2 decoded{};
    size_t off = 0u;
    uint8_t domain = 0u;
    uint16_t reserved = 0u;
    if (!getU8(data, len, off, domain)
        || !getU8(data, len, off, decoded.activeLaneMask)) {
        return ParseStatus::Truncated;
    }
    for (uint16_t& action : decoded.actions)
        if (!getU16(data, len, off, action)) return ParseStatus::Truncated;
    if (!getU16(data, len, off, reserved))
        return ParseStatus::Truncated;
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    ParseStatus status = getLifecycleV2(data, len, off, decoded.before);
    if (status != ParseStatus::Ok) return status;
    status = getLifecycleV2(data, len, off, decoded.after);
    if (status != ParseStatus::Ok) return status;
    if (!getI32(data, len, off, decoded.dispatcherResult)
        || !getU64(data, len, off, decoded.preStateHash)
        || !getU64(data, len, off, decoded.postStateHash)) {
        return ParseStatus::Truncated;
    }
    if (!isKnownInputDomain(domain)) return ParseStatus::BadLaneShape;
    decoded.inputDomain = static_cast<InputDomain>(domain);
    if (!isValidConfirmedGameFrameCoreV2(decoded, allowedLaneMask)) {
        for (size_t lane = 0u; lane < kSourceLanes; ++lane) {
            if ((decoded.activeLaneMask & (1u << lane)) != 0u
                && !isValidActionMask(decoded.actions[lane])) {
                return ParseStatus::BadActionMask;
            }
        }
        return ParseStatus::BadLaneShape;
    }
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeGameFrameRecordV2(
    const ConfirmedGameFrameV2& frame, std::vector<uint8_t>& out) {
    if (frame.replayFrame >= kMaxTimelineFrames) return false;
    std::vector<uint8_t> encoded;
    encoded.reserve(kGameFrameRecordBytesV2);
    if (!encodeConfirmedGameFrameCoreV2(frame.core, encoded)) return false;
    putU64(encoded, frame.rollingDigest);
    if (encoded.size() != kGameFrameRecordBytesV2) return false;
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeGameFrameRecordV2(
    const uint8_t* data, size_t len, uint32_t replayFrame,
    uint8_t allowedLaneMask, ConfirmedGameFrameV2& out) {
    if (data == nullptr || len < kGameFrameRecordBytesV2)
        return ParseStatus::Truncated;
    if (len > kGameFrameRecordBytesV2) return ParseStatus::LimitExceeded;
    if (replayFrame >= kMaxTimelineFrames) return ParseStatus::LimitExceeded;
    ConfirmedGameFrameV2 decoded{};
    decoded.sourceFrame = replayFrame;
    decoded.replayFrame = replayFrame;
    ParseStatus status = decodeConfirmedGameFrameCoreV2(
        data, kConfirmedGameFrameCoreBytesV2, allowedLaneMask,
        decoded.core);
    if (status != ParseStatus::Ok) return status;
    size_t off = kConfirmedGameFrameCoreBytesV2;
    if (!getU64(data, len, off, decoded.rollingDigest))
        return ParseStatus::Truncated;
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeConfirmedGameFrameTransportV2(
    const ConfirmedGameFrameV2& frame, std::vector<uint8_t>& out) {
    if (frame.replayFrame >= kMaxTimelineFrames) return false;
    std::vector<uint8_t> encoded;
    encoded.reserve(kConfirmedGameFrameTransportBytesV2);
    putU32(encoded, frame.sourceFrame);
    putU32(encoded, frame.replayFrame);
    if (!encodeConfirmedGameFrameCoreV2(frame.core, encoded)) return false;
    putU64(encoded, frame.rollingDigest);
    if (encoded.size() != kConfirmedGameFrameTransportBytesV2) return false;
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeConfirmedGameFrameTransportV2(
    const uint8_t* data, size_t len, uint8_t allowedLaneMask,
    ConfirmedGameFrameV2& out) {
    if (data == nullptr || len < kConfirmedGameFrameTransportBytesV2)
        return ParseStatus::Truncated;
    if (len > kConfirmedGameFrameTransportBytesV2)
        return ParseStatus::LimitExceeded;
    ConfirmedGameFrameV2 decoded{};
    size_t off = 0u;
    if (!getU32(data, len, off, decoded.sourceFrame)
        || !getU32(data, len, off, decoded.replayFrame)) {
        return ParseStatus::Truncated;
    }
    if (decoded.replayFrame >= kMaxTimelineFrames)
        return ParseStatus::LimitExceeded;
    ParseStatus status = decodeConfirmedGameFrameCoreV2(
        data + off, kConfirmedGameFrameCoreBytesV2, allowedLaneMask,
        decoded.core);
    if (status != ParseStatus::Ok) return status;
    off += kConfirmedGameFrameCoreBytesV2;
    if (!getU64(data, len, off, decoded.rollingDigest))
        return ParseStatus::Truncated;
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeConfirmedGameEventV2(
    const ConfirmedGameEventV2& event, std::vector<uint8_t>& out) {
    if (!isValidConfirmedGameEventV2(event)) return false;
    const size_t before = out.size();
    putEventV2(out, event);
    return out.size() - before == kConfirmedGameEventBytesV2;
}

ParseStatus decodeConfirmedGameEventV2(
    const uint8_t* data, size_t len, ConfirmedGameEventV2& out) {
    if (data == nullptr || len < kConfirmedGameEventBytesV2)
        return ParseStatus::Truncated;
    if (len > kConfirmedGameEventBytesV2)
        return ParseStatus::LimitExceeded;
    size_t off = 0u;
    return getEventV2(data, len, off, out);
}

bool encodeConfirmedGameEndV2(
    const ConfirmedGameEndV2& end, std::vector<uint8_t>& out) {
    if (!isValidConfirmedGameEndV2(end)) return false;
    const size_t before = out.size();
    putU16(out, static_cast<uint16_t>(end.reason));
    putU16(out, 0u);
    putU32(out, end.nextRecord);
    putU8(out, end.winningSideMask);
    putU8(out, end.roundTallies[0]);
    putU8(out, end.roundTallies[1]);
    putU8(out, 0u);
    putI32(out, end.winnerSlot);
    putU64(out, end.postTailStateHash);
    putU64(out, end.finalRollingDigest);
    putBytes(
        out, end.canonicalGameDigest.data(),
        end.canonicalGameDigest.size());
    return out.size() - before == kConfirmedGameEndBytesV2;
}

ParseStatus decodeConfirmedGameEndV2(
    const uint8_t* data, size_t len, ConfirmedGameEndV2& out) {
    if (data == nullptr || len < kConfirmedGameEndBytesV2)
        return ParseStatus::Truncated;
    if (len > kConfirmedGameEndBytesV2)
        return ParseStatus::LimitExceeded;
    ConfirmedGameEndV2 decoded{};
    size_t off = 0u;
    uint16_t reason = 0u;
    uint16_t reserved16 = 0u;
    uint8_t reserved8 = 0u;
    if (!getU16(data, len, off, reason)
        || !getU16(data, len, off, reserved16)
        || !getU32(data, len, off, decoded.nextRecord)
        || !getU8(data, len, off, decoded.winningSideMask)
        || !getU8(data, len, off, decoded.roundTallies[0])
        || !getU8(data, len, off, decoded.roundTallies[1])
        || !getU8(data, len, off, reserved8)
        || !getI32(data, len, off, decoded.winnerSlot)
        || !getU64(data, len, off, decoded.postTailStateHash)
        || !getU64(data, len, off, decoded.finalRollingDigest)
        || !getBytes(
            data, len, off, decoded.canonicalGameDigest.data(),
            decoded.canonicalGameDigest.size())) {
        return ParseStatus::Truncated;
    }
    if (reserved16 != 0u || reserved8 != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (!isKnownTerminalReason(reason))
        return ParseStatus::UnsupportedFeature;
    decoded.reason = static_cast<TerminalReason>(reason);
    if (!isValidConfirmedGameEndV2(decoded))
        return ParseStatus::BadMatchIndex;
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeInputBlockV2(
    const ConfirmedGameFrameV2* frames, size_t count,
    std::vector<uint8_t>& out) {
    if (frames == nullptr || count == 0u
        || count > kMaxRecordsPerInputBlock) {
        return false;
    }
    const uint32_t first = frames[0].replayFrame;
    uint32_t last = 0u;
    if (!addChecked32(first, static_cast<uint32_t>(count) - 1u, last)
        || last >= kMaxTimelineFrames) {
        return false;
    }
    size_t body = 0u;
    size_t total = 0u;
    if (!mulChecked(count, kGameFrameRecordBytesV2, body)
        || !addChecked(kInputBlockHeaderBytesV2, body, total)
        || total > kMaxChunkPayloadBytes) {
        return false;
    }
    std::vector<uint8_t> encoded;
    encoded.reserve(total);
    putU32(encoded, first);
    putU32(encoded, static_cast<uint32_t>(count));
    for (size_t i = 0u; i < count; ++i) {
        if (frames[i].replayFrame != first + static_cast<uint32_t>(i)
            || !encodeGameFrameRecordV2(frames[i], encoded)) {
            return false;
        }
    }
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeInputBlockV2(
    const uint8_t* data, size_t len, uint32_t expectedFirstRecord,
    uint32_t recordCount, uint8_t allowedLaneMask,
    std::vector<ConfirmedGameFrameV2>& out) {
    if (recordCount == 0u || recordCount > kMaxRecordsPerInputBlock)
        return ParseStatus::LimitExceeded;
    size_t body = 0u;
    size_t expected = 0u;
    if (!mulChecked(recordCount, kGameFrameRecordBytesV2, body)
        || !addChecked(kInputBlockHeaderBytesV2, body, expected)) {
        return ParseStatus::IntegerOverflow;
    }
    if (data == nullptr || len < expected) return ParseStatus::Truncated;
    if (len > expected) return ParseStatus::LimitExceeded;
    size_t off = 0u;
    uint32_t first = 0u;
    uint32_t count = 0u;
    if (!getU32(data, len, off, first)
        || !getU32(data, len, off, count)) {
        return ParseStatus::Truncated;
    }
    if (first != expectedFirstRecord || count != recordCount)
        return ParseStatus::FrameGap;
    uint32_t last = 0u;
    if (!addChecked32(first, count - 1u, last))
        return ParseStatus::IntegerOverflow;
    if (last >= kMaxTimelineFrames) return ParseStatus::LimitExceeded;
    std::vector<ConfirmedGameFrameV2> decoded(count);
    for (uint32_t i = 0u; i < count; ++i) {
        const ParseStatus status = decodeGameFrameRecordV2(
            data + off, kGameFrameRecordBytesV2, first + i,
            allowedLaneMask, decoded[i]);
        if (status != ParseStatus::Ok) return status;
        off += kGameFrameRecordBytesV2;
    }
    out.swap(decoded);
    return ParseStatus::Ok;
}

bool encodeEventBlockV2(
    const ConfirmedGameEventV2* events, size_t count,
    std::vector<uint8_t>& out) {
    if (events == nullptr || count == 0u || count > kMaxEventsPerBlock)
        return false;
    const uint32_t first = events[0].eventSequence;
    size_t body = 0u;
    size_t total = 0u;
    if (!mulChecked(count, kConfirmedGameEventBytesV2, body)
        || !addChecked(kEventBlockHeaderBytesV2, body, total)
        || total > kMaxChunkPayloadBytes) {
        return false;
    }
    std::vector<uint8_t> encoded;
    encoded.reserve(total);
    putU32(encoded, first);
    putU32(encoded, static_cast<uint32_t>(count));
    for (size_t i = 0u; i < count; ++i) {
        if (events[i].eventSequence != first + static_cast<uint32_t>(i)
            || !encodeConfirmedGameEventV2(events[i], encoded)) {
            return false;
        }
    }
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeEventBlockV2(
    const uint8_t* data, size_t len, uint32_t expectedFirstEventSequence,
    uint32_t eventCount, std::vector<ConfirmedGameEventV2>& out) {
    if (eventCount == 0u || eventCount > kMaxEventsPerBlock)
        return ParseStatus::LimitExceeded;
    size_t body = 0u;
    size_t expected = 0u;
    if (!mulChecked(eventCount, kConfirmedGameEventBytesV2, body)
        || !addChecked(kEventBlockHeaderBytesV2, body, expected)) {
        return ParseStatus::IntegerOverflow;
    }
    if (data == nullptr || len < expected) return ParseStatus::Truncated;
    if (len > expected) return ParseStatus::LimitExceeded;
    size_t off = 0u;
    uint32_t first = 0u;
    uint32_t count = 0u;
    if (!getU32(data, len, off, first)
        || !getU32(data, len, off, count)) {
        return ParseStatus::Truncated;
    }
    if (first != expectedFirstEventSequence || count != eventCount)
        return ParseStatus::SequenceRegression;
    std::vector<ConfirmedGameEventV2> decoded(count);
    for (uint32_t i = 0u; i < count; ++i) {
        ParseStatus status = decodeConfirmedGameEventV2(
            data + off, kConfirmedGameEventBytesV2, decoded[i]);
        if (status != ParseStatus::Ok) return status;
        if (decoded[i].eventSequence != first + i)
            return ParseStatus::SequenceRegression;
        off += kConfirmedGameEventBytesV2;
    }
    out.swap(decoded);
    return ParseStatus::Ok;
}

bool encodeTickBundleV2(
    const CanonicalTickBundleV2& bundle, std::vector<uint8_t>& out) {
    if (bundle.preEvents.count > kMaxEventsPerTickV2
        || bundle.postEvents.count > kMaxEventsPerTickV2) {
        return false;
    }
    std::vector<uint8_t> encoded;
    putU8(encoded, bundle.preEvents.count);
    putU8(encoded, bundle.postEvents.count);
    putU16(encoded, 0u);
    for (size_t i = 0u; i < bundle.preEvents.count; ++i)
        if (!encodeConfirmedGameEventV2(
                bundle.preEvents.events[i], encoded)) return false;
    if (!encodeConfirmedGameFrameTransportV2(bundle.frame, encoded))
        return false;
    for (size_t i = 0u; i < bundle.postEvents.count; ++i)
        if (!encodeConfirmedGameEventV2(
                bundle.postEvents.events[i], encoded)) return false;
    if (encoded.size() > kMaxTimelineBundleBytesV2) return false;
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeTickBundleV2(
    const uint8_t* data, size_t len, uint8_t allowedLaneMask,
    CanonicalTickBundleV2& out) {
    if (data == nullptr || len < 4u + kConfirmedGameFrameTransportBytesV2)
        return ParseStatus::Truncated;
    size_t off = 0u;
    CanonicalTickBundleV2 decoded{};
    uint16_t reserved = 0u;
    if (!getU8(data, len, off, decoded.preEvents.count)
        || !getU8(data, len, off, decoded.postEvents.count)
        || !getU16(data, len, off, reserved)) {
        return ParseStatus::Truncated;
    }
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (decoded.preEvents.count > kMaxEventsPerTickV2
        || decoded.postEvents.count > kMaxEventsPerTickV2) {
        return ParseStatus::LimitExceeded;
    }
    size_t eventCount =
        static_cast<size_t>(decoded.preEvents.count)
        + static_cast<size_t>(decoded.postEvents.count);
    size_t eventBytes = 0u;
    size_t expected = 0u;
    if (!mulChecked(eventCount, kConfirmedGameEventBytesV2, eventBytes)
        || !addChecked(
            4u + kConfirmedGameFrameTransportBytesV2, eventBytes,
            expected)) {
        return ParseStatus::IntegerOverflow;
    }
    if (len < expected) return ParseStatus::Truncated;
    if (len > expected) return ParseStatus::LimitExceeded;
    for (size_t i = 0u; i < decoded.preEvents.count; ++i) {
        ParseStatus status = decodeConfirmedGameEventV2(
            data + off, kConfirmedGameEventBytesV2,
            decoded.preEvents.events[i]);
        if (status != ParseStatus::Ok) return status;
        off += kConfirmedGameEventBytesV2;
    }
    ParseStatus status = decodeConfirmedGameFrameTransportV2(
        data + off, kConfirmedGameFrameTransportBytesV2,
        allowedLaneMask, decoded.frame);
    if (status != ParseStatus::Ok) return status;
    off += kConfirmedGameFrameTransportBytesV2;
    for (size_t i = 0u; i < decoded.postEvents.count; ++i) {
        status = decodeConfirmedGameEventV2(
            data + off, kConfirmedGameEventBytesV2,
            decoded.postEvents.events[i]);
        if (status != ParseStatus::Ok) return status;
        off += kConfirmedGameEventBytesV2;
    }
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeConfirmedGameStartV2(
    const ConfirmedGameStartV2& start, std::vector<uint8_t>& out) {
    std::vector<uint8_t> encoded;
    encoded.reserve(
        kGameManifestBytesV2 + kDeterministicGameStartBytesV2);
    if (!encodeGameManifestV2(start.manifest, encoded)
        || !encodeDeterministicGameStartV2(start.start, encoded)) {
        return false;
    }
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeConfirmedGameStartV2(
    const uint8_t* data, size_t len, ConfirmedGameStartV2& out) {
    constexpr size_t kBytes =
        kGameManifestBytesV2 + kDeterministicGameStartBytesV2;
    if (data == nullptr || len < kBytes) return ParseStatus::Truncated;
    if (len > kBytes) return ParseStatus::LimitExceeded;
    ConfirmedGameStartV2 decoded{};
    ParseStatus status = decodeGameManifestV2(
        data, kGameManifestBytesV2, decoded.manifest);
    if (status != ParseStatus::Ok) return status;
    status = decodeDeterministicGameStartV2(
        data + kGameManifestBytesV2, kDeterministicGameStartBytesV2,
        decoded.start);
    if (status != ParseStatus::Ok) return status;
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeBeginGameBundleV2(
    const BeginGameBundleV2& begin, std::vector<uint8_t>& out) {
    if (begin.frameZero.frame.replayFrame != 0u) return false;
    std::vector<uint8_t> encoded;
    if (!encodeConfirmedGameStartV2(begin.start, encoded)
        || !encodeTickBundleV2(begin.frameZero, encoded)) {
        return false;
    }
    if (encoded.size() > kMaxTimelineBundleBytesV2) return false;
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeBeginGameBundleV2(
    const uint8_t* data, size_t len, BeginGameBundleV2& out) {
    constexpr size_t kStartBytes =
        kGameManifestBytesV2 + kDeterministicGameStartBytesV2;
    if (data == nullptr || len <= kStartBytes) return ParseStatus::Truncated;
    BeginGameBundleV2 decoded{};
    ParseStatus status =
        decodeConfirmedGameStartV2(data, kStartBytes, decoded.start);
    if (status != ParseStatus::Ok) return status;
    status = decodeTickBundleV2(
        data + kStartBytes, len - kStartBytes,
        decoded.start.manifest.activeLaneMask, decoded.frameZero);
    if (status != ParseStatus::Ok) return status;
    if (decoded.frameZero.frame.replayFrame != 0u)
        return ParseStatus::FrameGap;
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeEndGameBundleV2(
    const EndGameBundleV2& end, std::vector<uint8_t>& out) {
    if (!isValidConfirmedGameEndV2(end.end)
        || end.containsFinalFrame
            != (end.end.reason == TerminalReason::Completed)) {
        return false;
    }
    if (end.containsFinalFrame) {
        if (end.finalFrame.frame.replayFrame + 1u
                != end.end.nextRecord
            || end.finalFrame.postEvents.count == 0u
            || end.finalFrame.postEvents.events[
                   end.finalFrame.postEvents.count - 1u].kind
                != GameEventKindV2::GameResult) {
            return false;
        }
    } else if (end.finalFrame != CanonicalTickBundleV2{}) {
        return false;
    }
    std::vector<uint8_t> encoded;
    putU8(encoded, end.containsFinalFrame ? 1u : 0u);
    putU8(encoded, 0u);
    putU16(encoded, 0u);
    if (end.containsFinalFrame
        && !encodeTickBundleV2(end.finalFrame, encoded)) {
        return false;
    }
    if (!encodeConfirmedGameEndV2(end.end, encoded)
        || encoded.size() > kMaxTimelineBundleBytesV2) {
        return false;
    }
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeEndGameBundleV2(
    const uint8_t* data, size_t len, uint8_t allowedLaneMask,
    EndGameBundleV2& out) {
    if (data == nullptr || len < 4u + kConfirmedGameEndBytesV2)
        return ParseStatus::Truncated;
    EndGameBundleV2 decoded{};
    size_t off = 0u;
    uint8_t contains = 0u;
    uint8_t reserved8 = 0u;
    uint16_t reserved16 = 0u;
    if (!getU8(data, len, off, contains)
        || !getU8(data, len, off, reserved8)
        || !getU16(data, len, off, reserved16)) {
        return ParseStatus::Truncated;
    }
    if (contains > 1u || reserved8 != 0u || reserved16 != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    decoded.containsFinalFrame = contains != 0u;
    const size_t endOffset = len - kConfirmedGameEndBytesV2;
    if (decoded.containsFinalFrame) {
        if (endOffset <= off) return ParseStatus::Truncated;
        const ParseStatus status = decodeTickBundleV2(
            data + off, endOffset - off, allowedLaneMask,
            decoded.finalFrame);
        if (status != ParseStatus::Ok) return status;
    } else if (endOffset != off) {
        return ParseStatus::LimitExceeded;
    }
    ParseStatus status = decodeConfirmedGameEndV2(
        data + endOffset, kConfirmedGameEndBytesV2, decoded.end);
    if (status != ParseStatus::Ok) return status;
    std::vector<uint8_t> canonical;
    if (!encodeEndGameBundleV2(decoded, canonical))
        return ParseStatus::BadMatchIndex;
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeFooterIndexV2(
    const FooterIndexV2& footer, std::vector<uint8_t>& out) {
    return encodeFooterIndexV2Impl(footer, false, out);
}

bool encodeFooterIndexFileDigestProjectionV2(
    const FooterIndexV2& footer, std::vector<uint8_t>& out) {
    return encodeFooterIndexV2Impl(footer, true, out);
}

ParseStatus decodeFooterIndexV2(
    const uint8_t* data, size_t len, FooterIndexV2& out) {
    if (data == nullptr || len < kFooterFixedBytesV2)
        return ParseStatus::Truncated;
    FooterIndexV2 decoded{};
    size_t off = 0u;
    uint32_t roundCount = 0u;
    uint64_t reserved = 0u;
    if (!getU32(data, len, off, decoded.chunkCount)
        || !getU32(data, len, off, decoded.frameCount)
        || !getU32(data, len, off, decoded.eventCount)
        || !getU32(data, len, off, roundCount)
        || !getU64(data, len, off, decoded.manifestOffset)
        || !getU64(data, len, off, decoded.startOffset)
        || !getU64(data, len, off, decoded.firstInputOffset)
        || !getU64(data, len, off, decoded.gameEndOffset)
        || !getU64(data, len, off, decoded.finalRollingDigest)
        || !getBytes(
            data, len, off, decoded.canonicalGameDigest.data(),
            decoded.canonicalGameDigest.size())
        || !getU64(data, len, off, reserved)) {
        return ParseStatus::Truncated;
    }
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (roundCount > kMaxEventsPerBlock)
        return ParseStatus::LimitExceeded;
    size_t roundBytes = 0u;
    size_t expected = 0u;
    if (!mulChecked(roundCount, kRoundIndexEntryBytesV2, roundBytes)
        || !addChecked(kFooterFixedBytesV2, roundBytes, expected)) {
        return ParseStatus::IntegerOverflow;
    }
    if (len < expected) return ParseStatus::Truncated;
    if (len > expected) return ParseStatus::LimitExceeded;
    decoded.rounds.resize(roundCount);
    for (uint32_t i = 0u; i < roundCount; ++i) {
        const ParseStatus status =
            getRoundIndexV2(data, len, off, decoded.rounds[i]);
        if (status != ParseStatus::Ok) return status;
    }
    if (!getBytes(
            data, len, off, decoded.fileDigest.data(),
            decoded.fileDigest.size())) {
        return ParseStatus::Truncated;
    }
    std::vector<uint8_t> verify;
    if (!encodeFooterIndexV2(decoded, verify))
        return ParseStatus::BadMatchIndex;
    out = decoded;
    return ParseStatus::Ok;
}

}
