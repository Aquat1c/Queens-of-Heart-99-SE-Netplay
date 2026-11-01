#pragma once

#include "client/replay/ReplayParser.h"
#include "client/replay/ReplayWriter.h"

#include "replay/CharacterCatalog.h"
#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"
#include "replay/ReplayTypes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace qclient::replay::test {

using namespace qreplay;

struct FixtureOptions {
    uint32_t matchCount = 1u;
    uint32_t framesPerMatch = 8u;
    uint32_t roundsPerMatch = 2u;
    uint32_t startFrame = 100u;
    bool finalize = true;
    bool fourPlayer = false;
    bool cpuFighter = false;
    bool networkMetadata = false;
    bool selectorProof = true;
    uint64_t uuidLow = 0x0123456789ABCDEFull;
    uint64_t uuidHigh = 0xFEDCBA9876543210ull;
    int64_t recordedUnixSeconds = 1767225600;
    std::string title = "Fixture set";
    std::string laneOneName = "Player1";
    std::string laneTwoName = "Player2";
    uint8_t characterSlotZero = 8u;
    uint8_t characterSlotOne = 1u;
    uint8_t characterSlotTwo = 30u;
    uint8_t characterSlotThree = 21u;
    uint64_t runtimeSimulationId = 0xA11CE0000001ull;
    uint64_t contentSeed = 0x5Eu;
    uint64_t initialDigest = 0x1122334455667788ull;
    uint64_t initialStateHash = 0x99AABBCCDDEEFF00ull;
};

struct Fixture {
    std::vector<uint8_t> bytes;
    ReplayPrefix prefix{};
    SessionManifest manifest{};
    DeterministicStart sessionStart{};
    std::vector<MatchStartRecord> matchStarts;
    std::vector<MatchEndRecord> matchEnds;
    std::vector<ReplayFrameV1> frames;
    std::vector<ReplayEvent> events;
    SessionEndRecord sessionEnd{};
    uint64_t finalDigest = 0u;
    uint32_t lastFrame = 0u;
    std::vector<uint64_t> chunkOffsets;
    bool ok = false;
};

inline std::array<uint8_t, 32> digestFromSeed(uint64_t seed) {
    std::array<uint8_t, 32> out{};
    for (size_t i = 0; i < out.size(); ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        out[i] = static_cast<uint8_t>(seed >> 33);
    }
    return out;
}

inline SimulationContract makeContract(const FixtureOptions& options) {
    SimulationContract contract{};
    contract.executableSha256 = digestFromSeed(0xE7Eu);
    contract.runtimeSimulationId = options.runtimeSimulationId;
    contract.stateRegionFingerprint = 0x0F1E2D3C4B5A6978ull;
    contract.stateHashProjectionId = 0x1111222233334444ull;
    contract.stateHashAlgorithmId = 0x5555666677778888ull;
    contract.bootstrapCodecVersion = 3u;
    contract.eventSchemaVersion = 1u;
    contract.contentManifestRoot = digestFromSeed(options.contentSeed);
    contract.characterCatalogId = 0xC0FFEEC0FFEEC0FFull;
    contract.fpuControlWord = 0x027Fu;
    contract.mxcsr = 0x1F80u;
    return contract;
}

