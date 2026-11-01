#pragma once

#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"
#include "replay/ReplayTypes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qreplay {
namespace ipc {

constexpr uint32_t kReplayIpcMagic = 0x31505251u;

// Bump whenever ANY message shape on this channel changes.
constexpr uint16_t kReplayIpcWireVersion = 2u;

constexpr size_t kReplayIpcHeaderBytes = 44u;
constexpr size_t kReplayIpcHeaderCrcBytes = 40u;

constexpr uint32_t kReplayIpcMaxPayloadBytes = 512u * 1024u;

enum class Channel : uint8_t {
    Recorder = 1,
    Broker = 2,
};

enum class MessageType : uint8_t {
    Hello = 1,        // param0 = matchOrdinal, param1 = resumeFrame
    SeekRequest = 2,  // param0 = fromFrame,    param1 = targetFrame
    Bye = 3,

    SessionInfo = 16,
    SessionStart = 17,
    InputBlock = 18,   // param0 = firstFrame, param1 = count
    EventBlock = 19,   // param1 = count
    Terminal = 20,     // param0 = lastFrame,  payload = TerminalPayload

    RecorderHello = 32,        // param0 = sourceKind, param1 = generation
    RecorderFrames = 33,       // param0 = firstFrame, param1 = count
    RecorderEvents = 34,       // param1 = count
    RecorderTerminal = 35,     // param0 = lastFrame, payload = TerminalPayload
    RecorderManifest = 36,
    RecorderSessionStart = 37,
    RecorderMatchStart = 38,
    RecorderMatchEnd = 39,
};

constexpr bool isKnownChannel(uint8_t value) {
    return value == static_cast<uint8_t>(Channel::Recorder)
        || value == static_cast<uint8_t>(Channel::Broker);
}

constexpr bool isBrokerRequestType(uint8_t value) {
    return value >= 1u && value <= 3u;
}
constexpr bool isBrokerStreamType(uint8_t value) {
    return value >= 16u && value <= 20u;
}
constexpr bool isRecorderType(uint8_t value) {
    return value >= 32u && value <= 39u;
}

constexpr bool isKnownMessageType(uint8_t value) {
    return isBrokerRequestType(value) || isBrokerStreamType(value)
        || isRecorderType(value);
}

constexpr Channel channelForMessageType(uint8_t value) {
    return isRecorderType(value) ? Channel::Recorder : Channel::Broker;
}

enum class FrameStatus : uint8_t {
    Ok = 0,
    NeedMore,
    BadMagic,
    BadWireVersion,
    BadHeaderCrc,
    BadPayloadCrc,
    TooLarge,
    UnknownChannel,
    UnknownType,
};

inline const char* frameStatusText(FrameStatus status) {
    switch (status) {
        case FrameStatus::Ok: return "ok";
        case FrameStatus::NeedMore: return "need more bytes";
        case FrameStatus::BadMagic: return "bad frame magic";
        case FrameStatus::BadWireVersion: return "replay IPC wire version mismatch";
        case FrameStatus::BadHeaderCrc: return "bad frame header CRC";
        case FrameStatus::BadPayloadCrc: return "bad frame payload CRC";
        case FrameStatus::TooLarge: return "frame payload too large";
        case FrameStatus::UnknownChannel: return "unknown or wrong channel";
        case FrameStatus::UnknownType: return "unknown message type";
    }
    return "unknown";
}

struct FrameHeader {
    uint32_t magic = kReplayIpcMagic;
    uint16_t wireVersion = kReplayIpcWireVersion;
    uint8_t channel = static_cast<uint8_t>(Channel::Broker);
    uint8_t type = static_cast<uint8_t>(MessageType::Hello);
    uint32_t generation = 0u;
    uint64_t nonce = 0u;
    uint32_t sequence = 0u;
    uint32_t param0 = 0u;
    uint32_t param1 = 0u;
    uint32_t payloadBytes = 0u;
    uint32_t payloadCrc32c = 0u;
    uint32_t headerCrc32c = 0u;
};

inline void encodeHeader(const FrameHeader& header, std::vector<uint8_t>& out) {
    const size_t base = out.size();
    putU32(out, header.magic);
    putU16(out, header.wireVersion);
    putU8(out, header.channel);
    putU8(out, header.type);
    putU32(out, header.generation);
    putU64(out, header.nonce);
    putU32(out, header.sequence);
    putU32(out, header.param0);
    putU32(out, header.param1);
    putU32(out, header.payloadBytes);
    putU32(out, header.payloadCrc32c);
    putU32(out, crc32c(out.data() + base, kReplayIpcHeaderCrcBytes));
}

inline FrameStatus decodeHeader(const uint8_t* data, size_t len,
                                FrameHeader& out) {
    if (data == nullptr || len < kReplayIpcHeaderBytes)
        return FrameStatus::NeedMore;

    FrameHeader h{};
    size_t off = 0u;
    if (!getU32(data, len, off, h.magic)) return FrameStatus::NeedMore;
    if (h.magic != kReplayIpcMagic) return FrameStatus::BadMagic;

    if (!getU16(data, len, off, h.wireVersion)) return FrameStatus::NeedMore;
    if (!getU8(data, len, off, h.channel)) return FrameStatus::NeedMore;
    if (!getU8(data, len, off, h.type)) return FrameStatus::NeedMore;
    if (!getU32(data, len, off, h.generation)) return FrameStatus::NeedMore;
    if (!getU64(data, len, off, h.nonce)) return FrameStatus::NeedMore;
    if (!getU32(data, len, off, h.sequence)) return FrameStatus::NeedMore;
    if (!getU32(data, len, off, h.param0)) return FrameStatus::NeedMore;
    if (!getU32(data, len, off, h.param1)) return FrameStatus::NeedMore;
    if (!getU32(data, len, off, h.payloadBytes)) return FrameStatus::NeedMore;
    if (!getU32(data, len, off, h.payloadCrc32c)) return FrameStatus::NeedMore;
    if (!getU32(data, len, off, h.headerCrc32c)) return FrameStatus::NeedMore;

    if (crc32c(data, kReplayIpcHeaderCrcBytes) != h.headerCrc32c)
        return FrameStatus::BadHeaderCrc;

    if (h.wireVersion != kReplayIpcWireVersion)
        return FrameStatus::BadWireVersion;
    if (!isKnownChannel(h.channel)) return FrameStatus::UnknownChannel;
    if (!isKnownMessageType(h.type)) return FrameStatus::UnknownType;
    if (static_cast<uint8_t>(channelForMessageType(h.type)) != h.channel)
        return FrameStatus::UnknownChannel;
    if (h.payloadBytes > kReplayIpcMaxPayloadBytes)
        return FrameStatus::TooLarge;

    out = h;
    return FrameStatus::Ok;
}

inline bool frameBytes(const FrameHeader& header, size_t& out) {
    return addChecked(kReplayIpcHeaderBytes,
                      static_cast<size_t>(header.payloadBytes), out);
}

struct Frame {
    Channel channel = Channel::Broker;
    MessageType type = MessageType::Hello;
    uint16_t wireVersion = kReplayIpcWireVersion;
    uint32_t generation = 0u;
    uint64_t nonce = 0u;
    uint32_t sequence = 0u;
    uint32_t param0 = 0u;
    uint32_t param1 = 0u;
    std::vector<uint8_t> payload;
};

inline bool encodeFrame(const Frame& frame, std::vector<uint8_t>& out) {
    if (frame.payload.size() > kReplayIpcMaxPayloadBytes) return false;
    FrameHeader header{};
    header.magic = kReplayIpcMagic;
    header.wireVersion = frame.wireVersion;
    header.channel = static_cast<uint8_t>(frame.channel);
    header.type = static_cast<uint8_t>(frame.type);
    header.generation = frame.generation;
    header.nonce = frame.nonce;
    header.sequence = frame.sequence;
    header.param0 = frame.param0;
    header.param1 = frame.param1;
    header.payloadBytes = static_cast<uint32_t>(frame.payload.size());
    header.payloadCrc32c = frame.payload.empty()
        ? 0u
        : crc32c(frame.payload.data(), frame.payload.size());
    encodeHeader(header, out);
    if (!frame.payload.empty())
        putBytes(out, frame.payload.data(), frame.payload.size());
    return true;
}

inline FrameStatus decodeFrame(const uint8_t* data, size_t len, Frame& out,
                               size_t& consumed) {
    FrameHeader header{};
    const FrameStatus status = decodeHeader(data, len, header);
    if (status != FrameStatus::Ok) return status;

    size_t total = 0u;
    if (!frameBytes(header, total)) return FrameStatus::TooLarge;
    if (len < total) return FrameStatus::NeedMore;

    const uint8_t* payload = data + kReplayIpcHeaderBytes;
    const uint32_t crc =
        header.payloadBytes == 0u ? 0u : crc32c(payload, header.payloadBytes);
    if (crc != header.payloadCrc32c) return FrameStatus::BadPayloadCrc;

    out.channel = static_cast<Channel>(header.channel);
    out.type = static_cast<MessageType>(header.type);
    out.wireVersion = header.wireVersion;
    out.generation = header.generation;
    out.nonce = header.nonce;
    out.sequence = header.sequence;
    out.param0 = header.param0;
    out.param1 = header.param1;
    out.payload.assign(payload, payload + header.payloadBytes);
    consumed = total;
    return FrameStatus::Ok;
}

struct HelloPayload {
    uint64_t nonce = 0u;
    uint32_t gamePid = 0u;
    uint16_t wireVersion = kReplayIpcWireVersion;
    uint16_t featureBits = 0u;
    uint64_t manifestDigest = 0u;
};

constexpr size_t kHelloPayloadBytes = 28u;

inline bool encodeHello(const HelloPayload& hello, std::vector<uint8_t>& out) {
    const size_t before = out.size();
    putU64(out, hello.nonce);
    putU32(out, hello.gamePid);
    putU16(out, hello.wireVersion);
    putU16(out, hello.featureBits);
    putU64(out, hello.manifestDigest);
    putU32(out, 0u);
    return (out.size() - before) == kHelloPayloadBytes;
}

inline bool decodeHello(const uint8_t* data, size_t len, HelloPayload& out) {
    if (data == nullptr || len != kHelloPayloadBytes) return false;
    HelloPayload hello{};
    size_t off = 0u;
    uint32_t reserved = 0u;
    if (!getU64(data, len, off, hello.nonce)) return false;
    if (!getU32(data, len, off, hello.gamePid)) return false;
    if (!getU16(data, len, off, hello.wireVersion)) return false;
    if (!getU16(data, len, off, hello.featureBits)) return false;
    if (!getU64(data, len, off, hello.manifestDigest)) return false;
    if (!getU32(data, len, off, reserved) || reserved != 0u) return false;
    if (off != len) return false;
    if (hello.wireVersion != kReplayIpcWireVersion) return false;
    out = hello;
    return true;
}

struct SessionInfo {
    uint64_t manifestDigest = 0u;
    uint64_t sessionUuidLow = 0u;
    uint64_t sessionUuidHigh = 0u;
    uint64_t simulationContractId = 0u;
    uint64_t contentDigest = 0u;
    uint32_t matchId = 0u;
    uint32_t matchOrdinal = 0u;
    uint32_t firstTimelineFrame = 0u;
    uint32_t lastTimelineFrame = 0u;
    uint32_t totalFrames = 0u;
    uint32_t matchCount = 0u;
    uint8_t bootstrapKind = 2u;
    uint8_t vsRow = 1u;
    uint8_t cpuMask = 0u;
    uint8_t activeLaneMask = 0x03u;
    uint8_t activeSlotMask = 0x03u;
    std::array<uint8_t, 4> slotToSourceRoute{{0u, 1u, 2u, 3u}};
    uint64_t bootstrapFingerprint = 0u;
};

constexpr size_t kSessionInfoBytes = 88u;

inline bool encodeSessionInfo(const SessionInfo& info,
                              std::vector<uint8_t>& out) {
    const size_t before = out.size();
    putU64(out, info.manifestDigest);
    putU64(out, info.sessionUuidLow);
    putU64(out, info.sessionUuidHigh);
    putU64(out, info.simulationContractId);
    putU64(out, info.contentDigest);
    putU32(out, info.matchId);
    putU32(out, info.matchOrdinal);
    putU32(out, info.firstTimelineFrame);
    putU32(out, info.lastTimelineFrame);
    putU32(out, info.totalFrames);
    putU32(out, info.matchCount);
    putU8(out, info.bootstrapKind);
    putU8(out, info.vsRow);
    putU8(out, info.cpuMask);
    putU8(out, info.activeLaneMask);
    putU8(out, info.activeSlotMask);
    putU8(out, 0u);
    for (uint8_t lane : info.slotToSourceRoute) putU8(out, lane);
    putU16(out, 0u);
    putU64(out, info.bootstrapFingerprint);
    putU32(out, 0u);
    return (out.size() - before) == kSessionInfoBytes;
}

inline bool decodeSessionInfo(const uint8_t* data, size_t len,
                              SessionInfo& out) {
    if (data == nullptr || len != kSessionInfoBytes) return false;
    SessionInfo info{};
    size_t off = 0u;
    uint8_t reserved8 = 0u;
    uint16_t reserved16 = 0u;
    uint32_t reserved32 = 0u;
    if (!getU64(data, len, off, info.manifestDigest)) return false;
    if (!getU64(data, len, off, info.sessionUuidLow)) return false;
    if (!getU64(data, len, off, info.sessionUuidHigh)) return false;
    if (!getU64(data, len, off, info.simulationContractId)) return false;
    if (!getU64(data, len, off, info.contentDigest)) return false;
    if (!getU32(data, len, off, info.matchId)) return false;
    if (!getU32(data, len, off, info.matchOrdinal)) return false;
    if (!getU32(data, len, off, info.firstTimelineFrame)) return false;
    if (!getU32(data, len, off, info.lastTimelineFrame)) return false;
    if (!getU32(data, len, off, info.totalFrames)) return false;
    if (!getU32(data, len, off, info.matchCount)) return false;
    if (!getU8(data, len, off, info.bootstrapKind)) return false;
    if (!getU8(data, len, off, info.vsRow)) return false;
    if (!getU8(data, len, off, info.cpuMask)) return false;
    if (!getU8(data, len, off, info.activeLaneMask)) return false;
    if (!getU8(data, len, off, info.activeSlotMask)) return false;
    if (!getU8(data, len, off, reserved8) || reserved8 != 0u) return false;
    for (size_t i = 0; i < info.slotToSourceRoute.size(); ++i)
        if (!getU8(data, len, off, info.slotToSourceRoute[i])) return false;
    if (!getU16(data, len, off, reserved16) || reserved16 != 0u) return false;
    if (!getU64(data, len, off, info.bootstrapFingerprint)) return false;
    if (!getU32(data, len, off, reserved32) || reserved32 != 0u) return false;
    if (off != len) return false;

    if (!isKnownBootstrapKind(info.bootstrapKind)) return false;
    if (info.activeLaneMask == 0u || info.activeLaneMask > 0x0Fu) return false;
    if (info.activeSlotMask == 0u || info.activeSlotMask > 0x0Fu) return false;
    if (info.vsRow > 5u) return false;
    if (info.cpuMask > 0x0Fu) return false;
    for (uint8_t lane : info.slotToSourceRoute)
        if (lane >= kSourceLanes) return false;
    if (info.totalFrames > kMaxTimelineFrames) return false;
    if (info.matchCount > kMaxMatchesPerSet) return false;
    if (info.lastTimelineFrame != 0u
        && info.lastTimelineFrame < info.firstTimelineFrame) {
        return false;
    }
    out = info;
    return true;
}

inline uint64_t sessionBindingDigest(uint32_t matchId, uint32_t startFrame,
                                     uint32_t frameCount, uint64_t nonce) {
    return (static_cast<uint64_t>(matchId) << 32)
        ^ (static_cast<uint64_t>(startFrame) << 16)
        ^ static_cast<uint64_t>(frameCount) ^ nonce;
}

constexpr size_t kRecorderManifestHeaderBytes = kPrefixBytes + 4u;

inline bool encodeRecorderManifest(const ReplayPrefix& prefix,
                                   const uint8_t* manifest, size_t bytes,
                                   std::vector<uint8_t>& out) {
    if (manifest == nullptr || bytes == 0u || bytes > kMaxChunkPayloadBytes)
        return false;
    size_t total = 0u;
    if (!addChecked(kRecorderManifestHeaderBytes, bytes, total)) return false;
    if (total > kReplayIpcMaxPayloadBytes) return false;
    const size_t before = out.size();
    encodePrefix(prefix, out);
    putU32(out, static_cast<uint32_t>(bytes));
    putBytes(out, manifest, bytes);
    return (out.size() - before) == total;
}

inline bool decodeRecorderManifest(const uint8_t* data, size_t len,
                                   ReplayPrefix& prefix,
                                   SessionManifest& manifest) {
    if (data == nullptr || len < kRecorderManifestHeaderBytes) return false;
    ReplayPrefix decodedPrefix{};
    if (decodePrefix(data, kPrefixBytes, decodedPrefix) != ParseStatus::Ok)
        return false;
    size_t off = kPrefixBytes;
    uint32_t manifestBytes = 0u;
    if (!getU32(data, len, off, manifestBytes)) return false;
    if (manifestBytes == 0u || manifestBytes > kMaxChunkPayloadBytes)
        return false;
    size_t total = 0u;
    if (!addChecked(off, static_cast<size_t>(manifestBytes), total))
        return false;
    if (total != len) return false;
    SessionManifest decoded{};
    if (decodeSessionManifest(data + off, manifestBytes, decoded)
        != ParseStatus::Ok) {
        return false;
    }
    prefix = decodedPrefix;
    manifest = decoded;
    return true;
}

struct TerminalPayload {
    uint16_t reason = 0u;
    uint32_t lastTimelineFrame = 0u;
    uint64_t postTailStateHash = 0u;
    uint64_t rollingDigest = 0u;
};

constexpr size_t kTerminalPayloadBytes = 24u;

inline bool encodeTerminal(const TerminalPayload& terminal,
                           std::vector<uint8_t>& out) {
    const size_t before = out.size();
    putU16(out, terminal.reason);
    putU16(out, 0u);
    putU32(out, terminal.lastTimelineFrame);
    putU64(out, terminal.postTailStateHash);
    putU64(out, terminal.rollingDigest);
    return (out.size() - before) == kTerminalPayloadBytes;
}

inline bool decodeTerminal(const uint8_t* data, size_t len,
                           TerminalPayload& out) {
    if (data == nullptr || len != kTerminalPayloadBytes) return false;
    TerminalPayload terminal{};
    size_t off = 0u;
    uint16_t reserved = 0u;
    if (!getU16(data, len, off, terminal.reason)) return false;
    if (!getU16(data, len, off, reserved) || reserved != 0u) return false;
    if (!getU32(data, len, off, terminal.lastTimelineFrame)) return false;
    if (!getU64(data, len, off, terminal.postTailStateHash)) return false;
    if (!getU64(data, len, off, terminal.rollingDigest)) return false;
    if (off != len) return false;
    if (!isKnownTerminalReason(terminal.reason)) return false;
    out = terminal;
    return true;
}

namespace v3 {

constexpr uint32_t kMagic = 0x33505251u;
constexpr uint16_t kWireVersion = 3u;
constexpr size_t kHeaderBytes = 64u;
constexpr size_t kHeaderCrcBytes = 60u;
constexpr uint32_t kMaxPayloadBytes = 512u * 1024u;
constexpr uint32_t kMaxGameEventsV3 =
    kMaxTimelineEventsV2;

enum class Channel : uint8_t {
    Recorder = 1,
    Playback = 2,
};

enum class MessageType : uint8_t {
    RecorderHello = 1,
    RecorderGameBegin = 2,
    RecorderFrames = 3,
    RecorderEvents = 4,
    RecorderGameEnd = 5,
    RecorderDrain = 6,
    RecorderBye = 7,
    RecorderPalette = 8,

