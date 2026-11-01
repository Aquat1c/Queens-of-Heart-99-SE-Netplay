#include "ReplayAnnotations.h"

#include "ReplayParser.h"
#include "platform/WidePath.h"
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

namespace qclient::replay {

using namespace qreplay;

namespace {

constexpr uint8_t kFlagFavorite = 0x01u;
constexpr uint8_t kFlagStructural = 0x02u;
constexpr uint8_t kFlagSimulation = 0x04u;
constexpr uint8_t kFlagProvenance = 0x08u;
constexpr uint8_t kFlagMask = 0x0Fu;

}

bool ReplayAnnotationStore::validTitle(const std::string& text) {
    return text.size() <= kMaxLabelBytes && isCanonicalReplayText(text);
}

bool ReplayAnnotationStore::validNote(const std::string& text) {
    return text.size() <= kMaxNoteBytes && isCanonicalReplayText(text);
}

bool ReplayAnnotationStore::validTag(const std::string& text) {
    return !text.empty() && text.size() <= kMaxTagBytes
        && isCanonicalReplayText(text);
}

bool ReplayAnnotationStore::validTagSet(const std::vector<std::string>& tags) {
    if (tags.size() > kMaxTags) return false;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (!validTag(tags[i])) return false;
        for (size_t j = 0; j < i; ++j) {
            if (tags[i] == tags[j]) return false;
        }
    }
    return true;
}

void ReplayAnnotationStore::clear() {
    entries_.clear();
    dirty_ = true;
}

const ReplayAnnotation* ReplayAnnotationStore::find(uint64_t uuidLow,
                                                    uint64_t uuidHigh) const {
    for (const ReplayAnnotation& entry : entries_) {
        if (entry.sessionUuidLow == uuidLow
            && entry.sessionUuidHigh == uuidHigh) {
            return &entry;
        }
    }
    return nullptr;
}

ReplayAnnotation* ReplayAnnotationStore::mutableEntry(uint64_t uuidLow,
                                                      uint64_t uuidHigh) {
    for (ReplayAnnotation& entry : entries_) {
        if (entry.sessionUuidLow == uuidLow
            && entry.sessionUuidHigh == uuidHigh) {
            return &entry;
        }
    }
    if (entries_.size() >= kMaxEntries) return nullptr;
    ReplayAnnotation entry{};
    entry.sessionUuidLow = uuidLow;
    entry.sessionUuidHigh = uuidHigh;
    entries_.push_back(entry);
    return &entries_.back();
}

bool ReplayAnnotationStore::setFavorite(uint64_t uuidLow, uint64_t uuidHigh,
                                        bool favorite) {
    ReplayAnnotation* entry = mutableEntry(uuidLow, uuidHigh);
    if (entry == nullptr) return false;
    entry->favorite = favorite;
    dirty_ = true;
    return true;
}

bool ReplayAnnotationStore::setTitle(uint64_t uuidLow, uint64_t uuidHigh,
                                     const std::string& title) {
    if (!validTitle(title)) return false;
    ReplayAnnotation* entry = mutableEntry(uuidLow, uuidHigh);
    if (entry == nullptr) return false;
    entry->title = title;
    dirty_ = true;
    return true;
}

bool ReplayAnnotationStore::setNote(uint64_t uuidLow, uint64_t uuidHigh,
                                    const std::string& note) {
    if (!validNote(note)) return false;
    ReplayAnnotation* entry = mutableEntry(uuidLow, uuidHigh);
    if (entry == nullptr) return false;
    entry->note = note;
    dirty_ = true;
    return true;
}

bool ReplayAnnotationStore::setTags(uint64_t uuidLow, uint64_t uuidHigh,
                                    const std::vector<std::string>& tags) {
    if (!validTagSet(tags)) return false;
    ReplayAnnotation* entry = mutableEntry(uuidLow, uuidHigh);
    if (entry == nullptr) return false;
    entry->tags = tags;
    dirty_ = true;
    return true;
}

