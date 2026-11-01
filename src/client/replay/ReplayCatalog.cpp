#include "ReplayCatalog.h"

#include "ReplayAnnotations.h"
#include "platform/WidePath.h"
#include "replay/CharacterCatalog.h"
#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <chrono>
#include <cstring>
#include <cwchar>

namespace qclient::replay {

using namespace qreplay;

namespace {

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001B3ull;

uint64_t fnv1a(uint64_t seed, const uint8_t* data, size_t len) {
    uint64_t hash = seed;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

std::string toLowerAscii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
    }
    return text;
}

std::wstring toLowerW(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + 32);
    }
    return text;
}

bool endsWithLower(const std::wstring& text, const wchar_t* suffix) {
    const std::wstring lower = toLowerW(text);
    const size_t len = std::wcslen(suffix);
    return lower.size() >= len
        && lower.compare(lower.size() - len, len, suffix) == 0;
}

bool contains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return haystack.find(needle) != std::string::npos;
}

bool containsId(const std::vector<uint8_t>& ids, uint8_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool hasV2Prefix(const std::wstring& path) {
    FileByteSource source;
    uint8_t prefix[8] = {};
    if (!source.open(path)
        || !source.read(0u, prefix, sizeof(prefix))) {
        return false;
    }
    const uint32_t magic =
        static_cast<uint32_t>(prefix[0])
        | (static_cast<uint32_t>(prefix[1]) << 8u)
        | (static_cast<uint32_t>(prefix[2]) << 16u)
        | (static_cast<uint32_t>(prefix[3]) << 24u);
    const uint16_t major =
        static_cast<uint16_t>(prefix[4])
        | static_cast<uint16_t>(
            static_cast<uint16_t>(prefix[5]) << 8u);
    return magic == kReplayMagic
        && major == kContainerVersionMajorV2;
}

}

uint8_t ReplayEntry::activeFighterCount() const {
    uint8_t count = 0u;
    if (isV2) {
        for (const FighterSlotDescriptorV2& slot :
             startV2.manifest.slots) {
            if (slot.ownership != SlotOwnership::Inactive) ++count;
        }
        return count;
    }
    for (const FighterSlotDescriptor& slot : manifest.slots) {
        if (slot.ownership != SlotOwnership::Inactive) ++count;
    }
    return count;
}

uint8_t ReplayEntry::humanFighterCount() const {
    uint8_t count = 0u;
    if (isV2) {
        for (const FighterSlotDescriptorV2& slot :
             startV2.manifest.slots) {
            if (slot.ownership == SlotOwnership::RecordedHuman) ++count;
        }
        return count;
    }
    for (const FighterSlotDescriptor& slot : manifest.slots) {
        if (slot.ownership == SlotOwnership::RecordedHuman) ++count;
    }
    return count;
}

uint8_t ReplayEntry::cpuFighterCount() const {
    uint8_t count = 0u;
    if (isV2) {
        for (const FighterSlotDescriptorV2& slot :
             startV2.manifest.slots) {
            if (slot.ownership == SlotOwnership::NativeCpu) ++count;
        }
        return count;
    }
    for (const FighterSlotDescriptor& slot : manifest.slots) {
        if (slot.ownership == SlotOwnership::NativeCpu) ++count;
    }
    return count;
}

bool ReplayEntry::usesNonIdentityRoute() const {
    if (isV2) {
        for (size_t i = 0; i < kFighterSlots; ++i) {
            if (startV2.manifest.slotToSourceRoute[i]
                != static_cast<uint8_t>(i)) {
                return true;
            }
        }
        return false;
    }
    for (size_t i = 0; i < kFighterSlots; ++i) {
        if (manifest.slotToSourceRoute[i] != static_cast<uint8_t>(i))
            return true;
    }
    return false;
}

uint32_t ReplayEntry::durationFrames() const {
    if (totalFrames != 0u) return totalFrames;
    if (lastTimelineFrame >= firstTimelineFrame)
        return lastTimelineFrame - firstTimelineFrame;
    return 0u;
}

int64_t ReplayEntry::recordedUnixSeconds() const {
    if (isV2
        && startV2.manifest.display.recordedUnixSeconds != 0) {
        return startV2.manifest.display.recordedUnixSeconds;
    }
    if (manifest.display.recordedUnixSeconds != 0)
        return manifest.display.recordedUnixSeconds;
    return identity.modifiedUnixSeconds;
}

const ReplayEntry* ReplaySnapshot::findByStableId(uint64_t stableId) const {
    for (const ReplayEntry& entry : entries) {
        if (entry.stableId == stableId) return &entry;
    }
    return nullptr;
}

uint64_t stableIdForSession(uint64_t uuidLow, uint64_t uuidHigh) {
    uint8_t bytes[16];
    for (unsigned i = 0; i < 8u; ++i) {
        bytes[i] = static_cast<uint8_t>(uuidLow >> (i * 8u));
        bytes[8u + i] = static_cast<uint8_t>(uuidHigh >> (i * 8u));
    }
    const uint64_t hash = fnv1a(kFnvOffset, bytes, sizeof(bytes));
    return hash != 0u ? hash : 1u;
}

uint64_t stableIdForPath(const std::wstring& path) {
    const std::string narrow = qplat::narrow(toLowerW(path));
    const uint64_t hash = fnv1a(
        kFnvOffset ^ 0x9E3779B97F4A7C15ull,
        reinterpret_cast<const uint8_t*>(narrow.data()), narrow.size());
    return hash != 0u ? hash : 2u;
}

const char* sortKeyText(SortKey key) {
    switch (key) {
        case SortKey::GameOrder: return "Game order (1, 2, 3...)";
        case SortKey::Newest: return "Newest first";
        case SortKey::Oldest: return "Oldest first";
        case SortKey::TitleAsc: return "Alphabetical (A-Z)";
        case SortKey::DurationDesc: return "Duration (longest first)";
        case SortKey::MatchCountDesc: return "Match count (most first)";
        case SortKey::ResultAsc: return "Result";
        case SortKey::StatusAsc: return "Status";
    }
    return "Newest";
}

