#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace qreplay {

constexpr uint32_t kReplayMagic = 0x52393951u;
constexpr uint32_t kChunkSyncWord = 0x4B4E4843u;

constexpr uint16_t kContainerVersionMajor = 1u;
constexpr uint16_t kContainerVersionMinor = 0u;
constexpr uint16_t kContainerReaderMajor = 1u;

constexpr uint16_t kContainerVersionMajorV2 = 2u;
constexpr uint16_t kContainerVersionMinorV2 = 0u;
constexpr uint16_t kContainerReaderMajorV2 = 2u;

constexpr const char* kReplayExtension = ".q99r";

constexpr uint64_t kMaxFileBytes = 512ull * 1024ull * 1024ull;
constexpr uint32_t kMaxChunkPayloadBytes = 8u * 1024u * 1024u;
constexpr uint32_t kMaxCheckpointDecodedBytes = 64u * 1024u * 1024u;
constexpr uint32_t kMaxMatchesPerSet = 4096u;
constexpr uint32_t kMaxChunksPerFile = 1000000u;
constexpr uint32_t kMaxTimelineFrames = 5236000u;
constexpr uint32_t kMaxRecordsPerInputBlock = 4096u;
constexpr uint32_t kMaxEventsPerBlock = 1024u;
constexpr size_t kMaxLabelBytes = 64u;
constexpr size_t kMaxNoteBytes = 1024u;
constexpr size_t kMaxTagBytes = 32u;
constexpr size_t kMaxTags = 16u;
constexpr uint32_t kMaxCodecExpansionRatio = 64u;

constexpr size_t kSourceLanes = 4u;
constexpr size_t kFighterSlots = 4u;

// Compact action mask: bits 0..3 = A,B,C,D and bits 8..11 = Up,Down,Left,Right; any other bit is a hard decode failure. Native code uses the expanded form (qinput::kLogicalFightMask).
constexpr uint16_t kActionMask = 0x0F0Fu;

constexpr bool isValidActionMask(uint16_t action) {
    return (action & ~kActionMask) == 0u;
}

enum class InputDomain : uint8_t {
    LogicalSourceV1 = 1,
    EffectiveFighterV1 = 2,
};

constexpr bool isKnownInputDomain(uint8_t value) {
    return value == static_cast<uint8_t>(InputDomain::LogicalSourceV1)
        || value == static_cast<uint8_t>(InputDomain::EffectiveFighterV1);
}

enum class SlotOwnership : uint8_t {
    Inactive = 0,
    RecordedHuman = 1,
    NativeCpu = 2,
};

constexpr bool isKnownSlotOwnership(uint8_t value) { return value <= 2u; }

enum class CpuContract : uint8_t {
    None = 0,
    NativeAiDeterministicV1 = 1,
    EffectiveFighterV1 = 2,
};

constexpr bool isKnownCpuContract(uint8_t value) { return value <= 2u; }

struct FighterIdentityV1 {
    uint8_t nativeCharacterId = 0xFFu; // 0..33 selectable; 0xFF = inactive.
    uint8_t paletteIndex = 0u;
};

struct SelectorProofV1 {
    uint8_t cursorCell = 0u;
    uint8_t alternateFlag = 0u;
    uint8_t confirmState = 0u;  // native selector lifecycle 0..4
};

constexpr uint8_t kInactiveCharacterId = 0xFFu;
constexpr uint8_t kMaxSelectableCharacterId = 33u;
constexpr uint8_t kSelectorCells = 24u;
constexpr uint8_t kMaxPaletteIndex = 3u;

enum class BootstrapKind : uint8_t {
    NetworkVsDirectBootV1 = 1,
    OfflineVsDirectBootV1 = 2,
    NativeOfflineMenuS0V1 = 3,
};

constexpr bool isKnownBootstrapKind(uint8_t value) {
    return value >= 1u && value <= 3u;
}

enum class ReplaySourceKind : uint8_t {
    OfflineVsPlayer = 1,
    NetplayPlayer = 2,
    SpectatorSave = 3,
    RecoveredPrefix = 4,
};

constexpr bool isKnownSourceKind(uint8_t value) {
    return value >= 1u && value <= 4u;
}

enum class ChunkKind : uint16_t {
    SessionManifest = 1,
    SessionStart = 2,
    MatchStart = 3,
    InputBlock = 4,
    EventBlock = 5,
    ProofBlock = 6,
    Checkpoint = 7,
    MatchEnd = 8,
    SessionEnd = 9,
    FooterIndex = 10,
};

constexpr bool isCriticalChunkKind(ChunkKind kind) {
    switch (kind) {
        case ChunkKind::SessionManifest:
        case ChunkKind::SessionStart:
        case ChunkKind::MatchStart:
        case ChunkKind::InputBlock:
        case ChunkKind::MatchEnd:
        case ChunkKind::SessionEnd:
        case ChunkKind::FooterIndex:
            return true;
        case ChunkKind::EventBlock:
        case ChunkKind::ProofBlock:
        case ChunkKind::Checkpoint:
            return false;
    }
    return true;
}

constexpr bool isKnownChunkKind(uint16_t value) {
    return value >= 1u && value <= 10u;
}

constexpr bool isSingletonChunkKind(ChunkKind kind) {
    return kind == ChunkKind::SessionManifest
        || kind == ChunkKind::SessionStart
        || kind == ChunkKind::SessionEnd
        || kind == ChunkKind::FooterIndex;
}

enum class ChunkCodec : uint8_t {
    Raw = 0,
};

constexpr bool isKnownChunkCodec(uint8_t value) { return value == 0u; }

constexpr size_t kPrefixBytes = 48u;

struct ReplayPrefix {
    uint16_t containerMajor = kContainerVersionMajor;
    uint16_t containerMinor = kContainerVersionMinor;
    uint16_t minimumReaderMajor = kContainerReaderMajor;
    uint16_t prefixBytes = static_cast<uint16_t>(kPrefixBytes);
    uint32_t featureFlags = 0u;
    uint64_t sessionUuidLow = 0u;
    uint64_t sessionUuidHigh = 0u;
};

