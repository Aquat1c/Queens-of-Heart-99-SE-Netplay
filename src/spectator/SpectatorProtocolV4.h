#pragma once

#include "SpectatorProtocol.h"

#include "../replay/ReplayCodec.h"
#include "../replay/ReplayTypes.h"

#include <cstring>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qnetplay::spectator {

constexpr uint16_t kSpectatorVersionV4 = 4u;
constexpr uint16_t kConnectedPublicationSchemaV4 = 1u;
constexpr uint32_t kConnectedPublicationMagicV4 = 0x49503451u; // "Q4PI"
constexpr uint32_t kConnectedDescriptorMagicV4 = 0x53443451u; // "Q4DS"
constexpr uint16_t kConnectedDescriptorSchemaV4 = 2u;
constexpr uint16_t kLegacyConnectedDescriptorSchemaV4 = 1u;
constexpr size_t kLegacyConnectedDescriptorBytesV4 = 68u;
constexpr size_t kConnectedDescriptorBytesV4 =
    kLegacyConnectedDescriptorBytesV4
    + compat::kCompatibilityIdentityWireBytes;

constexpr size_t kMaxConnectedPublicationBytesV4 = 8192u;
constexpr size_t kMaxConnectedFragmentBytesV4 = 1024u;
constexpr size_t kMaxConnectedFragmentsV4 = 8u;
constexpr uint32_t kMaxConnectedSetItemsV4 = 524288u;
constexpr uint32_t kMaxConnectedSetRecordsV4 =
    qreplay::kMaxPlaybackPrebufferFramesV2;
constexpr uint64_t kMaxConnectedArchiveBytesV4 =
    128ull * 1024ull * 1024ull;
constexpr uint32_t kMaxConnectedUnackedItemsV4 = 8192u;
constexpr uint64_t kMaxConnectedUnackedBytesV4 =
    16ull * 1024ull * 1024ull;
constexpr uint64_t kSpectatorPostGameRetentionMsV4 = 120000ull;

struct ConnectedChainStampV4 {
    uint64_t setStreamId = 0u;
    uint32_t itemSequence = 0u;
    uint32_t recordCursorBefore = 0u;
    uint64_t setDigestBefore = 0u;
    uint64_t setDigestAfter = 0u;

    bool operator==(const ConnectedChainStampV4& o) const {
        return setStreamId == o.setStreamId
            && itemSequence == o.itemSequence
            && recordCursorBefore == o.recordCursorBefore
            && setDigestBefore == o.setDigestBefore
            && setDigestAfter == o.setDigestAfter;
    }
    bool operator!=(const ConnectedChainStampV4& o) const {
        return !(*this == o);
    }
};

struct SetEventPayloadV4 {
    uint32_t sourceTimelineFrame = 0u;
    SetEventKind kind = SetEventKind::MatchStarted;
    SetSegmentTerminalReason terminalReason =
        SetSegmentTerminalReason::Completed;
    uint32_t matchId = 0u;
    uint32_t rosterEpoch = 0u;
    uint64_t valueA = 0u;
    uint64_t valueB = 0u;
    uint32_t valueC = 0u;
    uint8_t selectorProofMask = 0u;
    uint8_t activeSlotMask = 0u;
    std::array<SetFighterIdentity, kSetFighterSlots> roster{};
    std::array<SetSelectorProof, kSetFighterSlots> selectorProof{};
    std::array<uint8_t, kSetFighterSlots> slotToSourceRoute{
        {0u, 1u, 2u, 3u}};
    std::array<uint8_t, kSetPaletteEntryBytes> paletteEntries{};
    uint64_t stateProof = 0u;

    bool operator==(const SetEventPayloadV4& o) const {
        return sourceTimelineFrame == o.sourceTimelineFrame
            && kind == o.kind && terminalReason == o.terminalReason
            && matchId == o.matchId && rosterEpoch == o.rosterEpoch
            && valueA == o.valueA && valueB == o.valueB
            && valueC == o.valueC
            && selectorProofMask == o.selectorProofMask
            && activeSlotMask == o.activeSlotMask && roster == o.roster
            && selectorProof == o.selectorProof
            && paletteEntries == o.paletteEntries
            && slotToSourceRoute == o.slotToSourceRoute
            && stateProof == o.stateProof;
    }
    bool operator!=(const SetEventPayloadV4& o) const {
        return !(*this == o);
    }
};