bool ReplayFilter::active() const {
    const ReplayFilter fresh{};
    return sourceOffline != fresh.sourceOffline
        || sourceNetplay != fresh.sourceNetplay
        || sourceSpectator != fresh.sourceSpectator
        || sourceRecovered != fresh.sourceRecovered
        || favoritesOnly != fresh.favoritesOnly
        || completion != fresh.completion || showCorrupt != fresh.showCorrupt
        || showUnsupported != fresh.showUnsupported
        || showIncomplete != fresh.showIncomplete
        || showRecording != fresh.showRecording
        || showContentMismatch != fresh.showContentMismatch
        || fighterCount != fresh.fighterCount || ownership != fresh.ownership
        || !characterIds.empty() || variant != fresh.variant
        || !sideZeroCharacterIds.empty() || !sideOneCharacterIds.empty()
        || fromUnixSeconds != 0 || toUnixSeconds != 0
        || !requiredTags.empty();
}

ReplayCatalogCache::~ReplayCatalogCache() { close(); }

bool ReplayCatalogCache::open(const std::wstring& cachePath) {
    close();
    path_ = cachePath;
    readOnly_ = false;
    loaded_ = false;

    const std::wstring lockPath = cachePath + L".lock";
    HANDLE lock = ::CreateFileW(
        qplat::longPathW(lockPath).c_str(), GENERIC_READ | GENERIC_WRITE,
        0 , nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lock == INVALID_HANDLE_VALUE) {
        readOnly_ = true;
    } else {
        lockHandle_ = lock;
    }

    FileByteSource source;
    if (!source.open(cachePath)) {
        loaded_ = true;
        return !readOnly_;
    }
    const uint64_t size = source.size();
    if (size == 0u || size > kMaxFileBytes) {
        discard();
        loaded_ = true;
        return !readOnly_;
    }
    std::vector<uint8_t> bytes;
    bytes.resize(static_cast<size_t>(size));
    if (!source.read(0u, bytes.data(), bytes.size()) || !decode(bytes.data(), bytes.size())) {
        discard();
    }
    loaded_ = true;
    return !readOnly_;
}

void ReplayCatalogCache::close() {
    if (lockHandle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(lockHandle_));
        lockHandle_ = nullptr;
    }
    records_.clear();
    path_.clear();
    readOnly_ = false;
    loaded_ = false;
    dirty_ = false;
}

void ReplayCatalogCache::discard() {
    records_.clear();
    dirty_ = true;
}

bool ReplayCatalogCache::lookup(const FileIdentity& identity,
                                ReplayEntry& out) const {
    if (!identity.known) return false;
    for (const Record& record : records_) {
        if (!record.identity.unchanged(identity)) continue;

        ReplayEntry entry{};
        entry.path = record.path;
        entry.identity = identity;
        entry.isPart = record.isPart;
        entry.parsed = record.parsed;
        entry.parseStatus = static_cast<ParseStatus>(record.parseStatus);
        entry.parseOffset = record.parseOffset;
        entry.firstTimelineFrame = record.firstTimelineFrame;
        entry.lastTimelineFrame = record.lastTimelineFrame;
        entry.hasSessionStart = record.hasSessionStart;
        entry.cached = true;

        if (record.prefixBytes.size() == kPrefixBytes
            && decodePrefix(record.prefixBytes.data(),
                            record.prefixBytes.size(), entry.prefix)
                == ParseStatus::Ok) {
            entry.stableId = stableIdForSession(entry.prefix.sessionUuidLow,
                                                entry.prefix.sessionUuidHigh);
        } else {
            entry.stableId = stableIdForPath(record.path);
        }
        if (!record.manifestBytes.empty()
            && decodeSessionManifest(record.manifestBytes.data(),
                                     record.manifestBytes.size(),
                                     entry.manifest) == ParseStatus::Ok) {
            entry.hasManifest = true;
        }
        if (!record.footerBytes.empty()) {
            FooterIndex footer{};
            if (decodeFooterIndex(record.footerBytes.data(),
                                  record.footerBytes.size(), footer)
                == ParseStatus::Ok) {
                entry.hasFooter = true;
                entry.matchCount = footer.matchCount;
                entry.totalFrames = footer.totalFrames;
                entry.finalRollingDigest = footer.finalRollingDigest;
                entry.fileDigest = footer.fileDigest;
                for (const MatchIndexEntry& index : footer.matches) {
                    if (entry.matches.size()
                        >= ReplayEntry::kMaxRetainedMatches) {
                        entry.matchesTruncated = true;
                        break;
                    }
                    MatchSpan span{};
                    span.matchId = index.matchId;
                    span.ordinal = index.ordinal;
                    span.firstTimelineFrame = index.firstTimelineFrame;
                    span.lastTimelineFrame = index.lastTimelineFrame;
                    span.frameCount = index.frameCount;
                    span.roundCount = index.roundCount;
                    span.matchStartOffset = index.matchStartOffset;
                    span.firstInputOffset = index.firstInputOffset;
                    span.winningSideMask = index.winningSideMask;
                    span.independentBootstrap = index.independentBootstrap;
                    span.hasEnd = true;
                    span.roster = index.roster;
                    span.ownership = index.ownership;
                    entry.matches.push_back(span);
                }
            }
        }
        const size_t slash = record.path.find_last_of(L"\\/");
        entry.fileName = slash == std::wstring::npos
            ? record.path
            : record.path.substr(slash + 1u);
        out = entry;
        return true;
    }
    return false;
}