constexpr uint32_t kFeatureFourPlayer = 0x00000001u;
constexpr uint32_t kFeatureCpuFighters = 0x00000002u;
constexpr uint32_t kFeaturePortableCheckpoint = 0x00000004u;
constexpr uint32_t kFeatureRequiredMask =
    kFeatureFourPlayer | kFeatureCpuFighters | kFeaturePortableCheckpoint;

constexpr uint32_t kFeatureCompatibilityIdentityV3 = 0x00000008u;
constexpr uint32_t kFeatureRequiredMaskV2 =
    kFeatureRequiredMask | kFeatureCompatibilityIdentityV3;

constexpr size_t kChunkHeaderBytes = 48u;

struct ChunkHeader {
    ChunkKind kind = ChunkKind::InputBlock;
    uint16_t schemaVersion = 1u;
    uint8_t critical = 1u;
    ChunkCodec codec = ChunkCodec::Raw;
    uint32_t sequence = 0u;
    uint32_t matchId = 0u;
    uint32_t firstTimelineFrame = 0u;
    uint32_t lastTimelineFrame = 0u;
    uint32_t recordCount = 0u;
    uint32_t payloadBytes = 0u;
    uint32_t decodedBytes = 0u;
    uint32_t payloadCrc32c = 0u;
    uint32_t headerCrc32c = 0u;  // Computed over the header with this field zeroed.
};

struct SimulationContract {
    std::array<uint8_t, 32> executableSha256{};
    uint64_t runtimeSimulationId = 0u;
    uint64_t stateRegionFingerprint = 0u;
    uint64_t stateHashProjectionId = 0u;
    uint64_t stateHashAlgorithmId = 0u;
    uint64_t bootstrapCodecVersion = 0u;
    uint64_t eventSchemaVersion = 0u;
    std::array<uint8_t, 32> contentManifestRoot{};
    uint64_t characterCatalogId = 0u;
    uint32_t fpuControlWord = 0x027Fu;
    uint32_t mxcsr = 0x1F80u;
};

struct SourceLaneDescriptor {
    bool active = false;
    std::string name;
};

struct FighterSlotDescriptor {
    SlotOwnership ownership = SlotOwnership::Inactive;
    uint8_t side = 0u; // 0 = slots 0+2 (P1+P3), 1 = slots 1+3 (P2+P4)
    FighterIdentityV1 identity{};
    SelectorProofV1 selectorProof{};
    bool selectorProofPresent = false;
};

struct DisplayMetadata {
    int64_t recordedUnixSeconds = 0;
    int32_t timezoneOffsetMinutes = 0;
    ReplaySourceKind sourceKind = ReplaySourceKind::OfflineVsPlayer;
    std::string routeLabel;
    uint16_t playerOneDelay = 0u;
    uint16_t playerTwoDelay = 0u;
    uint16_t rollbackWindow = 0u;
    uint32_t averageRttMicros = 0u;
    uint32_t lossPermille = 0u;
    bool networkMetadataPresent = false;
    std::string title;
};

struct SessionManifest {
    SimulationContract contract{};
    BootstrapKind bootstrapKind = BootstrapKind::OfflineVsDirectBootV1;
    uint8_t vsRow = 1u;                 // native VS layout row 0..5
    uint8_t cpuMask = 0u;               // native fighter+1400 ownership bits
    CpuContract cpuContract = CpuContract::None;
    // Native four-entry fighter-slot -> logical-source route (dword_4A37C4).
    std::array<uint8_t, kFighterSlots> slotToSourceRoute{{0u, 1u, 2u, 3u}};
    std::array<SourceLaneDescriptor, kSourceLanes> lanes{};
    std::array<FighterSlotDescriptor, kFighterSlots> slots{};
    uint32_t rosterEpoch = 0u;
    DisplayMetadata display{};
};

struct DeterministicStart {
    uint32_t seedGameplay = 0u;
    uint32_t seedCosmetic = 0u;
    uint32_t selector = 0u;
    uint32_t initialPhase = 0u;
    uint64_t bootstrapHash = 0u;
    uint32_t timerMode = 0u;
    uint32_t aiDifficulty = 0u;
    uint32_t lifeMultiplier = 0u;
    uint32_t roundsToWinMinusOne = 0u;
    uint32_t playerOneNameCodePage = 0u;
    uint32_t playerTwoNameCodePage = 0u;
    std::array<std::array<char, 32>, kSourceLanes> displayNames{};
    uint32_t firstTimelineFrame = 0u;
    uint64_t initialStateHash = 0u;
    uint64_t initialDigest = 0u;
};

struct LifecycleTag {
    uint32_t scene = 0u;
    uint32_t requestedScene = 0u;
    uint32_t sceneThreeState = 0u;
    uint32_t matchState = 0u;
    uint32_t roundState = 0u;
    uint32_t layoutGeneration = 0u;

    bool operator==(const LifecycleTag& o) const {
        return scene == o.scene && requestedScene == o.requestedScene
            && sceneThreeState == o.sceneThreeState
            && matchState == o.matchState && roundState == o.roundState
            && layoutGeneration == o.layoutGeneration;
    }
    bool operator!=(const LifecycleTag& o) const { return !(*this == o); }
};

struct ReplayFrameV1 {
    uint32_t timelineFrame = 0u;
    InputDomain inputDomain = InputDomain::LogicalSourceV1;
    uint8_t activeLaneMask = 0u; // bit N = lane N carries an action value
    std::array<uint16_t, kSourceLanes> actions{};
    uint32_t routeEpoch = 0u;
    uint64_t preStateHash = 0u;
    uint64_t postStateHash = 0u;
    LifecycleTag lifecycleBefore{};
    LifecycleTag lifecycleAfter{};
    int32_t dispatcherResult = 0;
    uint64_t rollingDigest = 0u;

