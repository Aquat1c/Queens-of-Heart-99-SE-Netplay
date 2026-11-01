#pragma once

#include "ReplayParser.h"
#include "ReplayParserV2.h"

#include "replay/ReplayTypes.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qclient::replay {

class ReplayAnnotationStore;

struct ReplayEntry {
    uint64_t stableId = 0u;

    std::wstring path;
    std::wstring fileName;
    std::wstring relativeDir;
    FileIdentity identity{};
    bool isPart = false;
    bool inTrash = false;
    bool writeLocked = false;

    bool parsed = false;
    qreplay::ParseStatus parseStatus = qreplay::ParseStatus::Ok;
    std::string parseDetail;
    uint64_t parseOffset = 0u;

    qreplay::ReplayPrefix prefix{};
    bool hasManifest = false;
    qreplay::SessionManifest manifest{};
    bool hasSessionStart = false;
    uint32_t firstTimelineFrame = 0u;
    uint32_t lastTimelineFrame = 0u;

    bool hasFooter = false;
    uint32_t matchCount = 0u;
    uint32_t totalFrames = 0u;
    uint64_t finalRollingDigest = 0u;
    std::array<uint8_t, 32> fileDigest{};

    static constexpr size_t kMaxRetainedMatches = 64u;
    std::vector<MatchSpan> matches;
    bool matchesTruncated = false;

    bool cached = false;

    bool isV2 = false;
    ReplayClassificationV2 classificationV2 =
        ReplayClassificationV2::Invalid;
    qreplay::ConfirmedGameStartV2 startV2{};
    bool hasEndV2 = false;
    qreplay::ConfirmedGameEndV2 endV2{};
    qreplay::ReplayCompatibilityIdentityEnvelopeV3 compatibilityIdentityV3{
        qreplay::ReplayCompatibilityIdentityStatusV3::LegacyUnattested, {}};
    uint32_t eventCountV2 = 0u;
    bool structurallyVerifiedV2 = false;
    bool fileDigestVerifiedV2 = false;

    uint8_t activeFighterCount() const;
    uint8_t humanFighterCount() const;
    uint8_t cpuFighterCount() const;
    bool usesNonIdentityRoute() const;
    uint32_t durationFrames() const;
    int64_t recordedUnixSeconds() const;
};

struct ReplaySnapshot {
    uint64_t generation = 0u;
    std::vector<ReplayEntry> entries;
    std::vector<std::wstring> roots;
    bool complete = false;
    bool cancelled = false;
    uint32_t filesSeen = 0u;
    uint32_t filesParsed = 0u;
    uint32_t filesFromCache = 0u;
    uint32_t filesRejected = 0u;
    uint32_t filesBeyondLimit = 0u;
    uint32_t directoriesBeyondDepth = 0u;
    uint64_t elapsedMs = 0u;
    bool cacheReadOnly = false;
    bool rootWritable = true;
    std::wstring effectiveRoot;

    const ReplayEntry* findByStableId(uint64_t stableId) const;
};

class ReplayCatalogCache {
public:
    static constexpr uint32_t kMagic = 0x43434352u;
    static constexpr uint32_t kVersion = 1u;
    static constexpr uint32_t kMaxRecords = 20000u;
    static constexpr uint64_t kMaxFileBytes = 64ull * 1024ull * 1024ull;
    static constexpr uint32_t kMaxManifestBytes = 64u * 1024u;
    static constexpr uint32_t kMaxFooterBytes = 512u * 1024u;

    ReplayCatalogCache() = default;
    ~ReplayCatalogCache();
    ReplayCatalogCache(const ReplayCatalogCache&) = delete;
    ReplayCatalogCache& operator=(const ReplayCatalogCache&) = delete;

    bool open(const std::wstring& cachePath);
    void close();
    bool readOnly() const { return readOnly_; }
    bool loaded() const { return loaded_; }

    bool lookup(const FileIdentity& identity, ReplayEntry& out) const;
    void store(const ReplayEntry& entry);
    bool flush();
    void discard();
    size_t size() const { return records_.size(); }

    std::vector<uint8_t> encode() const;
    bool decode(const uint8_t* data, size_t len);

private:
    struct Record {
        FileIdentity identity{};
        std::wstring path;
        bool isPart = false;
        bool parsed = false;
        uint16_t parseStatus = 0u;
        uint64_t parseOffset = 0u;
        std::vector<uint8_t> prefixBytes;
        std::vector<uint8_t> manifestBytes;
        std::vector<uint8_t> footerBytes;
        uint32_t firstTimelineFrame = 0u;
        uint32_t lastTimelineFrame = 0u;
        bool hasSessionStart = false;
    };