void ReplayCatalogCache::store(const ReplayEntry& entry) {
    if (readOnly_) return;
    if (!entry.identity.known) return;
    if (entry.isV2) return;

    Record record{};
    record.identity = entry.identity;
    record.path = entry.path;
    record.isPart = entry.isPart;
    record.parsed = entry.parsed;
    record.parseStatus = static_cast<uint16_t>(entry.parseStatus);
    record.parseOffset = entry.parseOffset;
    record.firstTimelineFrame = entry.firstTimelineFrame;
    record.lastTimelineFrame = entry.lastTimelineFrame;
    record.hasSessionStart = entry.hasSessionStart;
    encodePrefix(entry.prefix, record.prefixBytes);
    if (entry.hasManifest) {
        if (!encodeSessionManifest(entry.manifest, record.manifestBytes)
            || record.manifestBytes.size() > kMaxManifestBytes) {
            record.manifestBytes.clear();
        }
    }
    if (entry.hasFooter && !entry.matchesTruncated) {
        FooterIndex footer{};
        footer.chunkCount = 0u;
        footer.matchCount = static_cast<uint32_t>(entry.matches.size());
        footer.totalFrames = entry.totalFrames;
        footer.finalRollingDigest = entry.finalRollingDigest;
        footer.fileDigest = entry.fileDigest;
        for (const MatchSpan& span : entry.matches) {
            MatchIndexEntry index{};
            index.matchId = span.matchId;
            index.ordinal = span.ordinal;
            index.matchStartOffset = span.matchStartOffset;
            index.firstInputOffset = span.firstInputOffset;
            index.firstTimelineFrame = span.firstTimelineFrame;
            index.lastTimelineFrame = span.lastTimelineFrame;
            index.frameCount = span.frameCount;
            index.roundCount = span.roundCount;
            index.winningSideMask = span.winningSideMask;
            index.independentBootstrap = span.independentBootstrap;
            index.roster = span.roster;
            index.ownership = span.ownership;
            footer.matches.push_back(index);
        }
        if (!encodeFooterIndex(footer, record.footerBytes)
            || record.footerBytes.size() > kMaxFooterBytes) {
            record.footerBytes.clear();
        }
    }

    for (Record& existing : records_) {
        if (existing.identity.sameFile(entry.identity)) {
            existing = std::move(record);
            dirty_ = true;
            return;
        }
    }
    if (records_.size() >= kMaxRecords) records_.erase(records_.begin());
    records_.push_back(std::move(record));
    dirty_ = true;
}