    bool operator==(const ReplayFrameV1& o) const {
        return timelineFrame == o.timelineFrame
            && inputDomain == o.inputDomain
            && activeLaneMask == o.activeLaneMask
            && actions == o.actions
            && routeEpoch == o.routeEpoch
            && preStateHash == o.preStateHash
            && postStateHash == o.postStateHash
            && lifecycleBefore == o.lifecycleBefore
            && lifecycleAfter == o.lifecycleAfter
            && dispatcherResult == o.dispatcherResult
            && rollingDigest == o.rollingDigest;
    }
    bool operator!=(const ReplayFrameV1& o) const { return !(*this == o); }
};

bool isValidFrame(const ReplayFrameV1& frame);

enum class EventClass : uint8_t {
    DeterministicCommand = 1,
    Assertion = 2,
    PresentationMetadata = 3,
    Terminal = 4,
};

enum class EventKind : uint16_t {
    RosterSelected = 1,
    StageSelected = 2,
    MatchStarted = 3,
    RoundStarted = 4,
    RoundEnded = 5,
    MatchResult = 6,
    RematchRequested = 7,
    ReturnedToCharacterSelect = 8,
    RouteEpochChanged = 9,
    SessionPaused = 20,
    SessionResumed = 21,
    DelayChanged = 40,
    NetworkSample = 41,
    WallClockPause = 42,
    SegmentTerminal = 60,
};

constexpr bool isKnownEventKind(uint16_t value) {
    switch (static_cast<EventKind>(value)) {
        case EventKind::RosterSelected:
        case EventKind::StageSelected:
        case EventKind::MatchStarted:
        case EventKind::RoundStarted:
        case EventKind::RoundEnded:
        case EventKind::MatchResult:
        case EventKind::RematchRequested:
        case EventKind::ReturnedToCharacterSelect:
        case EventKind::RouteEpochChanged:
        case EventKind::SessionPaused:
        case EventKind::SessionResumed:
        case EventKind::DelayChanged:
        case EventKind::NetworkSample:
        case EventKind::WallClockPause:
        case EventKind::SegmentTerminal:
            return true;
    }
    return false;
}

EventClass eventClassOf(EventKind kind);

enum class TerminalReason : uint16_t {
    Completed = 1,
    Disconnected = 2,
    ProcessExit = 3,
    RecordingFailure = 4,
    RecoveredPrefix = 5,
    RecorderOverflow = 6,
};

constexpr bool isKnownTerminalReason(uint16_t value) {
    return value >= 1u && value <= 6u;
}

struct ReplayEvent {
    uint32_t timelineFrame = 0u;
    EventKind kind = EventKind::MatchStarted;
    uint32_t matchId = 0u;
    std::array<FighterIdentityV1, kFighterSlots> roster{};
    std::array<SelectorProofV1, kFighterSlots> selectorProof{};
    uint8_t selectorProofMask = 0u;
    uint32_t rosterEpoch = 0u;
    uint64_t valueA = 0u;
    uint64_t valueB = 0u;
    uint32_t valueC = 0u;
    TerminalReason terminalReason = TerminalReason::Completed;
    uint64_t stateProof = 0u;
    uint64_t rollingDigest = 0u;
};

bool isValidEvent(const ReplayEvent& event);

struct MatchStartRecord {
    uint32_t matchId = 0u;
    uint32_t ordinal = 0u;
    uint32_t firstTimelineFrame = 0u;
    uint32_t dependsOnMatchId = 0u;
    bool independentBootstrap = false;
    DeterministicStart start{};
    std::array<FighterSlotDescriptor, kFighterSlots> slots{};
    std::array<uint8_t, kFighterSlots> slotToSourceRoute{{0u, 1u, 2u, 3u}};
    uint8_t vsRow = 1u;
    uint8_t cpuMask = 0u;
    uint32_t rosterEpoch = 0u;
    uint32_t stageId = 0u;
    bool stageVerified = false;
};

struct MatchEndRecord {
    uint32_t matchId = 0u;
    uint32_t lastTimelineFrame = 0u;
    // Bit N set = fighter slot N was on the winning side.  Zero = draw/abort.
    uint8_t winningSideMask = 0u;
    uint8_t roundsSideZero = 0u;
    uint8_t roundsSideOne = 0u;
    TerminalReason reason = TerminalReason::Completed;
    uint64_t postTailStateHash = 0u;
    uint64_t rollingDigest = 0u;
};

struct SessionEndRecord {
    uint32_t lastTimelineFrame = 0u;
    TerminalReason reason = TerminalReason::Completed;
    uint64_t postTailStateHash = 0u;
    uint64_t rollingDigest = 0u;
    uint32_t matchCount = 0u;
};

struct MatchIndexEntry {
    uint32_t matchId = 0u;
    uint32_t ordinal = 0u;
    uint64_t matchStartOffset = 0u;
    uint64_t firstInputOffset = 0u;
    uint32_t firstTimelineFrame = 0u;
    uint32_t lastTimelineFrame = 0u;
    uint32_t frameCount = 0u;
    uint32_t roundCount = 0u;
    uint8_t winningSideMask = 0u;
    bool independentBootstrap = false;
    std::array<FighterIdentityV1, kFighterSlots> roster{};
    std::array<SlotOwnership, kFighterSlots> ownership{};
};

struct FooterIndex {
    uint32_t chunkCount = 0u;
    uint32_t matchCount = 0u;
    uint32_t totalFrames = 0u;
    uint64_t finalRollingDigest = 0u;
    std::vector<MatchIndexEntry> matches;
    std::array<uint8_t, 32> fileDigest{};
};

struct ReplayFile {
    ReplayPrefix prefix{};
    SessionManifest manifest{};
    DeterministicStart sessionStart{};
    std::vector<MatchStartRecord> matchStarts;
    std::vector<MatchEndRecord> matchEnds;
    std::vector<ReplayFrameV1> frames;
    std::vector<ReplayEvent> events;
    bool hasSessionEnd = false;
    SessionEndRecord sessionEnd{};
    bool hasFooter = false;
    FooterIndex footer{};
};