    std::vector<Record> records_;
    std::wstring path_;
    void* lockHandle_ = nullptr;
    bool readOnly_ = false;
    bool loaded_ = false;
    bool dirty_ = false;
};

class ReplayScanner {
public:
    struct Config {
        std::vector<std::wstring> roots;
        bool includeTrash = false;
        bool useCache = true;
        std::wstring cachePath;
        uint32_t maxFiles = 100000u;
        uint32_t loadChunkFiles = 24u;
        uint32_t maxLoadedFiles = 0u;
        uint32_t pollIntervalMs = 0u;
    };

    ReplayScanner();
    ~ReplayScanner();
    ReplayScanner(const ReplayScanner&) = delete;
    ReplayScanner& operator=(const ReplayScanner&) = delete;

    void start(const Config& config);
    void requestRefresh();
    void stop();
    bool scanning() const { return scanning_.load(); }
    uint64_t generation() const { return generation_.load(); }

    std::shared_ptr<const ReplaySnapshot> snapshot() const;

    using ProgressPublish = std::function<void(ReplaySnapshot)>;

    static ReplaySnapshot scanOnce(const Config& config,
                                   ReplayCatalogCache* cache,
                                   const std::atomic<bool>* cancelled,
                                   uint64_t generation,
                                   const ProgressPublish& publish = {});

    static bool loadEntry(const std::wstring& path, ReplayEntry& out);

private:
    void workerMain();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    Config config_{};
    std::shared_ptr<const ReplaySnapshot> snapshot_;
    std::unique_ptr<ReplayCatalogCache> cache_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> cancelled_{ false };
    std::atomic<bool> scanning_{ false };
    std::atomic<bool> refreshRequested_{ false };
    std::atomic<uint64_t> generation_{ 0u };
};

enum class SortKey : uint8_t {
    Newest = 0,
    Oldest,
    TitleAsc,
    DurationDesc,
    MatchCountDesc,
    ResultAsc,
    StatusAsc,
    GameOrder,
};

const char* sortKeyText(SortKey key);

enum class OwnershipMix : uint8_t { Any = 0, AllHuman, MixedHumanCpu, AllCpu };
enum class CompletionFilter : uint8_t { Any = 0, CompleteOnly, IncompleteOnly };
enum class VariantFilter : uint8_t { Any = 0, BaseOnly, AlternateOnly };

enum class ReplaySourceResolved : uint8_t {
    Unknown = 0,
    Offline,
    Netplay,
    Spectator,
    Recovered,
};

ReplaySourceResolved resolvedSourceOf(const ReplayEntry& entry);
const char* replaySourceLabel(ReplaySourceResolved source);

struct ReplayFilter {
    bool sourceOffline = true;
    bool sourceNetplay = true;
    bool sourceSpectator = true;
    bool sourceRecovered = true;
    bool favoritesOnly = false;
    CompletionFilter completion = CompletionFilter::Any;

    bool showCorrupt = true;
    bool showUnsupported = true;
    bool showIncomplete = true;
    bool showRecording = true;
    bool showContentMismatch = true;

    uint8_t fighterCount = 0u;
    OwnershipMix ownership = OwnershipMix::Any;

    std::vector<uint8_t> characterIds;
    bool requireAllCharacters = false;
    VariantFilter variant = VariantFilter::Any;
    std::vector<uint8_t> sideZeroCharacterIds;
    std::vector<uint8_t> sideOneCharacterIds;

    int64_t fromUnixSeconds = 0;
    int64_t toUnixSeconds = 0;

    std::vector<std::string> requiredTags;

    std::wstring directoryScope;

    bool active() const;
    void clear() {
        std::wstring scope = directoryScope;
        *this = ReplayFilter{};
        directoryScope = std::move(scope);
    }
};

struct QueryInputs {
    const ReplaySnapshot* snapshot = nullptr;
    const ReplayAnnotationStore* annotations = nullptr;
    const std::vector<qreplay::CompatibilityState>* states = nullptr;
    std::string query;
    ReplayFilter filter{};
    SortKey sort = SortKey::GameOrder;
};

std::vector<size_t> applyQuery(const QueryInputs& inputs);

bool entryMatchesQuery(const ReplayEntry& entry,
                       const ReplayAnnotationStore* annotations,
                       const std::string& lowerQuery);

uint64_t stableIdForSession(uint64_t uuidLow, uint64_t uuidHigh);
uint64_t stableIdForPath(const std::wstring& path);

}