std::vector<uint8_t> ReplayCatalogCache::encode() const {
    std::vector<uint8_t> body;
    for (const Record& record : records_) {
        putU64(body, record.identity.volumeId);
        putU64(body, record.identity.fileId);
        putU64(body, record.identity.sizeBytes);
        putI64(body, record.identity.modifiedUnixSeconds);
        uint8_t flags = 0u;
        if (record.isPart) flags |= 0x01u;
        if (record.parsed) flags |= 0x02u;
        if (record.hasSessionStart) flags |= 0x04u;
        putU8(body, flags);
        putU8(body, 0u);
        putU16(body, record.parseStatus);
        putU64(body, record.parseOffset);
        putU32(body, record.firstTimelineFrame);
        putU32(body, record.lastTimelineFrame);
        putString(body, qplat::narrow(record.path), 0xFFFFu);
        putU32(body, static_cast<uint32_t>(record.prefixBytes.size()));
        putBytes(body, record.prefixBytes.data(), record.prefixBytes.size());
        putU32(body, static_cast<uint32_t>(record.manifestBytes.size()));
        putBytes(body, record.manifestBytes.data(),
                 record.manifestBytes.size());
        putU32(body, static_cast<uint32_t>(record.footerBytes.size()));
        putBytes(body, record.footerBytes.data(), record.footerBytes.size());
    }
    std::vector<uint8_t> out;
    putU32(out, kMagic);
    putU32(out, kVersion);
    putU32(out, static_cast<uint32_t>(records_.size()));
    putU32(out, crc32c(body.data(), body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool ReplayCatalogCache::decode(const uint8_t* data, size_t len) {
    records_.clear();
    if (data == nullptr || len < 16u || len > kMaxFileBytes) return false;

    size_t off = 0u;
    uint32_t magic = 0u;
    uint32_t version = 0u;
    uint32_t count = 0u;
    uint32_t storedCrc = 0u;
    if (!getU32(data, len, off, magic) || !getU32(data, len, off, version)
        || !getU32(data, len, off, count)
        || !getU32(data, len, off, storedCrc)) {
        return false;
    }
    if (magic != kMagic || version != kVersion || count > kMaxRecords)
        return false;
    if (crc32c(data + 16u, len - 16u) != storedCrc) return false;

    std::vector<Record> loaded;
    loaded.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Record record{};
        uint8_t flags = 0u;
        uint8_t reserved = 0u;
        uint64_t modified = 0u;
        std::string path;
        if (!getU64(data, len, off, record.identity.volumeId)
            || !getU64(data, len, off, record.identity.fileId)
            || !getU64(data, len, off, record.identity.sizeBytes)
            || !getU64(data, len, off, modified)
            || !getU8(data, len, off, flags)
            || !getU8(data, len, off, reserved)
            || !getU16(data, len, off, record.parseStatus)
            || !getU64(data, len, off, record.parseOffset)
            || !getU32(data, len, off, record.firstTimelineFrame)
            || !getU32(data, len, off, record.lastTimelineFrame)) {
            return false;
        }
        if (reserved != 0u || (flags & 0xF8u) != 0u) return false;
        record.identity.modifiedUnixSeconds = static_cast<int64_t>(modified);
        record.identity.known = true;
        record.isPart = (flags & 0x01u) != 0u;
        record.parsed = (flags & 0x02u) != 0u;
        record.hasSessionStart = (flags & 0x04u) != 0u;
        if (getString(data, len, off, 0xFFFFu, path) != ParseStatus::Ok)
            return false;
        record.path = qplat::widen(path);

        uint32_t blobLength = 0u;
        if (!getU32(data, len, off, blobLength)
            || blobLength > kPrefixBytes) {
            return false;
        }
        record.prefixBytes.resize(blobLength);
        if (blobLength != 0u
            && !getBytes(data, len, off, record.prefixBytes.data(),
                         blobLength)) {
            return false;
        }
        if (!getU32(data, len, off, blobLength)
            || blobLength > kMaxManifestBytes) {
            return false;
        }
        record.manifestBytes.resize(blobLength);
        if (blobLength != 0u
            && !getBytes(data, len, off, record.manifestBytes.data(),
                         blobLength)) {
            return false;
        }
        if (!getU32(data, len, off, blobLength)
            || blobLength > kMaxFooterBytes) {
            return false;
        }
        record.footerBytes.resize(blobLength);
        if (blobLength != 0u
            && !getBytes(data, len, off, record.footerBytes.data(),
                         blobLength)) {
            return false;
        }
        loaded.push_back(std::move(record));
    }
    if (off != len) return false;
    records_.swap(loaded);
    return true;
}

bool ReplayCatalogCache::flush() {
    if (readOnly_ || path_.empty() || !dirty_) return !readOnly_;
    const std::vector<uint8_t> bytes = encode();
    if (bytes.size() > kMaxFileBytes) return false;
    const std::wstring temp = path_ + L".tmp";
    HANDLE handle = ::CreateFileW(qplat::longPathW(temp).c_str(), GENERIC_WRITE,
                                 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0u;
    const BOOL ok = ::WriteFile(handle, bytes.data(),
                                static_cast<DWORD>(bytes.size()), &wrote,
                                nullptr);
    ::FlushFileBuffers(handle);
    ::CloseHandle(handle);
    if (ok == 0 || wrote != bytes.size()) {
        ::DeleteFileW(qplat::longPathW(temp).c_str());
        return false;
    }
    if (!qplat::atomicReplaceFileW(temp, path_)) {
        ::DeleteFileW(qplat::longPathW(temp).c_str());
        return false;
    }
    dirty_ = false;
    return true;
}

bool ReplayScanner::loadEntry(const std::wstring& path, ReplayEntry& out) {
    out = ReplayEntry{};
    out.path = path;
    const size_t slash = path.find_last_of(L"\\/");
    out.fileName = slash == std::wstring::npos ? path : path.substr(slash + 1u);
    out.isPart = false;

    if (hasV2Prefix(path)) {
        ParseOptionsV2 options{};
        options.retainTimeline = false;
        options.allowUnsealedPrefix = true;
        options.sourceNameEndsInPart = false;
        options.collectChunkIndex = false;
        options.verifyFileDigest = false;
        ParsedReplayV2 parsed;
        const ParseResult result =
            ReplayParserV2::parseFile(path, options, parsed);

        out.identity = parsed.fileIdentity;
        if (!out.identity.known) queryFileIdentity(path, out.identity);
        out.parseStatus = result.status;
        out.parseOffset = result.offset;
        out.parseDetail = result.detail;
        out.parsed = result.ok();
        out.isV2 = true;
        out.classificationV2 = parsed.classification;
        out.startV2 = parsed.start;
        out.hasEndV2 = parsed.hasEnd;
        out.endV2 = parsed.end;
        out.compatibilityIdentityV3 = parsed.compatibilityIdentity;
        out.eventCountV2 = parsed.eventCount;
        out.structurallyVerifiedV2 = parsed.structurallyVerified;
        out.fileDigestVerifiedV2 = parsed.fileDigestVerified;

        out.prefix.containerMajor = parsed.prefix.containerMajor;
        out.prefix.containerMinor = parsed.prefix.containerMinor;
        out.prefix.minimumReaderMajor =
            parsed.prefix.minimumReaderMajor;
        out.prefix.featureFlags = parsed.prefix.featureFlags;
        out.prefix.sessionUuidLow = parsed.prefix.gameUuidLow;
        out.prefix.sessionUuidHigh = parsed.prefix.gameUuidHigh;
        out.hasManifest = parsed.hasManifest;
        out.hasSessionStart = parsed.hasStart;
        out.firstTimelineFrame = 0u;
        out.lastTimelineFrame =
            parsed.frameCount == 0u ? 0u : parsed.frameCount - 1u;
        out.hasFooter = parsed.hasFooter;
        out.matchCount = parsed.hasStart ? 1u : 0u;
        out.totalFrames = parsed.frameCount;
        out.finalRollingDigest = parsed.finalRollingDigest;
        if (parsed.hasFooter) out.fileDigest = parsed.footer.fileDigest;

        if (parsed.hasStart && parsed.frameCount != 0u) {
            MatchSpan span{};
            span.matchId =
                parsed.start.manifest.identity.gameGeneration;
            span.ordinal = 1u;
            span.firstTimelineFrame = 0u;
            span.lastTimelineFrame = parsed.frameCount - 1u;
            span.frameCount = parsed.frameCount;
            span.roundCount =
                static_cast<uint32_t>(parsed.footer.rounds.size());
            span.hasEnd = parsed.hasEnd;
            span.independentBootstrap = true;
            span.winningSideMask =
                parsed.hasEnd ? parsed.end.winningSideMask : 0u;
            span.endReason = parsed.hasEnd
                ? parsed.end.reason
                : TerminalReason::RecoveredPrefix;
            for (size_t slot = 0u; slot < kFighterSlots; ++slot) {
                span.roster[slot].nativeCharacterId =
                    parsed.start.manifest.slots[slot]
                        .identity.nativeCharacterId;
                span.roster[slot].paletteIndex =
                    parsed.start.manifest.slots[slot]
                        .identity.paletteIndex;
                span.ownership[slot] =
                    parsed.start.manifest.slots[slot].ownership;
            }
            out.matches.push_back(span);
        }
        out.stableId = parsed.hasManifest
            ? stableIdForSession(
                parsed.prefix.gameUuidLow,
                parsed.prefix.gameUuidHigh)
            : stableIdForPath(path);

        return out.parsed;
    }

    ParseOptions options{};
    options.mode = ParseMode::Metadata;
    ParsedReplay parsed;
    const ParseResult result = ReplayParser::parseFile(path, options, parsed);

    out.identity = parsed.identity;
    if (!out.identity.known) queryFileIdentity(path, out.identity);
    out.parseStatus = result.status;
    out.parseOffset = result.offset;
    out.parseDetail = result.detail;
    out.parsed = result.ok();
    out.prefix = parsed.file.prefix;
    out.hasManifest = parsed.hasManifest;
    out.manifest = parsed.file.manifest;
    out.hasSessionStart = parsed.hasSessionStart;
    out.firstTimelineFrame = parsed.firstTimelineFrame;
    out.lastTimelineFrame = parsed.lastTimelineFrame;
    out.hasFooter = parsed.hasFooter;
    if (parsed.hasFooter) {
        out.matchCount = parsed.file.footer.matchCount;
        out.totalFrames = parsed.file.footer.totalFrames;
        out.finalRollingDigest = parsed.file.footer.finalRollingDigest;
        out.fileDigest = parsed.file.footer.fileDigest;
    }
    for (const MatchSpan& span : parsed.matches) {
        if (out.matches.size() >= ReplayEntry::kMaxRetainedMatches) {
            out.matchesTruncated = true;
            break;
        }
        out.matches.push_back(span);
    }
    out.stableId = out.hasManifest
        ? stableIdForSession(out.prefix.sessionUuidLow,
                             out.prefix.sessionUuidHigh)
        : stableIdForPath(path);

    return out.parsed;
}

ReplaySnapshot ReplayScanner::scanOnce(const Config& config,
                                       ReplayCatalogCache* cache,
                                       const std::atomic<bool>* cancelled,
                                       uint64_t generation,
                                       const ProgressPublish& publish) {
    ReplaySnapshot snapshot{};
    snapshot.generation = generation;
    snapshot.roots = config.roots;
    snapshot.cacheReadOnly = cache != nullptr && cache->readOnly();
    const uint64_t startTick = ::GetTickCount64();

    struct ScanDir {
        std::wstring path;
        std::wstring root;
        uint32_t depth = 0u;
        bool isTrash = false;
    };
    constexpr uint32_t kMaxScanDepth = 4u;
    std::vector<ScanDir> directories;
    for (const std::wstring& root : config.roots) {
        if (root.empty()) continue;
        std::wstring normalized = qplat::absolutePathW(root);
        if (normalized.empty()) normalized = root;
        while (!normalized.empty()
               && (normalized.back() == L'\\' || normalized.back() == L'/')) {
            normalized.pop_back();
        }
        if (normalized.empty()) continue;
        directories.push_back(ScanDir{normalized, normalized, 0u, false});
        if (config.includeTrash) {
            const std::wstring trash = normalized + L"\\.trash";
            directories.push_back(ScanDir{trash, trash, 0u, true});
        }
    }
    if (!directories.empty()) {
        snapshot.effectiveRoot = directories.front().path;
        snapshot.rootWritable =
            qplat::isDirWritableW(directories.front().path);
    }

    struct Candidate {
        std::wstring path;
        std::wstring name;
        std::wstring originalName;
        uint64_t writeTime = 0u;
        bool isTrash = false;
        bool quarantined = false;
        std::wstring relativeDir;
    };
    std::vector<Candidate> candidates;

    bool budgetReached = false;
    for (size_t d = 0; d < directories.size() && !budgetReached; ++d) {
        // A copy, not a reference: the loop appends to `directories`, which can reallocate.
        const ScanDir dir = directories[d];
        const std::wstring& directory = dir.path;
        const bool isTrash = dir.isTrash;
        if (cancelled != nullptr && cancelled->load()) {
            snapshot.cancelled = true;
            break;
        }
        WIN32_FIND_DATAW found{};
        HANDLE handle = ::FindFirstFileW(
            qplat::longPathW(directory + L"\\*").c_str(), &found);
        if (handle == INVALID_HANDLE_VALUE) continue;
        do {
            if (cancelled != nullptr && cancelled->load()) {
                snapshot.cancelled = true;
                break;
            }
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
                const std::wstring childName = found.cFileName;
                if (childName == L"." || childName == L"..") continue;
                if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    != 0u) {
                    continue;
                }
                if (!isTrash && toLowerW(childName) == L".trash") continue;
                if (dir.depth + 1u > kMaxScanDepth) {
                    ++snapshot.directoriesBeyondDepth;
                    continue;
                }
                directories.push_back(ScanDir{
                    directory + L"\\" + childName, dir.root,
                    dir.depth + 1u, isTrash});
                continue;
            }
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
                continue;
            const std::wstring name = found.cFileName;
            std::wstring originalName;
            const bool quarantined = isTrash
                && qplat::originalNameFromTrashLeafW(
                       name, L".q99r", originalName);
            const bool accepted =
                endsWithLower(name, L".q99r") || quarantined;
            if (!accepted) continue;
            if (candidates.size() >= config.maxFiles) {
                budgetReached = true;
                break;
            }
            Candidate candidate{};
            candidate.path = directory + L"\\" + name;
            candidate.name = name;
            candidate.originalName = originalName;
            candidate.writeTime =
                (static_cast<uint64_t>(found.ftLastWriteTime.dwHighDateTime)
                 << 32)
                | found.ftLastWriteTime.dwLowDateTime;
            candidate.isTrash = isTrash;
            candidate.quarantined = quarantined;
            candidate.relativeDir =
                directory.size() > dir.root.size()
                    ? directory.substr(dir.root.size() + 1u)
                    : std::wstring();
            candidates.push_back(std::move(candidate));
        } while (::FindNextFileW(handle, &found) != 0);
        ::FindClose(handle);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.writeTime != b.writeTime)
                      return a.writeTime > b.writeTime;
                  return a.path < b.path;
              });
    snapshot.filesSeen = static_cast<uint32_t>(candidates.size());
    const size_t budget = config.maxLoadedFiles == 0u
        ? candidates.size()
        : static_cast<size_t>(config.maxLoadedFiles);
    if (candidates.size() > budget) {
        snapshot.filesBeyondLimit =
            static_cast<uint32_t>(candidates.size() - budget);
        candidates.resize(budget);
    }

    const size_t chunk = config.loadChunkFiles == 0u
        ? candidates.size()
        : static_cast<size_t>(config.loadChunkFiles);
    size_t sinceLastPublish = 0u;
    const auto publishPartial = [&]() {
        if (!publish || snapshot.entries.empty()) return;
        ReplaySnapshot partial = snapshot;
        partial.complete = false;
        partial.elapsedMs = ::GetTickCount64() - startTick;
        publish(std::move(partial));
    };

    for (const Candidate& candidate : candidates) {
        if (cancelled != nullptr && cancelled->load()) {
            snapshot.cancelled = true;
            break;
        }
        const bool v2 = hasV2Prefix(candidate.path);
        FileIdentity identity{};
        ReplayEntry entry{};
        bool served = false;
        if (!v2 && cache != nullptr
            && queryFileIdentity(candidate.path, identity)
            && cache->lookup(identity, entry)) {
            entry.path = candidate.path;
            entry.fileName = candidate.name;
            served = true;
            ++snapshot.filesFromCache;
        }
        if (!served) {
            ReplayScanner::loadEntry(candidate.path, entry);
            ++snapshot.filesParsed;
            if (cache != nullptr && !entry.isV2)
                cache->store(entry);
        }
        entry.inTrash = candidate.isTrash;
        entry.relativeDir = candidate.relativeDir;
        if (candidate.quarantined) entry.fileName = candidate.originalName;
        if (!entry.parsed) ++snapshot.filesRejected;
        snapshot.entries.push_back(std::move(entry));
        if (++sinceLastPublish >= chunk) {
            sinceLastPublish = 0u;
            publishPartial();
        }
    }

    snapshot.complete = !snapshot.cancelled;
    snapshot.elapsedMs = ::GetTickCount64() - startTick;
    if (cache != nullptr) cache->flush();
    return snapshot;
}