inline SessionManifest makeManifest(const FixtureOptions& options) {
    SessionManifest manifest{};
    manifest.contract = makeContract(options);
    manifest.bootstrapKind = BootstrapKind::OfflineVsDirectBootV1;
    manifest.vsRow = options.fourPlayer ? 5u : 1u;
    manifest.cpuMask = options.cpuFighter ? 0x02u : 0x00u;
    manifest.cpuContract = options.cpuFighter
        ? CpuContract::NativeAiDeterministicV1
        : CpuContract::None;
    manifest.slotToSourceRoute = { 0u, 1u, 2u, 3u };
    manifest.rosterEpoch = 1u;

    const uint8_t characters[kFighterSlots] = {
        options.characterSlotZero, options.characterSlotOne,
        options.characterSlotTwo, options.characterSlotThree,
    };
    const size_t active = options.fourPlayer ? 4u : 2u;
    for (size_t i = 0; i < kSourceLanes; ++i) {
        manifest.lanes[i].active = i < active;
        if (i == 0u) manifest.lanes[i].name = options.laneOneName;
        else if (i == 1u) manifest.lanes[i].name = options.laneTwoName;
        else if (i < active) manifest.lanes[i].name = "Player" + std::to_string(i + 1u);
    }
    for (size_t i = 0; i < kFighterSlots; ++i) {
        FighterSlotDescriptor& slot = manifest.slots[i];
        if (i >= active) {
            slot.ownership = SlotOwnership::Inactive;
            slot.identity.nativeCharacterId = kInactiveCharacterId;
            continue;
        }
        slot.ownership = (options.cpuFighter && i == 1u)
            ? SlotOwnership::NativeCpu
            : SlotOwnership::RecordedHuman;
        slot.side = static_cast<uint8_t>(i % 2u);
        slot.identity.nativeCharacterId = characters[i];
        slot.identity.paletteIndex = static_cast<uint8_t>(i % 4u);
        if (options.selectorProof) {
            const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();
            uint8_t baseId = 0u;
            bool alternate = false;
            if (catalog.inverseAlternate(characters[i], baseId, alternate)) {
                const uint8_t cell = catalog.cellForBaseId(baseId);
                if (cell != kNoCharacterId) {
                    slot.selectorProofPresent = true;
                    slot.selectorProof.cursorCell = cell;
                    slot.selectorProof.alternateFlag = alternate ? 1u : 0u;
                    slot.selectorProof.confirmState = 4u;
                }
            }
        }
    }

    manifest.display.recordedUnixSeconds = options.recordedUnixSeconds;
    manifest.display.timezoneOffsetMinutes = 0;
    manifest.display.sourceKind = ReplaySourceKind::OfflineVsPlayer;
    manifest.display.title = options.title;
    manifest.display.networkMetadataPresent = options.networkMetadata;
    if (options.networkMetadata) {
        manifest.display.playerOneDelay = 3u;
        manifest.display.playerTwoDelay = 3u;
        manifest.display.rollbackWindow = 8u;
        manifest.display.averageRttMicros = 45000u;
        manifest.display.lossPermille = 5u;
        manifest.display.routeLabel = "direct";
    }
    return manifest;
}

inline DeterministicStart makeStart(const FixtureOptions& options,
                                    uint32_t firstFrame) {
    DeterministicStart start{};
    start.seedGameplay = 0x12345678u;
    start.seedCosmetic = 0x9ABCDEF0u;
    start.selector = 7u;
    start.initialPhase = 2u;
    start.bootstrapHash = 0xDEADBEEFCAFEF00Dull;
    start.timerMode = 1u;
    start.aiDifficulty = 4u;
    start.lifeMultiplier = 2u;
    start.roundsToWinMinusOne = 1u;
    start.firstTimelineFrame = firstFrame;
    start.initialStateHash = options.initialStateHash;
    start.initialDigest = options.initialDigest;
    const char* names[kSourceLanes] = { "P1", "P2", "P3", "P4" };
    for (size_t i = 0; i < kSourceLanes; ++i) {
        std::snprintf(start.displayNames[i].data(), 32, "%s", names[i]);
    }
    return start;
}

inline ReplayFrameV1 makeFrame(uint32_t timelineFrame, uint8_t laneMask,
                               uint64_t& digest) {
    ReplayFrameV1 frame{};
    frame.timelineFrame = timelineFrame;
    frame.inputDomain = InputDomain::LogicalSourceV1;
    frame.activeLaneMask = laneMask;
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        if ((laneMask & (1u << lane)) == 0u) continue;
        const uint16_t buttons =
            static_cast<uint16_t>(1u << (lane % 4u));
        const uint16_t directions = static_cast<uint16_t>(
            1u << (8u + ((timelineFrame + lane) % 4u)));
        frame.actions[lane] = static_cast<uint16_t>(buttons | directions);
    }
    frame.routeEpoch = 1u;
    frame.preStateHash = 0xA000000000000000ull + timelineFrame;
    frame.postStateHash = 0xA000000000000000ull + timelineFrame + 1u;
    frame.lifecycleBefore.scene = 3u;
    frame.lifecycleBefore.matchState = 2u;
    frame.lifecycleAfter = frame.lifecycleBefore;
    frame.dispatcherResult = 0;
    digest = advanceReplayDigest(digest, frame);
    frame.rollingDigest = digest;
    return frame;
}