constexpr uint32_t kMaxPlaybackPrebufferFramesV2 = 300000u;
constexpr size_t kMaxEventsPerTickV2 = 8u;
constexpr size_t kFixedReplayTextBytesV2 = 64u;
constexpr uint8_t kFighterMaskV2 = 0x0Fu;
constexpr uint8_t kSideMaskV2 = 0x03u;
constexpr uint32_t kMinNativeStageIdV2 = 1u;
constexpr uint32_t kMaxNativeStageIdV2 = 28u;
constexpr uint32_t kMaxTimelineEventsV2 =
    kMaxTimelineFrames
    * static_cast<uint32_t>(2u * kMaxEventsPerTickV2);

template <size_t Capacity>
struct FixedUtf8V2 {
    uint8_t bytes = 0u;
    std::array<char, Capacity> data{};

    bool operator==(const FixedUtf8V2& o) const {
        return bytes == o.bytes && data == o.data;
    }
    bool operator!=(const FixedUtf8V2& o) const { return !(*this == o); }
};

using ReplayTextV2 = FixedUtf8V2<kFixedReplayTextBytesV2>;

struct GameIdentityV2 {
    uint64_t gameUuidLow = 0u;
    uint64_t gameUuidHigh = 0u;
    uint32_t gameGeneration = 0u;

    bool operator==(const GameIdentityV2& o) const {
        return gameUuidLow == o.gameUuidLow
            && gameUuidHigh == o.gameUuidHigh
            && gameGeneration == o.gameGeneration;
    }
    bool operator!=(const GameIdentityV2& o) const { return !(*this == o); }
};

enum class ReplayVerificationModeV2 : uint8_t {
    ByteExact = 1,
    BehaviourVerified = 2,
};

inline ReplayVerificationModeV2 replayVerificationModeForV2(
    const GameIdentityV2& identity) {
    return identity.gameGeneration > 1u
        ? ReplayVerificationModeV2::BehaviourVerified
        : ReplayVerificationModeV2::ByteExact;
}

inline const char* replayVerificationModeTextV2(
    ReplayVerificationModeV2 mode) {
    switch (mode) {
        case ReplayVerificationModeV2::ByteExact:
            return "byte-exact";
        case ReplayVerificationModeV2::BehaviourVerified:
            return "behaviour-verified";
    }
    return "unknown";
}

struct SimulationContractV2 {
    std::array<uint8_t, 32> executableSha256{};
    uint64_t nativeBuildIdentity = 0u;
    uint64_t runtimeSimulationId = 0u;
    uint64_t stateRegionSchemaFingerprint = 0u;
    uint64_t stateHashProjectionId = 0u;
    uint64_t stateHashAlgorithmId = 0u;
    std::array<uint8_t, 32> contentManifestRoot{};
    uint64_t bootstrapSchemaId = 0u;
    uint64_t eventSchemaId = 0u;
    uint64_t characterCatalogId = 0u;
    uint64_t terminalResultSchemaId = 0u;
    uint32_t compactActionDomainVersion = 0u;

    bool operator==(const SimulationContractV2& o) const {
        return executableSha256 == o.executableSha256
            && nativeBuildIdentity == o.nativeBuildIdentity
            && runtimeSimulationId == o.runtimeSimulationId
            && stateRegionSchemaFingerprint
                   == o.stateRegionSchemaFingerprint
            && stateHashProjectionId == o.stateHashProjectionId
            && stateHashAlgorithmId == o.stateHashAlgorithmId
            && contentManifestRoot == o.contentManifestRoot
            && bootstrapSchemaId == o.bootstrapSchemaId
            && eventSchemaId == o.eventSchemaId
            && characterCatalogId == o.characterCatalogId
            && terminalResultSchemaId == o.terminalResultSchemaId
            && compactActionDomainVersion == o.compactActionDomainVersion;
    }
    bool operator!=(const SimulationContractV2& o) const {
        return !(*this == o);
    }
};

enum class BootstrapKindV2 : uint8_t {
    MatchActiveDirectBootV1 = 1,
};

constexpr bool isKnownBootstrapKindV2(uint8_t value) {
    return value
        == static_cast<uint8_t>(BootstrapKindV2::MatchActiveDirectBootV1);
}

enum class ReplaySourceKindV2 : uint8_t {
    OfflineVsPlayer = 1,
    NetplayPlayer = 2,
    SpectatorSave = 3,
    RecoveredPrefix = 4,
};

constexpr bool isKnownReplaySourceKindV2(uint8_t value) {
    return value >= 1u && value <= 4u;
}

struct FighterIdentityV2 {
    uint8_t nativeCharacterId = kInactiveCharacterId;
    uint8_t paletteIndex = 0u;

    bool operator==(const FighterIdentityV2& o) const {
        return nativeCharacterId == o.nativeCharacterId
            && paletteIndex == o.paletteIndex;
    }
    bool operator!=(const FighterIdentityV2& o) const {
        return !(*this == o);
    }
};

struct SelectorProofV2 {
    uint8_t cursorCell = 0u;
    uint8_t alternateFlag = 0u;
    uint8_t confirmState = 0u;

    bool operator==(const SelectorProofV2& o) const {
        return cursorCell == o.cursorCell
            && alternateFlag == o.alternateFlag
            && confirmState == o.confirmState;
    }
    bool operator!=(const SelectorProofV2& o) const {
        return !(*this == o);
    }
};

struct SourceLaneDescriptorV2 {
    bool active = false;
    ReplayTextV2 name{};

    bool operator==(const SourceLaneDescriptorV2& o) const {
        return active == o.active && name == o.name;
    }
    bool operator!=(const SourceLaneDescriptorV2& o) const {
        return !(*this == o);
    }
};

struct FighterSlotDescriptorV2 {
    SlotOwnership ownership = SlotOwnership::Inactive;
    uint8_t side = 0u;
    FighterIdentityV2 identity{};
    bool selectorProofPresent = false;
    SelectorProofV2 selectorProof{};

    bool operator==(const FighterSlotDescriptorV2& o) const {
        return ownership == o.ownership && side == o.side
            && identity == o.identity
            && selectorProofPresent == o.selectorProofPresent
            && selectorProof == o.selectorProof;
    }
    bool operator!=(const FighterSlotDescriptorV2& o) const {
        return !(*this == o);
    }
};

struct StageIdentityV2 {
    uint32_t nativeStageId = 0u;
    bool verified = false;
    uint64_t catalogEntryId = 0u;