ReplayScanner::ReplayScanner() = default;

ReplayScanner::~ReplayScanner() { stop(); }

void ReplayScanner::start(const Config& config) {
    stop();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        config_ = config;
        snapshot_ = std::make_shared<const ReplaySnapshot>();
    }
    cancelled_.store(false);
    running_.store(true);
    refreshRequested_.store(true);
    worker_ = std::thread(&ReplayScanner::workerMain, this);
}

void ReplayScanner::requestRefresh() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!running_.load() || scanning_.load()) return;
    refreshRequested_.store(true);
    wake_.notify_all();
}

void ReplayScanner::stop() {
    if (!running_.load() && !worker_.joinable()) return;
    running_.store(false);
    cancelled_.store(true);
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    cancelled_.store(false);
}

std::shared_ptr<const ReplaySnapshot> ReplayScanner::snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return snapshot_;
}

void ReplayScanner::workerMain() {
    Config config;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        config = config_;
    }
    if (config.useCache && !config.cachePath.empty()) {
        cache_.reset(new ReplayCatalogCache());
        cache_->open(config.cachePath);
    }

    while (running_.load()) {
        bool scanRequested = false;
        bool publishProgress = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            scanRequested = refreshRequested_.exchange(false);
            if (scanRequested) scanning_.store(true);
            publishProgress = !snapshot_ || !snapshot_->complete;
        }
        if (scanRequested) {
            uint64_t generation = generation_.load() + 1u;
            const ProgressPublish publish =
                [this, &generation](ReplaySnapshot partial) {
                    if (!running_.load()) return;
                    std::lock_guard<std::mutex> guard(mutex_);
                    partial.generation = ++generation;
                    auto shared = std::make_shared<const ReplaySnapshot>(
                        std::move(partial));
                    snapshot_ = shared;
                    generation_.store(generation);
                };
            ReplaySnapshot scanned =
                scanOnce(config, cache_.get(), &cancelled_, generation,
                         publishProgress ? publish : ProgressPublish{});
            if (!running_.load()) break;
            scanned.generation = ++generation;
            auto published =
                std::make_shared<const ReplaySnapshot>(std::move(scanned));
            std::lock_guard<std::mutex> guard(mutex_);
            snapshot_ = published;
            generation_.store(generation);
            scanning_.store(false);
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (config.pollIntervalMs == 0u) {
            wake_.wait(lock, [this] {
                return !running_.load() || refreshRequested_.load();
            });
        } else {
            wake_.wait_for(lock,
                           std::chrono::milliseconds(config.pollIntervalMs),
                           [this] {
                               return !running_.load()
                                   || refreshRequested_.load();
                           });
            if (running_.load() && !refreshRequested_.load())
                refreshRequested_.store(true);
        }
    }
    scanning_.store(false);
    if (cache_) {
        cache_->flush();
        cache_->close();
        cache_.reset();
    }
}