inline ReplayEvent makeEvent(EventKind kind, uint32_t timelineFrame,
                             uint32_t matchId, uint64_t& digest) {
    ReplayEvent event{};
    event.timelineFrame = timelineFrame;
    event.kind = kind;
    event.matchId = matchId;
    for (size_t i = 0; i < kFighterSlots; ++i)
        event.roster[i].nativeCharacterId = kInactiveCharacterId;
    event.stateProof = 0xB000000000000000ull + timelineFrame;
    digest = advanceReplayDigest(digest, event);
    event.rollingDigest = digest;
    return event;
}

inline Fixture makeFixture(const FixtureOptions& options) {
    Fixture fixture{};
    fixture.prefix.containerMajor = kContainerVersionMajor;
    fixture.prefix.containerMinor = kContainerVersionMinor;
    fixture.prefix.minimumReaderMajor = kContainerReaderMajor;
    fixture.prefix.sessionUuidLow = options.uuidLow;
    fixture.prefix.sessionUuidHigh = options.uuidHigh;
    fixture.prefix.featureFlags = 0u;
    if (options.fourPlayer) fixture.prefix.featureFlags |= kFeatureFourPlayer;
    if (options.cpuFighter) fixture.prefix.featureFlags |= kFeatureCpuFighters;
    fixture.manifest = makeManifest(options);
    fixture.sessionStart = makeStart(options, options.startFrame);

    MemoryFileSink* sink = new MemoryFileSink();
    ReplayWriter writer;
    writer.setSink(std::unique_ptr<ReplayFileSink>(sink));

    WriterConfig config{};
    config.rootDirectory = L"R:\\replays";
    config.nameHint = options.title;
    config.recordedUnixSeconds = options.recordedUnixSeconds;
    config.flushEveryChunks = 4u;
    config.flushEveryMs = 0u;

    if (!writer.begin(config, fixture.prefix, fixture.manifest)) return fixture;
    fixture.chunkOffsets.push_back(writer.bytesWritten());
    if (!writer.writeSessionStart(fixture.sessionStart)) return fixture;
    fixture.chunkOffsets.push_back(writer.bytesWritten());

    uint64_t digest = options.initialDigest;
    const uint8_t laneMask = options.fourPlayer ? 0x0Fu : 0x03u;
    uint32_t frame = options.startFrame;

    for (uint32_t m = 0; m < options.matchCount; ++m) {
        MatchStartRecord match{};
        match.matchId = m + 1u;
        match.ordinal = m + 1u;
        match.firstTimelineFrame = frame;
        match.dependsOnMatchId = 0u;
        match.independentBootstrap = true;
        match.start = makeStart(options, frame);
        match.slots = fixture.manifest.slots;
        match.slotToSourceRoute = fixture.manifest.slotToSourceRoute;
        match.vsRow = fixture.manifest.vsRow;
        match.cpuMask = fixture.manifest.cpuMask;
        match.rosterEpoch = fixture.manifest.rosterEpoch;
        match.stageId = 0u;
        match.stageVerified = false;
        if (!writer.writeMatchStart(match)) return fixture;
        fixture.chunkOffsets.push_back(writer.bytesWritten());
        fixture.matchStarts.push_back(match);

        std::vector<ReplayFrameV1> block;
        for (uint32_t i = 0; i < options.framesPerMatch; ++i) {
            block.push_back(makeFrame(frame + i, laneMask, digest));
        }
        if (!block.empty()) {
            if (!writer.writeInputBlock(block.data(), block.size()))
                return fixture;
            fixture.chunkOffsets.push_back(writer.bytesWritten());
            fixture.frames.insert(fixture.frames.end(), block.begin(),
                                  block.end());
        }
        const uint32_t lastFrame =
            options.framesPerMatch != 0u ? frame + options.framesPerMatch - 1u
                                         : frame;

        std::vector<ReplayEvent> events;
        for (uint32_t r = 0; r < options.roundsPerMatch; ++r) {
            ReplayEvent ended{};
            ended.timelineFrame = lastFrame;
            ended.kind = EventKind::RoundEnded;
            ended.matchId = match.matchId;
            for (size_t i = 0; i < kFighterSlots; ++i)
                ended.roster[i].nativeCharacterId = kInactiveCharacterId;
            ended.valueA = (r % 2u) == 0u ? 0x05u : 0x0Au;
            ended.valueC = r + 1u;
            ended.stateProof = 0xB000000000000000ull + lastFrame;
            digest = advanceReplayDigest(digest, ended);
            ended.rollingDigest = digest;
            events.push_back(ended);
        }
        if (!events.empty()) {
            if (!writer.writeEventBlock(events.data(), events.size()))
                return fixture;
            fixture.chunkOffsets.push_back(writer.bytesWritten());
            fixture.events.insert(fixture.events.end(), events.begin(),
                                  events.end());
        }

        MatchEndRecord end{};
        end.matchId = match.matchId;
        end.lastTimelineFrame = lastFrame;
        end.winningSideMask = 0x05u;
        end.roundsSideZero = static_cast<uint8_t>((options.roundsPerMatch + 1u) / 2u);
        end.roundsSideOne = static_cast<uint8_t>(options.roundsPerMatch / 2u);
        end.reason = TerminalReason::Completed;
        end.postTailStateHash = 0xA000000000000000ull + lastFrame + 1u;
        end.rollingDigest = digest;
        if (!writer.writeMatchEnd(end)) return fixture;
        fixture.chunkOffsets.push_back(writer.bytesWritten());
        fixture.matchEnds.push_back(end);

        frame = lastFrame + 1u;
        fixture.lastFrame = lastFrame;
    }

    fixture.finalDigest = digest;
    if (options.finalize) {
        SessionEndRecord sessionEnd{};
        sessionEnd.lastTimelineFrame = fixture.lastFrame;
        sessionEnd.reason = TerminalReason::Completed;
        sessionEnd.postTailStateHash =
            0xA000000000000000ull + fixture.lastFrame + 1u;
        sessionEnd.rollingDigest = digest;
        sessionEnd.matchCount = options.matchCount;
        if (!writer.finalize(sessionEnd)) return fixture;
        fixture.sessionEnd = sessionEnd;
    } else {
        fixture.bytes = writer.stagedBytesForTesting();
        writer.abandon();
    }

    if (options.finalize) fixture.bytes = sink->data();
    fixture.ok = true;
    return fixture;
}

