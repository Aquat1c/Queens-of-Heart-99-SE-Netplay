#include "ReplayCodec.h"

#include "ReplayIntegrity.h"

#include <cstring>
#include <limits>

namespace qreplay {
namespace {

static_assert(
    kPrefixBytes == 4u + 2u + 2u + 2u + 2u + 4u + 4u + 8u + 8u + 8u + 4u,
    "prefix layout");
static_assert(
    kChunkHeaderBytes == 4u + 2u + 2u + 1u + 1u + 2u + 9u * 4u,
    "chunk header layout");
static_assert(
    kSimulationContractBytes == 32u + 6u * 8u + 32u + 8u + 4u + 4u,
    "simulation contract layout");
static_assert(
    kFighterSlotDescriptorBytes == 1u + 1u + 2u + 1u + 3u,
    "fighter slot descriptor layout");
static_assert(
    kDeterministicStartBytes
        == 4u * 4u + 8u + 4u * 4u + 2u * 4u
               + kSourceLanes * kDisplayNameBytes + 4u + 8u + 8u + 4u,
    "deterministic start layout");
static_assert(
    kMatchStartBytes
        == 4u * 4u + 4u + kDeterministicStartBytes
               + kFighterSlots * kFighterSlotDescriptorBytes + kFighterSlots
               + 4u + 4u + 4u,
    "match start layout");
static_assert(
    kMatchEndBytes == 4u + 4u + 2u + 1u + 1u + 1u + 3u + 8u + 8u,
    "match end layout");
static_assert(
    kSessionEndBytes == 4u + 2u + 2u + 8u + 8u + 4u + 4u,
    "session end layout");
static_assert(kInputBlockHeaderBytes == 4u + 4u, "input block header layout");
static_assert(
    kInputRecordBytes
        == 1u + 1u + kSourceLanes * 2u + 4u + 8u + 8u + 24u + 24u + 4u + 8u,
    "input record layout");
static_assert(
    kEventRecordBytes
        == 4u + 2u + 2u + 4u + 4u + kFighterSlots * 2u + kFighterSlots * 3u
               + 1u + 3u + 8u + 8u + 4u + 8u + 8u,
    "event record layout");
static_assert(
    kMatchIndexEntryBytes
        == 4u + 4u + 8u + 8u + 4u + 4u + 4u + 4u + 1u + 1u + 2u
               + kFighterSlots * 2u + kFighterSlots,
    "match index entry layout");
static_assert(
    kFooterFixedBytes == 4u + 4u + 4u + 8u + 4u + 32u, "footer layout");
static_assert(
    kMaxNoteBytes <= kMaxEncodedStringBytes
        && kMaxLabelBytes <= kMaxEncodedStringBytes
        && kMaxTagBytes <= kMaxEncodedStringBytes,
    "string limits must fit a 16-bit length prefix");

constexpr uint8_t kSlotMask = 0x0Fu;
constexpr uint8_t kLaneMask = 0x0Fu;
constexpr uint8_t kMaxVsRow = 5u;
constexpr uint8_t kMaxConfirmState = 4u;
constexpr int32_t kMaxTimezoneOffsetMinutes = 1440;
constexpr uint32_t kMaxLossPermille = 1000u;

uint32_t asUnsigned32(int32_t value) {
    return value >= 0
        ? static_cast<uint32_t>(value)
        : 0xFFFFFFFFu - static_cast<uint32_t>(-(value + 1));
}

int32_t asSigned32(uint32_t value) {
    return value <= 0x7FFFFFFFu
        ? static_cast<int32_t>(value)
        : -static_cast<int32_t>(0xFFFFFFFFu - value) - 1;
}

uint64_t asUnsigned64(int64_t value) {
    return value >= 0
        ? static_cast<uint64_t>(value)
        : 0xFFFFFFFFFFFFFFFFull - static_cast<uint64_t>(-(value + 1));
}

int64_t asSigned64(uint64_t value) {
    return value <= 0x7FFFFFFFFFFFFFFFull
        ? static_cast<int64_t>(value)
        : -static_cast<int64_t>(0xFFFFFFFFFFFFFFFFull - value) - 1;
}

bool validIdentity(const FighterIdentityV1& identity) {
    if (identity.paletteIndex > kMaxPaletteIndex)
        return false;
    return identity.nativeCharacterId <= kMaxSelectableCharacterId
        || identity.nativeCharacterId == kInactiveCharacterId;
}

bool inactiveIdentity(const FighterIdentityV1& identity) {
    return identity.nativeCharacterId == kInactiveCharacterId
        && identity.paletteIndex == 0u;
}

bool validSelectorProof(const SelectorProofV1& proof) {
    return proof.cursorCell < kSelectorCells
        && proof.alternateFlag <= 1u
        && proof.confirmState <= kMaxConfirmState;
}

bool zeroSelectorProof(const SelectorProofV1& proof) {
    return proof.cursorCell == 0u
        && proof.alternateFlag == 0u
        && proof.confirmState == 0u;
}

bool validSlotDescriptor(const FighterSlotDescriptor& slot) {
    if (!isKnownSlotOwnership(static_cast<uint8_t>(slot.ownership)))
        return false;
    if (slot.side > 1u)
        return false;
    if (!validIdentity(slot.identity))
        return false;
    return slot.selectorProofPresent
        ? validSelectorProof(slot.selectorProof)
        : zeroSelectorProof(slot.selectorProof);
}

bool validRoute(const std::array<uint8_t, kFighterSlots>& route) {
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (route[slot] >= kSourceLanes)
            return false;
    }
    return true;
}

// A canonical display name is NUL terminated in its 32-byte field with at most 20 payload bytes, zero padding, and no control bytes; the payload is code-page text, not necessarily UTF-8.
bool validDisplayName(const std::array<char, 32>& name) {
    size_t length = 0;
    while (length < kDisplayNameBytes && name[length] != '\0')
        ++length;
    if (length >= kDisplayNameBytes || length > kDisplayNamePayloadBytes)
        return false;
    for (size_t i = length; i < kDisplayNameBytes; ++i) {
        if (name[i] != '\0')
            return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const uint8_t byte = static_cast<uint8_t>(name[i]);
        if (byte < 0x20u || byte == 0x7Fu)
            return false;
    }
    return true;
}

void putIdentity(std::vector<uint8_t>& out, const FighterIdentityV1& value) {
    putU8(out, value.nativeCharacterId);
    putU8(out, value.paletteIndex);
}

void putSelectorProof(
    std::vector<uint8_t>& out, const SelectorProofV1& value) {
    putU8(out, value.cursorCell);
    putU8(out, value.alternateFlag);
    putU8(out, value.confirmState);
}

void putSlotDescriptor(
    std::vector<uint8_t>& out, const FighterSlotDescriptor& slot) {
    putU8(out, static_cast<uint8_t>(slot.ownership));
    putU8(out, slot.side);
    putIdentity(out, slot.identity);
    putU8(out, slot.selectorProofPresent ? 1u : 0u);
    putSelectorProof(out, slot.selectorProof);
}

void putLifecycle(std::vector<uint8_t>& out, const LifecycleTag& tag) {
    putU32(out, tag.scene);
    putU32(out, tag.requestedScene);
    putU32(out, tag.sceneThreeState);
    putU32(out, tag.matchState);
    putU32(out, tag.roundState);
    putU32(out, tag.layoutGeneration);
}

bool getIdentity(
    const uint8_t* data, size_t len, size_t& off, FighterIdentityV1& out) {
    FighterIdentityV1 decoded{};
    if (!getU8(data, len, off, decoded.nativeCharacterId)
        || !getU8(data, len, off, decoded.paletteIndex)) {
        return false;
    }
    out = decoded;
    return true;
}

bool getSelectorProof(
    const uint8_t* data, size_t len, size_t& off, SelectorProofV1& out) {
    SelectorProofV1 decoded{};
    if (!getU8(data, len, off, decoded.cursorCell)
        || !getU8(data, len, off, decoded.alternateFlag)
        || !getU8(data, len, off, decoded.confirmState)) {
        return false;
    }
    out = decoded;
    return true;
}

ParseStatus getSlotDescriptor(
    const uint8_t* data, size_t len, size_t& off, FighterSlotDescriptor& out) {
    uint8_t ownership = 0u;
    uint8_t side = 0u;
    uint8_t proofPresent = 0u;
    FighterSlotDescriptor decoded{};
    if (!getU8(data, len, off, ownership)
        || !getU8(data, len, off, side)
        || !getIdentity(data, len, off, decoded.identity)
        || !getU8(data, len, off, proofPresent)
        || !getSelectorProof(data, len, off, decoded.selectorProof)) {
        return ParseStatus::Truncated;
    }
    if (!isKnownSlotOwnership(ownership) || proofPresent > 1u)
        return ParseStatus::BadLaneShape;
    decoded.ownership = static_cast<SlotOwnership>(ownership);
    decoded.side = side;
    decoded.selectorProofPresent = proofPresent != 0u;
    if (!validSlotDescriptor(decoded))
        return ParseStatus::BadLaneShape;
    out = decoded;
    return ParseStatus::Ok;
}

bool getLifecycle(
    const uint8_t* data, size_t len, size_t& off, LifecycleTag& out) {
    LifecycleTag decoded{};
    if (!getU32(data, len, off, decoded.scene)
        || !getU32(data, len, off, decoded.requestedScene)
        || !getU32(data, len, off, decoded.sceneThreeState)
        || !getU32(data, len, off, decoded.matchState)
        || !getU32(data, len, off, decoded.roundState)
        || !getU32(data, len, off, decoded.layoutGeneration)) {
        return false;
    }
    out = decoded;
    return true;
}

void putSimulationContract(
    std::vector<uint8_t>& out, const SimulationContract& contract) {
    putBytes(out, contract.executableSha256.data(),
             contract.executableSha256.size());
    putU64(out, contract.runtimeSimulationId);
    putU64(out, contract.stateRegionFingerprint);
    putU64(out, contract.stateHashProjectionId);
    putU64(out, contract.stateHashAlgorithmId);
    putU64(out, contract.bootstrapCodecVersion);
    putU64(out, contract.eventSchemaVersion);
    putBytes(out, contract.contentManifestRoot.data(),
             contract.contentManifestRoot.size());
    putU64(out, contract.characterCatalogId);
    putU32(out, contract.fpuControlWord);
    putU32(out, contract.mxcsr);
}

bool getSimulationContract(
    const uint8_t* data, size_t len, size_t& off, SimulationContract& out) {
    SimulationContract decoded{};
    if (!getBytes(data, len, off, decoded.executableSha256.data(),
                  decoded.executableSha256.size())
        || !getU64(data, len, off, decoded.runtimeSimulationId)
        || !getU64(data, len, off, decoded.stateRegionFingerprint)
        || !getU64(data, len, off, decoded.stateHashProjectionId)
        || !getU64(data, len, off, decoded.stateHashAlgorithmId)
        || !getU64(data, len, off, decoded.bootstrapCodecVersion)
        || !getU64(data, len, off, decoded.eventSchemaVersion)
        || !getBytes(data, len, off, decoded.contentManifestRoot.data(),
                     decoded.contentManifestRoot.size())
        || !getU64(data, len, off, decoded.characterCatalogId)
        || !getU32(data, len, off, decoded.fpuControlWord)
        || !getU32(data, len, off, decoded.mxcsr)) {
        return false;
    }
    out = decoded;
    return true;
}

bool putDeterministicStartBody(
    std::vector<uint8_t>& out, const DeterministicStart& start) {
    if (start.firstTimelineFrame > kMaxTimelineFrames)
        return false;
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        if (!validDisplayName(start.displayNames[lane]))
            return false;
    }
    putU32(out, start.seedGameplay);
    putU32(out, start.seedCosmetic);
    putU32(out, start.selector);
    putU32(out, start.initialPhase);
    putU64(out, start.bootstrapHash);
    putU32(out, start.timerMode);
    putU32(out, start.aiDifficulty);
    putU32(out, start.lifeMultiplier);
    putU32(out, start.roundsToWinMinusOne);
    putU32(out, start.playerOneNameCodePage);
    putU32(out, start.playerTwoNameCodePage);
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        putBytes(
            out,
            reinterpret_cast<const uint8_t*>(start.displayNames[lane].data()),
            kDisplayNameBytes);
    }
    putU32(out, start.firstTimelineFrame);
    putU64(out, start.initialStateHash);
    putU64(out, start.initialDigest);
    putU32(out, 0u);
    return true;
}

