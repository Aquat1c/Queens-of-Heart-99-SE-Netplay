#pragma once

#include "replay/ReplayTypes.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace qclient::replay {

struct RuntimeIdentity {
    bool known = false;

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

    bool supportsFourPlayer = false;
    bool supportsCpuFighters = false;
    bool supportsPortableCheckpoint = false;
    bool supportsEffectiveFighterDomain = false;

    uint16_t containerReaderMajor = qreplay::kContainerReaderMajor;
};

struct ReplayIdentitySnapshot {
    qreplay::ReplayPrefix prefix{};
    qreplay::SimulationContract contract{};

    bool structurallyValid = false;
    bool hasFooter = false;
    bool hasSessionStart = false;
    bool digestVerified = false;

    bool activelyRecording = false;
    bool recoverable = false;
    bool simulationVerified = false;

    uint8_t activeFighterCount = 2u;
    qreplay::CpuContract cpuContract = qreplay::CpuContract::None;
    qreplay::InputDomain inputDomain = qreplay::InputDomain::LogicalSourceV1;
    bool usesNonIdentityRoute = false;
    bool hasUnsupportedCheckpoint = false;
    bool checkpointIgnorable = false;
};

struct CompatibilityAllowEntry {
    uint64_t fileRuntimeSimulationId = 0u;
    uint64_t runtimeRuntimeSimulationId = 0u;
    uint64_t fileStateRegionFingerprint = 0u;
    uint64_t runtimeStateRegionFingerprint = 0u;
    uint64_t fileBootstrapCodecVersion = 0u;
    uint64_t runtimeBootstrapCodecVersion = 0u;
    uint64_t fileEventSchemaVersion = 0u;
    uint64_t runtimeEventSchemaVersion = 0u;
    const char* rationale = "";
};

class CompatibilityAllowlist {
public:
    static const CompatibilityAllowlist& builtin();

    CompatibilityAllowlist() = default;
    explicit CompatibilityAllowlist(std::vector<CompatibilityAllowEntry> entries)
        : entries_(std::move(entries)) {}

    const CompatibilityAllowEntry* find(const ReplayIdentitySnapshot& file,
                                        const RuntimeIdentity& runtime) const;
    size_t size() const { return entries_.size(); }

private:
    std::vector<CompatibilityAllowEntry> entries_;
};

qreplay::CompatibilityResult evaluate(const ReplayIdentitySnapshot& file,
                                      const RuntimeIdentity& runtime);

qreplay::CompatibilityResult evaluate(const ReplayIdentitySnapshot& file,
                                      const RuntimeIdentity& runtime,
                                      const CompatibilityAllowlist& allowlist);

struct ContentManifestPolicy {
    std::vector<std::wstring> excludedDirectories = {
        L"logs", L"replays", L"profiles", L"screenshots", L".trash", L".cache",
        L"build", L"dist", L"docs", L"licenses",
    };
    std::vector<std::wstring> excludedExtensions = {
        L".log", L".q99r", L".part", L".tmp", L".bak", L".dmp", L".pdb",
        L".zip", L".lnk",
    };
    std::vector<std::wstring> excludedNames = {
        L"qoh99_netplay.dll", L"qoh99caster.exe", L"ddraw.dll", L"sdl3.dll",
        L"qoh99extra.exe", L"qoh99_netplay.ini", L"qoh99_netplay_input.json",
        L"qohcnf.key", L"ddraw_wrapper.ini",
        L"qoh99caster.ini", L"qoh99caster-controls.json", L"manifest.sha256",
        L"launch_wine.sh", L"readme.md", L"readme.txt",
        L"ddraw_wrapper.ini.bak-visible",
        L"qoh99relay.exe",
        L"qoh99relay.exe.retired-superseded-by-caster-relay-verb",
    };
    uint32_t maxDepth = 6u;
    uint32_t maxFiles = 20000u;
    uint64_t maxTotalBytes = 4ull * 1024ull * 1024ull * 1024ull;

    bool excludes(const std::wstring& relativePath,
                  const std::wstring& fileName) const;
};

struct ContentFileEntry {
    std::string relativePath;
    uint64_t sizeBytes = 0u;
    std::array<uint8_t, 32> sha256{};
    bool fromCache = false;
};

struct ContentManifest {
    bool complete = false;
    std::array<uint8_t, 32> root{};
    std::vector<ContentFileEntry> entries;
    uint32_t filesHashed = 0u;
    uint32_t filesFromCache = 0u;
    uint64_t bytesHashed = 0u;
    std::string failureDetail;
};

class ContentHashCache {
public:
    struct Key {
        uint64_t volumeId = 0u;
        uint64_t fileId = 0u;
        uint64_t sizeBytes = 0u;
        int64_t modifiedUnixSeconds = 0;
        bool operator==(const Key& other) const {
            return volumeId == other.volumeId && fileId == other.fileId
                && sizeBytes == other.sizeBytes
                && modifiedUnixSeconds == other.modifiedUnixSeconds;
        }
    };

    bool lookup(const Key& key, std::array<uint8_t, 32>& out) const;
    void store(const Key& key, const std::array<uint8_t, 32>& digest);
    void clear();
    size_t size() const { return entries_.size(); }

    bool load(const std::wstring& path);
    bool save(const std::wstring& path) const;

    // `.cache` must stay in ContentManifestPolicy::excludedDirectories, or every launch changes the content root.
    static std::wstring pathForGameDirectory(const std::wstring& gameRoot) {
        return gameRoot + L"\\.cache\\qoh99-content-hashes.bin";
    }

    static constexpr uint32_t kMagic = 0x43485143u;
    static constexpr uint32_t kVersion = 1u;
    static constexpr size_t kMaxEntries = 65536u;

private:
    struct Entry {
        Key key;
        std::array<uint8_t, 32> digest{};
    };
    std::vector<Entry> entries_;
};

class ContentManifestBuilder {
public:
    static ContentManifest build(const std::wstring& gameDirectory,
                                 const ContentManifestPolicy& policy,
                                 ContentHashCache* cache,
                                 const volatile bool* cancelled = nullptr);

    static std::array<uint8_t, 32> computeRoot(
        std::vector<ContentFileEntry>& entries);
};

}
