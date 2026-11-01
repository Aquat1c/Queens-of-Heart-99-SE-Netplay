#include "ReplayCompatibility.h"

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
#include <cstring>

namespace qclient::replay {

using namespace qreplay;

namespace {

CompatibilityResult make(CompatibilityState state, CompatibilityReason reason,
                         bool watchable, const char* detail) {
    CompatibilityResult result{};
    result.state = state;
    result.reason = reason;
    result.watchable = watchable;
    result.detail = detail != nullptr ? detail : "";
    return result;
}

std::wstring toLowerW(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& ch : out) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + 32);
    }
    return out;
}

std::string toLowerAscii(const std::string& text) {
    std::string out = text;
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
    }
    return out;
}

constexpr uint64_t kUnixEpochInFileTimeTicks = 116444736000000000ull;

int64_t fileTimeToUnixSeconds(const FILETIME& ft) {
    const uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32)
        | static_cast<uint64_t>(ft.dwLowDateTime);
    if (ticks < kUnixEpochInFileTimeTicks) return 0;
    return static_cast<int64_t>((ticks - kUnixEpochInFileTimeTicks)
                                / 10000000ull);
}

}

const CompatibilityAllowlist& CompatibilityAllowlist::builtin() {
    static const CompatibilityAllowlist table;
    return table;
}

const CompatibilityAllowEntry* CompatibilityAllowlist::find(
    const ReplayIdentitySnapshot& file, const RuntimeIdentity& runtime) const {
    for (const CompatibilityAllowEntry& entry : entries_) {
        if (entry.fileRuntimeSimulationId == file.contract.runtimeSimulationId
            && entry.runtimeRuntimeSimulationId == runtime.runtimeSimulationId
            && entry.fileStateRegionFingerprint
                == file.contract.stateRegionFingerprint
            && entry.runtimeStateRegionFingerprint
                == runtime.stateRegionFingerprint
            && entry.fileBootstrapCodecVersion
                == file.contract.bootstrapCodecVersion
            && entry.runtimeBootstrapCodecVersion
                == runtime.bootstrapCodecVersion
            && entry.fileEventSchemaVersion == file.contract.eventSchemaVersion
            && entry.runtimeEventSchemaVersion == runtime.eventSchemaVersion) {
            return &entry;
        }
    }
    return nullptr;
}

CompatibilityResult evaluate(const ReplayIdentitySnapshot& file,
                             const RuntimeIdentity& runtime) {
    return evaluate(file, runtime, CompatibilityAllowlist::builtin());
}