ParseStatus getDeterministicStartBody(
    const uint8_t* data, size_t len, size_t& off, DeterministicStart& out) {
    DeterministicStart decoded{};
    uint32_t reserved = 0u;
    if (!getU32(data, len, off, decoded.seedGameplay)
        || !getU32(data, len, off, decoded.seedCosmetic)
        || !getU32(data, len, off, decoded.selector)
        || !getU32(data, len, off, decoded.initialPhase)
        || !getU64(data, len, off, decoded.bootstrapHash)
        || !getU32(data, len, off, decoded.timerMode)
        || !getU32(data, len, off, decoded.aiDifficulty)
        || !getU32(data, len, off, decoded.lifeMultiplier)
        || !getU32(data, len, off, decoded.roundsToWinMinusOne)
        || !getU32(data, len, off, decoded.playerOneNameCodePage)
        || !getU32(data, len, off, decoded.playerTwoNameCodePage)) {
        return ParseStatus::Truncated;
    }
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        if (!getBytes(
                data, len, off,
                reinterpret_cast<uint8_t*>(decoded.displayNames[lane].data()),
                kDisplayNameBytes)) {
            return ParseStatus::Truncated;
        }
    }
    if (!getU32(data, len, off, decoded.firstTimelineFrame)
        || !getU64(data, len, off, decoded.initialStateHash)
        || !getU64(data, len, off, decoded.initialDigest)
        || !getU32(data, len, off, reserved)) {
        return ParseStatus::Truncated;
    }
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (decoded.firstTimelineFrame > kMaxTimelineFrames)
        return ParseStatus::LimitExceeded;
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        if (!validDisplayName(decoded.displayNames[lane]))
            return ParseStatus::BadLaneShape;
    }
    out = decoded;
    return ParseStatus::Ok;
}

void putFrameRecord(std::vector<uint8_t>& out, const ReplayFrameV1& frame) {
    putU8(out, static_cast<uint8_t>(frame.inputDomain));
    putU8(out, frame.activeLaneMask);
    for (size_t lane = 0; lane < kSourceLanes; ++lane)
        putU16(out, frame.actions[lane]);
    putU32(out, frame.routeEpoch);
    putU64(out, frame.preStateHash);
    putU64(out, frame.postStateHash);
    putLifecycle(out, frame.lifecycleBefore);
    putLifecycle(out, frame.lifecycleAfter);
    putI32(out, frame.dispatcherResult);
    putU64(out, frame.rollingDigest);
}

ParseStatus getFrameRecord(
    const uint8_t* data, size_t len, size_t& off, uint32_t timelineFrame,
    ReplayFrameV1& out) {
    ReplayFrameV1 decoded{};
    uint8_t domain = 0u;
    decoded.timelineFrame = timelineFrame;
    if (!getU8(data, len, off, domain)
        || !getU8(data, len, off, decoded.activeLaneMask)) {
        return ParseStatus::Truncated;
    }
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        if (!getU16(data, len, off, decoded.actions[lane]))
            return ParseStatus::Truncated;
    }
    if (!getU32(data, len, off, decoded.routeEpoch)
        || !getU64(data, len, off, decoded.preStateHash)
        || !getU64(data, len, off, decoded.postStateHash)
        || !getLifecycle(data, len, off, decoded.lifecycleBefore)
        || !getLifecycle(data, len, off, decoded.lifecycleAfter)
        || !getI32(data, len, off, decoded.dispatcherResult)
        || !getU64(data, len, off, decoded.rollingDigest)) {
        return ParseStatus::Truncated;
    }
    if (!isKnownInputDomain(domain))
        return ParseStatus::BadLaneShape;
    decoded.inputDomain = static_cast<InputDomain>(domain);
    if ((decoded.activeLaneMask & static_cast<uint8_t>(~kLaneMask)) != 0u)
        return ParseStatus::BadLaneShape;
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        const bool active =
            (decoded.activeLaneMask & (1u << lane)) != 0u;
        if (active) {
            if (!isValidActionMask(decoded.actions[lane]))
                return ParseStatus::BadActionMask;
        } else if (decoded.actions[lane] != 0u) {
            return ParseStatus::BadLaneShape;
        }
    }
    out = decoded;
    return ParseStatus::Ok;
}

void putEventRecord(std::vector<uint8_t>& out, const ReplayEvent& event) {
    putU32(out, event.timelineFrame);
    putU16(out, static_cast<uint16_t>(event.kind));
    putU16(out, static_cast<uint16_t>(event.terminalReason));
    putU32(out, event.matchId);
    putU32(out, event.rosterEpoch);
    for (size_t slot = 0; slot < kFighterSlots; ++slot)
        putIdentity(out, event.roster[slot]);
    for (size_t slot = 0; slot < kFighterSlots; ++slot)
        putSelectorProof(out, event.selectorProof[slot]);
    putU8(out, event.selectorProofMask);
    putU8(out, 0u);
    putU8(out, 0u);
    putU8(out, 0u);
    putU64(out, event.valueA);
    putU64(out, event.valueB);
    putU32(out, event.valueC);
    putU64(out, event.stateProof);
    putU64(out, event.rollingDigest);
}