bool ReplayAnnotationStore::recordStructuralVerification(
    uint64_t uuidLow, uint64_t uuidHigh,
    const std::array<uint8_t, 32>& digest, int64_t unixSeconds) {
    ReplayAnnotation* entry = mutableEntry(uuidLow, uuidHigh);
    if (entry == nullptr) return false;
    entry->structuralVerified = true;
    entry->structuralVerifiedUnix = unixSeconds;
    entry->fileDigest = digest;
    dirty_ = true;
    return true;
}

bool ReplayAnnotationStore::recordSimulationVerification(
    uint64_t uuidLow, uint64_t uuidHigh,
    const std::array<uint8_t, 32>& contentRoot, uint64_t runtimeSimulationId,
    int64_t unixSeconds) {
    ReplayAnnotation* entry = mutableEntry(uuidLow, uuidHigh);
    if (entry == nullptr) return false;
    entry->simulationVerified = true;
    entry->simulationVerifiedUnix = unixSeconds;
    entry->verifiedContentRoot = contentRoot;
    entry->verifiedRuntimeSimulationId = runtimeSimulationId;
    dirty_ = true;
    return true;
}

bool ReplayAnnotationStore::recordProvenance(
    uint64_t uuidLow, uint64_t uuidHigh, uint64_t sourceUuidLow,
    uint64_t sourceUuidHigh, const std::array<uint8_t, 32>& sourceDigest,
    const std::string& sourceFileName) {
    if (sourceFileName.size() > kMaxLabelBytes * 2u
        || !isCanonicalReplayText(sourceFileName)) {
        return false;
    }
    ReplayAnnotation* entry = mutableEntry(uuidLow, uuidHigh);
    if (entry == nullptr) return false;
    entry->hasProvenance = true;
    entry->sourceSessionUuidLow = sourceUuidLow;
    entry->sourceSessionUuidHigh = sourceUuidHigh;
    entry->sourceDigest = sourceDigest;
    entry->sourceFileName = sourceFileName;
    dirty_ = true;
    return true;
}

