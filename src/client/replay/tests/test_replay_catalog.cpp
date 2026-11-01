#include "TestHarness.h"

#include "ReplayTestFixtures.h"

#include "client/replay/ReplayAnnotations.h"
#include "client/replay/ReplayCatalog.h"
#include "client/replay/ReplayFolderPlayback.h"
#include "client/replay/ReplayPlaybackOutcome.h"
#include "platform/WidePath.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

using namespace qreplay;
using namespace qclient::replay;
using namespace qclient::replay::test;

namespace {

ReplayScanner::Config configFor(const std::wstring& root) {
    ReplayScanner::Config config{};
    config.roots.push_back(root);
    config.useCache = false;
    config.pollIntervalMs = 0u;
    return config;
}

bool writeFixture(const std::wstring& path, const FixtureOptions& options) {
    const Fixture fixture = makeFixture(options);
    return fixture.ok && writeBytes(path, fixture.bytes);
}

}

QNP_TEST(replay_catalog_scans_an_empty_library) {
    TempDir dir(L"cat_empty");
    QNP_REQUIRE(dir.valid());
    const ReplaySnapshot snapshot =
        ReplayScanner::scanOnce(configFor(dir.path), nullptr, nullptr, 1u);
    QNP_CHECK(snapshot.complete);
    QNP_CHECK(snapshot.entries.empty());
    QNP_CHECK(snapshot.filesSeen == 0u);
    QNP_CHECK(snapshot.rootWritable);
}