static CompatibilityResult evaluateIdentity(
    const ReplayIdentitySnapshot& file,
    const RuntimeIdentity& runtime,
    const CompatibilityAllowlist& allowlist) {
    if (!file.structurallyValid) {
        return make(CompatibilityState::Corrupt,
                    CompatibilityReason::StructuralCorruption, false,
                    "framing, checksum, or record validation failed");
    }
    if (file.activelyRecording) {
        return make(CompatibilityState::Recording,
                    CompatibilityReason::ActivelyRecording, false,
                    "a recorder is still writing this file");
    }

    if (file.prefix.minimumReaderMajor > runtime.containerReaderMajor) {
        return make(CompatibilityState::Unsupported,
                    CompatibilityReason::ContainerMajorTooNew, false,
                    "the file requires a newer container reader");
    }

    const uint32_t features = file.prefix.featureFlags;
    if ((features & kFeatureFourPlayer) != 0u && !runtime.supportsFourPlayer) {
        return make(CompatibilityState::Unsupported,
                    CompatibilityReason::UnsupportedTeamConfiguration, false,
                    "four-player/team playback is not qualified in this build");
    }
    if (file.activeFighterCount > 2u && !runtime.supportsFourPlayer) {
        return make(CompatibilityState::Unsupported,
                    CompatibilityReason::UnsupportedTeamConfiguration, false,
                    "this set has more than two fighters");
    }
    if (file.usesNonIdentityRoute && !runtime.supportsFourPlayer) {
        return make(CompatibilityState::Unsupported,
                    CompatibilityReason::UnsupportedTeamConfiguration, false,
                    "non-identity slot-to-source routing is not qualified");
    }
    if (((features & kFeatureCpuFighters) != 0u
         || file.cpuContract != CpuContract::None)
        && !runtime.supportsCpuFighters) {
        return make(CompatibilityState::Unsupported,
                    CompatibilityReason::UnsupportedCpuContract, false,
                    "CPU fighter playback is not qualified in this build");
    }
    if (file.inputDomain == InputDomain::EffectiveFighterV1
        && !runtime.supportsEffectiveFighterDomain) {
        return make(CompatibilityState::Unsupported,
                    CompatibilityReason::UnsupportedInputDomain, false,
                    "the effective-fighter input domain is not implemented");
    }

    const bool checkpointProblem = file.hasUnsupportedCheckpoint
        || ((features & kFeaturePortableCheckpoint) != 0u
            && !runtime.supportsPortableCheckpoint);
    bool checkpointLimitation = false;
    if (checkpointProblem) {
        if (!file.checkpointIgnorable) {
            return make(CompatibilityState::Unsupported,
                        CompatibilityReason::UnsupportedCheckpointCodec, false,
                        "an unsupported checkpoint has no compatible fallback");
        }
        checkpointLimitation = true;
    }

    if (!file.hasSessionStart) {
        return make(CompatibilityState::Unsupported,
                    CompatibilityReason::BootstrapSchemaMismatch, false,
                    "no deterministic session start");
    }

    if (!runtime.known) {
        return make(CompatibilityState::FileIntact,
                    CompatibilityReason::None, false,
                    "structure and digest are valid; no installed build to "
                    "compare against");
    }

    struct Check {
        bool matches;
        CompatibilityReason reason;
        CompatibilityState state;
        const char* detail;
    };
    const SimulationContract& c = file.contract;
    const Check checks[] = {
        { c.executableSha256 == runtime.executableSha256,
          CompatibilityReason::ExecutableMismatch,
          CompatibilityState::ContentMismatch,
          "the installed QOH executable differs from the recorded one" },
        { c.contentManifestRoot == runtime.contentManifestRoot,
          CompatibilityReason::ContentManifestMismatch,
          CompatibilityState::ContentMismatch,
          "installed game content differs from the recorded content" },
        { c.characterCatalogId == runtime.characterCatalogId,
          CompatibilityReason::CharacterCatalogMismatch,
          CompatibilityState::ContentMismatch,
          "the roster catalog differs from the recorded one" },
        { c.runtimeSimulationId == runtime.runtimeSimulationId,
          CompatibilityReason::RuntimeSimulationMismatch,
          CompatibilityState::Unsupported,
          "the injected runtime simulation contract differs" },
        { c.stateRegionFingerprint == runtime.stateRegionFingerprint,
          CompatibilityReason::StateRegionMismatch,
          CompatibilityState::Unsupported,
          "the deterministic state region schema differs" },
        { c.stateHashProjectionId == runtime.stateHashProjectionId
              && c.stateHashAlgorithmId == runtime.stateHashAlgorithmId,
          CompatibilityReason::StateHashProjectionMismatch,
          CompatibilityState::Unsupported,
          "the state-hash projection or algorithm differs" },
        { c.bootstrapCodecVersion == runtime.bootstrapCodecVersion,
          CompatibilityReason::BootstrapSchemaMismatch,
          CompatibilityState::Unsupported,
          "the deterministic bootstrap schema differs" },
        { c.eventSchemaVersion == runtime.eventSchemaVersion,
          CompatibilityReason::EventSchemaMismatch,
          CompatibilityState::Unsupported,
          "the lifecycle event schema differs" },
        { c.fpuControlWord == runtime.fpuControlWord && c.mxcsr == runtime.mxcsr,
          CompatibilityReason::StateHashProjectionMismatch,
          CompatibilityState::Unsupported,
          "the floating-point environment contract differs" },
    };

    bool allMatch = true;
    for (const Check& check : checks) {
        if (!check.matches) { allMatch = false; break; }
    }

    if (!allMatch) {
        const CompatibilityAllowEntry* entry = allowlist.find(file, runtime);
        const bool contentEqual =
            c.executableSha256 == runtime.executableSha256
            && c.contentManifestRoot == runtime.contentManifestRoot
            && c.characterCatalogId == runtime.characterCatalogId
            && c.stateHashProjectionId == runtime.stateHashProjectionId
            && c.stateHashAlgorithmId == runtime.stateHashAlgorithmId
            && c.fpuControlWord == runtime.fpuControlWord
            && c.mxcsr == runtime.mxcsr;
        if (entry != nullptr && contentEqual) {
            std::string detail = "tested compatibility rule";
            if (entry->rationale != nullptr && entry->rationale[0] != '\0') {
                detail += ": ";
                detail += entry->rationale;
            }
            if (checkpointLimitation)
                detail += "; an unsupported checkpoint will be skipped";
            CompatibilityResult result =
                make(CompatibilityState::Compatible, CompatibilityReason::None,
                     true, "");
            result.detail = detail;
            return result;
        }
        for (const Check& check : checks) {
            if (check.matches) continue;
            return make(check.state, check.reason, false, check.detail);
        }
    }

    if (file.simulationVerified) {
        return make(CompatibilityState::SimulationVerified,
                    CompatibilityReason::None, true,
                    "a matching build completed deterministic verification");
    }
    if (checkpointLimitation) {
        return make(CompatibilityState::Compatible, CompatibilityReason::None,
                    true,
                    "identities match; an unsupported checkpoint will be "
                    "skipped and playback restarts from a compatible bootstrap");
    }
    return make(CompatibilityState::Exact, CompatibilityReason::None, true,
                "every required identity matches");
}