struct SpectatorPerGameStartV4 {
    ConnectedChainStampV4 chain{};
    qreplay::GameIdentityV2 game{};
    qreplay::ConfirmedGameStartV2 start{};

    bool operator==(const SpectatorPerGameStartV4& o) const {
        return chain == o.chain && game == o.game && start == o.start;
    }
};

struct ConnectedConfirmedRecordV4 {
    ConnectedChainStampV4 chain{};
    bool hasReplayGameIdentity = false;
    qreplay::GameIdentityV2 game{};
    uint32_t sourceTimelineFrame = 0u;
    qreplay::FixedEventListV2 preEvents{};
    qreplay::ConfirmedGameFrameCoreV2 frame{};
    qreplay::FixedEventListV2 postEvents{};

    bool operator==(const ConnectedConfirmedRecordV4& o) const {
        return chain == o.chain
            && hasReplayGameIdentity == o.hasReplayGameIdentity
            && game == o.game
            && sourceTimelineFrame == o.sourceTimelineFrame
            && preEvents == o.preEvents && frame == o.frame
            && postEvents == o.postEvents;
    }
};

struct ConnectedSetEventV4 {
    ConnectedChainStampV4 chain{};
    SetEventPayloadV4 event{};

    bool operator==(const ConnectedSetEventV4& o) const {
        return chain == o.chain && event == o.event;
    }
};

struct ConnectedPaletteV4 {
    ConnectedChainStampV4 chain{};
    qreplay::ReplayPaletteV2 palette{};

    bool operator==(const ConnectedPaletteV4& o) const {
        return chain == o.chain
            && palette.side == o.palette.side
            && palette.variant == o.palette.variant
            && palette.present == o.palette.present
            && std::memcmp(palette.table, o.palette.table,
                           qreplay::kReplayPaletteTableBytes) == 0;
    }
};

struct SpectatorPerGameEndV4 {
    ConnectedChainStampV4 chain{};
    qreplay::GameIdentityV2 game{};
    uint32_t setNextRecordCursor = 0u;
    qreplay::ConfirmedGameEndV2 end{};

    bool operator==(const SpectatorPerGameEndV4& o) const {
        return chain == o.chain && game == o.game
            && setNextRecordCursor == o.setNextRecordCursor
            && end == o.end;
    }
};

struct ConnectedBeginGameBundleV4 {
    SpectatorPerGameStartV4 start{};
    ConnectedConfirmedRecordV4 frameZero{};

    bool operator==(const ConnectedBeginGameBundleV4& o) const {
        return start == o.start && frameZero == o.frameZero;
    }
};

struct ConnectedEndGameBundleV4 {
    bool containsFinalRecord = false;
    ConnectedConfirmedRecordV4 finalRecord{};
    SpectatorPerGameEndV4 end{};

    bool operator==(const ConnectedEndGameBundleV4& o) const {
        return containsFinalRecord == o.containsFinalRecord
            && finalRecord == o.finalRecord && end == o.end;
    }
};

enum class ConnectedSetTerminalReasonV4 : uint16_t {
    SourceStopped = 1,
    SourceProcessExit = 2,
    SourceDisconnected = 3,
    PublisherOverflow = 4,
    SidecarFailure = 5,
    ProtocolFailure = 6,
};

constexpr bool isKnownConnectedSetTerminalReasonV4(uint16_t value) {
    return value >= 1u && value <= 6u;
}

struct ConnectedSetSealV4 {
    ConnectedChainStampV4 chain{};
    ConnectedSetTerminalReasonV4 reason =
        ConnectedSetTerminalReasonV4::SourceStopped;
    uint32_t setNextRecordCursor = 0u;
    uint64_t postTailStateHash = 0u;

    bool operator==(const ConnectedSetSealV4& o) const {
        return chain == o.chain && reason == o.reason
            && setNextRecordCursor == o.setNextRecordCursor
            && postTailStateHash == o.postTailStateHash;
    }
};

enum class ConnectedItemKindV4 : uint16_t {
    PerGameStart = 1,
    ConfirmedRecord = 2,
    SetEvent = 3,
    PerGameEnd = 4,
    SetSeal = 5,
    Palette = 6,
};