bool entryMatchesQuery(const ReplayEntry& entry,
                       const ReplayAnnotationStore* annotations,
                       const std::string& lowerQuery) {
    if (lowerQuery.empty()) return true;

    if (contains(toLowerAscii(qplat::narrow(entry.fileName)), lowerQuery))
        return true;
    if (contains(toLowerAscii(entry.manifest.display.title), lowerQuery))
        return true;
    for (const SourceLaneDescriptor& lane : entry.manifest.lanes) {
        if (lane.active && contains(toLowerAscii(lane.name), lowerQuery))
            return true;
    }
    if (annotations != nullptr) {
        const ReplayAnnotation* annotation = annotations->find(
            entry.prefix.sessionUuidLow, entry.prefix.sessionUuidHigh);
        if (annotation != nullptr) {
            if (contains(toLowerAscii(annotation->title), lowerQuery))
                return true;
            if (contains(toLowerAscii(annotation->note), lowerQuery))
                return true;
            for (const std::string& tag : annotation->tags) {
                if (contains(toLowerAscii(tag), lowerQuery)) return true;
            }
        }
    }
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();
    for (const FighterSlotDescriptor& slot : entry.manifest.slots) {
        if (slot.ownership == SlotOwnership::Inactive) continue;
        if (catalog.matchesSearch(slot.identity.nativeCharacterId, lowerQuery))
            return true;
    }
    for (const MatchSpan& span : entry.matches) {
        for (const FighterIdentityV1& identity : span.roster) {
            if (identity.nativeCharacterId == kInactiveCharacterId) continue;
            if (catalog.matchesSearch(identity.nativeCharacterId, lowerQuery))
                return true;
        }
    }
    return false;
}