CompatibilityResult evaluate(const ReplayIdentitySnapshot& file,
                             const RuntimeIdentity& runtime,
                             const CompatibilityAllowlist& allowlist) {
    CompatibilityResult result = evaluateIdentity(file, runtime, allowlist);
    if (file.hasFooter) return result;

    if (!result.watchable) return result;

    CompatibilityResult incomplete = result;
    incomplete.state = CompatibilityState::Incomplete;
    incomplete.reason = CompatibilityReason::MissingFooter;
    incomplete.watchable = false;
    incomplete.detail =
        "the recording is incomplete and has no finalized footer proof";
    return incomplete;
}

bool ContentManifestPolicy::excludes(const std::wstring& relativePath,
                                     const std::wstring& fileName) const {
    const std::wstring lowerName = toLowerW(fileName);
    for (const std::wstring& name : excludedNames) {
        if (toLowerW(name) == lowerName) return true;
    }
    const size_t dot = lowerName.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        const std::wstring extension = lowerName.substr(dot);
        for (const std::wstring& excluded : excludedExtensions) {
            if (toLowerW(excluded) == extension) return true;
        }
    }
    const std::wstring lowerPath = toLowerW(relativePath);
    size_t start = 0u;
    while (start < lowerPath.size()) {
        size_t end = lowerPath.find_first_of(L"\\/", start);
        if (end == std::wstring::npos) end = lowerPath.size();
        const std::wstring component = lowerPath.substr(start, end - start);
        for (const std::wstring& directory : excludedDirectories) {
            if (toLowerW(directory) == component) return true;
        }
        start = end + 1u;
    }
    return false;
}

bool ContentHashCache::lookup(const Key& key,
                              std::array<uint8_t, 32>& out) const {
    for (const Entry& entry : entries_) {
        if (entry.key == key) {
            out = entry.digest;
            return true;
        }
    }
    return false;
}

void ContentHashCache::store(const Key& key,
                             const std::array<uint8_t, 32>& digest) {
    for (Entry& entry : entries_) {
        if (entry.key == key) {
            entry.digest = digest;
            return;
        }
    }
    if (entries_.size() >= kMaxEntries) entries_.erase(entries_.begin());
    Entry entry{};
    entry.key = key;
    entry.digest = digest;
    entries_.push_back(entry);
}

void ContentHashCache::clear() { entries_.clear(); }