    bool operator==(const StageIdentityV2& o) const {
        return nativeStageId == o.nativeStageId
            && verified == o.verified
            && catalogEntryId == o.catalogEntryId;
    }
    bool operator!=(const StageIdentityV2& o) const {
        return !(*this == o);
    }
};

enum DisplayMetadataFlagV2 : uint8_t {
    DisplayMetadataNetworkV2 = 1u << 0,
    DisplayMetadataSeriesV2 = 1u << 1,
    DisplayMetadataPreviousGameDigestV2 = 1u << 2,
};

struct DisplayMetadataV2 {
    int64_t recordedUnixSeconds = 0;
    int32_t timezoneOffsetMinutes = 0;
    ReplaySourceKindV2 sourceKind = ReplaySourceKindV2::OfflineVsPlayer;
    uint8_t flags = 0u;
    uint16_t playerOneDelay = 0u;
    uint16_t playerTwoDelay = 0u;
    uint16_t rollbackWindow = 0u;
    uint32_t averageRttMicros = 0u;
    uint32_t jitterMicros = 0u;
    uint32_t lossPermille = 0u;
    ReplayTextV2 title{};
    ReplayTextV2 routeLabel{};
    uint64_t seriesUuidLow = 0u;
    uint64_t seriesUuidHigh = 0u;
    uint32_t gameOrdinalInSeries = 0u;
    std::array<uint8_t, 32> previousGameDigest{};

    bool operator==(const DisplayMetadataV2& o) const {
        return recordedUnixSeconds == o.recordedUnixSeconds
            && timezoneOffsetMinutes == o.timezoneOffsetMinutes
            && sourceKind == o.sourceKind && flags == o.flags
            && playerOneDelay == o.playerOneDelay
            && playerTwoDelay == o.playerTwoDelay
            && rollbackWindow == o.rollbackWindow
            && averageRttMicros == o.averageRttMicros
            && jitterMicros == o.jitterMicros
            && lossPermille == o.lossPermille && title == o.title
            && routeLabel == o.routeLabel
            && seriesUuidLow == o.seriesUuidLow
            && seriesUuidHigh == o.seriesUuidHigh
            && gameOrdinalInSeries == o.gameOrdinalInSeries
            && previousGameDigest == o.previousGameDigest;
    }
    bool operator!=(const DisplayMetadataV2& o) const {
        return !(*this == o);
    }
};

struct GameManifestV2 {
    GameIdentityV2 identity{};
    SimulationContractV2 contract{};
    BootstrapKindV2 bootstrapKind =
        BootstrapKindV2::MatchActiveDirectBootV1;
    uint8_t vsRow = 1u;
    uint8_t activeLaneMask = 0x03u;
    uint8_t activeFighterMask = 0x03u;
    uint8_t cpuMask = 0u;
    CpuContract cpuContract = CpuContract::None;
    std::array<uint8_t, kFighterSlots> slotToSourceRoute{
        {0u, 1u, 2u, 3u}};
    std::array<SourceLaneDescriptorV2, kSourceLanes> lanes{};
    std::array<FighterSlotDescriptorV2, kFighterSlots> slots{};
    uint32_t routeEpoch = 0u;
    uint32_t rosterEpoch = 0u;
    uint32_t layoutGeneration = 0u;
    StageIdentityV2 stage{};
    DisplayMetadataV2 display{};

    bool operator==(const GameManifestV2& o) const {
        return identity == o.identity && contract == o.contract
            && bootstrapKind == o.bootstrapKind && vsRow == o.vsRow
            && activeLaneMask == o.activeLaneMask
            && activeFighterMask == o.activeFighterMask
            && cpuMask == o.cpuMask && cpuContract == o.cpuContract
            && slotToSourceRoute == o.slotToSourceRoute && lanes == o.lanes
            && slots == o.slots && routeEpoch == o.routeEpoch
            && rosterEpoch == o.rosterEpoch
            && layoutGeneration == o.layoutGeneration && stage == o.stage
            && display == o.display;
    }
    bool operator!=(const GameManifestV2& o) const {
        return !(*this == o);
    }
};

struct MatchConfigurationV2 {
    uint32_t timerMode = 0u;
    uint32_t aiDifficulty = 0u;
    uint32_t lifeMultiplier = 0u;
    uint32_t roundsToWinMinusOne = 0u;

    bool operator==(const MatchConfigurationV2& o) const {
        return timerMode == o.timerMode && aiDifficulty == o.aiDifficulty
            && lifeMultiplier == o.lifeMultiplier
            && roundsToWinMinusOne == o.roundsToWinMinusOne;
    }
    bool operator!=(const MatchConfigurationV2& o) const {
        return !(*this == o);
    }
};

struct RngStateV2 {
    uint32_t gameplay = 0u;
    uint32_t cosmetic = 0u;
    uint32_t selector = 0u;

    bool operator==(const RngStateV2& o) const {
        return gameplay == o.gameplay && cosmetic == o.cosmetic
            && selector == o.selector;
    }
    bool operator!=(const RngStateV2& o) const { return !(*this == o); }
};

struct FpuContractV2 {
    uint16_t x87ControlWord = 0x027Fu;
    uint32_t mxcsr = 0x1F80u;

    bool operator==(const FpuContractV2& o) const {
        return x87ControlWord == o.x87ControlWord && mxcsr == o.mxcsr;
    }
    bool operator!=(const FpuContractV2& o) const { return !(*this == o); }
};

constexpr size_t kNativeDisplayNameBytesV2 = 32u;
constexpr size_t kNativeDisplayNamePayloadBytesV2 = 20u;

struct NativeDisplayStateV2 {
    std::array<uint32_t, kSourceLanes> codePages{};
    std::array<
        std::array<char, kNativeDisplayNameBytesV2>, kSourceLanes> names{};

    bool operator==(const NativeDisplayStateV2& o) const {
        return codePages == o.codePages && names == o.names;
    }
    bool operator!=(const NativeDisplayStateV2& o) const {
        return !(*this == o);
    }
};

