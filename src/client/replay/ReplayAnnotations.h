#pragma once

#include "replay/ReplayTypes.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace qclient::replay {

struct ReplayAnnotation {
    uint64_t sessionUuidLow = 0u;
    uint64_t sessionUuidHigh = 0u;
    std::array<uint8_t, 32> fileDigest{};

    bool favorite = false;
    std::string title;
    std::vector<std::string> tags;
    std::string note;

    bool structuralVerified = false;
    int64_t structuralVerifiedUnix = 0;
    bool simulationVerified = false;
    int64_t simulationVerifiedUnix = 0;
    std::array<uint8_t, 32> verifiedContentRoot{};
    uint64_t verifiedRuntimeSimulationId = 0u;

    bool hasProvenance = false;
    uint64_t sourceSessionUuidLow = 0u;
    uint64_t sourceSessionUuidHigh = 0u;
    std::array<uint8_t, 32> sourceDigest{};
    std::string sourceFileName;
};

class ReplayAnnotationStore {
public:
    static constexpr uint32_t kMagic = 0x314E4152u;
    static constexpr uint32_t kVersion = 1u;
    static constexpr size_t kMaxEntries = 100000u;
    static constexpr size_t kMaxFileBytes = 32u * 1024u * 1024u;

    bool load(const std::wstring& path);
    bool save(const std::wstring& path) const;

    const std::wstring& path() const { return path_; }
    bool dirty() const { return dirty_; }
    bool saveIfDirty();

    const ReplayAnnotation* find(uint64_t uuidLow, uint64_t uuidHigh) const;
    ReplayAnnotation* mutableEntry(uint64_t uuidLow, uint64_t uuidHigh);

    bool setFavorite(uint64_t uuidLow, uint64_t uuidHigh, bool favorite);
    bool setTitle(uint64_t uuidLow, uint64_t uuidHigh,
                  const std::string& title);
    bool setNote(uint64_t uuidLow, uint64_t uuidHigh, const std::string& note);
    bool setTags(uint64_t uuidLow, uint64_t uuidHigh,
                 const std::vector<std::string>& tags);
    bool recordStructuralVerification(uint64_t uuidLow, uint64_t uuidHigh,
                                      const std::array<uint8_t, 32>& digest,
                                      int64_t unixSeconds);
    bool recordSimulationVerification(
        uint64_t uuidLow, uint64_t uuidHigh,
        const std::array<uint8_t, 32>& contentRoot,
        uint64_t runtimeSimulationId, int64_t unixSeconds);
    bool recordProvenance(uint64_t uuidLow, uint64_t uuidHigh,
                          uint64_t sourceUuidLow, uint64_t sourceUuidHigh,
                          const std::array<uint8_t, 32>& sourceDigest,
                          const std::string& sourceFileName);
    bool erase(uint64_t uuidLow, uint64_t uuidHigh);

    size_t size() const { return entries_.size(); }
    const std::vector<ReplayAnnotation>& entries() const { return entries_; }
    void clear();

    static bool validTitle(const std::string& text);
    static bool validNote(const std::string& text);
    static bool validTag(const std::string& text);
    static bool validTagSet(const std::vector<std::string>& tags);

    std::vector<uint8_t> encode() const;
    bool decode(const uint8_t* data, size_t len);

private:
    std::vector<ReplayAnnotation> entries_;
    std::wstring path_;
    mutable bool dirty_ = false;
};

}