bool ContentHashCache::load(const std::wstring& path) {
    entries_.clear();
    FileByteSource source;
    if (!source.open(path)) return false;
    const uint64_t size = source.size();
    if (size < 16u || size > 16u * 1024u * 1024u) return false;
    std::vector<uint8_t> bytes;
    bytes.resize(static_cast<size_t>(size));
    if (!source.read(0u, bytes.data(), bytes.size())) return false;

    size_t off = 0u;
    uint32_t magic = 0u;
    uint32_t version = 0u;
    uint32_t count = 0u;
    uint32_t storedCrc = 0u;
    if (!getU32(bytes.data(), bytes.size(), off, magic)
        || !getU32(bytes.data(), bytes.size(), off, version)
        || !getU32(bytes.data(), bytes.size(), off, count)
        || !getU32(bytes.data(), bytes.size(), off, storedCrc)) {
        return false;
    }
    if (magic != kMagic || version != kVersion || count > kMaxEntries)
        return false;
    const size_t entryBytes = 8u + 8u + 8u + 8u + 32u;
    size_t expected = 0u;
    if (!mulChecked(count, entryBytes, expected)
        || !addChecked(expected, 16u, expected) || expected != bytes.size()) {
        return false;
    }
    if (crc32c(bytes.data() + 16u, bytes.size() - 16u) != storedCrc)
        return false;

    for (uint32_t i = 0; i < count; ++i) {
        Entry entry{};
        uint64_t modified = 0u;
        if (!getU64(bytes.data(), bytes.size(), off, entry.key.volumeId)
            || !getU64(bytes.data(), bytes.size(), off, entry.key.fileId)
            || !getU64(bytes.data(), bytes.size(), off, entry.key.sizeBytes)
            || !getU64(bytes.data(), bytes.size(), off, modified)
            || !getBytes(bytes.data(), bytes.size(), off, entry.digest.data(),
                         entry.digest.size())) {
            entries_.clear();
            return false;
        }
        entry.key.modifiedUnixSeconds = static_cast<int64_t>(modified);
        entries_.push_back(entry);
    }
    return true;
}

bool ContentHashCache::save(const std::wstring& path) const {
    std::vector<uint8_t> body;
    for (const Entry& entry : entries_) {
        putU64(body, entry.key.volumeId);
        putU64(body, entry.key.fileId);
        putU64(body, entry.key.sizeBytes);
        putU64(body, static_cast<uint64_t>(entry.key.modifiedUnixSeconds));
        putBytes(body, entry.digest.data(), entry.digest.size());
    }
    std::vector<uint8_t> bytes;
    putU32(bytes, kMagic);
    putU32(bytes, kVersion);
    putU32(bytes, static_cast<uint32_t>(entries_.size()));
    putU32(bytes, crc32c(body.data(), body.size()));
    bytes.insert(bytes.end(), body.begin(), body.end());

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
    return qplat::atomicReplaceFileW(temp, path);
}

std::array<uint8_t, 32> ContentManifestBuilder::computeRoot(
    std::vector<ContentFileEntry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const ContentFileEntry& a, const ContentFileEntry& b) {
                  return a.relativePath < b.relativePath;
              });
    Sha256 sha;
    static const char kDomain[] = "q99r-content-manifest-v1";
    sha.update(reinterpret_cast<const uint8_t*>(kDomain), sizeof(kDomain) - 1u);
    for (const ContentFileEntry& entry : entries) {
        sha.update(reinterpret_cast<const uint8_t*>(entry.relativePath.data()),
                   entry.relativePath.size());
        const uint8_t separator = 0x1Fu;
        sha.update(&separator, 1u);
        uint8_t size[8];
        for (unsigned i = 0; i < 8u; ++i)
            size[i] = static_cast<uint8_t>(entry.sizeBytes >> (i * 8u));
        sha.update(size, 8u);
        sha.update(entry.sha256.data(), entry.sha256.size());
        const uint8_t record = 0x1Eu;
        sha.update(&record, 1u);
    }
    std::array<uint8_t, 32> root{};
    sha.finish(root);
    return root;
}