QNP_TEST(replay_catalog_publishes_the_newest_page_before_the_rest) {
    TempDir dir(L"cat_paged");
    QNP_REQUIRE(dir.valid());

    std::vector<std::wstring> paths;
    for (unsigned i = 0; i < 12u; ++i) {
        FixtureOptions options{};
        options.uuidLow = 100u + i;
        options.title = "set " + std::to_string(i);
        const std::wstring path =
            dir.file((L"r" + std::to_wstring(i) + L".q99r").c_str());
        QNP_REQUIRE(writeFixture(path, options));
        paths.push_back(path);
    }
    for (unsigned i = 0; i < paths.size(); ++i) {
        HANDLE handle = ::CreateFileW(paths[i].c_str(), FILE_WRITE_ATTRIBUTES,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
        QNP_REQUIRE(handle != INVALID_HANDLE_VALUE);
        ULARGE_INTEGER stamp{};
        stamp.QuadPart = 132000000000000000ull
            + static_cast<unsigned long long>(i) * 100000000ull;
        FILETIME when{};
        when.dwLowDateTime = stamp.LowPart;
        when.dwHighDateTime = stamp.HighPart;
        QNP_CHECK(::SetFileTime(handle, nullptr, nullptr, &when) != 0);
        ::CloseHandle(handle);
    }

    ReplayScanner::Config config = configFor(dir.path);
    config.loadChunkFiles = 4u;
    std::vector<ReplaySnapshot> partials;
    const ReplaySnapshot finished = ReplayScanner::scanOnce(
        config, nullptr, nullptr, 1u,
        [&partials](ReplaySnapshot partial) {
            partials.push_back(std::move(partial));
        });

    QNP_CHECK(finished.complete);
    QNP_CHECK(finished.entries.size() == 12u);
    QNP_CHECK(finished.filesSeen == 12u);
    QNP_CHECK(finished.filesBeyondLimit == 0u);

    QNP_REQUIRE(partials.size() >= 2u);
    QNP_CHECK(partials.front().entries.size() == 4u);
    for (const ReplaySnapshot& partial : partials)
        QNP_CHECK(!partial.complete);

    for (size_t i = 0; i < finished.entries.size(); ++i) {
        const std::wstring expected =
            L"r" + std::to_wstring(11u - i) + L".q99r";
        QNP_CHECK(finished.entries[i].fileName == expected);
    }

    for (const ReplaySnapshot& partial : partials) {
        QNP_REQUIRE(partial.entries.size() <= finished.entries.size());
        for (size_t i = 0; i < partial.entries.size(); ++i) {
            QNP_CHECK(partial.entries[i].path == finished.entries[i].path);
        }
    }
}

QNP_TEST(replay_catalog_reports_what_a_load_ceiling_left_out) {
    TempDir dir(L"cat_ceiling");
    QNP_REQUIRE(dir.valid());
    for (unsigned i = 0; i < 6u; ++i) {
        FixtureOptions options{};
        options.uuidLow = 200u + i;
        QNP_REQUIRE(writeFixture(
            dir.file((L"c" + std::to_wstring(i) + L".q99r").c_str()), options));
    }
    ReplayScanner::Config config = configFor(dir.path);
    config.maxLoadedFiles = 2u;
    const ReplaySnapshot snapshot =
        ReplayScanner::scanOnce(config, nullptr, nullptr, 1u);
    QNP_CHECK(snapshot.complete);
    QNP_CHECK(snapshot.entries.size() == 2u);
    QNP_CHECK(snapshot.filesSeen == 6u);
    QNP_CHECK(snapshot.filesBeyondLimit == 4u);
}

QNP_TEST(replay_catalog_scans_a_mixed_library) {
    TempDir dir(L"cat_mixed");
    QNP_REQUIRE(dir.valid());

    FixtureOptions good{};
    good.uuidLow = 1u;
    good.title = "Good set";
    QNP_REQUIRE(writeFixture(dir.file(L"good.q99r"), good));

    FixtureOptions part{};
    part.uuidLow = 2u;
    part.finalize = false;
    part.title = "Interrupted";
    QNP_REQUIRE(writeFixture(dir.file(L"stale.q99r.part"), part));

    FixtureOptions broken{};
    broken.uuidLow = 3u;
    Fixture brokenFixture = makeFixture(broken);
    QNP_REQUIRE(brokenFixture.ok);
    for (size_t i = kPrefixBytes + 8u; i < brokenFixture.bytes.size(); i += 7u)
        brokenFixture.bytes[i] ^= 0xA5u;
    QNP_REQUIRE(writeBytes(dir.file(L"broken.q99r"), brokenFixture.bytes));

    QNP_REQUIRE(writeBytes(dir.file(L"notes.txt"),
                           std::vector<uint8_t>(16u, 0x41u)));
    QNP_REQUIRE(writeBytes(dir.file(L"junk.q99r"),
                           std::vector<uint8_t>(200u, 0x00u)));

    const ReplaySnapshot snapshot =
        ReplayScanner::scanOnce(configFor(dir.path), nullptr, nullptr, 7u);
    QNP_CHECK(snapshot.complete);
    QNP_CHECK(snapshot.generation == 7u);
    QNP_CHECK(snapshot.entries.size() == 3u);
    QNP_CHECK(snapshot.filesRejected >= 2u);

    uint32_t parsed = 0u;
    for (const ReplayEntry& entry : snapshot.entries) {
        if (entry.parsed) ++parsed;
    }
    QNP_CHECK(parsed == 1u);

    const ReplaySnapshot again =
        ReplayScanner::scanOnce(configFor(dir.path), nullptr, nullptr, 8u);
    QNP_REQUIRE(again.entries.size() == snapshot.entries.size());
    for (size_t i = 0; i < again.entries.size(); ++i)
        QNP_CHECK(again.entries[i].stableId == snapshot.entries[i].stableId);
}

QNP_TEST(replay_catalog_ignores_all_legacy_part_artifacts) {
    TempDir dir(L"cat_part");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    options.finalize = false;
    QNP_REQUIRE(writeFixture(dir.file(L"stale.q99r.part"), options));

    options.uuidLow = 99u;
    const Fixture live = makeFixture(options);
    QNP_REQUIRE(live.ok);
    const std::wstring livePath = dir.file(L"live.q99r.part");
    HANDLE handle = ::CreateFileW(livePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    QNP_REQUIRE(handle != INVALID_HANDLE_VALUE);
    DWORD wrote = 0u;
    ::WriteFile(handle, live.bytes.data(),
                static_cast<DWORD>(live.bytes.size()), &wrote, nullptr);
    ::FlushFileBuffers(handle);

    const ReplaySnapshot snapshot =
        ReplayScanner::scanOnce(configFor(dir.path), nullptr, nullptr, 1u);
    ::CloseHandle(handle);

    QNP_CHECK(snapshot.entries.empty());
}

QNP_TEST(replay_catalog_ignores_reparse_points_and_foreign_extensions) {
    TempDir dir(L"cat_reparse");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    QNP_REQUIRE(writeFixture(dir.file(L"real.q99r"), options));
    QNP_REQUIRE(::CreateDirectoryW((dir.path + L"\\folder.q99r").c_str(),
                                   nullptr) != 0);
    QNP_REQUIRE(writeFixture(dir.file(L"--force=..\\..\\x.q99r"), options));

    const ReplaySnapshot snapshot =
        ReplayScanner::scanOnce(configFor(dir.path), nullptr, nullptr, 1u);
    for (const ReplayEntry& entry : snapshot.entries) {
        QNP_CHECK(entry.fileName != L"folder.q99r");
        QNP_CHECK(entry.path.compare(0, dir.path.size(), dir.path) == 0);
    }
}

QNP_TEST(replay_catalog_scan_is_cancellable) {
    TempDir dir(L"cat_cancel");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    for (int i = 0; i < 12; ++i) {
        wchar_t name[32];
        _snwprintf_s(name, 32, _TRUNCATE, L"r%02d.q99r", i);
        options.uuidLow = static_cast<uint64_t>(i + 1);
        QNP_REQUIRE(writeFixture(dir.file(name), options));
    }
    std::atomic<bool> cancelled{ true };
    const ReplaySnapshot snapshot =
        ReplayScanner::scanOnce(configFor(dir.path), nullptr, &cancelled, 1u);
    QNP_CHECK(snapshot.cancelled);
    QNP_CHECK(!snapshot.complete);
}

QNP_TEST(replay_catalog_worker_publishes_a_snapshot_without_blocking) {
    TempDir dir(L"cat_worker");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    for (int i = 0; i < 6; ++i) {
        wchar_t name[32];
        _snwprintf_s(name, 32, _TRUNCATE, L"w%02d.q99r", i);
        options.uuidLow = static_cast<uint64_t>(100 + i);
        QNP_REQUIRE(writeFixture(dir.file(name), options));
    }

    ReplayScanner scanner;
    ReplayScanner::Config config = configFor(dir.path);
    config.pollIntervalMs = 0u;
    scanner.start(config);
    std::shared_ptr<const ReplaySnapshot> snapshot;
    for (int spin = 0; spin < 400; ++spin) {
        snapshot = scanner.snapshot();
        if (snapshot && snapshot->complete && snapshot->entries.size() == 6u)
            break;
        ::Sleep(10);
    }
    scanner.stop();
    QNP_REQUIRE(snapshot != nullptr);
    QNP_CHECK(snapshot->entries.size() == 6u);
    QNP_CHECK(snapshot->generation >= 1u);
}

QNP_TEST(replay_catalog_refresh_keeps_the_complete_list_and_coalesces_requests) {
    TempDir dir(L"cat_refresh");
    QNP_REQUIRE(dir.valid());
    for (unsigned i = 0u; i < 128u; ++i) {
        FixtureOptions options{};
        options.uuidLow = i + 1u;
        QNP_REQUIRE(writeFixture(
            dir.file((L"r" + std::to_wstring(i) + L".q99r").c_str()),
            options));
    }
    ReplayScanner scanner;
    ReplayScanner::Config config = configFor(dir.path);
    config.loadChunkFiles = 1u;
    scanner.start(config);
    auto initial = scanner.snapshot();
    const uint64_t initialDeadline = ::GetTickCount64() + 10000u;
    while ((!initial || !initial->complete)
           && ::GetTickCount64() < initialDeadline) {
        ::Sleep(1);
        initial = scanner.snapshot();
    }
    QNP_REQUIRE(initial && initial->complete);
    QNP_REQUIRE(initial->entries.size() == 128u);

    scanner.requestRefresh();
    bool sawScanning = false;
    bool droppedEntries = false;
    bool repeatedRefresh = false;
    auto refreshed = initial;
    const uint64_t refreshDeadline = ::GetTickCount64() + 10000u;
    while (::GetTickCount64() < refreshDeadline) {
        if (scanner.scanning()) {
            sawScanning = true;
            if (!repeatedRefresh) {
                scanner.requestRefresh();
                scanner.requestRefresh();
                repeatedRefresh = true;
            }
        }
        refreshed = scanner.snapshot();
        if (refreshed && (!refreshed->complete
                          || refreshed->entries.size() != 128u))
            droppedEntries = true;
        if (refreshed && refreshed->complete
            && refreshed->generation != initial->generation)
            break;
        ::Sleep(0);
    }
    QNP_REQUIRE(refreshed && refreshed->complete);
    QNP_CHECK(sawScanning);
    QNP_CHECK(!droppedEntries);
    QNP_CHECK(refreshed->generation != initial->generation);
    ::Sleep(150);
    QNP_CHECK(scanner.snapshot()->generation == refreshed->generation);
    QNP_CHECK(!scanner.scanning());
}

QNP_TEST(replay_folder_playback_uses_the_whole_directory_in_recorded_game_order) {
    ReplaySnapshot snapshot{};
    snapshot.complete = true;
    auto add = [&](const wchar_t* name, const wchar_t* directory,
                   int64_t time, uint32_t ordinal, uint64_t series) {
        ReplayEntry entry{};
        entry.path = name;
        entry.relativeDir = directory;
        entry.isV2 = true;
        entry.startV2.manifest.display.recordedUnixSeconds = time;
        entry.startV2.manifest.display.flags = DisplayMetadataSeriesV2;
        entry.startV2.manifest.display.seriesUuidLow = series;
        entry.startV2.manifest.display.gameOrdinalInSeries = ordinal;
        snapshot.entries.push_back(entry);
    };
    add(L"a.q99r", L"set", 10, 3, 1);
    add(L"b.q99r", L"set", 11, 0, 1);
    add(L"c.q99r", L"set", 12, 2, 1);
    add(L"nested.q99r", L"set\\child", 1, 1, 1);
    add(L"sibling.q99r", L"set2", 1, 1, 1);
    add(L"trash.q99r", L"set", 1, 1, 1);
    snapshot.entries.back().inTrash = true;
    add(L"partial.q99r.part", L"set", 1, 1, 1);
    snapshot.entries.back().isPart = true;
    add(L"next-set.q99r", L"set", 20, 1, 2);
    QNP_CHECK(folderPlaybackOrder(snapshot, L"set")
              == std::vector<size_t>({ 1u, 2u, 0u, 7u }));
    QNP_CHECK(folderPlaybackOrder(snapshot, L"empty").empty());
    snapshot.complete = false;
    QNP_CHECK(folderPlaybackOrder(snapshot, L"set").empty());
}

QNP_TEST(replay_folder_playback_continues_after_played_through_divergence) {
    using namespace qreplay::ipc::v3;
    PlaybackResultPayload result{};
    result.hasRecordedTerminal = true;
    result.recordedTerminal = static_cast<uint16_t>(TerminalReason::Completed);
    result.reason = PlaybackResultReason::VerifiedCompleted;
    QNP_CHECK(folderPlaybackCompleted(0, true, result));
    result.reason = PlaybackResultReason::VerifiedBehaviourOnly;
    QNP_CHECK(folderPlaybackCompleted(0, true, result));
    result.reason = PlaybackResultReason::CompletedWithDivergence;
    QNP_CHECK(folderPlaybackCompleted(0, true, result));
    QNP_CHECK(!folderPlaybackCompleted(1, true, result));
    QNP_CHECK(!folderPlaybackCompleted(0, false, result));
    result.hasRecordedTerminal = false;
    QNP_CHECK(!folderPlaybackCompleted(0, true, result));
    result.hasRecordedTerminal = true;
    result.recordedTerminal = static_cast<uint16_t>(TerminalReason::Disconnected);
    QNP_CHECK(!folderPlaybackCompleted(0, true, result));
    result.recordedTerminal = static_cast<uint16_t>(TerminalReason::Completed);
    for (PlaybackResultReason reason : {
             PlaybackResultReason::UserExit,
             PlaybackResultReason::VerifiedAbnormalEnd,
             PlaybackResultReason::VerifiedUnsealedPrefix,
             PlaybackResultReason::InternalFailure }) {
        result.reason = reason;
        QNP_CHECK(!folderPlaybackCompleted(0, true, result));
    }
}

QNP_TEST(replay_viewer_native_close_is_neither_failure_nor_verified_completion) {
    ReplayBrokerRunResultV2 result{};
    for (auto state : {ReplayBrokerStateV2::MissingResult,
                       ReplayBrokerStateV2::ProcessExitedWithoutResult}) {
        result.state = state;
        QNP_CHECK(replayClosedBeforeResultV2(true, true, result));
        QNP_CHECK(!replayClosedBeforeResultV2(false, true, result));
        QNP_CHECK(!replayClosedBeforeResultV2(true, false, result));
        result.hasPlaybackResult = true;
        result.playbackResult.reason = qreplay::ipc::v3::PlaybackResultReason::InternalFailure;
        QNP_CHECK(!replayClosedBeforeResultV2(true, true, result));
        result.hasPlaybackResult = false;
    }
    for (auto state : {ReplayBrokerStateV2::ProtocolFailure,
                       ReplayBrokerStateV2::IoFailure,
                       ReplayBrokerStateV2::TimedOut,
                       ReplayBrokerStateV2::RuntimeFailure}) {
        result.state = state;
        QNP_CHECK(!replayClosedBeforeResultV2(true, true, result));
    }
}

QNP_TEST(replay_completion_text_distinguishes_divergence_from_verification) {
    using Reason = qreplay::ipc::v3::PlaybackResultReason;
    const std::string divergent = replayPlaybackCompletionTextV2(Reason::CompletedWithDivergence);
    QNP_CHECK(divergent.find("differences") != std::string::npos);
    QNP_CHECK(divergent.find("verified") == std::string::npos);
    QNP_CHECK(std::string(replayPlaybackCompletionTextV2(Reason::VerifiedBehaviourOnly))
              .find("behaviour-verified") != std::string::npos);
    QNP_CHECK(std::string(replayPlaybackCompletionTextV2(Reason::VerifiedCompleted))
              .find("byte-exact") != std::string::npos);
}

QNP_TEST(replay_catalog_cache_accelerates_without_becoming_authority) {
    TempDir library(L"cat_cachelib");
    TempDir cacheDir(L"cat_cachedir");
    QNP_REQUIRE(library.valid() && cacheDir.valid());
    FixtureOptions options{};
    for (int i = 0; i < 5; ++i) {
        wchar_t name[32];
        _snwprintf_s(name, 32, _TRUNCATE, L"c%02d.q99r", i);
        options.uuidLow = static_cast<uint64_t>(200 + i);
        QNP_REQUIRE(writeFixture(library.file(name), options));
    }
    const std::wstring cachePath = cacheDir.file(L"catalog.bin");

    ReplayScanner::Config config = configFor(library.path);
    config.useCache = true;
    config.cachePath = cachePath;

    {
        ReplayCatalogCache cache;
        QNP_CHECK(cache.open(cachePath));
        QNP_CHECK(!cache.readOnly());
        const ReplaySnapshot first =
            ReplayScanner::scanOnce(config, &cache, nullptr, 1u);
        QNP_CHECK(first.entries.size() == 5u);
        QNP_CHECK(first.filesParsed == 5u);
        QNP_CHECK(first.filesFromCache == 0u);
        QNP_CHECK(cache.flush());
    }
    {
        ReplayCatalogCache cache;
        QNP_CHECK(cache.open(cachePath));
        const ReplaySnapshot second =
            ReplayScanner::scanOnce(config, &cache, nullptr, 2u);
        QNP_CHECK(second.entries.size() == 5u);
        QNP_CHECK(second.filesFromCache == 5u);
        for (const ReplayEntry& entry : second.entries) {
            QNP_CHECK(entry.cached);
            QNP_CHECK(entry.hasManifest);
            QNP_CHECK(entry.hasFooter);
        }
    }
    {
        FixtureOptions changed{};
        changed.uuidLow = 999u;
        changed.matchCount = 2u;
        QNP_REQUIRE(writeFixture(library.file(L"c00.q99r"), changed));
        ReplayCatalogCache cache;
        QNP_CHECK(cache.open(cachePath));
        const ReplaySnapshot third =
            ReplayScanner::scanOnce(config, &cache, nullptr, 3u);
        QNP_CHECK(third.filesFromCache < 5u);
    }
}

QNP_TEST(replay_catalog_cache_discards_corruption_and_version_mismatch) {
    TempDir dir(L"cat_cachebad");
    QNP_REQUIRE(dir.valid());
    const std::wstring cachePath = dir.file(L"catalog.bin");

    {
        QNP_REQUIRE(writeBytes(cachePath, std::vector<uint8_t>(512u, 0x5Au)));
        ReplayCatalogCache cache;
        QNP_CHECK(cache.open(cachePath));
        QNP_CHECK(cache.size() == 0u);
    }
    {
        std::vector<uint8_t> bytes;
        putU32(bytes, ReplayCatalogCache::kMagic);
        putU32(bytes, ReplayCatalogCache::kVersion + 1u);
        putU32(bytes, 0u);
        putU32(bytes, crc32c(nullptr, 0u));
        QNP_REQUIRE(writeBytes(cachePath, bytes));
        ReplayCatalogCache cache;
        QNP_CHECK(cache.open(cachePath));
        QNP_CHECK(cache.size() == 0u);
    }
    {
        ReplayCatalogCache source;
        ReplayEntry entry{};
        entry.identity.known = true;
        entry.identity.volumeId = 1u;
        entry.identity.fileId = 2u;
        entry.identity.sizeBytes = 3u;
        entry.path = L"C:\\replays\\x.q99r";
        source.store(entry);
        std::vector<uint8_t> bytes = source.encode();
        QNP_REQUIRE(bytes.size() > 20u);
        bytes[18] ^= 0xFFu;
        ReplayCatalogCache cache;
        QNP_CHECK(!cache.decode(bytes.data(), bytes.size()));
        QNP_CHECK(cache.size() == 0u);
    }
}

QNP_TEST(replay_catalog_cache_lock_downgrades_to_read_only) {
    TempDir dir(L"cat_lock");
    QNP_REQUIRE(dir.valid());
    const std::wstring cachePath = dir.file(L"catalog.bin");

    ReplayCatalogCache first;
    QNP_CHECK(first.open(cachePath));
    QNP_CHECK(!first.readOnly());

    ReplayCatalogCache second;
    const bool writable = second.open(cachePath);
    QNP_CHECK(!writable);
    QNP_CHECK(second.readOnly());
    ReplayEntry entry{};
    entry.identity.known = true;
    entry.identity.volumeId = 5u;
    entry.identity.fileId = 6u;
    second.store(entry);
    QNP_CHECK(second.size() == 0u);
}

QNP_TEST(replay_annotations_round_trip_and_reject_bad_text) {
    TempDir dir(L"cat_annot");
    QNP_REQUIRE(dir.valid());
    const std::wstring path = dir.file(L"annotations.bin");

    ReplayAnnotationStore store;
    QNP_CHECK(store.setFavorite(1u, 2u, true));
    QNP_CHECK(store.setTitle(1u, 2u, "Grand final"));
    QNP_CHECK(store.setNote(1u, 2u, "third round was the good one"));
    QNP_CHECK(store.setTags(1u, 2u, { "tourney", "clutch" }));
    QNP_CHECK(store.recordStructuralVerification(1u, 2u, digestFromSeed(3u),
                                                 1767225600));
    QNP_CHECK(store.recordSimulationVerification(1u, 2u, digestFromSeed(4u),
                                                 0x1234u, 1767225601));
    QNP_CHECK(store.recordProvenance(1u, 2u, 9u, 10u, digestFromSeed(5u),
                                     "old.q99r.part"));
    QNP_CHECK(store.save(path));

    ReplayAnnotationStore loaded;
    QNP_REQUIRE(loaded.load(path));
    const ReplayAnnotation* entry = loaded.find(1u, 2u);
    QNP_REQUIRE(entry != nullptr);
    QNP_CHECK(entry->favorite);
    QNP_CHECK(entry->title == "Grand final");
    QNP_CHECK(entry->note == "third round was the good one");
    QNP_CHECK(entry->tags.size() == 2u);
    QNP_CHECK(entry->structuralVerified);
    QNP_CHECK(entry->simulationVerified);
    QNP_CHECK(entry->verifiedRuntimeSimulationId == 0x1234u);
    QNP_CHECK(entry->hasProvenance);
    QNP_CHECK(entry->sourceFileName == "old.q99r.part");

    QNP_CHECK(!store.setTitle(1u, 2u, std::string(kMaxLabelBytes + 1u, 'x')));
    QNP_CHECK(!store.setNote(1u, 2u, std::string(kMaxNoteBytes + 1u, 'x')));
    QNP_CHECK(!store.setTitle(1u, 2u, "bad\x1b[31m"));
    QNP_CHECK(!store.setNote(1u, 2u, std::string("bad\xC0\xAF")));
    QNP_CHECK(!store.setTags(1u, 2u, { "a", "a" }));
    QNP_CHECK(!store.setTags(1u, 2u, std::vector<std::string>(kMaxTags + 1u,
                                                             "t")));
    QNP_CHECK(!store.setTags(1u, 2u, { std::string(kMaxTagBytes + 1u, 't') }));
    QNP_CHECK(store.find(1u, 2u)->title == "Grand final");
}

QNP_TEST(replay_annotations_reject_a_corrupt_store) {
    ReplayAnnotationStore store;
    QNP_CHECK(store.setTitle(1u, 1u, "x"));
    std::vector<uint8_t> bytes = store.encode();
    QNP_REQUIRE(bytes.size() > 20u);

    {
        std::vector<uint8_t> broken = bytes;
        broken[20] ^= 0x01u;
        ReplayAnnotationStore other;
        QNP_CHECK(!other.decode(broken.data(), broken.size()));
        QNP_CHECK(other.size() == 0u);
    }
    {
        std::vector<uint8_t> broken = bytes;
        broken[4] = 0x09u;
        ReplayAnnotationStore other;
        QNP_CHECK(!other.decode(broken.data(), broken.size()));
    }
    {
        std::vector<uint8_t> broken = bytes;
        broken.push_back(0u);
        ReplayAnnotationStore other;
        QNP_CHECK(!other.decode(broken.data(), broken.size()));
    }
    {
        for (size_t cut = 0; cut < bytes.size(); ++cut) {
            ReplayAnnotationStore other;
            QNP_CHECK(!other.decode(bytes.data(), cut));
            QNP_CHECK(other.size() == 0u);
        }
    }
}

QNP_TEST(replay_catalog_default_sort_keeps_sets_in_game_order) {
    ReplaySnapshot snapshot;
    const auto add = [&](uint64_t set, uint32_t game, int64_t date,
                         const wchar_t* name) {
        ReplayEntry entry;
        entry.isV2 = true;
        entry.fileName = name;
        entry.path = name;
        entry.stableId = 100u - snapshot.entries.size();
        auto& display = entry.startV2.manifest.display;
        display.flags = DisplayMetadataSeriesV2;
        display.seriesUuidLow = set;
        display.gameOrdinalInSeries = game;
        display.recordedUnixSeconds = date;
        snapshot.entries.push_back(entry);
    };
    add(1, 10, 310, L"a-game10.q99r");
    add(2, 2, 315, L"b-game2.q99r");
    add(1, 2, 320, L"a-game2.q99r");
    add(0, 0, 400, L"standalone.q99r");
    add(2, 1, 200, L"b-game1.q99r");
    add(1, 0, 300, L"a-game1.q99r");
    add(0, 0, 150, L"missing-series.q99r");
    add(1, 3, 100, L"missing-flag.q99r");
    snapshot.entries.back().startV2.manifest.display.flags = 0;
    add(0, 0, 0, L"unknown-date.q99r");
    QueryInputs inputs;
    inputs.snapshot = &snapshot;
    QNP_CHECK((applyQuery(inputs) == std::vector<size_t>{3, 5, 2, 0, 4, 1, 6, 7, 8}));
    inputs.filter.toUnixSeconds = 315;
    QNP_CHECK((applyQuery(inputs) == std::vector<size_t>{5, 0, 4, 1, 6, 7, 8}));
}

QNP_TEST(replay_catalog_game_order_is_stable_for_ties_and_uuid_high_bits) {
    ReplaySnapshot snapshot;
    for (const wchar_t* name : {L"b.q99r", L"a.q99r", L"set1-game2.q99r",
                                L"set2-game1.q99r", L"set1-game1.q99r", L"set1-game1b.q99r"}) {
        ReplayEntry entry;
        entry.fileName = name;
        entry.path = name;
        entry.identity.modifiedUnixSeconds = 100;
        entry.stableId = snapshot.entries.size() + 1u;
        snapshot.entries.push_back(entry);
    }
    for (size_t i = 2; i < snapshot.entries.size(); ++i) {
        auto& entry = snapshot.entries[i];
        entry.isV2 = true;
        auto& display = entry.startV2.manifest.display;
        display.flags = DisplayMetadataSeriesV2;
        display.seriesUuidLow = 7;
        display.seriesUuidHigh = i == 3 ? 2 : 1;
        display.gameOrdinalInSeries = i == 2 ? 2 : 1;
    }
    QueryInputs inputs;
    inputs.snapshot = &snapshot;
    const std::vector<std::wstring> expected = {
        L"a.q99r", L"b.q99r", L"set1-game1.q99r", L"set1-game1b.q99r",
        L"set1-game2.q99r", L"set2-game1.q99r"
    };
    for (size_t rotation = 0; rotation < snapshot.entries.size(); ++rotation) {
        std::vector<std::wstring> paths;
        for (size_t index : applyQuery(inputs)) paths.push_back(snapshot.entries[index].path);
        QNP_CHECK(paths == expected);
        std::rotate(snapshot.entries.begin(), snapshot.entries.begin() + 1, snapshot.entries.end());
    }
}

QNP_TEST(replay_catalog_sort_labels_describe_the_order) {
    QNP_CHECK(std::string(sortKeyText(SortKey::TitleAsc)) == "Alphabetical (A-Z)");
    QNP_CHECK(std::string(sortKeyText(SortKey::MatchCountDesc)) == "Match count (most first)");
}

QNP_TEST(replay_catalog_query_filters_and_sorts_stably) {
    TempDir dir(L"cat_query");
    QNP_REQUIRE(dir.valid());

    FixtureOptions oneVsOne{};
    oneVsOne.uuidLow = 11u;
    oneVsOne.title = "Alpha";
    oneVsOne.laneOneName = "Ann";
    oneVsOne.laneTwoName = "Ben";
    oneVsOne.recordedUnixSeconds = 1767225600;
    QNP_REQUIRE(writeFixture(dir.file(L"a.q99r"), oneVsOne));

    FixtureOptions team{};
    team.uuidLow = 22u;
    team.fourPlayer = true;
    team.title = "Bravo";
    team.recordedUnixSeconds = 1767312000;
    team.matchCount = 3u;
    QNP_REQUIRE(writeFixture(dir.file(L"b.q99r"), team));

    FixtureOptions cpu{};
    cpu.uuidLow = 33u;
    cpu.cpuFighter = true;
    cpu.title = "Charlie";
    cpu.recordedUnixSeconds = 1767398400;
    QNP_REQUIRE(writeFixture(dir.file(L"c.q99r"), cpu));

    const ReplaySnapshot snapshot =
        ReplayScanner::scanOnce(configFor(dir.path), nullptr, nullptr, 1u);
    QNP_REQUIRE(snapshot.entries.size() == 3u);

    QueryInputs inputs{};
    inputs.snapshot = &snapshot;

    {
        inputs.sort = SortKey::Newest;
        const std::vector<size_t> order = applyQuery(inputs);
        QNP_REQUIRE(order.size() == 3u);
        QNP_CHECK(snapshot.entries[order[0]].manifest.display.title == "Charlie");
        QNP_CHECK(snapshot.entries[order[2]].manifest.display.title == "Alpha");
    }
    {
        inputs.sort = SortKey::Oldest;
        const std::vector<size_t> order = applyQuery(inputs);
        QNP_REQUIRE(order.size() == 3u);
        QNP_CHECK(snapshot.entries[order[0]].manifest.display.title == "Alpha");
    }
    {
        inputs.sort = SortKey::MatchCountDesc;
        const std::vector<size_t> order = applyQuery(inputs);
        QNP_REQUIRE(order.size() == 3u);
        QNP_CHECK(snapshot.entries[order[0]].matchCount == 3u);
    }
    {
        inputs.sort = SortKey::Newest;
        inputs.filter.fighterCount = 4u;
        std::vector<size_t> order = applyQuery(inputs);
        QNP_CHECK(order.size() == 1u);
        QNP_CHECK(snapshot.entries[order[0]].manifest.display.title == "Bravo");
        inputs.filter.fighterCount = 2u;
        order = applyQuery(inputs);
        QNP_CHECK(order.size() == 2u);
        inputs.filter.fighterCount = 0u;
    }
    {
        inputs.filter.ownership = OwnershipMix::AllHuman;
        std::vector<size_t> order = applyQuery(inputs);
        QNP_CHECK(order.size() == 2u);
        inputs.filter.ownership = OwnershipMix::MixedHumanCpu;
        order = applyQuery(inputs);
        QNP_CHECK(order.size() == 1u);
        QNP_CHECK(snapshot.entries[order[0]].manifest.display.title == "Charlie");
        inputs.filter.ownership = OwnershipMix::Any;
    }
    {
        inputs.query = "ann";
        QNP_CHECK(applyQuery(inputs).size() == 1u);
        inputs.query = "bravo";
        QNP_CHECK(applyQuery(inputs).size() == 1u);
        inputs.query = "akari";
        QNP_CHECK(applyQuery(inputs).size() == 3u);
        inputs.query = "young akari";
        QNP_CHECK(applyQuery(inputs).size() == 1u);
        inputs.query = "nothing matches this";
        QNP_CHECK(applyQuery(inputs).empty());
        inputs.query.clear();
    }
    {
        inputs.filter.characterIds = { 21u };
        QNP_CHECK(applyQuery(inputs).size() == 1u);
        inputs.filter.characterIds = { 8u };
        QNP_CHECK(applyQuery(inputs).size() == 3u);
        inputs.filter.characterIds.clear();
    }
    {
        inputs.filter.sideZeroCharacterIds = { 30u };
        QNP_CHECK(applyQuery(inputs).size() == 1u);
        inputs.filter.sideZeroCharacterIds.clear();
        inputs.filter.sideOneCharacterIds = { 30u };
        QNP_CHECK(applyQuery(inputs).empty());
        inputs.filter.sideOneCharacterIds.clear();
    }
    {
        inputs.filter.completion = CompletionFilter::IncompleteOnly;
        QNP_CHECK(applyQuery(inputs).empty());
        inputs.filter.completion = CompletionFilter::Any;
        inputs.filter.sourceOffline = false;
        QNP_CHECK(applyQuery(inputs).empty());
        inputs.filter.sourceOffline = true;
    }

    ReplaySnapshot tied = snapshot;
    for (ReplayEntry& entry : tied.entries)
        entry.manifest.display.recordedUnixSeconds = 1767225600;
    QueryInputs tiedInputs{};
    tiedInputs.snapshot = &tied;
    const std::vector<size_t> firstOrder = applyQuery(tiedInputs);
    for (int i = 0; i < 8; ++i)
        QNP_CHECK(applyQuery(tiedInputs) == firstOrder);
}

QNP_TEST(replay_catalog_scans_set_folders_and_records_where_files_live) {
    TempDir dir(L"catalog-recursive");
    QNP_REQUIRE(dir.valid());

    FixtureOptions loose{};
    loose.uuidLow = 1u;
    loose.title = "Loose";
    QNP_REQUIRE(writeFixture(dir.file(L"loose.q99r"), loose));

    const std::wstring setDir = dir.file(L"09-20_alice-vs-bob");
    QNP_REQUIRE(qplat::ensureDirW(setDir));
    FixtureOptions g1{};
    g1.uuidLow = 2u;
    g1.title = "Game one";
    QNP_REQUIRE(writeFixture(setDir + L"\\g1.q99r", g1));
    FixtureOptions g2{};
    g2.uuidLow = 3u;
    g2.title = "Game two";
    QNP_REQUIRE(writeFixture(setDir + L"\\g2.q99r", g2));

    const std::wstring nested = dir.file(L"2026-09") + L"\\tournament";
    QNP_REQUIRE(qplat::ensureDirW(nested));
    FixtureOptions deep{};
    deep.uuidLow = 4u;
    deep.title = "Nested";
    QNP_REQUIRE(writeFixture(nested + L"\\deep.q99r", deep));

    const ReplaySnapshot snapshot =
        ReplayScanner::scanOnce(configFor(dir.path), nullptr, nullptr, 1u);
    QNP_REQUIRE(snapshot.entries.size() == 4u);

    const auto folderOf = [&](const char* title) {
        for (const ReplayEntry& entry : snapshot.entries) {
            if (entry.manifest.display.title == title)
                return entry.relativeDir;
        }
        return std::wstring(L"<missing>");
    };
    QNP_CHECK(folderOf("Loose") == L"");
    QNP_CHECK(folderOf("Game one") == L"09-20_alice-vs-bob");
    QNP_CHECK(folderOf("Game two") == L"09-20_alice-vs-bob");
    QNP_CHECK(folderOf("Nested") == L"2026-09\\tournament");

    ReplayScanner::Config bounded = configFor(dir.path);
    bounded.maxFiles = 2u;
    const ReplaySnapshot capped =
        ReplayScanner::scanOnce(bounded, nullptr, nullptr, 2u);
    QNP_CHECK(capped.entries.size() == 2u);
}

QNP_TEST(replay_catalog_resolves_source_from_the_manifest_the_file_has) {
    {
        ReplayEntry entry{};
        entry.isV2 = true;
        entry.classificationV2 = ReplayClassificationV2::Sealed;
        entry.startV2.manifest.display.sourceKind =
            ReplaySourceKindV2::NetplayPlayer;
        QNP_CHECK(entry.manifest.display.sourceKind
                  == ReplaySourceKind::OfflineVsPlayer);
        QNP_CHECK(resolvedSourceOf(entry) == ReplaySourceResolved::Netplay);

        entry.startV2.manifest.display.sourceKind =
            ReplaySourceKindV2::SpectatorSave;
        QNP_CHECK(resolvedSourceOf(entry) == ReplaySourceResolved::Spectator);

        entry.startV2.manifest.display.sourceKind =
            ReplaySourceKindV2::OfflineVsPlayer;
        QNP_CHECK(resolvedSourceOf(entry) == ReplaySourceResolved::Offline);

        entry.classificationV2 = ReplayClassificationV2::MetadataOnly;
        QNP_CHECK(resolvedSourceOf(entry) == ReplaySourceResolved::Offline);

        entry.classificationV2 = ReplayClassificationV2::Invalid;
        QNP_CHECK(resolvedSourceOf(entry) == ReplaySourceResolved::Unknown);
    }
    {
        ReplayEntry entry{};
        entry.hasManifest = true;
        entry.manifest.display.sourceKind = ReplaySourceKind::SpectatorSave;
        QNP_CHECK(resolvedSourceOf(entry) == ReplaySourceResolved::Spectator);
        entry.hasManifest = false;
        QNP_CHECK(resolvedSourceOf(entry) == ReplaySourceResolved::Unknown);
    }

    ReplaySnapshot snapshot{};
    auto add = [&snapshot](uint64_t id, ReplaySourceKindV2 kind,
                           ReplayClassificationV2 classification) {
        ReplayEntry entry{};
        entry.stableId = id;
        entry.isV2 = true;
        entry.classificationV2 = classification;
        entry.startV2.manifest.display.sourceKind = kind;
        snapshot.entries.push_back(entry);
    };
    add(1u, ReplaySourceKindV2::NetplayPlayer, ReplayClassificationV2::Sealed);
    add(2u, ReplaySourceKindV2::OfflineVsPlayer, ReplayClassificationV2::Sealed);
    add(3u, ReplaySourceKindV2::SpectatorSave, ReplayClassificationV2::Sealed);
    add(4u, ReplaySourceKindV2::OfflineVsPlayer,
        ReplayClassificationV2::Invalid);

    QueryInputs inputs{};
    inputs.snapshot = &snapshot;
    QNP_REQUIRE(applyQuery(inputs).size() == 4u);

    const auto idsWith = [&](bool netplay, bool offline, bool spectator) {
        inputs.filter.sourceNetplay = netplay;
        inputs.filter.sourceOffline = offline;
        inputs.filter.sourceSpectator = spectator;
        std::vector<uint64_t> ids;
        for (size_t index : applyQuery(inputs))
            ids.push_back(snapshot.entries[index].stableId);
        return ids;
    };

    {
        const std::vector<uint64_t> ids = idsWith(true, false, false);
        QNP_REQUIRE(ids.size() == 2u);
        QNP_CHECK(ids[0] == 1u || ids[1] == 1u);
        QNP_CHECK(ids[0] == 4u || ids[1] == 4u);
    }
    {
        const std::vector<uint64_t> ids = idsWith(false, false, true);
        QNP_REQUIRE(ids.size() == 2u);
        QNP_CHECK(ids[0] == 3u || ids[1] == 3u);
    }
    {
        const std::vector<uint64_t> ids = idsWith(false, false, false);
        QNP_REQUIRE(ids.size() == 1u);
        QNP_CHECK(ids[0] == 4u);
    }

    QNP_CHECK(std::string(replaySourceLabel(ReplaySourceResolved::Netplay))
              == "Network");
    QNP_CHECK(std::string(replaySourceLabel(ReplaySourceResolved::Spectator))
              == "Spectated");
}

QNP_TEST(replay_catalog_query_uses_annotations) {
    TempDir dir(L"cat_annotquery");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    options.uuidLow = 55u;
    options.title = "Plain";
    QNP_REQUIRE(writeFixture(dir.file(L"one.q99r"), options));
    const ReplaySnapshot snapshot =
        ReplayScanner::scanOnce(configFor(dir.path), nullptr, nullptr, 1u);
    QNP_REQUIRE(snapshot.entries.size() == 1u);
    const ReplayEntry& entry = snapshot.entries[0];

    ReplayAnnotationStore store;
    QNP_REQUIRE(store.setFavorite(entry.prefix.sessionUuidLow,
                                  entry.prefix.sessionUuidHigh, true));
    QNP_REQUIRE(store.setTags(entry.prefix.sessionUuidLow,
                              entry.prefix.sessionUuidHigh, { "highlight" }));
    QNP_REQUIRE(store.setTitle(entry.prefix.sessionUuidLow,
                               entry.prefix.sessionUuidHigh, "Renamed locally"));

    QueryInputs inputs{};
    inputs.snapshot = &snapshot;
    inputs.annotations = &store;
    inputs.filter.favoritesOnly = true;
    QNP_CHECK(applyQuery(inputs).size() == 1u);

    inputs.filter.favoritesOnly = false;
    inputs.filter.requiredTags = { "highlight" };
    QNP_CHECK(applyQuery(inputs).size() == 1u);
    inputs.filter.requiredTags = { "missing" };
    QNP_CHECK(applyQuery(inputs).empty());
    inputs.filter.requiredTags.clear();

    inputs.query = "renamed";
    QNP_CHECK(applyQuery(inputs).size() == 1u);
}

QNP_TEST(replay_catalog_indexes_ten_thousand_rows_without_unbounded_growth) {
    ReplaySnapshot snapshot{};
    snapshot.entries.reserve(10000u);
    for (uint32_t i = 0; i < 10000u; ++i) {
        ReplayEntry entry{};
        entry.stableId = stableIdForSession(i + 1u, 0u);
        entry.prefix.sessionUuidLow = i + 1u;
        entry.parsed = true;
        entry.hasManifest = true;
        entry.hasFooter = true;
        entry.matchCount = 1u + (i % 4u);
        entry.totalFrames = 600u + i;
        entry.manifest = SessionManifest{};
        entry.manifest.display.recordedUnixSeconds =
            1767225600 + static_cast<int64_t>(i % 500u);
        entry.manifest.slots[0].ownership = SlotOwnership::RecordedHuman;
        entry.manifest.slots[0].identity.nativeCharacterId =
            static_cast<uint8_t>(i % 34u);
        entry.manifest.slots[1].ownership = SlotOwnership::RecordedHuman;
        entry.manifest.slots[1].identity.nativeCharacterId =
            static_cast<uint8_t>((i + 7u) % 34u);
        snapshot.entries.push_back(std::move(entry));
    }
    QueryInputs inputs{};
    inputs.snapshot = &snapshot;
    const uint64_t start = ::GetTickCount64();
    const std::vector<size_t> all = applyQuery(inputs);
    inputs.filter.characterIds = { 8u };
    const std::vector<size_t> filtered = applyQuery(inputs);
    inputs.filter.characterIds.clear();
    inputs.query = "akari";
    const std::vector<size_t> searched = applyQuery(inputs);
    const uint64_t elapsed = ::GetTickCount64() - start;

    QNP_CHECK(all.size() == 10000u);
    QNP_CHECK(!filtered.empty() && filtered.size() < all.size());
    QNP_CHECK(!searched.empty());
    QNP_CHECK(elapsed < 4000u);

    QNP_CHECK(ReplayEntry::kMaxRetainedMatches <= 64u);
}