enum class ConnectedPublicationKindV4 : uint16_t {
    BeginGameBundle = 1,
    ConfirmedRecord = 2,
    SetEvent = 3,
    EndGameBundle = 4,
    SetSeal = 5,
    Palette = 6,
};

constexpr bool isKnownConnectedPublicationKindV4(uint16_t value) {
    return value >= 1u && value <= 6u;
}

struct ConnectedPublicationV4 {
    ConnectedPublicationKindV4 kind =
        ConnectedPublicationKindV4::ConfirmedRecord;
    ConnectedBeginGameBundleV4 begin{};
    ConnectedConfirmedRecordV4 record{};
    ConnectedSetEventV4 setEvent{};
    ConnectedEndGameBundleV4 end{};
    ConnectedSetSealV4 seal{};
    ConnectedPaletteV4 palette{};

    bool operator==(const ConnectedPublicationV4& o) const {
        if (kind != o.kind) return false;
        switch (kind) {
            case ConnectedPublicationKindV4::BeginGameBundle:
                return begin == o.begin;
            case ConnectedPublicationKindV4::ConfirmedRecord:
                return record == o.record;
            case ConnectedPublicationKindV4::SetEvent:
                return setEvent == o.setEvent;
            case ConnectedPublicationKindV4::EndGameBundle:
                return end == o.end;
            case ConnectedPublicationKindV4::SetSeal:
                return seal == o.seal;
            case ConnectedPublicationKindV4::Palette:
                return palette == o.palette;
        }
        return false;
    }
};

struct SpectatorResumeCursorV4 {
    uint64_t setStreamId = 0u;
    uint32_t nextItemSequence = 0u;
    uint32_t nextRecordCursor = 0u;
    uint64_t setDigestBeforeNext = 0u;
    uint32_t viewerGeneration = 0u;

    bool operator==(const SpectatorResumeCursorV4& o) const {
        return setStreamId == o.setStreamId
            && nextItemSequence == o.nextItemSequence
            && nextRecordCursor == o.nextRecordCursor
            && setDigestBeforeNext == o.setDigestBeforeNext
            && viewerGeneration == o.viewerGeneration;
    }
};

struct SpectatorAckV4 {
    uint64_t viewerId = 0u;
    SpectatorResumeCursorV4 committed{};
};

struct SpectatorSetSealAckV4 {
    uint64_t viewerId = 0u;
    uint64_t setStreamId = 0u;
    uint32_t sealItemSequence = 0u;
    uint32_t setNextRecordCursor = 0u;
    uint64_t finalSetDigest = 0u;
    uint32_t viewerGeneration = 0u;

    bool operator==(const SpectatorSetSealAckV4& o) const {
        return viewerId == o.viewerId
            && setStreamId == o.setStreamId
            && sealItemSequence == o.sealItemSequence
            && setNextRecordCursor == o.setNextRecordCursor
            && finalSetDigest == o.finalSetDigest
            && viewerGeneration == o.viewerGeneration;
    }
};

struct ConnectedSetDescriptorV4 {
    uint64_t setStreamId = 0u;
    SessionIdentity sourceIdentity{};
    uint64_t simulationContract = 0u;
    uint64_t initialDescriptorDigest = 0u;
    compat::CompatibilityIdentityWire compatibilityIdentity{};
    bool contentIdentityAttested = false;
};

enum class ConnectedCodecErrorV4 : uint8_t {
    None,
    TooShort,
    TooLarge,
    BadMagic,
    BadVersion,
    BadKind,
    BadLength,
    BadChecksum,
    BadScalar,
    BadPayload,
    TrailingBytes,
};

bool encodeConnectedSetDescriptorV4(
    const ConnectedSetDescriptorV4& descriptor,
    std::vector<uint8_t>& out);
ConnectedCodecErrorV4 decodeConnectedSetDescriptorV4(
    const uint8_t* bytes, size_t length,
    ConnectedSetDescriptorV4& out);

bool encodeConnectedPublicationV4(
    const ConnectedPublicationV4& publication, std::vector<uint8_t>& out);
ConnectedCodecErrorV4 decodeConnectedPublicationV4(
    const uint8_t* bytes, size_t length, ConnectedPublicationV4& out);

bool encodeSpectatorResumeCursorV4(
    const SpectatorResumeCursorV4& cursor, std::vector<uint8_t>& out);