    CasterGameAccepted = 16,
    CasterGameCommitted = 17,
    CasterGameFailed = 18,
    CasterRecorderStatus = 19,
    CasterDrainAck = 20,

    PlaybackHello = 32,
    PlaybackCommandAck = 33,
    PlaybackStatus = 34,
    PlaybackResult = 35,
    PlaybackBye = 36,
    PlaybackNextGameAck = 37,

    PlaybackSessionInfo = 48,
    PlaybackGameStart = 49,
    PlaybackFrames = 50,
    PlaybackEvents = 51,
    PlaybackEndpoint = 52,
    PlaybackCommand = 53,
    PlaybackPalette = 54,
    PlaybackNextGame = 55,
};

constexpr bool isKnownMessageType(uint8_t value) {
    return (value >= 1u && value <= 8u)
        || (value >= 16u && value <= 20u)
        || (value >= 32u && value <= 37u)
        || (value >= 48u && value <= 55u);
}

constexpr Channel channelForMessageType(uint8_t value) {
    return value < 32u ? Channel::Recorder : Channel::Playback;
}

constexpr bool requiresGameBinding(MessageType type) {
    switch (type) {
        case MessageType::RecorderGameBegin:
        case MessageType::RecorderFrames:
        case MessageType::RecorderEvents:
        case MessageType::RecorderGameEnd:
        case MessageType::RecorderPalette:
        case MessageType::CasterGameAccepted:
        case MessageType::CasterGameCommitted:
        case MessageType::CasterGameFailed:
        case MessageType::PlaybackHello:
        case MessageType::PlaybackCommandAck:
        case MessageType::PlaybackStatus:
        case MessageType::PlaybackResult:
        case MessageType::PlaybackBye:
        case MessageType::PlaybackNextGameAck:
        case MessageType::PlaybackSessionInfo:
        case MessageType::PlaybackGameStart:
        case MessageType::PlaybackFrames:
        case MessageType::PlaybackEvents:
        case MessageType::PlaybackEndpoint:
        case MessageType::PlaybackCommand:
        case MessageType::PlaybackPalette:
        case MessageType::PlaybackNextGame:
            return true;
        case MessageType::RecorderHello:
        case MessageType::RecorderDrain:
        case MessageType::RecorderBye:
        case MessageType::CasterRecorderStatus:
        case MessageType::CasterDrainAck:
            return false;
    }
    return true;
}

enum class FrameStatus : uint8_t {
    Ok = 0,
    NeedMore,
    BadMagic,
    BadWireVersion,
    BadHeaderCrc,
    BadPayloadCrc,
    TooLarge,
    UnknownChannel,
    UnknownType,
    BadLaunchBinding,
    BadGameBinding,
};

struct FrameHeader {
    uint32_t magic = kMagic;
    uint16_t wireVersion = kWireVersion;
    uint8_t channel = static_cast<uint8_t>(Channel::Recorder);
    uint8_t type = static_cast<uint8_t>(MessageType::RecorderHello);
    uint32_t launchGeneration = 0u;
    uint32_t gameGeneration = 0u;
    uint64_t gameUuidLow = 0u;
    uint64_t gameUuidHigh = 0u;
    uint64_t launchNonce = 0u;
    uint32_t sequence = 0u;
    uint32_t param0 = 0u;
    uint32_t param1 = 0u;
    uint32_t payloadBytes = 0u;
    uint32_t payloadCrc32c = 0u;
    uint32_t headerCrc32c = 0u;
};

static_assert(
    kHeaderBytes
        == 4u + 2u + 1u + 1u + 4u + 4u + 8u + 8u + 8u
             + 6u * 4u,
    "wire-v3 header layout");

inline void encodeHeader(
    const FrameHeader& header, std::vector<uint8_t>& out) {
    const size_t base = out.size();
    putU32(out, kMagic);
    putU16(out, kWireVersion);
    putU8(out, header.channel);
    putU8(out, header.type);
    putU32(out, header.launchGeneration);
    putU32(out, header.gameGeneration);
    putU64(out, header.gameUuidLow);
    putU64(out, header.gameUuidHigh);
    putU64(out, header.launchNonce);
    putU32(out, header.sequence);
    putU32(out, header.param0);
    putU32(out, header.param1);
    putU32(out, header.payloadBytes);
    putU32(out, header.payloadCrc32c);
    putU32(out, crc32c(out.data() + base, kHeaderCrcBytes));
}

inline FrameStatus decodeHeader(
    const uint8_t* data, size_t len, FrameHeader& out) {
    if (data == nullptr || len < kHeaderBytes) return FrameStatus::NeedMore;
    FrameHeader decoded{};
    size_t off = 0u;
    if (!getU32(data, len, off, decoded.magic)
        || !getU16(data, len, off, decoded.wireVersion)
        || !getU8(data, len, off, decoded.channel)
        || !getU8(data, len, off, decoded.type)
        || !getU32(data, len, off, decoded.launchGeneration)
        || !getU32(data, len, off, decoded.gameGeneration)
        || !getU64(data, len, off, decoded.gameUuidLow)
        || !getU64(data, len, off, decoded.gameUuidHigh)
        || !getU64(data, len, off, decoded.launchNonce)
        || !getU32(data, len, off, decoded.sequence)
        || !getU32(data, len, off, decoded.param0)
        || !getU32(data, len, off, decoded.param1)
        || !getU32(data, len, off, decoded.payloadBytes)
        || !getU32(data, len, off, decoded.payloadCrc32c)
        || !getU32(data, len, off, decoded.headerCrc32c)) {
        return FrameStatus::NeedMore;
    }
    if (decoded.magic != kMagic) return FrameStatus::BadMagic;
    if (crc32c(data, kHeaderCrcBytes) != decoded.headerCrc32c)
        return FrameStatus::BadHeaderCrc;
    if (decoded.wireVersion != kWireVersion)
        return FrameStatus::BadWireVersion;
    if (decoded.channel != static_cast<uint8_t>(Channel::Recorder)
        && decoded.channel != static_cast<uint8_t>(Channel::Playback)) {
        return FrameStatus::UnknownChannel;
    }
    if (!isKnownMessageType(decoded.type)) return FrameStatus::UnknownType;
    if (decoded.channel
        != static_cast<uint8_t>(channelForMessageType(decoded.type))) {
        return FrameStatus::UnknownChannel;
    }
    if (decoded.payloadBytes > kMaxPayloadBytes)
        return FrameStatus::TooLarge;
    if (decoded.launchGeneration == 0u || decoded.launchNonce == 0u)
        return FrameStatus::BadLaunchBinding;
    const MessageType type = static_cast<MessageType>(decoded.type);
    const bool hasGame = decoded.gameGeneration != 0u
        && (decoded.gameUuidLow != 0u || decoded.gameUuidHigh != 0u);
    if (requiresGameBinding(type)) {
        if (!hasGame) return FrameStatus::BadGameBinding;
    } else if (decoded.gameGeneration != 0u
               || decoded.gameUuidLow != 0u
               || decoded.gameUuidHigh != 0u) {
        return FrameStatus::BadGameBinding;
    }
    out = decoded;
    return FrameStatus::Ok;
}

struct Frame {
    Channel channel = Channel::Recorder;
    MessageType type = MessageType::RecorderHello;
    uint32_t launchGeneration = 0u;
    GameIdentityV2 game{};
    uint64_t launchNonce = 0u;
    uint32_t sequence = 0u;
    uint32_t param0 = 0u;
    uint32_t param1 = 0u;
    std::vector<uint8_t> payload;
};

inline bool encodeFrame(const Frame& frame, std::vector<uint8_t>& out) {
    if (frame.payload.size() > kMaxPayloadBytes
        || frame.launchGeneration == 0u || frame.launchNonce == 0u
        || !isKnownMessageType(static_cast<uint8_t>(frame.type))
        || static_cast<uint8_t>(frame.channel)
            != static_cast<uint8_t>(
                channelForMessageType(static_cast<uint8_t>(frame.type)))) {
        return false;
    }
    const bool needGame = requiresGameBinding(frame.type);
    if (needGame != isValidGameIdentityV2(frame.game))
        return false;
    FrameHeader header{};
    header.channel = static_cast<uint8_t>(frame.channel);
    header.type = static_cast<uint8_t>(frame.type);
    header.launchGeneration = frame.launchGeneration;
    header.gameGeneration = needGame ? frame.game.gameGeneration : 0u;
    header.gameUuidLow = needGame ? frame.game.gameUuidLow : 0u;
    header.gameUuidHigh = needGame ? frame.game.gameUuidHigh : 0u;
    header.launchNonce = frame.launchNonce;
    header.sequence = frame.sequence;
    header.param0 = frame.param0;
    header.param1 = frame.param1;
    header.payloadBytes = static_cast<uint32_t>(frame.payload.size());
    header.payloadCrc32c = frame.payload.empty()
        ? 0u : crc32c(frame.payload.data(), frame.payload.size());
    encodeHeader(header, out);
    if (!frame.payload.empty())
        putBytes(out, frame.payload.data(), frame.payload.size());
    return true;
}

inline FrameStatus decodeFrame(
    const uint8_t* data, size_t len, Frame& out, size_t& consumed) {
    FrameHeader header{};
    FrameStatus status = decodeHeader(data, len, header);
    if (status != FrameStatus::Ok) return status;
    size_t total = 0u;
    if (!addChecked(kHeaderBytes, header.payloadBytes, total))
        return FrameStatus::TooLarge;
    if (len < total) return FrameStatus::NeedMore;
    const uint8_t* payload = data + kHeaderBytes;
    const uint32_t payloadCrc = header.payloadBytes == 0u
        ? 0u : crc32c(payload, header.payloadBytes);
    if (payloadCrc != header.payloadCrc32c)
        return FrameStatus::BadPayloadCrc;
    Frame decoded{};
    decoded.channel = static_cast<Channel>(header.channel);
    decoded.type = static_cast<MessageType>(header.type);
    decoded.launchGeneration = header.launchGeneration;
    decoded.game.gameGeneration = header.gameGeneration;
    decoded.game.gameUuidLow = header.gameUuidLow;
    decoded.game.gameUuidHigh = header.gameUuidHigh;
    decoded.launchNonce = header.launchNonce;
    decoded.sequence = header.sequence;
    decoded.param0 = header.param0;
    decoded.param1 = header.param1;
    decoded.payload.assign(payload, payload + header.payloadBytes);
    out = decoded;
    consumed = total;
    return FrameStatus::Ok;
}

inline bool isZeroDigest(const std::array<uint8_t, 32>& digest) {
    return std::all_of(
        digest.begin(), digest.end(),
        [](uint8_t byte) { return byte == 0u; });
}

inline void putSimulationContract(
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

inline bool getSimulationContract(
    const uint8_t* data, size_t len, size_t& off,
    SimulationContractV2& contract) {
    uint32_t reserved = 0u;
    return getBytes(
               data, len, off, contract.executableSha256.data(),
               contract.executableSha256.size())
        && getU64(data, len, off, contract.nativeBuildIdentity)
        && getU64(data, len, off, contract.runtimeSimulationId)
        && getU64(
            data, len, off, contract.stateRegionSchemaFingerprint)
        && getU64(data, len, off, contract.stateHashProjectionId)
        && getU64(data, len, off, contract.stateHashAlgorithmId)
        && getBytes(
            data, len, off, contract.contentManifestRoot.data(),
            contract.contentManifestRoot.size())
        && getU64(data, len, off, contract.bootstrapSchemaId)
        && getU64(data, len, off, contract.eventSchemaId)
        && getU64(data, len, off, contract.characterCatalogId)
        && getU64(data, len, off, contract.terminalResultSchemaId)
        && getU32(data, len, off, contract.compactActionDomainVersion)
        && getU32(data, len, off, reserved) && reserved == 0u
        && isValidSimulationContractV2(contract);
}

enum RecorderFeatureV3 : uint16_t {
    RecorderFeatureCompositeBoundariesV3 = 1u << 0,
    RecorderFeatureConfirmedOnlyV3 = 1u << 1,
    RecorderFeatureFourSourceLanesV3 = 1u << 2,
    RecorderFeatureCommitAckV3 = 1u << 3,
};

constexpr uint16_t kRecorderRequiredFeaturesV3 =
    RecorderFeatureCompositeBoundariesV3
    | RecorderFeatureConfirmedOnlyV3
    | RecorderFeatureFourSourceLanesV3
    | RecorderFeatureCommitAckV3;
constexpr uint32_t kMaxRecorderQueueCapacityV3 = 65536u;

struct RecorderHelloPayload {
    uint32_t gamePid = 0u;
    uint16_t featureBits = kRecorderRequiredFeaturesV3;
    uint64_t runtimeCompatibilityId = 0u;
    uint32_t queueCapacity = 0u;
};

constexpr size_t kRecorderHelloPayloadBytes = 24u;

inline bool encodeRecorderHello(
    const RecorderHelloPayload& hello, std::vector<uint8_t>& out) {
    if (hello.gamePid == 0u
        || hello.featureBits != kRecorderRequiredFeaturesV3
        || hello.runtimeCompatibilityId == 0u
        || hello.queueCapacity < 2u
        || hello.queueCapacity > kMaxRecorderQueueCapacityV3) {
        return false;
    }
    const size_t before = out.size();
    putU32(out, hello.gamePid);
    putU16(out, kWireVersion);
    putU16(out, hello.featureBits);
    putU64(out, hello.runtimeCompatibilityId);
    putU32(out, hello.queueCapacity);
    putU32(out, 0u);
    return out.size() - before == kRecorderHelloPayloadBytes;
}

inline bool decodeRecorderHello(
    const uint8_t* data, size_t len, RecorderHelloPayload& out) {
    if (data == nullptr || len != kRecorderHelloPayloadBytes) return false;
    RecorderHelloPayload decoded{};
    size_t off = 0u;
    uint16_t wire = 0u;
    uint32_t reserved = 0u;
    if (!getU32(data, len, off, decoded.gamePid)
        || !getU16(data, len, off, wire)
        || !getU16(data, len, off, decoded.featureBits)
        || !getU64(data, len, off, decoded.runtimeCompatibilityId)
        || !getU32(data, len, off, decoded.queueCapacity)
        || !getU32(data, len, off, reserved)) {
        return false;
    }
    if (wire != kWireVersion || reserved != 0u
        || decoded.gamePid == 0u
        || decoded.featureBits != kRecorderRequiredFeaturesV3
        || decoded.runtimeCompatibilityId == 0u
        || decoded.queueCapacity < 2u
        || decoded.queueCapacity > kMaxRecorderQueueCapacityV3) {
        return false;
    }
    out = decoded;
    return true;
}

struct RecorderDrainPayload {
    uint32_t nextPublisherSequence = 0u;
    uint32_t gamesBegun = 0u;
    uint32_t gamesEnded = 0u;
    bool abandonedOpenGame = false;
};

constexpr size_t kRecorderDrainPayloadBytes = 16u;

inline bool encodeRecorderDrain(
    const RecorderDrainPayload& drain, std::vector<uint8_t>& out) {
    if (drain.nextPublisherSequence == 0u
        || drain.gamesEnded > drain.gamesBegun
        || drain.gamesBegun - drain.gamesEnded
            != (drain.abandonedOpenGame ? 1u : 0u)) {
        return false;
    }
    const size_t before = out.size();
    putU32(out, drain.nextPublisherSequence);
    putU32(out, drain.gamesBegun);
    putU32(out, drain.gamesEnded);
    putU8(out, drain.abandonedOpenGame ? 1u : 0u);
    putU8(out, 0u);
    putU16(out, 0u);
    return out.size() - before == kRecorderDrainPayloadBytes;
}

inline bool decodeRecorderDrain(
    const uint8_t* data, size_t len, RecorderDrainPayload& out) {
    if (data == nullptr || len != kRecorderDrainPayloadBytes)
        return false;
    RecorderDrainPayload decoded{};
    size_t off = 0u;
    uint8_t abandoned = 0u;
    uint8_t reserved8 = 0u;
    uint16_t reserved16 = 0u;
    if (!getU32(data, len, off, decoded.nextPublisherSequence)
        || !getU32(data, len, off, decoded.gamesBegun)
        || !getU32(data, len, off, decoded.gamesEnded)
        || !getU8(data, len, off, abandoned)
        || !getU8(data, len, off, reserved8)
        || !getU16(data, len, off, reserved16)
        || abandoned > 1u || reserved8 != 0u
        || reserved16 != 0u) {
        return false;
    }
    decoded.abandonedOpenGame = abandoned != 0u;
    std::vector<uint8_t> canonical;
    if (!encodeRecorderDrain(decoded, canonical)
        || canonical.size() != len
        || !std::equal(canonical.begin(), canonical.end(), data)) {
        return false;
    }
    out = decoded;
    return true;
}

enum class RecorderByeReason : uint16_t {
    NormalAfterDrain = 1,
    ProcessExit = 2,
    ChannelFailure = 3,
    InternalFailure = 4,
};

struct RecorderByePayload {
    RecorderByeReason reason = RecorderByeReason::InternalFailure;
    bool drainCompleted = false;
    uint32_t nextPublisherSequence = 0u;
    uint64_t detail = 0u;
};

constexpr size_t kRecorderByePayloadBytes = 16u;

inline bool encodeRecorderBye(
    const RecorderByePayload& bye, std::vector<uint8_t>& out) {
    const uint16_t reason = static_cast<uint16_t>(bye.reason);
    if (reason < 1u || reason > 4u
        || bye.nextPublisherSequence == 0u
        || (bye.reason == RecorderByeReason::NormalAfterDrain
            && !bye.drainCompleted)) {
        return false;
    }
    const size_t before = out.size();
    putU16(out, reason);
    putU8(out, bye.drainCompleted ? 1u : 0u);
    putU8(out, 0u);
    putU32(out, bye.nextPublisherSequence);
    putU64(out, bye.detail);
    return out.size() - before == kRecorderByePayloadBytes;
}

inline bool decodeRecorderBye(
    const uint8_t* data, size_t len, RecorderByePayload& out) {
    if (data == nullptr || len != kRecorderByePayloadBytes)
        return false;
    RecorderByePayload decoded{};
    size_t off = 0u;
    uint16_t reason = 0u;
    uint8_t drained = 0u;
    uint8_t reserved = 0u;
    if (!getU16(data, len, off, reason)
        || !getU8(data, len, off, drained)
        || !getU8(data, len, off, reserved)
        || !getU32(data, len, off, decoded.nextPublisherSequence)
        || !getU64(data, len, off, decoded.detail)
        || reason < 1u || reason > 4u || drained > 1u
        || reserved != 0u) {
        return false;
    }
    decoded.reason = static_cast<RecorderByeReason>(reason);
    decoded.drainCompleted = drained != 0u;
    std::vector<uint8_t> canonical;
    if (!encodeRecorderBye(decoded, canonical)
        || canonical.size() != len
        || !std::equal(canonical.begin(), canonical.end(), data)) {
        return false;
    }
    out = decoded;
    return true;
}

inline bool encodeRecorderGameBegin(
    const BeginGameBundleV2& begin, std::vector<uint8_t>& out) {
    return encodeBeginGameBundleV2(begin, out);
}

inline ParseStatus decodeRecorderGameBegin(
    const uint8_t* data, size_t len, BeginGameBundleV2& out) {
    return decodeBeginGameBundleV2(data, len, out);
}

inline bool encodePlaybackPalette(
    const ReplayPaletteV2& palette, std::vector<uint8_t>& out) {
    return encodePaletteBlockV2(palette, out);
}

inline ParseStatus decodePlaybackPalette(
    const uint8_t* data, size_t len, ReplayPaletteV2& out) {
    return decodePaletteBlockV2(data, len, out);
}

inline bool encodeRecorderPalette(
    const ReplayPaletteV2& palette, std::vector<uint8_t>& out) {
    return encodePaletteBlockV2(palette, out);
}

inline ParseStatus decodeRecorderPalette(
    const uint8_t* data, size_t len, ReplayPaletteV2& out) {
    return decodePaletteBlockV2(data, len, out);
}

inline bool encodeRecorderEvents(
    const ConfirmedGameEventV2* events, size_t count,
    std::vector<uint8_t>& out) {
    return encodeEventBlockV2(events, count, out);
}

inline ParseStatus decodeRecorderEvents(
    const uint8_t* data, size_t len,
    uint32_t expectedFirstEventSequence, uint32_t eventCount,
    std::vector<ConfirmedGameEventV2>& out) {
    return decodeEventBlockV2(
        data, len, expectedFirstEventSequence, eventCount, out);
}

inline bool encodeRecorderGameEnd(
    const EndGameBundleV2& end, std::vector<uint8_t>& out) {
    return encodeEndGameBundleV2(end, out);
}

inline ParseStatus decodeRecorderGameEnd(
    const uint8_t* data, size_t len, uint8_t allowedLaneMask,
    EndGameBundleV2& out) {
    return decodeEndGameBundleV2(
        data, len, allowedLaneMask, out);
}

inline bool encodeRecorderFrames(
    const ConfirmedGameFrameV2* frames, size_t count,
    std::vector<uint8_t>& out) {
    if (frames == nullptr || count == 0u
        || count > kMaxRecordsPerInputBlock) return false;
    const uint32_t first = frames[0].replayFrame;
    size_t bytes = 0u;
    if (!mulChecked(count, kConfirmedGameFrameTransportBytesV2, bytes)
        || bytes > kMaxPayloadBytes) return false;
    std::vector<uint8_t> encoded;
    encoded.reserve(bytes);
    for (size_t i = 0u; i < count; ++i) {
        if (frames[i].replayFrame != first + static_cast<uint32_t>(i)
            || !encodeConfirmedGameFrameTransportV2(frames[i], encoded)) {
            return false;
        }
    }
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

inline ParseStatus decodeRecorderFrames(
    const uint8_t* data, size_t len, uint32_t firstRecord,
    uint32_t count, uint8_t allowedLaneMask,
    std::vector<ConfirmedGameFrameV2>& out) {
    if (count == 0u || count > kMaxRecordsPerInputBlock)
        return ParseStatus::LimitExceeded;
    size_t expected = 0u;
    if (!mulChecked(count, kConfirmedGameFrameTransportBytesV2, expected))
        return ParseStatus::IntegerOverflow;
    if (data == nullptr || len < expected) return ParseStatus::Truncated;
    if (len > expected) return ParseStatus::LimitExceeded;
    std::vector<ConfirmedGameFrameV2> decoded(count);
    size_t off = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        ParseStatus status = decodeConfirmedGameFrameTransportV2(
            data + off, kConfirmedGameFrameTransportBytesV2,
            allowedLaneMask, decoded[i]);
        if (status != ParseStatus::Ok) return status;
        if (decoded[i].replayFrame != firstRecord + i)
            return ParseStatus::FrameGap;
        off += kConfirmedGameFrameTransportBytesV2;
    }
    out.swap(decoded);
    return ParseStatus::Ok;
}

struct CasterGameAcceptedPayload {
    uint32_t nextRecord = 0u;
    uint32_t nextEventSequence = 0u;
};

constexpr size_t kCasterGameAcceptedPayloadBytes = 16u;

inline bool encodeCasterGameAccepted(
    const CasterGameAcceptedPayload& accepted,
    std::vector<uint8_t>& out) {
    if (accepted.nextRecord != 1u
        || accepted.nextEventSequence < 2u
        || accepted.nextEventSequence > kMaxGameEventsV3) {
        return false;
    }
    const size_t before = out.size();
    putU32(out, accepted.nextRecord);
    putU32(out, accepted.nextEventSequence);
    putU64(out, 0u);
    return out.size() - before == kCasterGameAcceptedPayloadBytes;
}

inline bool decodeCasterGameAccepted(
    const uint8_t* data, size_t len, CasterGameAcceptedPayload& out) {
    if (data == nullptr || len != kCasterGameAcceptedPayloadBytes)
        return false;
    CasterGameAcceptedPayload decoded{};
    size_t off = 0u;
    uint64_t reserved = 0u;
    if (!getU32(data, len, off, decoded.nextRecord)
        || !getU32(data, len, off, decoded.nextEventSequence)
        || !getU64(data, len, off, reserved)
        || reserved != 0u || decoded.nextRecord != 1u
        || decoded.nextEventSequence < 2u
        || decoded.nextEventSequence > kMaxGameEventsV3) {
        return false;
    }
    out = decoded;
    return true;
}

struct CasterGameCommittedPayload {
    uint32_t nextRecord = 0u;
    TerminalReason reason = TerminalReason::Completed;
    std::array<uint8_t, 32> canonicalGameDigest{};
    std::array<uint8_t, 32> fileDigest{};
    uint64_t fileBytes = 0u;
};

constexpr size_t kCasterGameCommittedPayloadBytes = 80u;

inline bool encodeCasterGameCommitted(
    const CasterGameCommittedPayload& committed,
    std::vector<uint8_t>& out) {
    if (committed.nextRecord == 0u
        || committed.nextRecord > kMaxTimelineFrames
        || isZeroDigest(committed.canonicalGameDigest)
        || isZeroDigest(committed.fileDigest)
        || committed.fileBytes == 0u
        || committed.fileBytes > kMaxFileBytes
        || !isKnownTerminalReason(
            static_cast<uint16_t>(committed.reason))) return false;
    const size_t before = out.size();
    putU32(out, committed.nextRecord);
    putU16(out, static_cast<uint16_t>(committed.reason));
    putU16(out, 0u);
    putBytes(
        out, committed.canonicalGameDigest.data(),
        committed.canonicalGameDigest.size());
    putBytes(out, committed.fileDigest.data(), committed.fileDigest.size());
    putU64(out, committed.fileBytes);
    return out.size() - before == kCasterGameCommittedPayloadBytes;
}

inline bool decodeCasterGameCommitted(
    const uint8_t* data, size_t len, CasterGameCommittedPayload& out) {
    if (data == nullptr || len != kCasterGameCommittedPayloadBytes)
        return false;
    CasterGameCommittedPayload decoded{};
    size_t off = 0u;
    uint16_t reason = 0u;
    uint16_t reserved = 0u;
    if (!getU32(data, len, off, decoded.nextRecord)
        || !getU16(data, len, off, reason)
        || !getU16(data, len, off, reserved)
        || !getBytes(
            data, len, off, decoded.canonicalGameDigest.data(),
            decoded.canonicalGameDigest.size())
        || !getBytes(
            data, len, off, decoded.fileDigest.data(),
            decoded.fileDigest.size())
        || !getU64(data, len, off, decoded.fileBytes)) return false;
    if (reserved != 0u || decoded.nextRecord == 0u
        || decoded.nextRecord > kMaxTimelineFrames
        || isZeroDigest(decoded.canonicalGameDigest)
        || isZeroDigest(decoded.fileDigest)
        || decoded.fileBytes == 0u || decoded.fileBytes > kMaxFileBytes
        || !isKnownTerminalReason(reason)) return false;
    decoded.reason = static_cast<TerminalReason>(reason);
    out = decoded;
    return true;
}

enum class RecorderFailureCode : uint16_t {
    Protocol = 1,
    Identity = 2,
    Sequence = 3,
    Validation = 4,
    Storage = 5,
    Flush = 6,
    Rename = 7,
    ProcessExit = 8,
    Internal = 9,
};

struct CasterGameFailedPayload {
    RecorderFailureCode code = RecorderFailureCode::Internal;
    uint16_t stage = 0u;
    uint32_t nextRecord = 0u;
    uint32_t nextEventSequence = 0u;
    uint32_t platformError = 0u;
    uint64_t detail = 0u;
};

constexpr size_t kCasterGameFailedPayloadBytes = 24u;

inline bool encodeCasterGameFailed(
    const CasterGameFailedPayload& failed, std::vector<uint8_t>& out) {
    const uint16_t code = static_cast<uint16_t>(failed.code);
    if (code < 1u || code > 9u
        || failed.nextRecord > kMaxTimelineFrames
        || failed.nextEventSequence > kMaxGameEventsV3) return false;
    const size_t before = out.size();
    putU16(out, code);
    putU16(out, failed.stage);
    putU32(out, failed.nextRecord);
    putU32(out, failed.nextEventSequence);
    putU32(out, failed.platformError);
    putU64(out, failed.detail);
    return out.size() - before == kCasterGameFailedPayloadBytes;
}

inline bool decodeCasterGameFailed(
    const uint8_t* data, size_t len, CasterGameFailedPayload& out) {
    if (data == nullptr || len != kCasterGameFailedPayloadBytes)
        return false;
    CasterGameFailedPayload decoded{};
    size_t off = 0u;
    uint16_t code = 0u;
    if (!getU16(data, len, off, code)
        || !getU16(data, len, off, decoded.stage)
        || !getU32(data, len, off, decoded.nextRecord)
        || !getU32(data, len, off, decoded.nextEventSequence)
        || !getU32(data, len, off, decoded.platformError)
        || !getU64(data, len, off, decoded.detail)
        || code < 1u || code > 9u
        || decoded.nextRecord > kMaxTimelineFrames
        || decoded.nextEventSequence > kMaxGameEventsV3) return false;
    decoded.code = static_cast<RecorderFailureCode>(code);
    out = decoded;
    return true;
}

enum class RecorderHostState : uint8_t {
    Listening = 1,
    Connected = 2,
    WaitingForGame = 3,
    WritingGame = 4,
    FinalizingGame = 5,
    Draining = 6,
    Finished = 7,
    FailedTransport = 8,
    FailedFile = 9,
};

struct CasterRecorderStatusPayload {
    RecorderHostState state = RecorderHostState::Listening;
    uint8_t processAlive = 0u;
    uint8_t commitPending = 0u;
    uint32_t nextRecord = 0u;
    uint32_t nextEventSequence = 0u;
    uint32_t queueHighWater = 0u;
    uint32_t droppedCount = 0u;
    uint32_t chunkCount = 0u;
    uint64_t bytesWritten = 0u;
    uint64_t flushLatencyMicros = 0u;
    uint64_t lastPostStateHash = 0u;
    uint64_t lastRollingDigest = 0u;
};

constexpr size_t kCasterRecorderStatusPayloadBytes = 64u;

inline bool encodeCasterRecorderStatus(
    const CasterRecorderStatusPayload& status,
    std::vector<uint8_t>& out) {
    const uint8_t state = static_cast<uint8_t>(status.state);
    if (state < 1u || state > 9u || status.processAlive > 1u
        || status.commitPending > 1u
        || status.nextRecord > kMaxTimelineFrames
        || status.nextEventSequence > kMaxGameEventsV3) return false;
    const size_t before = out.size();
    putU8(out, state);
    putU8(out, status.processAlive);
    putU8(out, status.commitPending);
    putU8(out, 0u);
    putU32(out, status.nextRecord);
    putU32(out, status.nextEventSequence);
    putU32(out, status.queueHighWater);
    putU32(out, status.droppedCount);
    putU32(out, status.chunkCount);
    putU32(out, 0u);
    putU64(out, status.bytesWritten);
    putU64(out, status.flushLatencyMicros);
    putU64(out, status.lastPostStateHash);
    putU64(out, status.lastRollingDigest);
    putU32(out, 0u);
    return out.size() - before == kCasterRecorderStatusPayloadBytes;
}

inline bool decodeCasterRecorderStatus(
    const uint8_t* data, size_t len, CasterRecorderStatusPayload& out) {
    if (data == nullptr || len != kCasterRecorderStatusPayloadBytes)
        return false;
    CasterRecorderStatusPayload decoded{};
    size_t off = 0u;
    uint8_t state = 0u;
    uint8_t reserved8 = 0u;
    uint32_t reserved32 = 0u;
    uint32_t reservedTail = 0u;
    if (!getU8(data, len, off, state)
        || !getU8(data, len, off, decoded.processAlive)
        || !getU8(data, len, off, decoded.commitPending)
        || !getU8(data, len, off, reserved8)
        || !getU32(data, len, off, decoded.nextRecord)
        || !getU32(data, len, off, decoded.nextEventSequence)
        || !getU32(data, len, off, decoded.queueHighWater)
        || !getU32(data, len, off, decoded.droppedCount)
        || !getU32(data, len, off, decoded.chunkCount)
        || !getU32(data, len, off, reserved32)
        || !getU64(data, len, off, decoded.bytesWritten)
        || !getU64(data, len, off, decoded.flushLatencyMicros)
        || !getU64(data, len, off, decoded.lastPostStateHash)
        || !getU64(data, len, off, decoded.lastRollingDigest)
        || !getU32(data, len, off, reservedTail)) {
        return false;
    }
    if (state < 1u || state > 9u || decoded.processAlive > 1u
        || decoded.commitPending > 1u || reserved8 != 0u
        || reserved32 != 0u || reservedTail != 0u
        || decoded.nextRecord > kMaxTimelineFrames
        || decoded.nextEventSequence > kMaxGameEventsV3) return false;
    decoded.state = static_cast<RecorderHostState>(state);
    out = decoded;
    return true;
}

struct CasterDrainAckPayload {
    uint32_t unresolvedGames = 0u;
    uint32_t committedGames = 0u;
    uint32_t failedGames = 0u;
};

constexpr size_t kCasterDrainAckPayloadBytes = 16u;

inline bool encodeCasterDrainAck(
    const CasterDrainAckPayload& ack, std::vector<uint8_t>& out) {
    const size_t before = out.size();
    putU32(out, ack.unresolvedGames);
    putU32(out, ack.committedGames);
    putU32(out, ack.failedGames);
    putU32(out, 0u);
    return out.size() - before == kCasterDrainAckPayloadBytes;
}

inline bool decodeCasterDrainAck(
    const uint8_t* data, size_t len, CasterDrainAckPayload& out) {
    if (data == nullptr || len != kCasterDrainAckPayloadBytes)
        return false;
    CasterDrainAckPayload decoded{};
    size_t off = 0u;
    uint32_t reserved = 0u;
    if (!getU32(data, len, off, decoded.unresolvedGames)
        || !getU32(data, len, off, decoded.committedGames)
        || !getU32(data, len, off, decoded.failedGames)
        || !getU32(data, len, off, reserved) || reserved != 0u) {
        return false;
    }
    out = decoded;
    return true;
}

enum PlaybackFeatureV3 : uint16_t {
    PlaybackFeatureFullPrebufferV3 = 1u << 0,
    PlaybackFeatureHeadlessV3 = 1u << 1,
    PlaybackFeaturePresentationAckV3 = 1u << 2,
    PlaybackFeatureDeviceIsolationV3 = 1u << 3,
    PlaybackFeatureFourSourceLanesV3 = 1u << 4,
};

constexpr uint16_t kPlaybackRequiredFeaturesV3 =
    PlaybackFeatureFullPrebufferV3
    | PlaybackFeatureHeadlessV3
    | PlaybackFeaturePresentationAckV3
    | PlaybackFeatureDeviceIsolationV3
    | PlaybackFeatureFourSourceLanesV3;

struct PlaybackHelloPayload {
    uint32_t gamePid = 0u;
    uint16_t featureBits = kPlaybackRequiredFeaturesV3;
    bool headless = false;
    uint32_t maxPrebufferFrames = kMaxPlaybackPrebufferFramesV2;
    uint16_t maxEventsPerTick =
        static_cast<uint16_t>(kMaxEventsPerTickV2);
    uint16_t containerReaderMajor = kContainerReaderMajorV2;
    uint64_t runtimeCompatibilityId = 0u;
    uint64_t stateHashAlgorithmId = 0u;
    uint64_t bootstrapSchemaId = 0u;
};

constexpr size_t kPlaybackHelloPayloadBytes = 40u;

inline bool encodePlaybackHello(
    const PlaybackHelloPayload& hello, std::vector<uint8_t>& out) {
    if (hello.gamePid == 0u
        || hello.featureBits != kPlaybackRequiredFeaturesV3
        || hello.maxPrebufferFrames != kMaxPlaybackPrebufferFramesV2
        || hello.maxEventsPerTick != kMaxEventsPerTickV2
        || hello.containerReaderMajor != kContainerReaderMajorV2
        || hello.runtimeCompatibilityId == 0u
        || hello.stateHashAlgorithmId == 0u
        || hello.bootstrapSchemaId == 0u) {
        return false;
    }
    const size_t before = out.size();
    putU32(out, hello.gamePid);
    putU16(out, hello.featureBits);
    putU8(out, hello.headless ? 1u : 0u);
    putU8(out, 0u);
    putU32(out, hello.maxPrebufferFrames);
    putU16(out, hello.maxEventsPerTick);
    putU16(out, hello.containerReaderMajor);
    putU64(out, hello.runtimeCompatibilityId);
    putU64(out, hello.stateHashAlgorithmId);
    putU64(out, hello.bootstrapSchemaId);
    return out.size() - before == kPlaybackHelloPayloadBytes;
}

inline bool decodePlaybackHello(
    const uint8_t* data, size_t len, PlaybackHelloPayload& out) {
    if (data == nullptr || len != kPlaybackHelloPayloadBytes)
        return false;
    PlaybackHelloPayload decoded{};
    size_t off = 0u;
    uint8_t headless = 0u;
    uint8_t reserved = 0u;
    if (!getU32(data, len, off, decoded.gamePid)
        || !getU16(data, len, off, decoded.featureBits)
        || !getU8(data, len, off, headless)
        || !getU8(data, len, off, reserved)
        || !getU32(data, len, off, decoded.maxPrebufferFrames)
        || !getU16(data, len, off, decoded.maxEventsPerTick)
        || !getU16(data, len, off, decoded.containerReaderMajor)
        || !getU64(data, len, off, decoded.runtimeCompatibilityId)
        || !getU64(data, len, off, decoded.stateHashAlgorithmId)
        || !getU64(data, len, off, decoded.bootstrapSchemaId)
        || headless > 1u || reserved != 0u) {
        return false;
    }
    decoded.headless = headless != 0u;
    std::vector<uint8_t> canonical;
    if (!encodePlaybackHello(decoded, canonical)
        || canonical.size() != len
        || !std::equal(canonical.begin(), canonical.end(), data)) {
        return false;
    }
    out = decoded;
    return true;
}

enum class PlaybackEndpointKind : uint8_t {
    RecordedGameEnd = 1,
    UnsealedValidatedPrefix = 2,
};

struct PlaybackEndpointPayload {
    PlaybackEndpointKind kind =
        PlaybackEndpointKind::RecordedGameEnd;
    bool hasRecordedGameEnd = false;
    uint32_t nextRecord = 0u;
    uint64_t postTailStateHash = 0u;
    uint64_t finalRollingDigest = 0u;
    uint64_t validatedSourceByteCount = 0u;
    std::array<uint8_t, 32> validatedSourceRangeDigest{};
    ConfirmedGameEndV2 recordedGameEnd{};
};

constexpr size_t kPlaybackEndpointPayloadBytes = 128u;

inline bool encodePlaybackEndpoint(
    const PlaybackEndpointPayload& endpoint, std::vector<uint8_t>& out) {
    const uint8_t kind = static_cast<uint8_t>(endpoint.kind);
    if (kind < 1u || kind > 2u || endpoint.nextRecord == 0u
        || endpoint.nextRecord > kMaxPlaybackPrebufferFramesV2
        || endpoint.validatedSourceByteCount == 0u
        || endpoint.validatedSourceByteCount > kMaxFileBytes
        || isZeroDigest(endpoint.validatedSourceRangeDigest)
        || endpoint.hasRecordedGameEnd
            != (endpoint.kind
                == PlaybackEndpointKind::RecordedGameEnd)) return false;
    if (endpoint.hasRecordedGameEnd
        && (endpoint.recordedGameEnd.nextRecord != endpoint.nextRecord
            || endpoint.recordedGameEnd.postTailStateHash
                != endpoint.postTailStateHash
            || endpoint.recordedGameEnd.finalRollingDigest
                != endpoint.finalRollingDigest
            || isZeroDigest(
                endpoint.recordedGameEnd.canonicalGameDigest))) {
        return false;
    }
    if (!endpoint.hasRecordedGameEnd) {
        const ConfirmedGameEndV2 zero{};
        if (endpoint.recordedGameEnd != zero) return false;
    }
    const size_t before = out.size();
    putU8(out, kind);
    putU8(out, endpoint.hasRecordedGameEnd ? 1u : 0u);
    putU16(out, 0u);
    putU32(out, endpoint.nextRecord);
    putU64(out, endpoint.postTailStateHash);
    putU64(out, endpoint.finalRollingDigest);
    putU64(out, endpoint.validatedSourceByteCount);
    putBytes(
        out, endpoint.validatedSourceRangeDigest.data(),
        endpoint.validatedSourceRangeDigest.size());
    if (endpoint.hasRecordedGameEnd) {
        if (!encodeConfirmedGameEndV2(endpoint.recordedGameEnd, out))
            return false;
    } else {
        std::array<uint8_t, kConfirmedGameEndBytesV2> zero{};
        putBytes(out, zero.data(), zero.size());
    }
    return out.size() - before == kPlaybackEndpointPayloadBytes;
}

inline bool decodePlaybackEndpoint(
    const uint8_t* data, size_t len, PlaybackEndpointPayload& out) {
    if (data == nullptr || len != kPlaybackEndpointPayloadBytes)
        return false;
    PlaybackEndpointPayload decoded{};
    size_t off = 0u;
    uint8_t kind = 0u;
    uint8_t hasEnd = 0u;
    uint16_t reserved = 0u;
    if (!getU8(data, len, off, kind)
        || !getU8(data, len, off, hasEnd)
        || !getU16(data, len, off, reserved)
        || !getU32(data, len, off, decoded.nextRecord)
        || !getU64(data, len, off, decoded.postTailStateHash)
        || !getU64(data, len, off, decoded.finalRollingDigest)
        || !getU64(data, len, off, decoded.validatedSourceByteCount)
        || !getBytes(
            data, len, off, decoded.validatedSourceRangeDigest.data(),
            decoded.validatedSourceRangeDigest.size())) return false;
    if (kind < 1u || kind > 2u || hasEnd > 1u || reserved != 0u
        || decoded.nextRecord == 0u) return false;
    decoded.kind = static_cast<PlaybackEndpointKind>(kind);
    decoded.hasRecordedGameEnd = hasEnd != 0u;
    if (decoded.hasRecordedGameEnd) {
        if (decodeConfirmedGameEndV2(
                data + off, kConfirmedGameEndBytesV2,
                decoded.recordedGameEnd) != ParseStatus::Ok) return false;
    } else {
        for (size_t i = 0u; i < kConfirmedGameEndBytesV2; ++i)
            if (data[off + i] != 0u) return false;
    }
    std::vector<uint8_t> canonical;
    if (!encodePlaybackEndpoint(decoded, canonical)) return false;
    out = decoded;
    return true;
}

struct PlaybackSessionInfoPayload {
    uint16_t containerMajor = kContainerVersionMajorV2;
    uint16_t containerMinor = kContainerVersionMinorV2;
    uint16_t minimumReaderMajor = kContainerReaderMajorV2;
    uint32_t featureFlags = 0u;
    uint32_t frameCount = 0u;
    uint32_t eventCount = 0u;
    uint8_t activeLaneMask = 0u;
    uint8_t activeFighterMask = 0u;
    bool hasFileDigest = false;
    bool hasCanonicalGameDigest = false;
    std::array<uint8_t, 32> fileDigest{};
    std::array<uint8_t, 32> canonicalGameDigest{};
    uint64_t validatedSourceByteCount = 0u;
    std::array<uint8_t, 32> validatedSourceRangeDigest{};
    SimulationContractV2 recordedContract{};
    PlaybackEndpointPayload endpoint{};
};

constexpr size_t kPlaybackSessionInfoPayloadBytes = 400u;

inline bool encodePlaybackSessionInfo(
    const PlaybackSessionInfoPayload& info,
    std::vector<uint8_t>& out) {
    if (info.containerMajor != kContainerVersionMajorV2
        || info.containerMinor != kContainerVersionMinorV2
        || info.minimumReaderMajor != kContainerReaderMajorV2
        || (info.featureFlags & ~kFeatureRequiredMaskV2) != 0u
        || info.frameCount == 0u
        || info.frameCount > kMaxPlaybackPrebufferFramesV2
        || info.activeLaneMask == 0u
        || (info.activeLaneMask & ~0x0Fu) != 0u
        || info.activeFighterMask == 0u
        || (info.activeFighterMask & ~0x0Fu) != 0u
        || info.eventCount
            > info.frameCount
                * static_cast<uint32_t>(2u * kMaxEventsPerTickV2)
        || info.validatedSourceByteCount == 0u
        || isZeroDigest(info.validatedSourceRangeDigest)
        || !isValidSimulationContractV2(info.recordedContract)
        || info.endpoint.nextRecord != info.frameCount
        || info.endpoint.validatedSourceByteCount
            != info.validatedSourceByteCount
        || info.endpoint.validatedSourceRangeDigest
            != info.validatedSourceRangeDigest) {
        return false;
    }
    const bool sealed =
        info.endpoint.kind == PlaybackEndpointKind::RecordedGameEnd;
    if (info.hasFileDigest != sealed
        || info.hasCanonicalGameDigest != sealed
        || (!info.hasFileDigest && !isZeroDigest(info.fileDigest))
        || (!info.hasCanonicalGameDigest
            && !isZeroDigest(info.canonicalGameDigest))
        || (info.hasFileDigest && isZeroDigest(info.fileDigest))
        || (info.hasCanonicalGameDigest
            && (isZeroDigest(info.canonicalGameDigest)
                || !info.endpoint.hasRecordedGameEnd
                || info.endpoint.recordedGameEnd.canonicalGameDigest
                    != info.canonicalGameDigest))) {
        return false;
    }
    std::vector<uint8_t> endpoint;
    if (!encodePlaybackEndpoint(info.endpoint, endpoint)
        || endpoint.size() != kPlaybackEndpointPayloadBytes) {
        return false;
    }
    const size_t before = out.size();
    putU16(out, info.containerMajor);
    putU16(out, info.containerMinor);
    putU16(out, info.minimumReaderMajor);
    putU16(out, 0u);
    putU32(out, info.featureFlags);
    putU32(out, info.frameCount);
    putU32(out, info.eventCount);
    putU8(out, info.activeLaneMask);
    putU8(out, info.activeFighterMask);
    uint8_t digestFlags = 0u;
    if (info.hasFileDigest) digestFlags |= 1u << 0;
    if (info.hasCanonicalGameDigest) digestFlags |= 1u << 1;
    putU8(out, digestFlags);
    putU8(out, 0u);
    putBytes(out, info.fileDigest.data(), info.fileDigest.size());
    putBytes(
        out, info.canonicalGameDigest.data(),
        info.canonicalGameDigest.size());
    putU64(out, info.validatedSourceByteCount);
    putBytes(
        out, info.validatedSourceRangeDigest.data(),
        info.validatedSourceRangeDigest.size());
    putSimulationContract(out, info.recordedContract);
    putBytes(out, endpoint.data(), endpoint.size());
    return out.size() - before == kPlaybackSessionInfoPayloadBytes;
}

inline bool decodePlaybackSessionInfo(
    const uint8_t* data, size_t len,
    PlaybackSessionInfoPayload& out) {
    if (data == nullptr || len != kPlaybackSessionInfoPayloadBytes)
        return false;
    PlaybackSessionInfoPayload decoded{};
    size_t off = 0u;
    uint16_t reserved16 = 0u;
    uint8_t digestFlags = 0u;
    uint8_t reserved8 = 0u;
    if (!getU16(data, len, off, decoded.containerMajor)
        || !getU16(data, len, off, decoded.containerMinor)
        || !getU16(data, len, off, decoded.minimumReaderMajor)
        || !getU16(data, len, off, reserved16)
        || !getU32(data, len, off, decoded.featureFlags)
        || !getU32(data, len, off, decoded.frameCount)
        || !getU32(data, len, off, decoded.eventCount)
        || !getU8(data, len, off, decoded.activeLaneMask)
        || !getU8(data, len, off, decoded.activeFighterMask)
        || !getU8(data, len, off, digestFlags)
        || !getU8(data, len, off, reserved8)
        || !getBytes(
            data, len, off, decoded.fileDigest.data(),
            decoded.fileDigest.size())
        || !getBytes(
            data, len, off, decoded.canonicalGameDigest.data(),
            decoded.canonicalGameDigest.size())
        || !getU64(
            data, len, off, decoded.validatedSourceByteCount)
        || !getBytes(
            data, len, off,
            decoded.validatedSourceRangeDigest.data(),
            decoded.validatedSourceRangeDigest.size())
        || !getSimulationContract(
            data, len, off, decoded.recordedContract)
        || reserved16 != 0u || reserved8 != 0u
        || (digestFlags & ~0x03u) != 0u) {
        return false;
    }
    decoded.hasFileDigest = (digestFlags & (1u << 0)) != 0u;
    decoded.hasCanonicalGameDigest =
        (digestFlags & (1u << 1)) != 0u;
    if (decodePlaybackEndpoint(
            data + off, kPlaybackEndpointPayloadBytes,
            decoded.endpoint) == false) {
        return false;
    }
    off += kPlaybackEndpointPayloadBytes;
    std::vector<uint8_t> canonical;
    if (off != len
        || !encodePlaybackSessionInfo(decoded, canonical)
        || canonical.size() != len
        || !std::equal(canonical.begin(), canonical.end(), data)) {
        return false;
    }
    out = decoded;
    return true;
}

struct PlaybackNextGamePayload {
    uint64_t switchId = 0u;
    GameIdentityV2 game{};
    uint32_t frameCount = 0u;
    uint32_t eventCount = 0u;
    uint64_t validatedSourceByteCount = 0u;
    std::array<uint8_t, 32> validatedSourceRangeDigest{};
    PlaybackEndpointKind endpointKind = PlaybackEndpointKind::RecordedGameEnd;
    bool supersededStateSchema = false;
    uint16_t presentationFlags = 0u;
    SimulationContractV2 runtimeContract{};
};

constexpr size_t kPlaybackNextGamePayloadBytes = 224u;

inline bool encodePlaybackNextGame(
    const PlaybackNextGamePayload& next, std::vector<uint8_t>& out) {
    if (next.switchId == 0u || !isValidGameIdentityV2(next.game)
        || next.frameCount == 0u
        || next.frameCount > kMaxPlaybackPrebufferFramesV2
        || next.eventCount > next.frameCount * (2u * kMaxEventsPerTickV2)
        || next.validatedSourceByteCount == 0u
        || next.validatedSourceByteCount > kMaxFileBytes
        || isZeroDigest(next.validatedSourceRangeDigest)
        || (next.endpointKind != PlaybackEndpointKind::RecordedGameEnd
            && next.endpointKind != PlaybackEndpointKind::UnsealedValidatedPrefix)
        || (next.presentationFlags & ~31u) != 0u
        || !isValidSimulationContractV2(next.runtimeContract)) return false;
    const size_t before = out.size();
    putU64(out, next.switchId);
    putU32(out, next.game.gameGeneration);
    putU64(out, next.game.gameUuidLow);
    putU64(out, next.game.gameUuidHigh);
    putU32(out, next.frameCount);
    putU32(out, next.eventCount);
    putU64(out, next.validatedSourceByteCount);
    putBytes(out, next.validatedSourceRangeDigest.data(),
             next.validatedSourceRangeDigest.size());
    putU8(out, static_cast<uint8_t>(next.endpointKind));
    putU8(out, next.supersededStateSchema ? 1u : 0u);
    putU16(out, next.presentationFlags);
    putSimulationContract(out, next.runtimeContract);
    return out.size() - before == kPlaybackNextGamePayloadBytes;
}

inline bool decodePlaybackNextGame(
    const uint8_t* data, size_t len, PlaybackNextGamePayload& out) {
    if (data == nullptr || len != kPlaybackNextGamePayloadBytes) return false;
    PlaybackNextGamePayload decoded{};
    size_t off = 0u;
    uint8_t endpoint = 0u, superseded = 0u;
    if (!getU64(data, len, off, decoded.switchId)
        || !getU32(data, len, off, decoded.game.gameGeneration)
        || !getU64(data, len, off, decoded.game.gameUuidLow)
        || !getU64(data, len, off, decoded.game.gameUuidHigh)
        || !getU32(data, len, off, decoded.frameCount)
        || !getU32(data, len, off, decoded.eventCount)
        || !getU64(data, len, off, decoded.validatedSourceByteCount)
        || !getBytes(data, len, off, decoded.validatedSourceRangeDigest.data(),
                     decoded.validatedSourceRangeDigest.size())
        || !getU8(data, len, off, endpoint)
        || !getU8(data, len, off, superseded)
        || !getU16(data, len, off, decoded.presentationFlags)
        || !getSimulationContract(data, len, off, decoded.runtimeContract)
        || superseded > 1u || off != len) return false;
    decoded.endpointKind = static_cast<PlaybackEndpointKind>(endpoint);
    decoded.supersededStateSchema = superseded != 0u;
    std::vector<uint8_t> canonical;
    if (!encodePlaybackNextGame(decoded, canonical)
        || canonical.size() != len
        || !std::equal(canonical.begin(), canonical.end(), data)) return false;
    out = decoded;
    return true;
}

inline bool encodePlaybackGameStart(
    const ConfirmedGameStartV2& start, std::vector<uint8_t>& out) {
    return encodeConfirmedGameStartV2(start, out);
}

inline ParseStatus decodePlaybackGameStart(
    const uint8_t* data, size_t len, ConfirmedGameStartV2& out) {
    return decodeConfirmedGameStartV2(data, len, out);
}

inline bool encodePlaybackFrames(
    const ConfirmedGameFrameV2* frames, size_t count,
    std::vector<uint8_t>& out) {
    return encodeRecorderFrames(frames, count, out);
}

inline ParseStatus decodePlaybackFrames(
    const uint8_t* data, size_t len, uint32_t firstRecord,
    uint32_t count, uint8_t allowedLaneMask,
    std::vector<ConfirmedGameFrameV2>& out) {
    return decodeRecorderFrames(
        data, len, firstRecord, count, allowedLaneMask, out);
}

inline bool encodePlaybackEvents(
    const ConfirmedGameEventV2* events, size_t count,
    std::vector<uint8_t>& out) {
    return encodeEventBlockV2(events, count, out);
}

inline ParseStatus decodePlaybackEvents(
    const uint8_t* data, size_t len,
    uint32_t expectedFirstEventSequence, uint32_t eventCount,
    std::vector<ConfirmedGameEventV2>& out) {
    return decodeEventBlockV2(
        data, len, expectedFirstEventSequence, eventCount, out);
}

// Viewer transport (pause, resume, frame-step, set-speed) is kinds 1..4; new
// producers MUST reuse these numbers, never append a second encoding.
enum class PlaybackCommandKind : uint8_t {
    Pause = 1,
    Resume = 2,
    Step = 3,
    SpeedSet = 4,
    Restart = 5,
    SeekFrame = 6,
    SeekRound = 7,
    Exit = 8,
};

constexpr bool isKnownPlaybackCommandKind(uint8_t value) {
    return value >= 1u && value <= 8u;
}

constexpr bool isPresentationOnlyPlaybackCommandKind(
    PlaybackCommandKind kind) {
    return kind == PlaybackCommandKind::Pause
        || kind == PlaybackCommandKind::Resume
        || kind == PlaybackCommandKind::Step
        || kind == PlaybackCommandKind::SpeedSet;
}

constexpr bool isSupportedPlaybackSpeedV3(uint64_t value) {
    return value == 250u || value == 500u || value == 1000u
        || value == 2000u || value == 4000u || value == 8000u;
}

inline constexpr uint32_t kPlaybackSpeedLadderV3[] = {
    250u, 500u, 1000u, 2000u, 4000u, 8000u,
};
constexpr size_t kPlaybackSpeedLadderCountV3 =
    sizeof(kPlaybackSpeedLadderV3) / sizeof(kPlaybackSpeedLadderV3[0]);

inline bool nextPlaybackSpeedV3(
    uint32_t current, bool faster, uint32_t& out) {
    for (size_t i = 0u; i != kPlaybackSpeedLadderCountV3; ++i) {
        if (kPlaybackSpeedLadderV3[i] != current) continue;
        size_t next = i;
        if (faster && i + 1u != kPlaybackSpeedLadderCountV3)
            next = i + 1u;
        else if (!faster && i != 0u)
            next = i - 1u;
        out = kPlaybackSpeedLadderV3[next];
        return true;
    }
    return false;
}

inline bool isValidPlaybackCommandArgument(
    PlaybackCommandKind kind, uint64_t argument) {
    switch (kind) {
        case PlaybackCommandKind::Pause:
        case PlaybackCommandKind::Resume:
        case PlaybackCommandKind::Step:
        case PlaybackCommandKind::Restart:
        case PlaybackCommandKind::Exit:
            return argument == 0u;
        case PlaybackCommandKind::SpeedSet:
            return isSupportedPlaybackSpeedV3(argument);
        case PlaybackCommandKind::SeekFrame:
            return argument < kMaxPlaybackPrebufferFramesV2;
        case PlaybackCommandKind::SeekRound:
            return argument >= 1u
                && argument <= kMaxPlaybackPrebufferFramesV2;
    }
    return false;
}

struct PlaybackCommandPayload {
    uint64_t commandId = 0u;
    PlaybackCommandKind kind = PlaybackCommandKind::Pause;
    uint64_t argument = 0u;
};

constexpr size_t kPlaybackCommandPayloadBytes = 24u;

inline bool encodePlaybackCommand(
    const PlaybackCommandPayload& command,
    std::vector<uint8_t>& out) {
    if (command.commandId == 0u
        || !isValidPlaybackCommandArgument(
            command.kind, command.argument)) {
        return false;
    }
    const size_t before = out.size();
    putU64(out, command.commandId);
    putU8(out, static_cast<uint8_t>(command.kind));
    putU8(out, 0u);
    putU16(out, 0u);
    putU32(out, 0u);
    putU64(out, command.argument);
    return out.size() - before == kPlaybackCommandPayloadBytes;
}

inline bool decodePlaybackCommand(
    const uint8_t* data, size_t len, PlaybackCommandPayload& out) {
    if (data == nullptr || len != kPlaybackCommandPayloadBytes)
        return false;
    PlaybackCommandPayload decoded{};
    size_t off = 0u;
    uint8_t kind = 0u;
    uint8_t reserved8 = 0u;
    uint16_t reserved16 = 0u;
    uint32_t reserved32 = 0u;
    if (!getU64(data, len, off, decoded.commandId)
        || !getU8(data, len, off, kind)
        || !getU8(data, len, off, reserved8)
        || !getU16(data, len, off, reserved16)
        || !getU32(data, len, off, reserved32)
        || !getU64(data, len, off, decoded.argument)
        || !isKnownPlaybackCommandKind(kind)
        || reserved8 != 0u || reserved16 != 0u
        || reserved32 != 0u) {
        return false;
    }
    decoded.kind = static_cast<PlaybackCommandKind>(kind);
    std::vector<uint8_t> canonical;
    if (!encodePlaybackCommand(decoded, canonical)
        || canonical.size() != len
        || !std::equal(canonical.begin(), canonical.end(), data)) {
        return false;
    }
    out = decoded;
    return true;
}

enum class PlaybackRuntimePhase : uint8_t {
    WaitingForChannel = 1,
    WaitingForStart = 2,
    Bootstrapping = 3,
    Ready = 4,
    Playing = 5,
    Paused = 6,
    Buffering = 7,
    Seeking = 8,
    TerminalArmed = 9,
    AwaitingTerminalPresentation = 10,
    Complete = 11,
    Failed = 12,
};

constexpr bool isKnownPlaybackRuntimePhase(uint8_t value) {
    return value >= 1u && value <= 12u;
}

enum class PlaybackCommandAckResult : uint8_t {
    Applied = 1,
    Queued = 2,
    RejectedPhase = 3,
    RejectedArgument = 4,
    StaleCommand = 5,
    InternalFailure = 6,
};

struct PlaybackCommandAckPayload {
    uint64_t commandId = 0u;
    PlaybackCommandKind kind = PlaybackCommandKind::Pause;
    PlaybackCommandAckResult result =
        PlaybackCommandAckResult::InternalFailure;
    PlaybackRuntimePhase phase = PlaybackRuntimePhase::WaitingForChannel;
    uint64_t effectiveArgument = 0u;
    uint32_t nextRecord = 0u;
    uint32_t detail = 0u;
};

constexpr size_t kPlaybackCommandAckPayloadBytes = 32u;

inline bool encodePlaybackCommandAck(
    const PlaybackCommandAckPayload& ack,
    std::vector<uint8_t>& out) {
    const uint8_t kind = static_cast<uint8_t>(ack.kind);
    const uint8_t result = static_cast<uint8_t>(ack.result);
    const uint8_t phase = static_cast<uint8_t>(ack.phase);
    if (ack.commandId == 0u
        || !isKnownPlaybackCommandKind(kind)
        || result < 1u || result > 6u
        || !isKnownPlaybackRuntimePhase(phase)
        || ack.nextRecord > kMaxPlaybackPrebufferFramesV2
        || ((ack.result == PlaybackCommandAckResult::Applied
             || ack.result == PlaybackCommandAckResult::Queued)
            && !isValidPlaybackCommandArgument(
                ack.kind, ack.effectiveArgument))) {
        return false;
    }
    const size_t before = out.size();
    putU64(out, ack.commandId);
    putU8(out, kind);
    putU8(out, result);
    putU8(out, phase);
    putU8(out, 0u);
    putU64(out, ack.effectiveArgument);
    putU32(out, ack.nextRecord);
    putU32(out, ack.detail);
    putU32(out, 0u);
    return out.size() - before == kPlaybackCommandAckPayloadBytes;
}

inline bool decodePlaybackCommandAck(
    const uint8_t* data, size_t len,
    PlaybackCommandAckPayload& out) {
    if (data == nullptr || len != kPlaybackCommandAckPayloadBytes)
        return false;
    PlaybackCommandAckPayload decoded{};
    size_t off = 0u;
    uint8_t kind = 0u;
    uint8_t result = 0u;
    uint8_t phase = 0u;
    uint8_t reserved8 = 0u;
    uint32_t reserved32 = 0u;
    if (!getU64(data, len, off, decoded.commandId)
        || !getU8(data, len, off, kind)
        || !getU8(data, len, off, result)
        || !getU8(data, len, off, phase)
        || !getU8(data, len, off, reserved8)
        || !getU64(data, len, off, decoded.effectiveArgument)
        || !getU32(data, len, off, decoded.nextRecord)
        || !getU32(data, len, off, decoded.detail)
        || !getU32(data, len, off, reserved32)
        || !isKnownPlaybackCommandKind(kind)
        || result < 1u || result > 6u
        || !isKnownPlaybackRuntimePhase(phase)
        || reserved8 != 0u || reserved32 != 0u) {
        return false;
    }
    decoded.kind = static_cast<PlaybackCommandKind>(kind);
    decoded.result = static_cast<PlaybackCommandAckResult>(result);
    decoded.phase = static_cast<PlaybackRuntimePhase>(phase);
    std::vector<uint8_t> canonical;
    if (!encodePlaybackCommandAck(decoded, canonical)
        || canonical.size() != len
        || !std::equal(canonical.begin(), canonical.end(), data)) {
        return false;
    }
    out = decoded;
    return true;
}

enum class PlaybackResultReason : uint16_t {
    VerifiedCompleted = 1,
    VerifiedAbnormalEnd = 2,
    VerifiedUnsealedPrefix = 3,
    UserExit = 4,
    Unsupported = 5,
    BrokerFailure = 6,
    BootstrapMismatch = 7,
    FramePreHashMismatch = 8,
    FramePostHashMismatch = 9,
    RollingDigestMismatch = 10,
    TerminalMismatch = 11,
    PresentationAckTimeout = 12,
    DeviceIsolationViolation = 13,
    InternalFailure = 14,
    VerifiedBehaviourOnly = 15,
    CompletedWithDivergence = 16,
};

inline bool isVerifiedPlaybackResultReason(PlaybackResultReason reason) {
    return reason == PlaybackResultReason::VerifiedCompleted
        || reason == PlaybackResultReason::VerifiedAbnormalEnd
        || reason == PlaybackResultReason::VerifiedUnsealedPrefix
        || reason == PlaybackResultReason::VerifiedBehaviourOnly;
}

inline bool isPlayedThroughPlaybackResultReason(PlaybackResultReason reason) {
    return isVerifiedPlaybackResultReason(reason)
        || reason == PlaybackResultReason::CompletedWithDivergence;
}

struct PlaybackStatusPayload {
    PlaybackRuntimePhase phase = PlaybackRuntimePhase::WaitingForChannel;
    bool paused = false;
    bool buffering = false;
    bool seeking = false;
    bool headless = false;
    bool terminalArmed = false;
    bool hasFailure = false;
    PlaybackResultReason failureReason =
        PlaybackResultReason::InternalFailure;
    uint32_t nextRecord = 0u;
    uint32_t endpointNextRecord = 0u;
    uint32_t nextEventSequence = 0u;
    uint32_t bufferedFrames = 0u;
    uint32_t speedPermille = 1000u;
    uint32_t presentationSerial = 0u;
    uint32_t terminalPresentationSerial = 0u;
    uint64_t rollingDigest = 0u;
    uint64_t postStateHash = 0u;
    uint64_t lastCommandId = 0u;
    uint64_t droppedCommands = 0u;
    uint64_t deviceCallCount = 0u;
    uint32_t mismatchFrame = 0u;
};

constexpr size_t kPlaybackStatusPayloadBytes = 80u;

inline bool encodePlaybackStatus(
    const PlaybackStatusPayload& status,
    std::vector<uint8_t>& out) {
    const uint8_t phase = static_cast<uint8_t>(status.phase);
    const uint16_t failure =
        static_cast<uint16_t>(status.failureReason);
    if (!isKnownPlaybackRuntimePhase(phase)
        || !isSupportedPlaybackSpeedV3(status.speedPermille)
        || status.nextRecord > kMaxPlaybackPrebufferFramesV2
        || status.endpointNextRecord > kMaxPlaybackPrebufferFramesV2
        || status.nextRecord > status.endpointNextRecord
        || status.bufferedFrames
            > status.endpointNextRecord - status.nextRecord
        || status.hasFailure !=
            (status.phase == PlaybackRuntimePhase::Failed)
        || (status.hasFailure
            && (failure < 5u || failure > 14u))) {
        return false;
    }
    const bool waiting =
        status.phase == PlaybackRuntimePhase::WaitingForChannel
        || status.phase == PlaybackRuntimePhase::WaitingForStart;
    if (waiting
        && (status.nextRecord != 0u
            || status.endpointNextRecord != 0u
            || status.nextEventSequence != 0u
            || status.bufferedFrames != 0u
            || status.rollingDigest != 0u
            || status.postStateHash != 0u)) {
        return false;
    }
    uint8_t flags = 0u;
    if (status.paused) flags |= 1u << 0;
    if (status.buffering) flags |= 1u << 1;
    if (status.seeking) flags |= 1u << 2;
    if (status.headless) flags |= 1u << 3;
    if (status.terminalArmed) flags |= 1u << 4;
    if (status.hasFailure) flags |= 1u << 5;
    const size_t before = out.size();
    putU8(out, phase);
    putU8(out, flags);
    putU16(out, status.hasFailure ? failure : 0u);
    putU32(out, status.nextRecord);
    putU32(out, status.endpointNextRecord);
    putU32(out, status.nextEventSequence);
    putU32(out, status.bufferedFrames);
    putU32(out, status.speedPermille);
    putU32(out, status.presentationSerial);
    putU32(out, status.terminalPresentationSerial);
    putU64(out, status.rollingDigest);
    putU64(out, status.postStateHash);
    putU64(out, status.lastCommandId);
    putU64(out, status.droppedCommands);
    putU64(out, status.deviceCallCount);
    putU32(out, status.mismatchFrame);
    putU32(out, 0u);
    return out.size() - before == kPlaybackStatusPayloadBytes;
}

inline bool decodePlaybackStatus(
    const uint8_t* data, size_t len, PlaybackStatusPayload& out) {
    if (data == nullptr || len != kPlaybackStatusPayloadBytes)
        return false;
    PlaybackStatusPayload decoded{};
    size_t off = 0u;
    uint8_t phase = 0u;
    uint8_t flags = 0u;
    uint16_t failure = 0u;
    uint32_t reserved = 0u;
    if (!getU8(data, len, off, phase)
        || !getU8(data, len, off, flags)
        || !getU16(data, len, off, failure)
        || !getU32(data, len, off, decoded.nextRecord)
        || !getU32(data, len, off, decoded.endpointNextRecord)
        || !getU32(data, len, off, decoded.nextEventSequence)
        || !getU32(data, len, off, decoded.bufferedFrames)
        || !getU32(data, len, off, decoded.speedPermille)
        || !getU32(data, len, off, decoded.presentationSerial)
        || !getU32(
            data, len, off, decoded.terminalPresentationSerial)
        || !getU64(data, len, off, decoded.rollingDigest)
        || !getU64(data, len, off, decoded.postStateHash)
        || !getU64(data, len, off, decoded.lastCommandId)
        || !getU64(data, len, off, decoded.droppedCommands)
        || !getU64(data, len, off, decoded.deviceCallCount)
        || !getU32(data, len, off, decoded.mismatchFrame)
        || !getU32(data, len, off, reserved)
        || !isKnownPlaybackRuntimePhase(phase)
        || (flags & ~0x3Fu) != 0u || reserved != 0u) {
        return false;
    }
    decoded.phase = static_cast<PlaybackRuntimePhase>(phase);
    decoded.paused = (flags & (1u << 0)) != 0u;
    decoded.buffering = (flags & (1u << 1)) != 0u;
    decoded.seeking = (flags & (1u << 2)) != 0u;
    decoded.headless = (flags & (1u << 3)) != 0u;
    decoded.terminalArmed = (flags & (1u << 4)) != 0u;
    decoded.hasFailure = (flags & (1u << 5)) != 0u;
    if (decoded.hasFailure) {
        if (failure < 5u || failure > 14u) return false;
        decoded.failureReason =
            static_cast<PlaybackResultReason>(failure);
    } else if (failure != 0u) {
        return false;
    }
    std::vector<uint8_t> canonical;
    if (!encodePlaybackStatus(decoded, canonical)
        || canonical.size() != len
        || !std::equal(canonical.begin(), canonical.end(), data)) {
        return false;
    }
    out = decoded;
    return true;
}

enum class PlaybackByeReason : uint16_t {
    NormalAfterResult = 1,
    UserExitAfterResult = 2,
    FailureAfterResult = 3,
    ChannelClosedWithoutResult = 4,
};

struct PlaybackByePayload {
    PlaybackByeReason reason =
        PlaybackByeReason::ChannelClosedWithoutResult;
    bool resultSent = false;
    uint32_t nextRecord = 0u;
    uint64_t lastCommandId = 0u;
    uint64_t detail = 0u;
};

constexpr size_t kPlaybackByePayloadBytes = 24u;

inline bool encodePlaybackBye(
    const PlaybackByePayload& bye, std::vector<uint8_t>& out) {
    const uint16_t reason = static_cast<uint16_t>(bye.reason);
    if (reason < 1u || reason > 4u
        || bye.nextRecord > kMaxPlaybackPrebufferFramesV2
        || bye.resultSent
            != (bye.reason
                != PlaybackByeReason::ChannelClosedWithoutResult)) {
        return false;
    }
    const size_t before = out.size();
    putU16(out, reason);
    putU8(out, bye.resultSent ? 1u : 0u);
    putU8(out, 0u);
    putU32(out, bye.nextRecord);
    putU64(out, bye.lastCommandId);
    putU64(out, bye.detail);
    return out.size() - before == kPlaybackByePayloadBytes;
}

inline bool decodePlaybackBye(
    const uint8_t* data, size_t len, PlaybackByePayload& out) {
    if (data == nullptr || len != kPlaybackByePayloadBytes)
        return false;
    PlaybackByePayload decoded{};
    size_t off = 0u;
    uint16_t reason = 0u;
    uint8_t resultSent = 0u;
    uint8_t reserved = 0u;
    if (!getU16(data, len, off, reason)
        || !getU8(data, len, off, resultSent)
        || !getU8(data, len, off, reserved)
        || !getU32(data, len, off, decoded.nextRecord)
        || !getU64(data, len, off, decoded.lastCommandId)
        || !getU64(data, len, off, decoded.detail)
        || reason < 1u || reason > 4u
        || resultSent > 1u || reserved != 0u) {
        return false;
    }
    decoded.reason = static_cast<PlaybackByeReason>(reason);
    decoded.resultSent = resultSent != 0u;
    std::vector<uint8_t> canonical;
    if (!encodePlaybackBye(decoded, canonical)
        || canonical.size() != len
        || !std::equal(canonical.begin(), canonical.end(), data)) {
        return false;
    }
    out = decoded;
    return true;
}

struct PlaybackResultPayload {
    PlaybackResultReason reason = PlaybackResultReason::InternalFailure;
    uint16_t presentationFlags = 0u;
    PlaybackEndpointKind endpointKind =
        PlaybackEndpointKind::RecordedGameEnd;
    bool hasFileDigest = false;
    bool hasCanonicalGameDigest = false;
    bool hasRecordedTerminal = false;
    bool headless = false;
    std::array<uint8_t, 32> fileDigest{};
    std::array<uint8_t, 32> canonicalGameDigest{};
    uint64_t validatedSourceByteCount = 0u;
    std::array<uint8_t, 32> validatedSourceRangeDigest{};
    SimulationContractV2 runtimeContract{};
    uint32_t nextRecordConsumed = 0u;
    uint64_t finalRollingDigest = 0u;
    uint64_t postTailStateHash = 0u;
    uint16_t recordedTerminal = 0u;
    uint64_t deviceCallCount = 0u;
    uint32_t mismatchFrame = 0u;
    uint32_t mismatchRegion = 0u;
    uint64_t expectedValue = 0u;
    uint64_t actualValue = 0u;
};

constexpr size_t kPlaybackResultPayloadBytes = 308u;

inline bool encodePlaybackResult(
    const PlaybackResultPayload& result, std::vector<uint8_t>& out) {
    const uint16_t reason = static_cast<uint16_t>(result.reason);
    const uint8_t endpoint = static_cast<uint8_t>(result.endpointKind);
    // Upper bound must track the LAST PlaybackResultReason; otherwise the peer refuses the result and drops the channel.
    if (reason < 1u || reason > 16u || endpoint < 1u || endpoint > 2u
        || (result.presentationFlags & ~28u) != 0u
        || result.validatedSourceByteCount == 0u
        || result.validatedSourceByteCount > kMaxFileBytes
        || isZeroDigest(result.validatedSourceRangeDigest)
        || !isValidSimulationContractV2(result.runtimeContract)
        || result.nextRecordConsumed > kMaxPlaybackPrebufferFramesV2)
        return false;
    uint8_t flags = 0u;
    if (result.hasFileDigest) flags |= 1u << 0;
    if (result.hasCanonicalGameDigest) flags |= 1u << 1;
    if (result.hasRecordedTerminal) flags |= 1u << 2;
    if (result.headless) flags |= 1u << 3;
    const bool sealed =
        result.endpointKind == PlaybackEndpointKind::RecordedGameEnd;
    if (result.hasFileDigest != sealed
        || result.hasCanonicalGameDigest != sealed
        || result.hasRecordedTerminal != sealed
        || (!result.hasFileDigest && !isZeroDigest(result.fileDigest))
        || (!result.hasCanonicalGameDigest
            && !isZeroDigest(result.canonicalGameDigest))
        || (result.hasFileDigest && isZeroDigest(result.fileDigest))
        || (result.hasCanonicalGameDigest
            && isZeroDigest(result.canonicalGameDigest))
        || (!result.hasRecordedTerminal
            && result.recordedTerminal != 0u)
        || (result.hasRecordedTerminal
            && !isKnownTerminalReason(result.recordedTerminal))) {
        return false;
    }
    if ((result.reason == PlaybackResultReason::VerifiedCompleted
            && (!sealed
                || result.recordedTerminal
                    != static_cast<uint16_t>(
                        TerminalReason::Completed)))
        || (result.reason == PlaybackResultReason::VerifiedAbnormalEnd
            && (!sealed
                || result.recordedTerminal
                    == static_cast<uint16_t>(
                        TerminalReason::Completed)))
        || (result.reason == PlaybackResultReason::VerifiedUnsealedPrefix
            && sealed)
        || (isVerifiedPlaybackResultReason(result.reason)
            && result.nextRecordConsumed == 0u)
        || (isVerifiedPlaybackResultReason(result.reason)
            && result.deviceCallCount != 0u)
        || (result.reason
                == PlaybackResultReason::DeviceIsolationViolation
            && result.deviceCallCount == 0u)) {
        return false;
    }
    const size_t before = out.size();
    putU16(out, reason);
    putU8(out, endpoint);
    putU8(out, flags);
    putBytes(out, result.fileDigest.data(), result.fileDigest.size());
    putBytes(
        out, result.canonicalGameDigest.data(),
        result.canonicalGameDigest.size());
    putU64(out, result.validatedSourceByteCount);
    putBytes(
        out, result.validatedSourceRangeDigest.data(),
        result.validatedSourceRangeDigest.size());
    putBytes(
        out, result.runtimeContract.executableSha256.data(),
        result.runtimeContract.executableSha256.size());
    putU64(out, result.runtimeContract.nativeBuildIdentity);
    putU64(out, result.runtimeContract.runtimeSimulationId);
    putU64(out, result.runtimeContract.stateRegionSchemaFingerprint);
    putU64(out, result.runtimeContract.stateHashProjectionId);
    putU64(out, result.runtimeContract.stateHashAlgorithmId);
    putBytes(
        out, result.runtimeContract.contentManifestRoot.data(),
        result.runtimeContract.contentManifestRoot.size());
    putU64(out, result.runtimeContract.bootstrapSchemaId);
    putU64(out, result.runtimeContract.eventSchemaId);
    putU64(out, result.runtimeContract.characterCatalogId);
    putU64(out, result.runtimeContract.terminalResultSchemaId);
    putU32(out, result.runtimeContract.compactActionDomainVersion);
    putU32(out, 0u);
    putU32(out, result.nextRecordConsumed);
    putU64(out, result.finalRollingDigest);
    putU64(out, result.postTailStateHash);
    putU16(out, result.recordedTerminal);
    putU16(out, result.presentationFlags);
    putU64(out, result.deviceCallCount);
    putU32(out, result.mismatchFrame);
    putU32(out, result.mismatchRegion);
    putU64(out, result.expectedValue);
    putU64(out, result.actualValue);
    return out.size() - before == kPlaybackResultPayloadBytes;
}

inline bool decodePlaybackResult(
    const uint8_t* data, size_t len, PlaybackResultPayload& out) {
    if (data == nullptr || len != kPlaybackResultPayloadBytes)
        return false;
    PlaybackResultPayload decoded{};
    size_t off = 0u;
    uint16_t reason = 0u;
    uint8_t endpoint = 0u;
    uint8_t flags = 0u;
    uint32_t reserved32 = 0u;
    uint16_t reserved16 = 0u;
    if (!getU16(data, len, off, reason)
        || !getU8(data, len, off, endpoint)
        || !getU8(data, len, off, flags)
        || !getBytes(
            data, len, off, decoded.fileDigest.data(),
            decoded.fileDigest.size())
        || !getBytes(
            data, len, off, decoded.canonicalGameDigest.data(),
            decoded.canonicalGameDigest.size())
        || !getU64(data, len, off, decoded.validatedSourceByteCount)
        || !getBytes(
            data, len, off, decoded.validatedSourceRangeDigest.data(),
            decoded.validatedSourceRangeDigest.size())
        || !getBytes(
            data, len, off,
            decoded.runtimeContract.executableSha256.data(),
            decoded.runtimeContract.executableSha256.size())
        || !getU64(
            data, len, off,
            decoded.runtimeContract.nativeBuildIdentity)
        || !getU64(
            data, len, off,
            decoded.runtimeContract.runtimeSimulationId)
        || !getU64(
            data, len, off,
            decoded.runtimeContract.stateRegionSchemaFingerprint)
        || !getU64(
            data, len, off,
            decoded.runtimeContract.stateHashProjectionId)
        || !getU64(
            data, len, off,
            decoded.runtimeContract.stateHashAlgorithmId)
        || !getBytes(
            data, len, off,
            decoded.runtimeContract.contentManifestRoot.data(),
            decoded.runtimeContract.contentManifestRoot.size())
        || !getU64(
            data, len, off,
            decoded.runtimeContract.bootstrapSchemaId)
        || !getU64(
            data, len, off,
            decoded.runtimeContract.eventSchemaId)
        || !getU64(
            data, len, off,
            decoded.runtimeContract.characterCatalogId)
        || !getU64(
            data, len, off,
            decoded.runtimeContract.terminalResultSchemaId)
        || !getU32(
            data, len, off,
            decoded.runtimeContract.compactActionDomainVersion)
        || !getU32(data, len, off, reserved32)
        || !getU32(data, len, off, decoded.nextRecordConsumed)
        || !getU64(data, len, off, decoded.finalRollingDigest)
        || !getU64(data, len, off, decoded.postTailStateHash)
        || !getU16(data, len, off, decoded.recordedTerminal)
        || !getU16(data, len, off, reserved16)
        || !getU64(data, len, off, decoded.deviceCallCount)
        || !getU32(data, len, off, decoded.mismatchFrame)
        || !getU32(data, len, off, decoded.mismatchRegion)
        || !getU64(data, len, off, decoded.expectedValue)
        || !getU64(data, len, off, decoded.actualValue)) return false;
    // Upper bound must track the LAST PlaybackResultReason; otherwise the peer refuses the result and drops the channel.
    if (reason < 1u || reason > 16u || endpoint < 1u || endpoint > 2u
        || (flags & 0xF0u) != 0u || reserved32 != 0u
        || (reserved16 & ~28u) != 0u) return false;
    decoded.presentationFlags = reserved16;
    decoded.reason = static_cast<PlaybackResultReason>(reason);
    decoded.endpointKind = static_cast<PlaybackEndpointKind>(endpoint);
    decoded.hasFileDigest = (flags & (1u << 0)) != 0u;
    decoded.hasCanonicalGameDigest = (flags & (1u << 1)) != 0u;
    decoded.hasRecordedTerminal = (flags & (1u << 2)) != 0u;
    decoded.headless = (flags & (1u << 3)) != 0u;
    std::vector<uint8_t> canonical;
    if (!encodePlaybackResult(decoded, canonical)
        || canonical.size() != len
        || !std::equal(canonical.begin(), canonical.end(), data)) {
        return false;
    }
    out = decoded;
    return true;
}

}

}
}