ParseStatus getEventRecord(
    const uint8_t* data, size_t len, size_t& off, ReplayEvent& out) {
    ReplayEvent decoded{};
    uint16_t kind = 0u;
    uint16_t terminalReason = 0u;
    uint8_t reserved[3] = { 0u, 0u, 0u };
    if (!getU32(data, len, off, decoded.timelineFrame)
        || !getU16(data, len, off, kind)
        || !getU16(data, len, off, terminalReason)
        || !getU32(data, len, off, decoded.matchId)
        || !getU32(data, len, off, decoded.rosterEpoch)) {
        return ParseStatus::Truncated;
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (!getIdentity(data, len, off, decoded.roster[slot]))
            return ParseStatus::Truncated;
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (!getSelectorProof(data, len, off, decoded.selectorProof[slot]))
            return ParseStatus::Truncated;
    }
    if (!getU8(data, len, off, decoded.selectorProofMask)
        || !getBytes(data, len, off, reserved, sizeof(reserved))
        || !getU64(data, len, off, decoded.valueA)
        || !getU64(data, len, off, decoded.valueB)
        || !getU32(data, len, off, decoded.valueC)
        || !getU64(data, len, off, decoded.stateProof)
        || !getU64(data, len, off, decoded.rollingDigest)) {
        return ParseStatus::Truncated;
    }
    if (reserved[0] != 0u || reserved[1] != 0u || reserved[2] != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (!isKnownEventKind(kind) || !isKnownTerminalReason(terminalReason))
        return ParseStatus::BadEventPayload;
    decoded.kind = static_cast<EventKind>(kind);
    decoded.terminalReason = static_cast<TerminalReason>(terminalReason);
    if (!isValidEvent(decoded))
        return ParseStatus::BadEventPayload;
    out = decoded;
    return ParseStatus::Ok;
}

bool putMatchIndexEntry(
    std::vector<uint8_t>& out, const MatchIndexEntry& entry) {
    if (entry.matchId == 0u
        || entry.ordinal >= kMaxMatchesPerSet
        || entry.matchStartOffset >= entry.firstInputOffset
        || entry.firstInputOffset > kMaxFileBytes
        || entry.firstTimelineFrame > entry.lastTimelineFrame
        || entry.lastTimelineFrame > kMaxTimelineFrames
        || entry.frameCount
               > entry.lastTimelineFrame - entry.firstTimelineFrame + 1u
        || (entry.winningSideMask & static_cast<uint8_t>(~kSlotMask)) != 0u) {
        return false;
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (!validIdentity(entry.roster[slot])
            || !isKnownSlotOwnership(
                   static_cast<uint8_t>(entry.ownership[slot]))) {
            return false;
        }
    }
    putU32(out, entry.matchId);
    putU32(out, entry.ordinal);
    putU64(out, entry.matchStartOffset);
    putU64(out, entry.firstInputOffset);
    putU32(out, entry.firstTimelineFrame);
    putU32(out, entry.lastTimelineFrame);
    putU32(out, entry.frameCount);
    putU32(out, entry.roundCount);
    putU8(out, entry.winningSideMask);
    putU8(out, entry.independentBootstrap ? 1u : 0u);
    putU16(out, 0u);
    for (size_t slot = 0; slot < kFighterSlots; ++slot)
        putIdentity(out, entry.roster[slot]);
    for (size_t slot = 0; slot < kFighterSlots; ++slot)
        putU8(out, static_cast<uint8_t>(entry.ownership[slot]));
    return true;
}

ParseStatus getMatchIndexEntry(
    const uint8_t* data, size_t len, size_t& off, MatchIndexEntry& out) {
    MatchIndexEntry decoded{};
    uint8_t independentBootstrap = 0u;
    uint16_t reserved = 0u;
    uint8_t ownership[kFighterSlots] = { 0u, 0u, 0u, 0u };
    if (!getU32(data, len, off, decoded.matchId)
        || !getU32(data, len, off, decoded.ordinal)
        || !getU64(data, len, off, decoded.matchStartOffset)
        || !getU64(data, len, off, decoded.firstInputOffset)
        || !getU32(data, len, off, decoded.firstTimelineFrame)
        || !getU32(data, len, off, decoded.lastTimelineFrame)
        || !getU32(data, len, off, decoded.frameCount)
        || !getU32(data, len, off, decoded.roundCount)
        || !getU8(data, len, off, decoded.winningSideMask)
        || !getU8(data, len, off, independentBootstrap)
        || !getU16(data, len, off, reserved)) {
        return ParseStatus::Truncated;
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (!getIdentity(data, len, off, decoded.roster[slot]))
            return ParseStatus::Truncated;
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (!getU8(data, len, off, ownership[slot]))
            return ParseStatus::Truncated;
    }
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (independentBootstrap > 1u)
        return ParseStatus::BadMatchIndex;
    decoded.independentBootstrap = independentBootstrap != 0u;
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (!isKnownSlotOwnership(ownership[slot]))
            return ParseStatus::BadMatchIndex;
        decoded.ownership[slot] = static_cast<SlotOwnership>(ownership[slot]);
    }
    if (decoded.ordinal >= kMaxMatchesPerSet
        || decoded.lastTimelineFrame > kMaxTimelineFrames
        || decoded.firstInputOffset > kMaxFileBytes) {
        return ParseStatus::LimitExceeded;
    }
    if (decoded.matchId == 0u
        || decoded.matchStartOffset >= decoded.firstInputOffset
        || decoded.firstTimelineFrame > decoded.lastTimelineFrame
        || decoded.frameCount
               > decoded.lastTimelineFrame - decoded.firstTimelineFrame + 1u) {
        return ParseStatus::BadMatchIndex;
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (!validIdentity(decoded.roster[slot]))
            return ParseStatus::BadMatchIndex;
    }
    if ((decoded.winningSideMask & static_cast<uint8_t>(~kSlotMask)) != 0u)
        return ParseStatus::BadMatchIndex;
    out = decoded;
    return ParseStatus::Ok;
}

}

void putU8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void putU16(std::vector<uint8_t>& out, uint16_t value) {
    for (unsigned i = 0; i < 2u; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8u)));
}

void putU32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8u)));
}

void putU64(std::vector<uint8_t>& out, uint64_t value) {
    for (unsigned i = 0; i < 8u; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8u)));
}

void putI32(std::vector<uint8_t>& out, int32_t value) {
    putU32(out, asUnsigned32(value));
}

void putI64(std::vector<uint8_t>& out, int64_t value) {
    putU64(out, asUnsigned64(value));
}

void putBytes(std::vector<uint8_t>& out, const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0)
        return;
    out.insert(out.end(), data, data + len);
}

bool getU8(const uint8_t* data, size_t len, size_t& off, uint8_t& out) {
    if (data == nullptr || off > len || len - off < 1u)
        return false;
    out = data[off];
    off += 1u;
    return true;
}

bool getU16(const uint8_t* data, size_t len, size_t& off, uint16_t& out) {
    if (data == nullptr || off > len || len - off < 2u)
        return false;
    out = static_cast<uint16_t>(
        static_cast<uint16_t>(data[off])
        | static_cast<uint16_t>(static_cast<uint16_t>(data[off + 1u]) << 8));
    off += 2u;
    return true;
}

bool getU32(const uint8_t* data, size_t len, size_t& off, uint32_t& out) {
    if (data == nullptr || off > len || len - off < 4u)
        return false;
    uint32_t value = 0u;
    for (unsigned i = 0; i < 4u; ++i)
        value |= static_cast<uint32_t>(data[off + i]) << (i * 8u);
    out = value;
    off += 4u;
    return true;
}

bool getU64(const uint8_t* data, size_t len, size_t& off, uint64_t& out) {
    if (data == nullptr || off > len || len - off < 8u)
        return false;
    uint64_t value = 0u;
    for (unsigned i = 0; i < 8u; ++i)
        value |= static_cast<uint64_t>(data[off + i]) << (i * 8u);
    out = value;
    off += 8u;
    return true;
}

bool getI32(const uint8_t* data, size_t len, size_t& off, int32_t& out) {
    uint32_t raw = 0u;
    if (!getU32(data, len, off, raw))
        return false;
    out = asSigned32(raw);
    return true;
}

bool getI64(const uint8_t* data, size_t len, size_t& off, int64_t& out) {
    uint64_t raw = 0u;
    if (!getU64(data, len, off, raw))
        return false;
    out = asSigned64(raw);
    return true;
}

bool getBytes(
    const uint8_t* data, size_t len, size_t& off, uint8_t* dst, size_t count) {
    if (count == 0u)
        return off <= len;
    if (data == nullptr || dst == nullptr || off > len || len - off < count)
        return false;
    std::memcpy(dst, data + off, count);
    off += count;
    return true;
}

bool addChecked(size_t a, size_t b, size_t& out) {
    if (a > (std::numeric_limits<size_t>::max)() - b)
        return false;
    out = a + b;
    return true;
}