ConnectedCodecErrorV4 decodeSpectatorResumeCursorV4(
    const uint8_t* bytes, size_t length, SpectatorResumeCursorV4& out);
bool encodeSpectatorAckV4(
    const SpectatorAckV4& ack, std::vector<uint8_t>& out);
ConnectedCodecErrorV4 decodeSpectatorAckV4(
    const uint8_t* bytes, size_t length, SpectatorAckV4& out);
bool encodeSpectatorSetSealAckV4(
    const SpectatorSetSealAckV4& ack, std::vector<uint8_t>& out);
ConnectedCodecErrorV4 decodeSpectatorSetSealAckV4(
    const uint8_t* bytes, size_t length, SpectatorSetSealAckV4& out);

struct ConnectedPublicationFragmentV4 {
    uint64_t setStreamId = 0u;
    uint32_t firstItemSequence = 0u;
    ConnectedPublicationKindV4 publicationKind =
        ConnectedPublicationKindV4::ConfirmedRecord;
    uint16_t fragmentIndex = 0u;
    uint16_t fragmentCount = 0u;
    uint32_t totalBytes = 0u;
    uint32_t byteOffset = 0u;
    uint32_t publicationCrc32c = 0u;
    std::vector<uint8_t> bytes;
};

bool fragmentConnectedPublicationV4(
    uint64_t setStreamId, uint32_t firstItemSequence,
    ConnectedPublicationKindV4 kind, const std::vector<uint8_t>& publication,
    std::vector<ConnectedPublicationFragmentV4>& out);
bool encodeConnectedFragmentV4(
    const ConnectedPublicationFragmentV4& fragment,
    std::vector<uint8_t>& out);
ConnectedCodecErrorV4 decodeConnectedFragmentV4(
    const uint8_t* bytes, size_t length,
    ConnectedPublicationFragmentV4& out);

uint64_t computeConnectedGenesisDigestV4(
    const ConnectedSetDescriptorV4& descriptor);

bool connectedDescriptorMatchesCompatibilityIdentityV4(
    const ConnectedSetDescriptorV4& descriptor,
    const compat::CompatibilityIdentityWire& expectedIdentity);

class ConnectedChainBuilderV4 {
public:
    bool reset(const ConnectedSetDescriptorV4& descriptor);
    bool stamp(ConnectedPublicationV4& publication);

    uint64_t setStreamId() const { return setStreamId_; }
    uint32_t nextItemSequence() const { return nextItemSequence_; }
    uint32_t nextRecordCursor() const { return nextRecordCursor_; }
    uint64_t digest() const { return digest_; }
    bool sealed() const { return sealed_; }

private:
    uint64_t setStreamId_ = 0u;
    uint32_t nextItemSequence_ = 0u;
    uint32_t nextRecordCursor_ = 0u;
    uint64_t digest_ = 0u;
    bool bound_ = false;
    bool sealed_ = false;
    bool gameOpen_ = false;
    qreplay::GameIdentityV2 openGame_{};
};

bool rebaseConnectedRecordV4(
    const ConnectedConfirmedRecordV4& connected,
    uint32_t gameStartRecordCursor,
    uint32_t& nextEventSequence,
    uint64_t& rollingDigest,
    qreplay::CanonicalTickBundleV2& out);

bool isCanonicalConnectedRecordV4(
    const ConnectedConfirmedRecordV4& record);
bool isValidSetEventPayloadV4(const SetEventPayloadV4& event);
uint32_t firstItemSequenceV4(const ConnectedPublicationV4& publication);
uint32_t nextItemSequenceV4(const ConnectedPublicationV4& publication);
uint32_t firstRecordCursorV4(const ConnectedPublicationV4& publication);
uint32_t nextRecordCursorV4(const ConnectedPublicationV4& publication);
uint64_t firstDigestV4(const ConnectedPublicationV4& publication);
uint64_t finalDigestV4(const ConnectedPublicationV4& publication);
bool verifyConnectedPublicationChainV4(
    const ConnectedPublicationV4& publication, uint64_t setStreamId,
    uint32_t expectedItemSequence, uint32_t expectedRecordCursor,
    uint64_t expectedDigest, uint32_t& nextItemSequence,
    uint32_t& nextRecordCursor, uint64_t& nextDigest);

}