bool ReplayAnnotationStore::erase(uint64_t uuidLow, uint64_t uuidHigh) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].sessionUuidLow == uuidLow
            && entries_[i].sessionUuidHigh == uuidHigh) {
            entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(i));
            dirty_ = true;
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> ReplayAnnotationStore::encode() const {
    std::vector<uint8_t> body;
    for (const ReplayAnnotation& entry : entries_) {
        putU64(body, entry.sessionUuidLow);
        putU64(body, entry.sessionUuidHigh);
        putBytes(body, entry.fileDigest.data(), entry.fileDigest.size());
        uint8_t flags = 0u;
        if (entry.favorite) flags |= kFlagFavorite;
        if (entry.structuralVerified) flags |= kFlagStructural;
        if (entry.simulationVerified) flags |= kFlagSimulation;
        if (entry.hasProvenance) flags |= kFlagProvenance;
        putU8(body, flags);
        putU8(body, 0u);
        putU8(body, 0u);
        putU8(body, 0u);
        putI64(body, entry.structuralVerifiedUnix);
        putI64(body, entry.simulationVerifiedUnix);
        putBytes(body, entry.verifiedContentRoot.data(),
                 entry.verifiedContentRoot.size());
        putU64(body, entry.verifiedRuntimeSimulationId);
        putU64(body, entry.sourceSessionUuidLow);
        putU64(body, entry.sourceSessionUuidHigh);
        putBytes(body, entry.sourceDigest.data(), entry.sourceDigest.size());
        putString(body, entry.title, kMaxLabelBytes);
        putString(body, entry.note, kMaxNoteBytes);
        putString(body, entry.sourceFileName, kMaxLabelBytes * 2u);
        putU8(body, static_cast<uint8_t>(entry.tags.size()));
        for (const std::string& tag : entry.tags)
            putString(body, tag, kMaxTagBytes);
    }

    std::vector<uint8_t> out;
    putU32(out, kMagic);
    putU32(out, kVersion);
    putU32(out, static_cast<uint32_t>(entries_.size()));
    putU32(out, crc32c(body.data(), body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool ReplayAnnotationStore::decode(const uint8_t* data, size_t len) {
    entries_.clear();
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
    if (magic != kMagic) return false;
    if (version != kVersion) return false;
    if (count > kMaxEntries) return false;
    if (crc32c(data + 16u, len - 16u) != storedCrc) return false;

    std::vector<ReplayAnnotation> loaded;
    loaded.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ReplayAnnotation entry{};
        uint8_t flags = 0u;
        uint8_t reservedA = 0u;
        uint8_t reservedB = 0u;
        uint8_t reservedC = 0u;
        uint8_t tagCount = 0u;
        if (!getU64(data, len, off, entry.sessionUuidLow)
            || !getU64(data, len, off, entry.sessionUuidHigh)
            || !getBytes(data, len, off, entry.fileDigest.data(),
                         entry.fileDigest.size())
            || !getU8(data, len, off, flags)
            || !getU8(data, len, off, reservedA)
            || !getU8(data, len, off, reservedB)
            || !getU8(data, len, off, reservedC)
            || !getI64(data, len, off, entry.structuralVerifiedUnix)
            || !getI64(data, len, off, entry.simulationVerifiedUnix)
            || !getBytes(data, len, off, entry.verifiedContentRoot.data(),
                         entry.verifiedContentRoot.size())
            || !getU64(data, len, off, entry.verifiedRuntimeSimulationId)
            || !getU64(data, len, off, entry.sourceSessionUuidLow)
            || !getU64(data, len, off, entry.sourceSessionUuidHigh)
            || !getBytes(data, len, off, entry.sourceDigest.data(),
                         entry.sourceDigest.size())) {
            entries_.clear();
            return false;
        }
        if ((flags & static_cast<uint8_t>(~kFlagMask)) != 0u || reservedA != 0u
            || reservedB != 0u || reservedC != 0u) {
            entries_.clear();
            return false;
        }
        if (getString(data, len, off, kMaxLabelBytes, entry.title)
                != ParseStatus::Ok
            || getString(data, len, off, kMaxNoteBytes, entry.note)
                != ParseStatus::Ok
            || getString(data, len, off, kMaxLabelBytes * 2u,
                         entry.sourceFileName) != ParseStatus::Ok
            || !getU8(data, len, off, tagCount)) {
            entries_.clear();
            return false;
        }
        if (tagCount > kMaxTags) {
            entries_.clear();
            return false;
        }
        for (uint8_t t = 0; t < tagCount; ++t) {
            std::string tag;
            if (getString(data, len, off, kMaxTagBytes, tag)
                != ParseStatus::Ok) {
                entries_.clear();
                return false;
            }
            entry.tags.push_back(tag);
        }
        entry.favorite = (flags & kFlagFavorite) != 0u;
        entry.structuralVerified = (flags & kFlagStructural) != 0u;
        entry.simulationVerified = (flags & kFlagSimulation) != 0u;
        entry.hasProvenance = (flags & kFlagProvenance) != 0u;
        if (!validTitle(entry.title) || !validNote(entry.note)
            || !validTagSet(entry.tags)) {
            entries_.clear();
            return false;
        }
        loaded.push_back(std::move(entry));
    }
    if (off != len) {
        entries_.clear();
        return false;
    }
    entries_.swap(loaded);
    dirty_ = false;
    return true;
}

bool ReplayAnnotationStore::load(const std::wstring& path) {
    entries_.clear();
    path_ = path;
    dirty_ = false;

    FileByteSource source;
    if (!source.open(path)) return false;
    const uint64_t size = source.size();
    if (size == 0u || size > kMaxFileBytes) return false;
    std::vector<uint8_t> bytes;
    bytes.resize(static_cast<size_t>(size));
    if (!source.read(0u, bytes.data(), bytes.size())) return false;
    return decode(bytes.data(), bytes.size());
}

bool ReplayAnnotationStore::save(const std::wstring& path) const {
    const std::vector<uint8_t> bytes = encode();
    const std::wstring temp = path + L".tmp";
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
    if (!qplat::atomicReplaceFileW(temp, path)) {
        ::DeleteFileW(qplat::longPathW(temp).c_str());
        return false;
    }
    dirty_ = false;
    return true;
}

bool ReplayAnnotationStore::saveIfDirty() {
    if (!dirty_ || path_.empty()) return true;
    return save(path_);
}

}
