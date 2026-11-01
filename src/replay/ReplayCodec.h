#pragma once

#include "ReplayTypes.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qreplay {

constexpr size_t kSimulationContractBytes = 128u;
constexpr size_t kFighterSlotDescriptorBytes = 8u;
constexpr size_t kDeterministicStartBytes = 200u;
constexpr size_t kMatchStartBytes = 268u;
constexpr size_t kMatchEndBytes = 32u;
constexpr size_t kSessionEndBytes = 32u;
constexpr size_t kInputBlockHeaderBytes = 8u;
constexpr size_t kInputRecordBytes = 90u;
constexpr size_t kEventRecordBytes = 76u;
constexpr size_t kMatchIndexEntryBytes = 56u;
constexpr size_t kFooterFixedBytes = 56u;

constexpr size_t kDisplayNameBytes = 32u;
constexpr size_t kDisplayNamePayloadBytes = 20u;

constexpr uint32_t kPrefixEndianMarker = 0x04030201u;

constexpr size_t kMaxEncodedStringBytes = 0xFFFFu;

void putU8(std::vector<uint8_t>& out, uint8_t value);
void putU16(std::vector<uint8_t>& out, uint16_t value);
void putU32(std::vector<uint8_t>& out, uint32_t value);
void putU64(std::vector<uint8_t>& out, uint64_t value);
void putI32(std::vector<uint8_t>& out, int32_t value);
void putI64(std::vector<uint8_t>& out, int64_t value);
void putBytes(std::vector<uint8_t>& out, const uint8_t* data, size_t len);

bool getU8(const uint8_t* data, size_t len, size_t& off, uint8_t& out);
bool getU16(const uint8_t* data, size_t len, size_t& off, uint16_t& out);
bool getU32(const uint8_t* data, size_t len, size_t& off, uint32_t& out);
bool getU64(const uint8_t* data, size_t len, size_t& off, uint64_t& out);
bool getI32(const uint8_t* data, size_t len, size_t& off, int32_t& out);
bool getI64(const uint8_t* data, size_t len, size_t& off, int64_t& out);
bool getBytes(
    const uint8_t* data, size_t len, size_t& off, uint8_t* dst, size_t count);

bool addChecked(size_t a, size_t b, size_t& out);
bool mulChecked(size_t a, size_t b, size_t& out);
bool addChecked32(uint32_t a, uint32_t b, uint32_t& out);

bool isCanonicalReplayText(const uint8_t* data, size_t len);
bool isCanonicalReplayText(const std::string& text);

bool putString(
    std::vector<uint8_t>& out, const std::string& text, size_t maxBytes);
ParseStatus getString(
    const uint8_t* data, size_t len, size_t& off, size_t maxBytes,
    std::string& out);

void encodePrefix(const ReplayPrefix& prefix, std::vector<uint8_t>& out);
ParseStatus decodePrefix(const uint8_t* data, size_t len, ReplayPrefix& out);

void encodeChunkHeader(const ChunkHeader& header, std::vector<uint8_t>& out);
ParseStatus decodeChunkHeader(
    const uint8_t* data, size_t len, ChunkHeader& out);

bool encodeSessionManifest(
    const SessionManifest& manifest, std::vector<uint8_t>& out);
ParseStatus decodeSessionManifest(
    const uint8_t* data, size_t len, SessionManifest& out);

bool encodeDeterministicStart(
    const DeterministicStart& start, std::vector<uint8_t>& out);
ParseStatus decodeDeterministicStart(
    const uint8_t* data, size_t len, DeterministicStart& out);

bool encodeMatchStart(
    const MatchStartRecord& record, std::vector<uint8_t>& out);
ParseStatus decodeMatchStart(
    const uint8_t* data, size_t len, MatchStartRecord& out);

bool encodeMatchEnd(const MatchEndRecord& record, std::vector<uint8_t>& out);
ParseStatus decodeMatchEnd(
    const uint8_t* data, size_t len, MatchEndRecord& out);

bool encodeSessionEnd(
    const SessionEndRecord& record, std::vector<uint8_t>& out);
ParseStatus decodeSessionEnd(
    const uint8_t* data, size_t len, SessionEndRecord& out);

bool encodeInputBlock(
    const ReplayFrameV1* frames, size_t count, std::vector<uint8_t>& out);
ParseStatus decodeInputBlock(
    const uint8_t* data, size_t len, uint32_t expectedFirstFrame,
    uint32_t recordCount, std::vector<ReplayFrameV1>& out);

bool encodeEventBlock(
    const ReplayEvent* events, size_t count, std::vector<uint8_t>& out);
ParseStatus decodeEventBlock(
    const uint8_t* data, size_t len, uint32_t count,
    std::vector<ReplayEvent>& out);

bool encodeFooterIndex(const FooterIndex& footer, std::vector<uint8_t>& out);
ParseStatus decodeFooterIndex(
    const uint8_t* data, size_t len, FooterIndex& out);

constexpr size_t kSimulationContractBytesV2 = 144u;
constexpr size_t kGameIdentityBytesV2 = 20u;
constexpr size_t kReplayTextBytesV2 = 1u + kFixedReplayTextBytesV2;
constexpr size_t kSourceLaneDescriptorBytesV2 = 1u + kReplayTextBytesV2;
constexpr size_t kFighterSlotDescriptorBytesV2 = 8u;
constexpr size_t kStageIdentityBytesV2 = 16u;
constexpr size_t kDisplayMetadataBytesV2 = 216u;
constexpr size_t kGameManifestBytesV2 = 716u;
constexpr size_t kDeterministicGameStartBytesV2 = 208u;
constexpr size_t kGameLifecycleBytesV2 = 48u;
constexpr size_t kConfirmedGameFrameCoreBytesV2 = 128u;
constexpr size_t kGameFrameRecordBytesV2 = 136u;
constexpr size_t kConfirmedGameFrameTransportBytesV2 = 144u;
constexpr size_t kConfirmedGameEventBytesV2 = 64u;
constexpr size_t kConfirmedGameEndBytesV2 = 64u;
constexpr size_t kInputBlockHeaderBytesV2 = 8u;
constexpr size_t kEventBlockHeaderBytesV2 = 8u;
constexpr size_t kRoundIndexEntryBytesV2 = 40u;
constexpr size_t kFooterFixedBytesV2 = 128u;