struct DeterministicGameStartV2 {
    MatchConfigurationV2 configuration{};
    RngStateV2 rng{};
    FpuContractV2 fpu{};
    NativeDisplayStateV2 nativeDisplay{};
    uint32_t initialPhase = 0u;
    uint64_t bootstrapFingerprint = 0u;
    uint64_t frameZeroPreStateHash = 0u;
    uint64_t initialRollingDigest = 0u;

    bool operator==(const DeterministicGameStartV2& o) const {
        return configuration == o.configuration && rng == o.rng
            && fpu == o.fpu && nativeDisplay == o.nativeDisplay
            && initialPhase == o.initialPhase
            && bootstrapFingerprint == o.bootstrapFingerprint
            && frameZeroPreStateHash == o.frameZeroPreStateHash
            && initialRollingDigest == o.initialRollingDigest;
    }
    bool operator!=(const DeterministicGameStartV2& o) const {
        return !(*this == o);
    }
};

struct ConfirmedGameStartV2 {
    GameManifestV2 manifest{};
    DeterministicGameStartV2 start{};

    bool operator==(const ConfirmedGameStartV2& o) const {
        return manifest == o.manifest && start == o.start;
    }
    bool operator!=(const ConfirmedGameStartV2& o) const {
        return !(*this == o);
    }
};

struct GameLifecycleV2 {
    uint32_t scene = 0u;
    uint32_t requestedScene = 0u;
    int32_t matchSub = 0;
    uint32_t sceneThreeState = 0u;
    uint32_t roundState = 0u;
    uint32_t roundCounter = 0u;
    uint32_t resultCounter = 0u;
    int32_t winnerSlot = -1;
    std::array<uint8_t, 2> roundTallies{};
    uint32_t routeEpoch = 0u;
    uint32_t rosterEpoch = 0u;
    uint32_t layoutGeneration = 0u;

    bool operator==(const GameLifecycleV2& o) const {
        return scene == o.scene && requestedScene == o.requestedScene
            && matchSub == o.matchSub
            && sceneThreeState == o.sceneThreeState
            && roundState == o.roundState
            && roundCounter == o.roundCounter
            && resultCounter == o.resultCounter
            && winnerSlot == o.winnerSlot
            && roundTallies == o.roundTallies
            && routeEpoch == o.routeEpoch && rosterEpoch == o.rosterEpoch
            && layoutGeneration == o.layoutGeneration;
    }
    bool operator!=(const GameLifecycleV2& o) const {
        return !(*this == o);
    }
};

struct ConfirmedGameFrameCoreV2 {
    InputDomain inputDomain = InputDomain::LogicalSourceV1;
    uint8_t activeLaneMask = 0u;
    std::array<uint16_t, kSourceLanes> actions{};
    GameLifecycleV2 before{};
    GameLifecycleV2 after{};
    int32_t dispatcherResult = 0;
    uint64_t preStateHash = 0u;
    uint64_t postStateHash = 0u;

    bool operator==(const ConfirmedGameFrameCoreV2& o) const {
        return inputDomain == o.inputDomain
            && activeLaneMask == o.activeLaneMask && actions == o.actions
            && before == o.before && after == o.after
            && dispatcherResult == o.dispatcherResult
            && preStateHash == o.preStateHash
            && postStateHash == o.postStateHash;
    }
    bool operator!=(const ConfirmedGameFrameCoreV2& o) const {
        return !(*this == o);
    }
};

struct ConfirmedGameFrameV2 {
    uint32_t sourceFrame = 0u;
    uint32_t replayFrame = 0u;
    ConfirmedGameFrameCoreV2 core{};
    uint64_t rollingDigest = 0u;

    bool operator==(const ConfirmedGameFrameV2& o) const {
        return sourceFrame == o.sourceFrame && replayFrame == o.replayFrame
            && core == o.core && rollingDigest == o.rollingDigest;
    }
    bool operator!=(const ConfirmedGameFrameV2& o) const {
        return !(*this == o);
    }
};

enum class GameEventPhaseV2 : uint8_t {
    PreFrame = 1,
    PostFrame = 2,
};

constexpr bool isKnownGameEventPhaseV2(uint8_t value) {
    return value >= 1u && value <= 2u;
}

enum class GameEventKindV2 : uint16_t {
    GameStarted = 1,
    RoundStarted = 2,
    RoundEnded = 3,
    GameResult = 4,
    DelayChanged = 40,
    NetworkSample = 41,
};

constexpr bool isKnownGameEventKindV2(uint16_t value) {
    return (value >= 1u && value <= 4u) || value == 40u || value == 41u;
}

constexpr bool isCanonicalGameEventV2(GameEventKindV2 kind) {
    return kind == GameEventKindV2::GameStarted
        || kind == GameEventKindV2::RoundStarted
        || kind == GameEventKindV2::RoundEnded
        || kind == GameEventKindV2::GameResult;
}

struct ConfirmedGameEventV2 {
    uint32_t replayFrame = 0u;
    uint32_t eventSequence = 0u;
    GameEventKindV2 kind = GameEventKindV2::GameStarted;
    GameEventPhaseV2 phase = GameEventPhaseV2::PreFrame;
    uint32_t transitionSiteId = 0u;
    uint8_t roundOrdinal = 0u;
    uint8_t winningSideMask = 0u;
    std::array<uint8_t, 2> roundTallies{};
    int32_t winnerSlot = -1;
    uint16_t playerOneDelay = 0u;
    uint16_t playerTwoDelay = 0u;
    uint16_t rollbackWindow = 0u;
    uint32_t averageRttMicros = 0u;
    uint32_t jitterMicros = 0u;
    uint32_t lossPermille = 0u;
    uint64_t stateProof = 0u;
    uint64_t rollingDigest = 0u;