bool mulChecked(size_t a, size_t b, size_t& out) {
    if (a != 0u && b > (std::numeric_limits<size_t>::max)() / a)
        return false;
    out = a * b;
    return true;
}

bool addChecked32(uint32_t a, uint32_t b, uint32_t& out) {
    if (a > 0xFFFFFFFFu - b)
        return false;
    out = a + b;
    return true;
}

bool isCanonicalReplayText(const uint8_t* data, size_t len) {
    if (len == 0u)
        return true;
    if (data == nullptr)
        return false;
    size_t i = 0;
    while (i < len) {
        const uint8_t lead = data[i];
        uint32_t codePoint = 0u;
        size_t extra = 0u;
        if (lead < 0x80u) {
            codePoint = lead;
            extra = 0u;
        } else if ((lead & 0xE0u) == 0xC0u) {
            codePoint = static_cast<uint32_t>(lead & 0x1Fu);
            extra = 1u;
        } else if ((lead & 0xF0u) == 0xE0u) {
            codePoint = static_cast<uint32_t>(lead & 0x0Fu);
            extra = 2u;
        } else if ((lead & 0xF8u) == 0xF0u) {
            codePoint = static_cast<uint32_t>(lead & 0x07u);
            extra = 3u;
        } else {
            return false;
        }
        if (len - i <= extra)
            return false;
        for (size_t k = 1u; k <= extra; ++k) {
            const uint8_t continuation = data[i + k];
            if ((continuation & 0xC0u) != 0x80u)
                return false;
            codePoint =
                (codePoint << 6) | static_cast<uint32_t>(continuation & 0x3Fu);
        }
        if ((extra == 1u && codePoint < 0x80u)
            || (extra == 2u && codePoint < 0x800u)
            || (extra == 3u && codePoint < 0x10000u)) {
            return false;
        }
        if (codePoint > 0x10FFFFu)
            return false;
        if (codePoint >= 0xD800u && codePoint <= 0xDFFFu)
            return false;
        if (codePoint < 0x20u || codePoint == 0x7Fu)
            return false;
        if (codePoint >= 0x80u && codePoint <= 0x9Fu)
            return false;
        i += extra + 1u;
    }
    return true;
}