namespace {

bool matchesCharacterFilter(const ReplayEntry& entry,
                            const ReplayFilter& filter) {
    if (filter.characterIds.empty() && filter.sideZeroCharacterIds.empty()
        && filter.sideOneCharacterIds.empty()
        && filter.variant == VariantFilter::Any) {
        return true;
    }
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();

    std::vector<uint8_t> present;
    std::vector<uint8_t> sideZero;
    std::vector<uint8_t> sideOne;
    auto record = [&](uint8_t id, uint8_t slot) {
        if (id == kInactiveCharacterId) return;
        if (!containsId(present, id)) present.push_back(id);
        std::vector<uint8_t>& side = (slot % 2u) == 0u ? sideZero : sideOne;
        if (!containsId(side, id)) side.push_back(id);
    };
    for (size_t i = 0; i < kFighterSlots; ++i) {
        const FighterSlotDescriptor& slot = entry.manifest.slots[i];
        if (slot.ownership == SlotOwnership::Inactive) continue;
        record(slot.identity.nativeCharacterId, static_cast<uint8_t>(i));
    }
    for (const MatchSpan& span : entry.matches) {
        for (size_t i = 0; i < kFighterSlots; ++i) {
            if (span.ownership[i] == SlotOwnership::Inactive) continue;
            record(span.roster[i].nativeCharacterId, static_cast<uint8_t>(i));
        }
    }

    if (!filter.characterIds.empty()) {
        if (filter.requireAllCharacters) {
            for (uint8_t id : filter.characterIds) {
                if (!containsId(present, id)) return false;
            }
        } else {
            bool any = false;
            for (uint8_t id : filter.characterIds) {
                if (containsId(present, id)) { any = true; break; }
            }
            if (!any) return false;
        }
    }
    for (uint8_t id : filter.sideZeroCharacterIds) {
        if (!containsId(sideZero, id)) return false;
    }
    for (uint8_t id : filter.sideOneCharacterIds) {
        if (!containsId(sideOne, id)) return false;
    }
    if (filter.variant != VariantFilter::Any) {
        bool sawAlternate = false;
        bool sawBase = false;
        for (uint8_t id : present) {
            const CharacterEntry* character = catalog.find(id);
            if (character == nullptr) continue;
            if (character->standAlternate) sawAlternate = true;
            else sawBase = true;
        }
        if (filter.variant == VariantFilter::AlternateOnly && !sawAlternate)
            return false;
        if (filter.variant == VariantFilter::BaseOnly && !sawBase) return false;
    }
    return true;
}

bool statusVisible(CompatibilityState state, const ReplayFilter& filter) {
    switch (state) {
        case CompatibilityState::Corrupt: return filter.showCorrupt;
        case CompatibilityState::Unsupported: return filter.showUnsupported;
        case CompatibilityState::Incomplete:
        case CompatibilityState::Recoverable: return filter.showIncomplete;
        case CompatibilityState::Recording: return filter.showRecording;
        case CompatibilityState::ContentMismatch:
            return filter.showContentMismatch;
        default: return true;
    }
}

int statusRank(CompatibilityState state) {
    switch (state) {
        case CompatibilityState::SimulationVerified: return 0;
        case CompatibilityState::Exact: return 1;
        case CompatibilityState::Compatible: return 2;
        case CompatibilityState::FileIntact: return 3;
        case CompatibilityState::Recording: return 4;
        case CompatibilityState::Recoverable: return 5;
        case CompatibilityState::Incomplete: return 6;
        case CompatibilityState::ContentMismatch: return 7;
        case CompatibilityState::Unsupported: return 8;
        case CompatibilityState::Corrupt: return 9;
    }
    return 10;
}

std::string sortTitleOf(const ReplayEntry& entry,
                        const ReplayAnnotationStore* annotations) {
    if (annotations != nullptr) {
        const ReplayAnnotation* annotation = annotations->find(
            entry.prefix.sessionUuidLow, entry.prefix.sessionUuidHigh);
        if (annotation != nullptr && !annotation->title.empty())
            return toLowerAscii(annotation->title);
    }
    if (!entry.manifest.display.title.empty())
        return toLowerAscii(entry.manifest.display.title);
    std::string names;
    for (const SourceLaneDescriptor& lane : entry.manifest.lanes) {
        if (!lane.active || lane.name.empty()) continue;
        if (!names.empty()) names += " vs ";
        names += lane.name;
    }
    if (!names.empty()) return toLowerAscii(names);
    return toLowerAscii(qplat::narrow(entry.fileName));
}

}

ReplaySourceResolved resolvedSourceOf(const ReplayEntry& entry) {
    if (entry.isV2) {
        if (entry.classificationV2 == ReplayClassificationV2::Invalid)
            return ReplaySourceResolved::Unknown;
        switch (entry.startV2.manifest.display.sourceKind) {
            case qreplay::ReplaySourceKindV2::OfflineVsPlayer:
                return ReplaySourceResolved::Offline;
            case qreplay::ReplaySourceKindV2::NetplayPlayer:
                return ReplaySourceResolved::Netplay;
            case qreplay::ReplaySourceKindV2::SpectatorSave:
                return ReplaySourceResolved::Spectator;
            case qreplay::ReplaySourceKindV2::RecoveredPrefix:
                return ReplaySourceResolved::Recovered;
        }
        return ReplaySourceResolved::Unknown;
    }
    if (!entry.hasManifest) return ReplaySourceResolved::Unknown;
    switch (entry.manifest.display.sourceKind) {
        case ReplaySourceKind::OfflineVsPlayer:
            return ReplaySourceResolved::Offline;
        case ReplaySourceKind::NetplayPlayer:
            return ReplaySourceResolved::Netplay;
        case ReplaySourceKind::SpectatorSave:
            return ReplaySourceResolved::Spectator;
        case ReplaySourceKind::RecoveredPrefix:
            return ReplaySourceResolved::Recovered;
    }
    return ReplaySourceResolved::Unknown;
}

const char* replaySourceLabel(ReplaySourceResolved source) {
    switch (source) {
        case ReplaySourceResolved::Offline:   return "Offline";
        case ReplaySourceResolved::Netplay:   return "Network";
        case ReplaySourceResolved::Spectator: return "Spectated";
        case ReplaySourceResolved::Recovered: return "Recovered";
        case ReplaySourceResolved::Unknown:   break;
    }
    return "Unknown";
}