    bool operator==(const ConfirmedGameEventV2& o) const {
        return replayFrame == o.replayFrame
            && eventSequence == o.eventSequence && kind == o.kind
            && phase == o.phase
            && transitionSiteId == o.transitionSiteId
            && roundOrdinal == o.roundOrdinal
            && winningSideMask == o.winningSideMask
            && roundTallies == o.roundTallies
            && winnerSlot == o.winnerSlot
            && playerOneDelay == o.playerOneDelay
            && playerTwoDelay == o.playerTwoDelay
            && rollbackWindow == o.rollbackWindow
            && averageRttMicros == o.averageRttMicros
            && jitterMicros == o.jitterMicros
            && lossPermille == o.lossPermille
            && stateProof == o.stateProof
            && rollingDigest == o.rollingDigest;
    }
    bool operator!=(const ConfirmedGameEventV2& o) const {
        return !(*this == o);
    }
};

struct FixedEventListV2 {
    uint8_t count = 0u;
    std::array<ConfirmedGameEventV2, kMaxEventsPerTickV2> events{};

    bool operator==(const FixedEventListV2& o) const {
        if (count != o.count || count > kMaxEventsPerTickV2) return false;
        for (size_t i = 0; i < count; ++i)
            if (events[i] != o.events[i]) return false;
        return true;
    }
    bool operator!=(const FixedEventListV2& o) const {
        return !(*this == o);
    }
};

struct CanonicalTickBundleV2 {
    FixedEventListV2 preEvents{};
    ConfirmedGameFrameV2 frame{};
    FixedEventListV2 postEvents{};

    bool operator==(const CanonicalTickBundleV2& o) const {
        return preEvents == o.preEvents && frame == o.frame
            && postEvents == o.postEvents;
    }
    bool operator!=(const CanonicalTickBundleV2& o) const {
        return !(*this == o);
    }
};

struct BeginGameBundleV2 {
    ConfirmedGameStartV2 start{};
    CanonicalTickBundleV2 frameZero{};

    bool operator==(const BeginGameBundleV2& o) const {
        return start == o.start && frameZero == o.frameZero;
    }
    bool operator!=(const BeginGameBundleV2& o) const {
        return !(*this == o);
    }
};

struct ConfirmedGameEndV2 {
    TerminalReason reason = TerminalReason::Completed;
    uint32_t nextRecord = 0u;
    uint8_t winningSideMask = 0u;
    std::array<uint8_t, 2> roundTallies{};
    int32_t winnerSlot = -1;
    uint64_t postTailStateHash = 0u;
    uint64_t finalRollingDigest = 0u;
    std::array<uint8_t, 32> canonicalGameDigest{};

    bool operator==(const ConfirmedGameEndV2& o) const {
        return reason == o.reason && nextRecord == o.nextRecord
            && winningSideMask == o.winningSideMask
            && roundTallies == o.roundTallies
            && winnerSlot == o.winnerSlot
            && postTailStateHash == o.postTailStateHash
            && finalRollingDigest == o.finalRollingDigest
            && canonicalGameDigest == o.canonicalGameDigest;
    }
    bool operator!=(const ConfirmedGameEndV2& o) const {
        return !(*this == o);
    }
};

struct EndGameBundleV2 {
    bool containsFinalFrame = false;
    CanonicalTickBundleV2 finalFrame{};
    ConfirmedGameEndV2 end{};

    bool operator==(const EndGameBundleV2& o) const {
        return containsFinalFrame == o.containsFinalFrame
            && finalFrame == o.finalFrame && end == o.end;
    }
    bool operator!=(const EndGameBundleV2& o) const {
        return !(*this == o);
    }
};

// One slot's custom colours: 256 bytes, four variants of sixteen BGRX entries.
constexpr size_t kReplayPaletteTableBytes = 256u;
constexpr size_t kReplayPaletteBlockBytes = 4u + kReplayPaletteTableBytes;

struct ReplayPaletteV2 {
    uint8_t side = 0;
    uint8_t variant = 0;
    uint8_t present = 0;
    uint8_t reserved = 0;
    uint8_t table[kReplayPaletteTableBytes] = {};
};

enum class PublisherRecordKindV2 : uint8_t {
    BeginGame = 1,
    Tick = 2,
    EndGame = 3,
    Palette = 4,
};

struct PublisherRecordV2 {
    PublisherRecordKindV2 kind = PublisherRecordKindV2::Tick;
    BeginGameBundleV2 begin{};
    CanonicalTickBundleV2 tick{};
    EndGameBundleV2 end{};
    ReplayPaletteV2 palette{};
};

static_assert(
    std::is_trivially_copyable<PublisherRecordV2>::value,
    "publisher records must remain fixed-copy hot-path values");

enum class GameCommitStatusV2 : uint8_t {
    Committed = 1,
    Failed = 2,
    Unknown = 3,
};

struct GameCommitAckV2 {
    GameIdentityV2 identity{};
    GameCommitStatusV2 status = GameCommitStatusV2::Unknown;
    uint32_t nextRecord = 0u;
    std::array<uint8_t, 32> canonicalGameDigest{};
    std::array<uint8_t, 32> fileDigest{};
};

enum RoundTransitionFlagV2 : uint32_t {
    RoundTransitionNoneV2 = 0u,
    RoundBecameActiveV2 = 1u << 0,
    RoundOutcomeCommittedV2 = 1u << 1,
    FinalGameResultCommittedV2 = 1u << 2,
};

struct RoundTransitionProofV2 {
    uint32_t flags = RoundTransitionNoneV2;
    uint32_t transitionSiteId = 0u;
    uint8_t roundOrdinal = 0u;
    uint8_t winningSideMask = 0u;
    int32_t winnerSlot = -1;
    std::array<uint8_t, 2> talliesBefore{};
    std::array<uint8_t, 2> talliesAfter{};
    uint32_t roundStateBefore = 0u;
    uint32_t roundStateAfter = 0u;
    uint32_t roundCounterBefore = 0u;
    uint32_t roundCounterAfter = 0u;
    uint32_t resultCounterBefore = 0u;
    uint32_t resultCounterAfter = 0u;
};

enum class ChunkKindV2 : uint16_t {
    GameManifest = 1,
    GameStart = 2,
    InputBlock = 3,
    EventBlock = 4,
    ProofBlock = 5,
    Checkpoint = 6,
    GameEnd = 7,
    FooterIndex = 8,
    CompatibilityIdentity = 9,
    // Presentation only: this data is in no hash, no proof and no post-state.
    Palette = 10,
};