bool isCanonicalReplayText(const std::string& text) {
    return isCanonicalReplayText(
        reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool putString(
    std::vector<uint8_t>& out, const std::string& text, size_t maxBytes) {
    if (maxBytes > kMaxEncodedStringBytes
        || text.size() > maxBytes
        || !isCanonicalReplayText(text)) {
        return false;
    }
    putU16(out, static_cast<uint16_t>(text.size()));
    putBytes(out, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    return true;
}

ParseStatus getString(
    const uint8_t* data, size_t len, size_t& off, size_t maxBytes,
    std::string& out) {
    uint16_t length = 0u;
    if (!getU16(data, len, off, length))
        return ParseStatus::Truncated;
    if (static_cast<size_t>(length) > maxBytes)
        return ParseStatus::LimitExceeded;
    if (off > len || len - off < static_cast<size_t>(length))
        return ParseStatus::Truncated;
    if (!isCanonicalReplayText(data + off, static_cast<size_t>(length)))
        return ParseStatus::BadUtf8;
    out.assign(
        reinterpret_cast<const char*>(data + off),
        static_cast<size_t>(length));
    off += static_cast<size_t>(length);
    return ParseStatus::Ok;
}

void encodePrefix(const ReplayPrefix& prefix, std::vector<uint8_t>& out) {
    std::vector<uint8_t> encoded;
    encoded.reserve(kPrefixBytes);
    putU32(encoded, kReplayMagic);
    putU16(encoded, prefix.containerMajor);
    putU16(encoded, prefix.containerMinor);
    putU16(encoded, prefix.minimumReaderMajor);
    putU16(encoded, static_cast<uint16_t>(kPrefixBytes));
    putU32(encoded, kPrefixEndianMarker);
    putU32(encoded, prefix.featureFlags);
    putU64(encoded, prefix.sessionUuidLow);
    putU64(encoded, prefix.sessionUuidHigh);
    putU64(encoded, 0u);
    putU32(encoded, 0u);
    const uint32_t crc = crc32c(encoded.data(), encoded.size());
    for (unsigned i = 0; i < 4u; ++i) {
        encoded[kPrefixBytes - 4u + i] =
            static_cast<uint8_t>(crc >> (i * 8u));
    }
    out.insert(out.end(), encoded.begin(), encoded.end());
}

ParseStatus decodePrefix(
    const uint8_t* data, size_t len, ReplayPrefix& out) {
    if (data == nullptr || len < kPrefixBytes)
        return ParseStatus::Truncated;

    size_t off = 0;
    uint32_t magic = 0u;
    if (!getU32(data, len, off, magic))
        return ParseStatus::Truncated;
    if (magic != kReplayMagic)
        return ParseStatus::BadMagic;

    uint8_t scratch[kPrefixBytes];
    std::memcpy(scratch, data, kPrefixBytes);
    size_t crcOffset = kPrefixBytes - 4u;
    uint32_t storedCrc = 0u;
    if (!getU32(data, len, crcOffset, storedCrc))
        return ParseStatus::Truncated;
    for (unsigned i = 0; i < 4u; ++i)
        scratch[kPrefixBytes - 4u + i] = 0u;
    if (crc32c(scratch, kPrefixBytes) != storedCrc)
        return ParseStatus::BadPrefixCrc;

    ReplayPrefix decoded{};
    uint16_t prefixBytes = 0u;
    uint32_t endianMarker = 0u;
    uint64_t reserved = 0u;
    if (!getU16(data, len, off, decoded.containerMajor)
        || !getU16(data, len, off, decoded.containerMinor)
        || !getU16(data, len, off, decoded.minimumReaderMajor)
        || !getU16(data, len, off, prefixBytes)
        || !getU32(data, len, off, endianMarker)
        || !getU32(data, len, off, decoded.featureFlags)
        || !getU64(data, len, off, decoded.sessionUuidLow)
        || !getU64(data, len, off, decoded.sessionUuidHigh)
        || !getU64(data, len, off, reserved)) {
        return ParseStatus::Truncated;
    }
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (prefixBytes != static_cast<uint16_t>(kPrefixBytes)
        || endianMarker != kPrefixEndianMarker
        || decoded.containerMajor == 0u
        || decoded.minimumReaderMajor == 0u
        || decoded.minimumReaderMajor > decoded.containerMajor
        || decoded.minimumReaderMajor > kContainerReaderMajor) {
        return ParseStatus::UnsupportedContainerVersion;
    }
    if ((decoded.featureFlags & ~kFeatureRequiredMask) != 0u)
        return ParseStatus::UnsupportedFeature;
    decoded.prefixBytes = prefixBytes;
    out = decoded;
    return ParseStatus::Ok;
}

void encodeChunkHeader(const ChunkHeader& header, std::vector<uint8_t>& out) {
    std::vector<uint8_t> encoded;
    encoded.reserve(kChunkHeaderBytes);
    putU32(encoded, kChunkSyncWord);
    putU16(encoded, static_cast<uint16_t>(header.kind));
    putU16(encoded, header.schemaVersion);
    putU8(encoded, header.critical);
    putU8(encoded, static_cast<uint8_t>(header.codec));
    putU16(encoded, 0u);
    putU32(encoded, header.sequence);
    putU32(encoded, header.matchId);
    putU32(encoded, header.firstTimelineFrame);
    putU32(encoded, header.lastTimelineFrame);
    putU32(encoded, header.recordCount);
    putU32(encoded, header.payloadBytes);
    putU32(encoded, header.decodedBytes);
    putU32(encoded, header.payloadCrc32c);
    putU32(encoded, 0u);
    const uint32_t crc = crc32c(encoded.data(), encoded.size());
    for (unsigned i = 0; i < 4u; ++i) {
        encoded[kChunkHeaderBytes - 4u + i] =
            static_cast<uint8_t>(crc >> (i * 8u));
    }
    out.insert(out.end(), encoded.begin(), encoded.end());
}

ParseStatus decodeChunkHeader(
    const uint8_t* data, size_t len, ChunkHeader& out) {
    if (data == nullptr || len < kChunkHeaderBytes)
        return ParseStatus::Truncated;

    size_t off = 0;
    uint32_t syncWord = 0u;
    if (!getU32(data, len, off, syncWord))
        return ParseStatus::Truncated;
    if (syncWord != kChunkSyncWord)
        return ParseStatus::BadSyncWord;

    uint8_t scratch[kChunkHeaderBytes];
    std::memcpy(scratch, data, kChunkHeaderBytes);
    size_t crcOffset = kChunkHeaderBytes - 4u;
    uint32_t storedCrc = 0u;
    if (!getU32(data, len, crcOffset, storedCrc))
        return ParseStatus::Truncated;
    for (unsigned i = 0; i < 4u; ++i)
        scratch[kChunkHeaderBytes - 4u + i] = 0u;
    if (crc32c(scratch, kChunkHeaderBytes) != storedCrc)
        return ParseStatus::BadChunkHeaderCrc;

    ChunkHeader decoded{};
    uint16_t kind = 0u;
    uint8_t codec = 0u;
    uint16_t reserved = 0u;
    if (!getU16(data, len, off, kind)
        || !getU16(data, len, off, decoded.schemaVersion)
        || !getU8(data, len, off, decoded.critical)
        || !getU8(data, len, off, codec)
        || !getU16(data, len, off, reserved)
        || !getU32(data, len, off, decoded.sequence)
        || !getU32(data, len, off, decoded.matchId)
        || !getU32(data, len, off, decoded.firstTimelineFrame)
        || !getU32(data, len, off, decoded.lastTimelineFrame)
        || !getU32(data, len, off, decoded.recordCount)
        || !getU32(data, len, off, decoded.payloadBytes)
        || !getU32(data, len, off, decoded.decodedBytes)
        || !getU32(data, len, off, decoded.payloadCrc32c)) {
        return ParseStatus::Truncated;
    }
    decoded.headerCrc32c = storedCrc;

    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (!isKnownChunkKind(kind) || decoded.schemaVersion == 0u)
        return ParseStatus::UnknownCriticalChunk;
    decoded.kind = static_cast<ChunkKind>(kind);
    if (decoded.critical > 1u
        || (decoded.critical != 0u) != isCriticalChunkKind(decoded.kind)) {
        return ParseStatus::UnknownCriticalChunk;
    }
    if (!isKnownChunkCodec(codec))
        return ParseStatus::UnknownChunkCodec;
    decoded.codec = static_cast<ChunkCodec>(codec);

    if (decoded.sequence >= kMaxChunksPerFile
        || decoded.payloadBytes > kMaxChunkPayloadBytes
        || decoded.decodedBytes > kMaxCheckpointDecodedBytes
        || decoded.firstTimelineFrame > kMaxTimelineFrames
        || decoded.lastTimelineFrame > kMaxTimelineFrames) {
        return ParseStatus::LimitExceeded;
    }
    if (decoded.codec == ChunkCodec::Raw
        && decoded.decodedBytes != decoded.payloadBytes) {
        return ParseStatus::LimitExceeded;
    }
    if (decoded.kind == ChunkKind::InputBlock
        && decoded.recordCount > kMaxRecordsPerInputBlock) {
        return ParseStatus::LimitExceeded;
    }
    if (decoded.kind == ChunkKind::EventBlock
        && decoded.recordCount > kMaxEventsPerBlock) {
        return ParseStatus::LimitExceeded;
    }
    if (decoded.firstTimelineFrame > decoded.lastTimelineFrame)
        return ParseStatus::FrameConflict;
    if (decoded.kind == ChunkKind::InputBlock && decoded.recordCount != 0u) {
        const uint32_t span =
            decoded.lastTimelineFrame - decoded.firstTimelineFrame + 1u;
        if (span != decoded.recordCount)
            return ParseStatus::FrameGap;
    }
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeSessionManifest(
    const SessionManifest& manifest, std::vector<uint8_t>& out) {
    if (!isKnownBootstrapKind(static_cast<uint8_t>(manifest.bootstrapKind))
        || !isKnownCpuContract(static_cast<uint8_t>(manifest.cpuContract))
        || !isKnownSourceKind(
               static_cast<uint8_t>(manifest.display.sourceKind))) {
        return false;
    }
    if (manifest.vsRow > kMaxVsRow
        || (manifest.cpuMask & static_cast<uint8_t>(~kSlotMask)) != 0u
        || !validRoute(manifest.slotToSourceRoute)) {
        return false;
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (!validSlotDescriptor(manifest.slots[slot]))
            return false;
    }
    if (manifest.display.recordedUnixSeconds < 0
        || manifest.display.timezoneOffsetMinutes < -kMaxTimezoneOffsetMinutes
        || manifest.display.timezoneOffsetMinutes > kMaxTimezoneOffsetMinutes
        || manifest.display.lossPermille > kMaxLossPermille) {
        return false;
    }

    std::vector<uint8_t> encoded;
    putSimulationContract(encoded, manifest.contract);
    putU8(encoded, static_cast<uint8_t>(manifest.bootstrapKind));
    putU8(encoded, manifest.vsRow);
    putU8(encoded, manifest.cpuMask);
    putU8(encoded, static_cast<uint8_t>(manifest.cpuContract));
    for (size_t slot = 0; slot < kFighterSlots; ++slot)
        putU8(encoded, manifest.slotToSourceRoute[slot]);
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        putU8(encoded, manifest.lanes[lane].active ? 1u : 0u);
        if (!putString(encoded, manifest.lanes[lane].name, kMaxLabelBytes))
            return false;
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot)
        putSlotDescriptor(encoded, manifest.slots[slot]);
    putU32(encoded, manifest.rosterEpoch);
    putI64(encoded, manifest.display.recordedUnixSeconds);
    putI32(encoded, manifest.display.timezoneOffsetMinutes);
    putU8(encoded, static_cast<uint8_t>(manifest.display.sourceKind));
    putU8(encoded, manifest.display.networkMetadataPresent ? 1u : 0u);
    putU16(encoded, manifest.display.playerOneDelay);
    putU16(encoded, manifest.display.playerTwoDelay);
    putU16(encoded, manifest.display.rollbackWindow);
    putU32(encoded, manifest.display.averageRttMicros);
    putU32(encoded, manifest.display.lossPermille);
    if (!putString(encoded, manifest.display.routeLabel, kMaxLabelBytes)
        || !putString(encoded, manifest.display.title, kMaxLabelBytes)) {
        return false;
    }
    putU32(encoded, 0u);
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeSessionManifest(
    const uint8_t* data, size_t len, SessionManifest& out) {
    if (data == nullptr && len != 0u)
        return ParseStatus::Truncated;

    SessionManifest decoded{};
    size_t off = 0;
    if (!getSimulationContract(data, len, off, decoded.contract))
        return ParseStatus::Truncated;

    uint8_t bootstrapKind = 0u;
    uint8_t cpuContract = 0u;
    if (!getU8(data, len, off, bootstrapKind)
        || !getU8(data, len, off, decoded.vsRow)
        || !getU8(data, len, off, decoded.cpuMask)
        || !getU8(data, len, off, cpuContract)) {
        return ParseStatus::Truncated;
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (!getU8(data, len, off, decoded.slotToSourceRoute[slot]))
            return ParseStatus::Truncated;
    }
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        uint8_t active = 0u;
        if (!getU8(data, len, off, active))
            return ParseStatus::Truncated;
        if (active > 1u)
            return ParseStatus::BadLaneShape;
        decoded.lanes[lane].active = active != 0u;
        const ParseStatus status = getString(
            data, len, off, kMaxLabelBytes, decoded.lanes[lane].name);
        if (status != ParseStatus::Ok)
            return status;
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        const ParseStatus status =
            getSlotDescriptor(data, len, off, decoded.slots[slot]);
        if (status != ParseStatus::Ok)
            return status;
    }
    uint8_t sourceKind = 0u;
    uint8_t networkMetadataPresent = 0u;
    uint32_t reserved = 0u;
    if (!getU32(data, len, off, decoded.rosterEpoch)
        || !getI64(data, len, off, decoded.display.recordedUnixSeconds)
        || !getI32(data, len, off, decoded.display.timezoneOffsetMinutes)
        || !getU8(data, len, off, sourceKind)
        || !getU8(data, len, off, networkMetadataPresent)
        || !getU16(data, len, off, decoded.display.playerOneDelay)
        || !getU16(data, len, off, decoded.display.playerTwoDelay)
        || !getU16(data, len, off, decoded.display.rollbackWindow)
        || !getU32(data, len, off, decoded.display.averageRttMicros)
        || !getU32(data, len, off, decoded.display.lossPermille)) {
        return ParseStatus::Truncated;
    }
    ParseStatus status = getString(
        data, len, off, kMaxLabelBytes, decoded.display.routeLabel);
    if (status != ParseStatus::Ok)
        return status;
    status =
        getString(data, len, off, kMaxLabelBytes, decoded.display.title);
    if (status != ParseStatus::Ok)
        return status;
    if (!getU32(data, len, off, reserved))
        return ParseStatus::Truncated;
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (off != len)
        return ParseStatus::LimitExceeded;

    if (!isKnownBootstrapKind(bootstrapKind)
        || !isKnownCpuContract(cpuContract)
        || !isKnownSourceKind(sourceKind)) {
        return ParseStatus::UnsupportedFeature;
    }
    if (networkMetadataPresent > 1u)
        return ParseStatus::BadLaneShape;
    decoded.bootstrapKind = static_cast<BootstrapKind>(bootstrapKind);
    decoded.cpuContract = static_cast<CpuContract>(cpuContract);
    decoded.display.sourceKind = static_cast<ReplaySourceKind>(sourceKind);
    decoded.display.networkMetadataPresent = networkMetadataPresent != 0u;

    if (decoded.vsRow > kMaxVsRow
        || (decoded.cpuMask & static_cast<uint8_t>(~kSlotMask)) != 0u
        || !validRoute(decoded.slotToSourceRoute)) {
        return ParseStatus::BadLaneShape;
    }
    if (decoded.display.recordedUnixSeconds < 0
        || decoded.display.timezoneOffsetMinutes < -kMaxTimezoneOffsetMinutes
        || decoded.display.timezoneOffsetMinutes > kMaxTimezoneOffsetMinutes
        || decoded.display.lossPermille > kMaxLossPermille) {
        return ParseStatus::LimitExceeded;
    }
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeDeterministicStart(
    const DeterministicStart& start, std::vector<uint8_t>& out) {
    std::vector<uint8_t> encoded;
    encoded.reserve(kDeterministicStartBytes);
    if (!putDeterministicStartBody(encoded, start))
        return false;
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeDeterministicStart(
    const uint8_t* data, size_t len, DeterministicStart& out) {
    if (data == nullptr || len < kDeterministicStartBytes)
        return ParseStatus::Truncated;
    if (len > kDeterministicStartBytes)
        return ParseStatus::LimitExceeded;
    DeterministicStart decoded{};
    size_t off = 0;
    const ParseStatus status =
        getDeterministicStartBody(data, len, off, decoded);
    if (status != ParseStatus::Ok)
        return status;
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeMatchStart(
    const MatchStartRecord& record, std::vector<uint8_t>& out) {
    if (record.matchId == 0u
        || record.ordinal >= kMaxMatchesPerSet
        || record.dependsOnMatchId == record.matchId
        || record.independentBootstrap != (record.dependsOnMatchId == 0u)
        || record.firstTimelineFrame > kMaxTimelineFrames
        || record.start.firstTimelineFrame != record.firstTimelineFrame) {
        return false;
    }
    if (record.vsRow > kMaxVsRow
        || (record.cpuMask & static_cast<uint8_t>(~kSlotMask)) != 0u
        || !validRoute(record.slotToSourceRoute)) {
        return false;
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (!validSlotDescriptor(record.slots[slot]))
            return false;
    }

    std::vector<uint8_t> encoded;
    encoded.reserve(kMatchStartBytes);
    putU32(encoded, record.matchId);
    putU32(encoded, record.ordinal);
    putU32(encoded, record.firstTimelineFrame);
    putU32(encoded, record.dependsOnMatchId);
    putU8(encoded, record.independentBootstrap ? 1u : 0u);
    putU8(encoded, record.vsRow);
    putU8(encoded, record.cpuMask);
    putU8(encoded, record.stageVerified ? 1u : 0u);
    if (!putDeterministicStartBody(encoded, record.start))
        return false;
    for (size_t slot = 0; slot < kFighterSlots; ++slot)
        putSlotDescriptor(encoded, record.slots[slot]);
    for (size_t slot = 0; slot < kFighterSlots; ++slot)
        putU8(encoded, record.slotToSourceRoute[slot]);
    putU32(encoded, record.rosterEpoch);
    putU32(encoded, record.stageId);
    putU32(encoded, 0u);
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeMatchStart(
    const uint8_t* data, size_t len, MatchStartRecord& out) {
    if (data == nullptr || len < kMatchStartBytes)
        return ParseStatus::Truncated;
    if (len > kMatchStartBytes)
        return ParseStatus::LimitExceeded;

    MatchStartRecord decoded{};
    size_t off = 0;
    uint8_t independentBootstrap = 0u;
    uint8_t stageVerified = 0u;
    uint32_t reserved = 0u;
    if (!getU32(data, len, off, decoded.matchId)
        || !getU32(data, len, off, decoded.ordinal)
        || !getU32(data, len, off, decoded.firstTimelineFrame)
        || !getU32(data, len, off, decoded.dependsOnMatchId)
        || !getU8(data, len, off, independentBootstrap)
        || !getU8(data, len, off, decoded.vsRow)
        || !getU8(data, len, off, decoded.cpuMask)
        || !getU8(data, len, off, stageVerified)) {
        return ParseStatus::Truncated;
    }
    ParseStatus status = getDeterministicStartBody(data, len, off, decoded.start);
    if (status != ParseStatus::Ok)
        return status;
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        status = getSlotDescriptor(data, len, off, decoded.slots[slot]);
        if (status != ParseStatus::Ok)
            return status;
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (!getU8(data, len, off, decoded.slotToSourceRoute[slot]))
            return ParseStatus::Truncated;
    }
    if (!getU32(data, len, off, decoded.rosterEpoch)
        || !getU32(data, len, off, decoded.stageId)
        || !getU32(data, len, off, reserved)) {
        return ParseStatus::Truncated;
    }
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (independentBootstrap > 1u || stageVerified > 1u)
        return ParseStatus::BadMatchIndex;
    decoded.independentBootstrap = independentBootstrap != 0u;
    decoded.stageVerified = stageVerified != 0u;

    if (decoded.ordinal >= kMaxMatchesPerSet
        || decoded.firstTimelineFrame > kMaxTimelineFrames) {
        return ParseStatus::LimitExceeded;
    }
    if (decoded.matchId == 0u
        || decoded.dependsOnMatchId == decoded.matchId
        || decoded.independentBootstrap != (decoded.dependsOnMatchId == 0u)
        || decoded.start.firstTimelineFrame != decoded.firstTimelineFrame) {
        return ParseStatus::BadMatchIndex;
    }
    if (decoded.vsRow > kMaxVsRow
        || (decoded.cpuMask & static_cast<uint8_t>(~kSlotMask)) != 0u
        || !validRoute(decoded.slotToSourceRoute)) {
        return ParseStatus::BadLaneShape;
    }
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeMatchEnd(
    const MatchEndRecord& record, std::vector<uint8_t>& out) {
    if (record.matchId == 0u
        || record.lastTimelineFrame > kMaxTimelineFrames
        || !isKnownTerminalReason(static_cast<uint16_t>(record.reason))
        || (record.winningSideMask & static_cast<uint8_t>(~kSlotMask)) != 0u) {
        return false;
    }
    std::vector<uint8_t> encoded;
    encoded.reserve(kMatchEndBytes);
    putU32(encoded, record.matchId);
    putU32(encoded, record.lastTimelineFrame);
    putU16(encoded, static_cast<uint16_t>(record.reason));
    putU8(encoded, record.winningSideMask);
    putU8(encoded, record.roundsSideZero);
    putU8(encoded, record.roundsSideOne);
    putU8(encoded, 0u);
    putU8(encoded, 0u);
    putU8(encoded, 0u);
    putU64(encoded, record.postTailStateHash);
    putU64(encoded, record.rollingDigest);
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeMatchEnd(
    const uint8_t* data, size_t len, MatchEndRecord& out) {
    if (data == nullptr || len < kMatchEndBytes)
        return ParseStatus::Truncated;
    if (len > kMatchEndBytes)
        return ParseStatus::LimitExceeded;

    MatchEndRecord decoded{};
    size_t off = 0;
    uint16_t reason = 0u;
    uint8_t reserved[3] = { 0u, 0u, 0u };
    if (!getU32(data, len, off, decoded.matchId)
        || !getU32(data, len, off, decoded.lastTimelineFrame)
        || !getU16(data, len, off, reason)
        || !getU8(data, len, off, decoded.winningSideMask)
        || !getU8(data, len, off, decoded.roundsSideZero)
        || !getU8(data, len, off, decoded.roundsSideOne)
        || !getBytes(data, len, off, reserved, sizeof(reserved))
        || !getU64(data, len, off, decoded.postTailStateHash)
        || !getU64(data, len, off, decoded.rollingDigest)) {
        return ParseStatus::Truncated;
    }
    if (reserved[0] != 0u || reserved[1] != 0u || reserved[2] != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (decoded.lastTimelineFrame > kMaxTimelineFrames)
        return ParseStatus::LimitExceeded;
    if (!isKnownTerminalReason(reason))
        return ParseStatus::UnsupportedFeature;
    decoded.reason = static_cast<TerminalReason>(reason);
    if (decoded.matchId == 0u)
        return ParseStatus::BadMatchIndex;
    if ((decoded.winningSideMask & static_cast<uint8_t>(~kSlotMask)) != 0u)
        return ParseStatus::BadLaneShape;
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeSessionEnd(
    const SessionEndRecord& record, std::vector<uint8_t>& out) {
    if (record.lastTimelineFrame > kMaxTimelineFrames
        || record.matchCount > kMaxMatchesPerSet
        || !isKnownTerminalReason(static_cast<uint16_t>(record.reason))) {
        return false;
    }
    std::vector<uint8_t> encoded;
    encoded.reserve(kSessionEndBytes);
    putU32(encoded, record.lastTimelineFrame);
    putU16(encoded, static_cast<uint16_t>(record.reason));
    putU16(encoded, 0u);
    putU64(encoded, record.postTailStateHash);
    putU64(encoded, record.rollingDigest);
    putU32(encoded, record.matchCount);
    putU32(encoded, 0u);
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeSessionEnd(
    const uint8_t* data, size_t len, SessionEndRecord& out) {
    if (data == nullptr || len < kSessionEndBytes)
        return ParseStatus::Truncated;
    if (len > kSessionEndBytes)
        return ParseStatus::LimitExceeded;

    SessionEndRecord decoded{};
    size_t off = 0;
    uint16_t reason = 0u;
    uint16_t reservedA = 0u;
    uint32_t reservedB = 0u;
    if (!getU32(data, len, off, decoded.lastTimelineFrame)
        || !getU16(data, len, off, reason)
        || !getU16(data, len, off, reservedA)
        || !getU64(data, len, off, decoded.postTailStateHash)
        || !getU64(data, len, off, decoded.rollingDigest)
        || !getU32(data, len, off, decoded.matchCount)
        || !getU32(data, len, off, reservedB)) {
        return ParseStatus::Truncated;
    }
    if (reservedA != 0u || reservedB != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (decoded.lastTimelineFrame > kMaxTimelineFrames
        || decoded.matchCount > kMaxMatchesPerSet) {
        return ParseStatus::LimitExceeded;
    }
    if (!isKnownTerminalReason(reason))
        return ParseStatus::UnsupportedFeature;
    decoded.reason = static_cast<TerminalReason>(reason);
    out = decoded;
    return ParseStatus::Ok;
}

bool encodeInputBlock(
    const ReplayFrameV1* frames, size_t count, std::vector<uint8_t>& out) {
    if (count > kMaxRecordsPerInputBlock)
        return false;
    if (count != 0u && frames == nullptr)
        return false;

    uint32_t firstFrame = 0u;
    if (count != 0u) {
        firstFrame = frames[0].timelineFrame;
        uint32_t last = 0u;
        if (!addChecked32(firstFrame, static_cast<uint32_t>(count) - 1u, last))
            return false;
        if (last > kMaxTimelineFrames)
            return false;
        for (size_t i = 0; i < count; ++i) {
            if (frames[i].timelineFrame != firstFrame + static_cast<uint32_t>(i))
                return false;
            if (!isValidFrame(frames[i]))
                return false;
        }
    }

    size_t body = 0;
    size_t total = 0;
    if (!mulChecked(count, kInputRecordBytes, body)
        || !addChecked(body, kInputBlockHeaderBytes, total)) {
        return false;
    }
    std::vector<uint8_t> encoded;
    encoded.reserve(total);
    putU32(encoded, firstFrame);
    putU32(encoded, 0u);
    for (size_t i = 0; i < count; ++i)
        putFrameRecord(encoded, frames[i]);
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeInputBlock(
    const uint8_t* data, size_t len, uint32_t expectedFirstFrame,
    uint32_t recordCount, std::vector<ReplayFrameV1>& out) {
    if (recordCount > kMaxRecordsPerInputBlock)
        return ParseStatus::LimitExceeded;

    size_t body = 0;
    size_t expectedBytes = 0;
    if (!mulChecked(static_cast<size_t>(recordCount), kInputRecordBytes, body)
        || !addChecked(body, kInputBlockHeaderBytes, expectedBytes)) {
        return ParseStatus::IntegerOverflow;
    }
    if (data == nullptr || len < expectedBytes)
        return ParseStatus::Truncated;
    if (len > expectedBytes)
        return ParseStatus::LimitExceeded;

    size_t off = 0;
    uint32_t firstFrame = 0u;
    uint32_t reserved = 0u;
    if (!getU32(data, len, off, firstFrame)
        || !getU32(data, len, off, reserved)) {
        return ParseStatus::Truncated;
    }
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (firstFrame != expectedFirstFrame)
        return ParseStatus::FrameGap;
    if (recordCount == 0u) {
        if (firstFrame > kMaxTimelineFrames)
            return ParseStatus::LimitExceeded;
        out.clear();
        return ParseStatus::Ok;
    }
    uint32_t lastFrame = 0u;
    if (!addChecked32(firstFrame, recordCount - 1u, lastFrame))
        return ParseStatus::IntegerOverflow;
    if (lastFrame > kMaxTimelineFrames)
        return ParseStatus::LimitExceeded;

    std::vector<ReplayFrameV1> frames;
    frames.resize(static_cast<size_t>(recordCount));
    for (uint32_t i = 0; i < recordCount; ++i) {
        const ParseStatus status = getFrameRecord(
            data, len, off, firstFrame + i, frames[static_cast<size_t>(i)]);
        if (status != ParseStatus::Ok)
            return status;
    }
    out.swap(frames);
    return ParseStatus::Ok;
}

bool encodeEventBlock(
    const ReplayEvent* events, size_t count, std::vector<uint8_t>& out) {
    if (count > kMaxEventsPerBlock)
        return false;
    if (count != 0u && events == nullptr)
        return false;
    for (size_t i = 0; i < count; ++i) {
        if (events[i].timelineFrame > kMaxTimelineFrames
            || !isValidEvent(events[i])) {
            return false;
        }
    }
    size_t total = 0;
    if (!mulChecked(count, kEventRecordBytes, total))
        return false;
    std::vector<uint8_t> encoded;
    encoded.reserve(total);
    for (size_t i = 0; i < count; ++i)
        putEventRecord(encoded, events[i]);
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeEventBlock(
    const uint8_t* data, size_t len, uint32_t count,
    std::vector<ReplayEvent>& out) {
    if (count > kMaxEventsPerBlock)
        return ParseStatus::LimitExceeded;
    size_t expectedBytes = 0;
    if (!mulChecked(
            static_cast<size_t>(count), kEventRecordBytes, expectedBytes)) {
        return ParseStatus::IntegerOverflow;
    }
    if (len < expectedBytes || (data == nullptr && expectedBytes != 0u))
        return ParseStatus::Truncated;
    if (len > expectedBytes)
        return ParseStatus::LimitExceeded;
    if (count == 0u) {
        out.clear();
        return ParseStatus::Ok;
    }

    std::vector<ReplayEvent> events;
    events.resize(static_cast<size_t>(count));
    size_t off = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const ParseStatus status =
            getEventRecord(data, len, off, events[static_cast<size_t>(i)]);
        if (status != ParseStatus::Ok)
            return status;
        if (events[static_cast<size_t>(i)].timelineFrame > kMaxTimelineFrames)
            return ParseStatus::LimitExceeded;
    }
    out.swap(events);
    return ParseStatus::Ok;
}

bool encodeFooterIndex(
    const FooterIndex& footer, std::vector<uint8_t>& out) {
    if (footer.chunkCount > kMaxChunksPerFile
        || footer.matchCount > kMaxMatchesPerSet
        || footer.totalFrames > kMaxTimelineFrames
        || footer.matches.size() != static_cast<size_t>(footer.matchCount)) {
        return false;
    }
    for (size_t i = 1; i < footer.matches.size(); ++i) {
        const MatchIndexEntry& previous = footer.matches[i - 1];
        const MatchIndexEntry& current = footer.matches[i];
        if (current.ordinal <= previous.ordinal
            || current.matchId == previous.matchId
            || current.matchStartOffset < previous.firstInputOffset) {
            return false;
        }
    }

    size_t body = 0;
    size_t total = 0;
    if (!mulChecked(footer.matches.size(), kMatchIndexEntryBytes, body)
        || !addChecked(body, kFooterFixedBytes, total)) {
        return false;
    }
    std::vector<uint8_t> encoded;
    encoded.reserve(total);
    putU32(encoded, footer.chunkCount);
    putU32(encoded, footer.matchCount);
    putU32(encoded, footer.totalFrames);
    putU64(encoded, footer.finalRollingDigest);
    putU32(encoded, 0u);
    for (size_t i = 0; i < footer.matches.size(); ++i) {
        if (!putMatchIndexEntry(encoded, footer.matches[i]))
            return false;
    }
    putBytes(encoded, footer.fileDigest.data(), footer.fileDigest.size());
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

ParseStatus decodeFooterIndex(
    const uint8_t* data, size_t len, FooterIndex& out) {
    if (data == nullptr || len < kFooterFixedBytes)
        return ParseStatus::Truncated;

    FooterIndex decoded{};
    size_t off = 0;
    uint32_t reserved = 0u;
    if (!getU32(data, len, off, decoded.chunkCount)
        || !getU32(data, len, off, decoded.matchCount)
        || !getU32(data, len, off, decoded.totalFrames)
        || !getU64(data, len, off, decoded.finalRollingDigest)
        || !getU32(data, len, off, reserved)) {
        return ParseStatus::Truncated;
    }
    if (reserved != 0u)
        return ParseStatus::UnsupportedContainerVersion;
    if (decoded.chunkCount > kMaxChunksPerFile
        || decoded.matchCount > kMaxMatchesPerSet
        || decoded.totalFrames > kMaxTimelineFrames) {
        return ParseStatus::LimitExceeded;
    }

    size_t body = 0;
    size_t expectedBytes = 0;
    if (!mulChecked(
            static_cast<size_t>(decoded.matchCount), kMatchIndexEntryBytes,
            body)
        || !addChecked(body, kFooterFixedBytes, expectedBytes)) {
        return ParseStatus::IntegerOverflow;
    }
    if (len < expectedBytes)
        return ParseStatus::Truncated;
    if (len > expectedBytes)
        return ParseStatus::LimitExceeded;

    std::vector<MatchIndexEntry> matches;
    matches.resize(static_cast<size_t>(decoded.matchCount));
    for (size_t i = 0; i < matches.size(); ++i) {
        const ParseStatus status =
            getMatchIndexEntry(data, len, off, matches[i]);
        if (status != ParseStatus::Ok)
            return status;
        if (i != 0u) {
            const MatchIndexEntry& previous = matches[i - 1];
            if (matches[i].ordinal <= previous.ordinal
                || matches[i].matchId == previous.matchId
                || matches[i].matchStartOffset < previous.firstInputOffset) {
                return ParseStatus::BadMatchIndex;
            }
        }
    }
    if (!getBytes(
            data, len, off, decoded.fileDigest.data(),
            decoded.fileDigest.size())) {
        return ParseStatus::Truncated;
    }
    decoded.matches.swap(matches);
    out = decoded;
    return ParseStatus::Ok;
}

bool isValidFrame(const ReplayFrameV1& frame) {
    if (!isKnownInputDomain(static_cast<uint8_t>(frame.inputDomain)))
        return false;
    if ((frame.activeLaneMask & static_cast<uint8_t>(~kLaneMask)) != 0u)
        return false;
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        const bool active = (frame.activeLaneMask & (1u << lane)) != 0u;
        if (active) {
            if (!isValidActionMask(frame.actions[lane]))
                return false;
        } else if (frame.actions[lane] != 0u) {
            return false;
        }
    }
    return true;
}

bool isValidEvent(const ReplayEvent& event) {
    if (!isKnownEventKind(static_cast<uint16_t>(event.kind)))
        return false;
    if (!isKnownTerminalReason(static_cast<uint16_t>(event.terminalReason)))
        return false;
    if ((event.selectorProofMask & static_cast<uint8_t>(~kSlotMask)) != 0u)
        return false;
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        if (!validIdentity(event.roster[slot]))
            return false;
        const bool present = (event.selectorProofMask & (1u << slot)) != 0u;
        if (present) {
            if (!validSelectorProof(event.selectorProof[slot]))
                return false;
        } else if (!zeroSelectorProof(event.selectorProof[slot])) {
            return false;
        }
    }
    if (event.kind != EventKind::RosterSelected) {
        if (event.selectorProofMask != 0u)
            return false;
        for (size_t slot = 0; slot < kFighterSlots; ++slot) {
            if (!inactiveIdentity(event.roster[slot]))
                return false;
        }
    }
    if (event.kind != EventKind::SegmentTerminal
        && event.terminalReason != TerminalReason::Completed) {
        return false;
    }
    if ((event.kind == EventKind::RoundEnded
         || event.kind == EventKind::MatchResult)
        && (event.valueA & ~static_cast<uint64_t>(kSlotMask)) != 0u) {
        return false;
    }
    return true;
}

EventClass eventClassOf(EventKind kind) {
    switch (kind) {
        case EventKind::RosterSelected:
        case EventKind::StageSelected:
        case EventKind::MatchStarted:
        case EventKind::RoundStarted:
        case EventKind::RoundEnded:
        case EventKind::MatchResult:
        case EventKind::RematchRequested:
        case EventKind::ReturnedToCharacterSelect:
        case EventKind::RouteEpochChanged:
            return EventClass::Assertion;
        case EventKind::SessionPaused:
        case EventKind::SessionResumed:
            return EventClass::DeterministicCommand;
        case EventKind::DelayChanged:
        case EventKind::NetworkSample:
        case EventKind::WallClockPause:
            return EventClass::PresentationMetadata;
        case EventKind::SegmentTerminal:
            return EventClass::Terminal;
    }
    return EventClass::Assertion;
}

const char* parseStatusText(ParseStatus status) {
    switch (status) {
        case ParseStatus::Ok: return "ok";
        case ParseStatus::Truncated: return "truncated";
        case ParseStatus::BadMagic: return "bad magic";
        case ParseStatus::BadPrefixCrc: return "bad prefix CRC";
        case ParseStatus::UnsupportedContainerVersion:
            return "unsupported container version";
        case ParseStatus::UnsupportedFeature: return "unsupported feature";
        case ParseStatus::BadSyncWord: return "bad chunk sync word";
        case ParseStatus::BadChunkHeaderCrc: return "bad chunk header CRC";
        case ParseStatus::BadChunkPayloadCrc: return "bad chunk payload CRC";
        case ParseStatus::UnknownCriticalChunk:
            return "unknown critical chunk";
        case ParseStatus::UnknownChunkCodec: return "unknown chunk codec";
        case ParseStatus::DuplicateSingletonChunk:
            return "duplicate singleton chunk";
        case ParseStatus::SequenceRegression: return "chunk sequence regression";
        case ParseStatus::MissingManifest: return "missing session manifest";
        case ParseStatus::MissingSessionStart: return "missing session start";
        case ParseStatus::MissingFooter: return "missing footer index";
        case ParseStatus::LimitExceeded: return "parser limit exceeded";
        case ParseStatus::IntegerOverflow: return "integer overflow";
        case ParseStatus::BadActionMask: return "bad action mask";
        case ParseStatus::BadLaneShape: return "bad lane shape";
        case ParseStatus::FrameGap: return "frame gap";
        case ParseStatus::FrameConflict: return "conflicting frame records";
        case ParseStatus::DigestMismatch: return "rolling digest mismatch";
        case ParseStatus::FileDigestMismatch: return "file digest mismatch";
        case ParseStatus::BadEventPayload: return "bad event payload";
        case ParseStatus::BadMatchIndex: return "bad match index";
        case ParseStatus::BadUtf8: return "bad UTF-8 text";
        case ParseStatus::IoError: return "I/O error";
        case ParseStatus::BadPalettePayload:
            return "malformed custom palette";
    }
    return "unknown parse status";
}

const char* compatibilityStateText(CompatibilityState state) {
    switch (state) {
        case CompatibilityState::Exact: return "Exact";
        case CompatibilityState::Compatible: return "Compatible";
        case CompatibilityState::FileIntact: return "File intact";
        case CompatibilityState::SimulationVerified:
            return "Simulation verified";
        case CompatibilityState::Recording: return "Recording";
        case CompatibilityState::Incomplete: return "Incomplete";
        case CompatibilityState::Recoverable: return "Recoverable";
        case CompatibilityState::ContentMismatch: return "Content mismatch";
        case CompatibilityState::Unsupported: return "Unsupported";
        case CompatibilityState::Corrupt: return "Corrupt";
    }
    return "Unsupported";
}

const char* compatibilityReasonText(CompatibilityReason reason) {
    switch (reason) {
        case CompatibilityReason::None: return "none";
        case CompatibilityReason::ContainerMajorTooNew:
            return "recorded by a newer replay container";
        case CompatibilityReason::ExecutableMismatch:
            return "different game executable";
        case CompatibilityReason::RuntimeSimulationMismatch:
            return "different simulation runtime";
        case CompatibilityReason::StateRegionMismatch:
            return "different state region set";
        case CompatibilityReason::StateHashProjectionMismatch:
            return "different state hash projection";
        case CompatibilityReason::BootstrapSchemaMismatch:
            return "different bootstrap schema";
        case CompatibilityReason::EventSchemaMismatch:
            return "different event schema";
        case CompatibilityReason::ContentManifestMismatch:
            return "different game content";
        case CompatibilityReason::CharacterCatalogMismatch:
            return "different character catalog";
        case CompatibilityReason::UnsupportedCheckpointCodec:
            return "unsupported checkpoint codec";
        case CompatibilityReason::UnsupportedTeamConfiguration:
            return "unsupported team configuration";
        case CompatibilityReason::UnsupportedCpuContract:
            return "unsupported CPU playback contract";
        case CompatibilityReason::UnsupportedInputDomain:
            return "unsupported input domain";
        case CompatibilityReason::MissingFooter:
            return "missing footer index";
        case CompatibilityReason::StructuralCorruption:
            return "structural corruption";
        case CompatibilityReason::ActivelyRecording:
            return "still being recorded";
    }
    return "unknown reason";
}

}