inline std::wstring makeTempDir(const wchar_t* tag) {
    wchar_t base[MAX_PATH];
    const DWORD length = ::GetTempPathW(MAX_PATH, base);
    if (length == 0u || length >= MAX_PATH) return std::wstring();
    wchar_t name[64];
    _snwprintf_s(name, 64, _TRUNCATE, L"q99r_%s_%lu_%lu", tag,
                 static_cast<unsigned long>(::GetCurrentProcessId()),
                 static_cast<unsigned long>(::GetTickCount()));
    std::wstring path = std::wstring(base) + name;
    if (!::CreateDirectoryW(path.c_str(), nullptr)
        && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        path += L"_b";
        ::CreateDirectoryW(path.c_str(), nullptr);
    }
    return path;
}

inline bool writeBytes(const std::wstring& path,
                       const std::vector<uint8_t>& bytes) {
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0u;
    const BOOL ok = bytes.empty()
        ? TRUE
        : ::WriteFile(handle, bytes.data(),
                      static_cast<DWORD>(bytes.size()), &wrote, nullptr);
    ::CloseHandle(handle);
    return ok != 0 && (bytes.empty() || wrote == bytes.size());
}

inline void removeTree(const std::wstring& directory) {
    if (directory.empty()) return;
    WIN32_FIND_DATAW found{};
    HANDLE handle = ::FindFirstFileW((directory + L"\\*").c_str(), &found);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring child = directory + L"\\" + name;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u)
                removeTree(child);
            else
                ::DeleteFileW(child.c_str());
        } while (::FindNextFileW(handle, &found) != 0);
        ::FindClose(handle);
    }
    ::RemoveDirectoryW(directory.c_str());
}

struct TempDir {
    std::wstring path;
    explicit TempDir(const wchar_t* tag) : path(makeTempDir(tag)) {}
    ~TempDir() { removeTree(path); }
    bool valid() const { return !path.empty(); }
    std::wstring file(const wchar_t* name) const { return path + L"\\" + name; }
};

class Xorshift64 {
public:
    explicit Xorshift64(uint64_t seed) : state_(seed != 0u ? seed : 0x9E3779B9ull) {}
    uint64_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }
    uint32_t below(uint32_t bound) {
        return bound == 0u ? 0u : static_cast<uint32_t>(next() % bound);
    }

private:
    uint64_t state_;
};

}