constexpr bool isKnownChunkKindV2(uint16_t value) {
    return value >= 1u && value <= 10u;
}

constexpr bool isCriticalChunkKindV2(ChunkKindV2 kind) {
    return kind != ChunkKindV2::ProofBlock
        && kind != ChunkKindV2::Checkpoint
        && kind != ChunkKindV2::Palette;
}

struct ReplayPrefixV2 {
    uint16_t containerMajor = kContainerVersionMajorV2;
    uint16_t containerMinor = kContainerVersionMinorV2;
    uint16_t minimumReaderMajor = kContainerReaderMajorV2;
    uint16_t prefixBytes = static_cast<uint16_t>(kPrefixBytes);
    uint32_t featureFlags = 0u;
    uint64_t gameUuidLow = 0u;
    uint64_t gameUuidHigh = 0u;
};

struct ChunkHeaderV2 {
    ChunkKindV2 kind = ChunkKindV2::InputBlock;
    uint16_t schemaVersion = 2u;
    uint8_t critical = 1u;
    ChunkCodec codec = ChunkCodec::Raw;
    uint32_t sequence = 0u;
    uint32_t gameGeneration = 0u;
    uint32_t firstRecord = 0u;
    uint32_t nextRecord = 0u;
    uint32_t recordCount = 0u;
    uint32_t payloadBytes = 0u;
    uint32_t decodedBytes = 0u;
    uint32_t payloadCrc32c = 0u;
    uint32_t headerCrc32c = 0u;
};

struct RoundIndexEntryV2 {
    uint8_t roundOrdinal = 0u;
    uint8_t winningSideMask = 0u;
    std::array<uint8_t, 2> roundTallies{};
    uint32_t firstRecord = 0u;
    uint32_t nextRecord = 0u;
    uint32_t firstEventSequence = 0u;
    uint32_t nextEventSequence = 0u;
    uint64_t firstChunkOffset = 0u;
    uint64_t lastChunkOffset = 0u;

    bool operator==(const RoundIndexEntryV2& o) const {
        return roundOrdinal == o.roundOrdinal
            && winningSideMask == o.winningSideMask
            && roundTallies == o.roundTallies
            && firstRecord == o.firstRecord
            && nextRecord == o.nextRecord
            && firstEventSequence == o.firstEventSequence
            && nextEventSequence == o.nextEventSequence
            && firstChunkOffset == o.firstChunkOffset
            && lastChunkOffset == o.lastChunkOffset;
    }
};

struct FooterIndexV2 {
    uint32_t chunkCount = 0u;
    uint32_t frameCount = 0u;
    uint32_t eventCount = 0u;
    uint64_t manifestOffset = 0u;
    uint64_t startOffset = 0u;
    uint64_t firstInputOffset = 0u;
    uint64_t gameEndOffset = 0u;
    uint64_t finalRollingDigest = 0u;
    std::array<uint8_t, 32> canonicalGameDigest{};
    std::vector<RoundIndexEntryV2> rounds;
    std::array<uint8_t, 32> fileDigest{};
};

bool isValidGameIdentityV2(const GameIdentityV2& identity);
bool isValidSimulationContractV2(
    const SimulationContractV2& contract);
bool isValidGameManifestV2(const GameManifestV2& manifest);
bool isValidDeterministicGameStartV2(const DeterministicGameStartV2& start);
bool isValidConfirmedGameFrameCoreV2(
    const ConfirmedGameFrameCoreV2& frame, uint8_t allowedLaneMask);
bool isValidConfirmedGameEventV2(const ConfirmedGameEventV2& event);
bool isValidConfirmedGameEndV2(const ConfirmedGameEndV2& end);

enum class ParseStatus : uint16_t {
    Ok = 0,
    Truncated,
    BadMagic,
    BadPrefixCrc,
    UnsupportedContainerVersion,
    UnsupportedFeature,
    BadSyncWord,
    BadChunkHeaderCrc,
    BadChunkPayloadCrc,
    UnknownCriticalChunk,
    UnknownChunkCodec,
    DuplicateSingletonChunk,
    SequenceRegression,
    MissingManifest,
    MissingSessionStart,
    MissingFooter,
    LimitExceeded,
    IntegerOverflow,
    BadActionMask,
    BadLaneShape,
    FrameGap,
    FrameConflict,
    DigestMismatch,
    FileDigestMismatch,
    BadEventPayload,
    BadMatchIndex,
    BadUtf8,
    IoError,
    BadPalettePayload,
};

const char* parseStatusText(ParseStatus status);

struct ParseResult {
    ParseStatus status = ParseStatus::Ok;
    uint64_t offset = 0u;
    uint32_t chunkSequence = 0u;
    uint32_t lastValidMatchId = 0u;
    uint32_t lastValidTimelineFrame = 0u;
    std::string detail;

    bool ok() const { return status == ParseStatus::Ok; }
};

enum class CompatibilityState : uint8_t {
    Exact = 0,
    Compatible,
    FileIntact,
    SimulationVerified,
    Recording,
    Incomplete,
    Recoverable,
    ContentMismatch,
    Unsupported,
    Corrupt,
};

const char* compatibilityStateText(CompatibilityState state);

enum class CompatibilityReason : uint16_t {
    None = 0,
    ContainerMajorTooNew,
    ExecutableMismatch,
    RuntimeSimulationMismatch,
    StateRegionMismatch,
    StateHashProjectionMismatch,
    BootstrapSchemaMismatch,
    EventSchemaMismatch,
    ContentManifestMismatch,
    CharacterCatalogMismatch,
    UnsupportedCheckpointCodec,
    UnsupportedTeamConfiguration,
    UnsupportedCpuContract,
    UnsupportedInputDomain,
    MissingFooter,
    StructuralCorruption,
    ActivelyRecording,
};

const char* compatibilityReasonText(CompatibilityReason reason);

struct CompatibilityResult {
    CompatibilityState state = CompatibilityState::Unsupported;
    CompatibilityReason reason = CompatibilityReason::None;
    bool watchable = false;
    std::string detail;
};

}