std::vector<size_t> applyQuery(const QueryInputs& inputs) {
    std::vector<size_t> result;
    if (inputs.snapshot == nullptr) return result;
    const ReplaySnapshot& snapshot = *inputs.snapshot;
    const ReplayFilter& filter = inputs.filter;
    const std::string lowerQuery = toLowerAscii(inputs.query);

    result.reserve(snapshot.entries.size());
    for (size_t i = 0; i < snapshot.entries.size(); ++i) {
        const ReplayEntry& entry = snapshot.entries[i];

        if (entry.relativeDir != filter.directoryScope) continue;

        const ReplaySourceResolved source = resolvedSourceOf(entry);
        const bool sourceAllowed =
            source == ReplaySourceResolved::Unknown
            || (source == ReplaySourceResolved::Offline && filter.sourceOffline)
            || (source == ReplaySourceResolved::Netplay && filter.sourceNetplay)
            || (source == ReplaySourceResolved::Spectator
                && filter.sourceSpectator)
            || (source == ReplaySourceResolved::Recovered
                && filter.sourceRecovered);
        if (!sourceAllowed) continue;

        if (filter.completion == CompletionFilter::CompleteOnly
            && !entry.hasFooter) {
            continue;
        }
        if (filter.completion == CompletionFilter::IncompleteOnly
            && entry.hasFooter) {
            continue;
        }
        if (filter.favoritesOnly) {
            const ReplayAnnotation* annotation =
                inputs.annotations != nullptr
                    ? inputs.annotations->find(entry.prefix.sessionUuidLow,
                                               entry.prefix.sessionUuidHigh)
                    : nullptr;
            if (annotation == nullptr || !annotation->favorite) continue;
        }
        if (!filter.requiredTags.empty()) {
            const ReplayAnnotation* annotation =
                inputs.annotations != nullptr
                    ? inputs.annotations->find(entry.prefix.sessionUuidLow,
                                               entry.prefix.sessionUuidHigh)
                    : nullptr;
            if (annotation == nullptr) continue;
            bool all = true;
            for (const std::string& tag : filter.requiredTags) {
                if (std::find(annotation->tags.begin(), annotation->tags.end(),
                              tag) == annotation->tags.end()) {
                    all = false;
                    break;
                }
            }
            if (!all) continue;
        }
        if (filter.fighterCount == 2u && entry.activeFighterCount() > 2u)
            continue;
        if (filter.fighterCount == 4u && entry.activeFighterCount() <= 2u)
            continue;
        if (filter.ownership != OwnershipMix::Any) {
            const uint8_t humans = entry.humanFighterCount();
            const uint8_t cpus = entry.cpuFighterCount();
            if (filter.ownership == OwnershipMix::AllHuman && cpus != 0u)
                continue;
            if (filter.ownership == OwnershipMix::AllCpu && humans != 0u)
                continue;
            if (filter.ownership == OwnershipMix::MixedHumanCpu
                && (humans == 0u || cpus == 0u)) {
                continue;
            }
        }
        if (!matchesCharacterFilter(entry, filter)) continue;

        const int64_t recorded = entry.recordedUnixSeconds();
        if (filter.fromUnixSeconds != 0 && recorded < filter.fromUnixSeconds)
            continue;
        if (filter.toUnixSeconds != 0 && recorded > filter.toUnixSeconds)
            continue;

        if (inputs.states != nullptr && i < inputs.states->size()
            && !statusVisible((*inputs.states)[i], filter)) {
            continue;
        }
        if (!entryMatchesQuery(entry, inputs.annotations, lowerQuery)) continue;
        result.push_back(i);
    }

    const SortKey sort = inputs.sort;
    const ReplaySnapshot* snap = &snapshot;
    const std::vector<CompatibilityState>* states = inputs.states;
    const ReplayAnnotationStore* annotations = inputs.annotations;

    using GroupKey = std::tuple<std::wstring, uint64_t, uint64_t, std::wstring>;
    std::vector<GroupKey> groups;
    std::map<GroupKey, int64_t> groupDates;
    if (sort == SortKey::GameOrder) {
        groups.resize(snapshot.entries.size());
        for (size_t i = 0; i < snapshot.entries.size(); ++i) {
            const ReplayEntry& entry = snapshot.entries[i];
            if (entry.relativeDir != filter.directoryScope) continue;
            const auto& display = entry.startV2.manifest.display;
            const bool series = entry.isV2
                && (display.flags & DisplayMetadataSeriesV2) != 0u
                && (display.seriesUuidLow != 0u || display.seriesUuidHigh != 0u);
            groups[i] = series
                ? GroupKey{entry.relativeDir, display.seriesUuidHigh,
                           display.seriesUuidLow, L""}
                : GroupKey{entry.relativeDir, 0u, 0u, entry.path};
            const int64_t date = entry.recordedUnixSeconds();
            const auto inserted = groupDates.emplace(groups[i], date);
            inserted.first->second = std::max(inserted.first->second, date);
        }
    }

    std::stable_sort(
        result.begin(), result.end(), [&](size_t a, size_t b) {
            const ReplayEntry& left = snap->entries[a];
            const ReplayEntry& right = snap->entries[b];
            switch (sort) {
                case SortKey::GameOrder: {
                    const GroupKey& lg = groups[a];
                    const GroupKey& rg = groups[b];
                    if (lg != rg) {
                        const int64_t lt = groupDates.at(lg);
                        const int64_t rt = groupDates.at(rg);
                        if (lt != rt) return lt > rt;
                        return lg < rg;
                    }
                    const uint32_t lo = left.startV2.manifest.display.gameOrdinalInSeries;
                    const uint32_t ro = right.startV2.manifest.display.gameOrdinalInSeries;
                    if (lo != ro) return lo < ro;
                    break;
                }
                case SortKey::Newest: {
                    const int64_t lt = left.recordedUnixSeconds();
                    const int64_t rt = right.recordedUnixSeconds();
                    if (lt != rt) return lt > rt;
                    break;
                }
                case SortKey::Oldest: {
                    const int64_t lt = left.recordedUnixSeconds();
                    const int64_t rt = right.recordedUnixSeconds();
                    if (lt != rt) return lt < rt;
                    break;
                }
                case SortKey::TitleAsc: {
                    const std::string lv = sortTitleOf(left, annotations);
                    const std::string rv = sortTitleOf(right, annotations);
                    if (lv != rv) return lv < rv;
                    break;
                }
                case SortKey::DurationDesc: {
                    if (left.durationFrames() != right.durationFrames())
                        return left.durationFrames() > right.durationFrames();
                    break;
                }
                case SortKey::MatchCountDesc: {
                    if (left.matchCount != right.matchCount)
                        return left.matchCount > right.matchCount;
                    break;
                }
                case SortKey::ResultAsc: {
                    const uint8_t lv = left.matches.empty()
                        ? 0xFFu
                        : left.matches.back().winningSideMask;
                    const uint8_t rv = right.matches.empty()
                        ? 0xFFu
                        : right.matches.back().winningSideMask;
                    if (lv != rv) return lv < rv;
                    break;
                }
                case SortKey::StatusAsc: {
                    const int lv = states != nullptr && a < states->size()
                        ? statusRank((*states)[a])
                        : 10;
                    const int rv = states != nullptr && b < states->size()
                        ? statusRank((*states)[b])
                        : 10;
                    if (lv != rv) return lv < rv;
                    break;
                }
            }
            if (left.fileName != right.fileName)
                return left.fileName < right.fileName;
            if (left.stableId != right.stableId)
                return left.stableId < right.stableId;
            return left.path < right.path;
        });
    return result;
}

}