bool setReplayTextV2(ReplayTextV2& out, const std::string& text);
std::string replayTextV2(const ReplayTextV2& text);
bool isValidReplayTextV2(const ReplayTextV2& text);

void encodePrefixV2(const ReplayPrefixV2& prefix, std::vector<uint8_t>& out);
ParseStatus decodePrefixV2(
    const uint8_t* data, size_t len, ReplayPrefixV2& out);

void encodeChunkHeaderV2(
    const ChunkHeaderV2& header, std::vector<uint8_t>& out);
ParseStatus decodeChunkHeaderV2(
    const uint8_t* data, size_t len, ChunkHeaderV2& out);

bool encodeGameManifestV2(
    const GameManifestV2& manifest, std::vector<uint8_t>& out);
ParseStatus decodeGameManifestV2(
    const uint8_t* data, size_t len, GameManifestV2& out);

bool encodeDeterministicGameStartV2(
    const DeterministicGameStartV2& start, std::vector<uint8_t>& out);
ParseStatus decodeDeterministicGameStartV2(
    const uint8_t* data, size_t len, DeterministicGameStartV2& out);

bool encodeConfirmedGameFrameCoreV2(
    const ConfirmedGameFrameCoreV2& frame, std::vector<uint8_t>& out);
ParseStatus decodeConfirmedGameFrameCoreV2(
    const uint8_t* data, size_t len, uint8_t allowedLaneMask,
    ConfirmedGameFrameCoreV2& out);

bool encodeGameFrameRecordV2(
    const ConfirmedGameFrameV2& frame, std::vector<uint8_t>& out);
ParseStatus decodeGameFrameRecordV2(
    const uint8_t* data, size_t len, uint32_t replayFrame,
    uint8_t allowedLaneMask, ConfirmedGameFrameV2& out);
bool encodeConfirmedGameFrameTransportV2(
    const ConfirmedGameFrameV2& frame, std::vector<uint8_t>& out);
ParseStatus decodeConfirmedGameFrameTransportV2(
    const uint8_t* data, size_t len, uint8_t allowedLaneMask,
    ConfirmedGameFrameV2& out);

bool encodeConfirmedGameEventV2(
    const ConfirmedGameEventV2& event, std::vector<uint8_t>& out);
ParseStatus decodeConfirmedGameEventV2(
    const uint8_t* data, size_t len, ConfirmedGameEventV2& out);

bool encodeConfirmedGameEndV2(
    const ConfirmedGameEndV2& end, std::vector<uint8_t>& out);
ParseStatus decodeConfirmedGameEndV2(
    const uint8_t* data, size_t len, ConfirmedGameEndV2& out);

bool encodeInputBlockV2(
    const ConfirmedGameFrameV2* frames, size_t count,
    std::vector<uint8_t>& out);
ParseStatus decodeInputBlockV2(
    const uint8_t* data, size_t len, uint32_t expectedFirstRecord,
    uint32_t recordCount, uint8_t allowedLaneMask,
    std::vector<ConfirmedGameFrameV2>& out);

bool encodeEventBlockV2(
    const ConfirmedGameEventV2* events, size_t count,
    std::vector<uint8_t>& out);
ParseStatus decodeEventBlockV2(
    const uint8_t* data, size_t len, uint32_t expectedFirstEventSequence,
    uint32_t eventCount, std::vector<ConfirmedGameEventV2>& out);

bool encodePaletteBlockV2(
    const ReplayPaletteV2& palette, std::vector<uint8_t>& out);
ParseStatus decodePaletteBlockV2(
    const uint8_t* data, size_t len, ReplayPaletteV2& out);

bool encodeTickBundleV2(
    const CanonicalTickBundleV2& bundle, std::vector<uint8_t>& out);
ParseStatus decodeTickBundleV2(
    const uint8_t* data, size_t len, uint8_t allowedLaneMask,
    CanonicalTickBundleV2& out);

bool encodeConfirmedGameStartV2(
    const ConfirmedGameStartV2& start, std::vector<uint8_t>& out);
ParseStatus decodeConfirmedGameStartV2(
    const uint8_t* data, size_t len, ConfirmedGameStartV2& out);

bool encodeBeginGameBundleV2(
    const BeginGameBundleV2& begin, std::vector<uint8_t>& out);
ParseStatus decodeBeginGameBundleV2(
    const uint8_t* data, size_t len, BeginGameBundleV2& out);

bool encodeEndGameBundleV2(
    const EndGameBundleV2& end, std::vector<uint8_t>& out);
ParseStatus decodeEndGameBundleV2(
    const uint8_t* data, size_t len, uint8_t allowedLaneMask,
    EndGameBundleV2& out);

bool encodeFooterIndexV2(
    const FooterIndexV2& footer, std::vector<uint8_t>& out);
bool encodeFooterIndexFileDigestProjectionV2(
    const FooterIndexV2& footer, std::vector<uint8_t>& out);
ParseStatus decodeFooterIndexV2(
    const uint8_t* data, size_t len, FooterIndexV2& out);

}