namespace {

bool hashOneFile(const std::wstring& path, std::array<uint8_t, 32>& out,
                 uint64_t& sizeOut, ContentHashCache::Key& keyOut) {
    FileByteSource source;
    if (!source.open(path)) return false;
    keyOut.volumeId = source.identity().volumeId;
    keyOut.fileId = source.identity().fileId;
    keyOut.sizeBytes = source.identity().sizeBytes;
    keyOut.modifiedUnixSeconds = source.identity().modifiedUnixSeconds;
    sizeOut = source.size();

    Sha256 sha;
    std::vector<uint8_t> buffer;
    buffer.resize(256u * 1024u);
    uint64_t done = 0u;
    while (done < sizeOut) {
        const size_t take = static_cast<size_t>(
            std::min<uint64_t>(buffer.size(), sizeOut - done));
        if (!source.read(done, buffer.data(), take)) return false;
        sha.update(buffer.data(), take);
        done += static_cast<uint64_t>(take);
    }
    sha.finish(out);
    return true;
}

struct BuildContext {
    const ContentManifestPolicy* policy = nullptr;
    ContentHashCache* cache = nullptr;
    const volatile bool* cancelled = nullptr;
    ContentManifest* manifest = nullptr;
    uint64_t totalBytes = 0u;
};

bool walkDirectory(BuildContext& ctx, const std::wstring& root,
                   const std::wstring& relative, uint32_t depth) {
    if (depth > ctx.policy->maxDepth) return true;
    if (ctx.cancelled != nullptr && *ctx.cancelled) return false;

    const std::wstring directory =
        relative.empty() ? root : (root + L"\\" + relative);
    WIN32_FIND_DATAW found{};
    HANDLE handle = ::FindFirstFileW(
        qplat::longPathW(directory + L"\\*").c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE) return true;

    bool ok = true;
    do {
        if (ctx.cancelled != nullptr && *ctx.cancelled) { ok = false; break; }
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
            continue;

        const std::wstring childRelative =
            relative.empty() ? name : (relative + L"\\" + name);
        if (ctx.policy->excludes(childRelative, name)) continue;

        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
            if (!walkDirectory(ctx, root, childRelative, depth + 1u)) {
                ok = false;
                break;
            }
            continue;
        }
        if (ctx.manifest->entries.size() >= ctx.policy->maxFiles) {
            ctx.manifest->failureDetail = "content file count limit reached";
            ok = false;
            break;
        }

        ContentFileEntry entry{};
        std::string canonical;
        canonical.reserve(childRelative.size());
        {
            const std::string narrow = qplat::narrow(childRelative);
            canonical = toLowerAscii(narrow);
            for (char& ch : canonical) {
                if (ch == '\\') ch = '/';
            }
        }
        entry.relativePath = canonical;

        const std::wstring fullPath = root + L"\\" + childRelative;
        ContentHashCache::Key key{};
        key.volumeId = 0u;
        FileIdentity identity{};
        if (queryFileIdentity(fullPath, identity)) {
            key.volumeId = identity.volumeId;
            key.fileId = identity.fileId;
            key.sizeBytes = identity.sizeBytes;
            key.modifiedUnixSeconds = identity.modifiedUnixSeconds;
        }
        bool haveDigest = false;
        if (ctx.cache != nullptr && identity.known
            && ctx.cache->lookup(key, entry.sha256)) {
            entry.sizeBytes = identity.sizeBytes;
            entry.fromCache = true;
            haveDigest = true;
            ++ctx.manifest->filesFromCache;
        }
        if (!haveDigest) {
            ContentHashCache::Key hashedKey{};
            uint64_t size = 0u;
            if (!hashOneFile(fullPath, entry.sha256, size, hashedKey)) {
                ctx.manifest->failureDetail = "could not read a content file";
                ok = false;
                break;
            }
            entry.sizeBytes = size;
            ctx.manifest->bytesHashed += size;
            if (ctx.cache != nullptr && hashedKey.volumeId != 0u)
                ctx.cache->store(hashedKey, entry.sha256);
        }
        ctx.totalBytes += entry.sizeBytes;
        if (ctx.totalBytes > ctx.policy->maxTotalBytes) {
            ctx.manifest->failureDetail = "content byte budget reached";
            ok = false;
            break;
        }
        ++ctx.manifest->filesHashed;
        ctx.manifest->entries.push_back(std::move(entry));
    } while (::FindNextFileW(handle, &found) != 0);

    ::FindClose(handle);
    return ok;
}

}

ContentManifest ContentManifestBuilder::build(
    const std::wstring& gameDirectory, const ContentManifestPolicy& policy,
    ContentHashCache* cache, const volatile bool* cancelled) {
    ContentManifest manifest{};
    std::wstring root = qplat::absolutePathW(gameDirectory);
    if (root.empty()) root = gameDirectory;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();
    if (root.empty() || !qplat::directoryExistsW(root)) {
        manifest.failureDetail = "game directory not found";
        return manifest;
    }

    BuildContext ctx{};
    ctx.policy = &policy;
    ctx.cache = cache;
    ctx.cancelled = cancelled;
    ctx.manifest = &manifest;

    const bool ok = walkDirectory(ctx, root, std::wstring(), 0u);
    manifest.root = computeRoot(manifest.entries);
    manifest.complete = ok && manifest.failureDetail.empty();
    return manifest;
}

}
