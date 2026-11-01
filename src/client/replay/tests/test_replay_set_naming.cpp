#include "TestHarness.h"

#include "client/replay/ReplaySetNaming.h"
#include "replay/ReplayCodec.h"

#include <string>

using namespace qreplay;
using namespace qclient::replay;

namespace {

GameManifestV2 manifestWith(const char* one, const char* two,
                            uint8_t characterOne, uint8_t characterTwo,
                            int64_t unixSeconds, uint32_t ordinal,
                            uint64_t seriesHigh) {
    GameManifestV2 manifest{};
    manifest.lanes[0].active = true;
    manifest.lanes[1].active = true;
    setReplayTextV2(manifest.lanes[0].name, one);
    setReplayTextV2(manifest.lanes[1].name, two);
    manifest.slots[0].identity.nativeCharacterId = characterOne;
    manifest.slots[1].identity.nativeCharacterId = characterTwo;
    manifest.display.recordedUnixSeconds = unixSeconds;
    manifest.display.timezoneOffsetMinutes = 0;
    manifest.display.gameOrdinalInSeries = ordinal;
    manifest.display.seriesUuidHigh = seriesHigh;
    return manifest;
}

constexpr int64_t kNoon = 1789905600;

}

QNP_TEST(replay_source_folder_names_every_kind) {
    qreplay::GameManifestV2 manifest{};
    manifest.display.sourceKind = qreplay::ReplaySourceKindV2::NetplayPlayer;
    QNP_CHECK(std::string(sourceFolderNameFor(manifest)) == "netplay");
    manifest.display.sourceKind = qreplay::ReplaySourceKindV2::OfflineVsPlayer;
    QNP_CHECK(std::string(sourceFolderNameFor(manifest)) == "offline");
    manifest.display.sourceKind = qreplay::ReplaySourceKindV2::SpectatorSave;
    QNP_CHECK(std::string(sourceFolderNameFor(manifest)) == "spectate");
    manifest.display.sourceKind = qreplay::ReplaySourceKindV2::RecoveredPrefix;
    QNP_CHECK(std::string(sourceFolderNameFor(manifest)) == "recovered");

    manifest.display.sourceKind =
        static_cast<qreplay::ReplaySourceKindV2>(0x7Fu);
    const std::string unknown = sourceFolderNameFor(manifest);
    QNP_CHECK(unknown == "other");

    for (const char* name : {"netplay", "offline", "spectate", "recovered",
                             "other"}) {
        QNP_CHECK(sanitizePathToken(name) == name);
    }
}

QNP_TEST(replay_set_naming_sanitizes_without_erasing_a_name) {
    QNP_CHECK(sanitizePathToken("Alice") == "Alice");
    QNP_CHECK(sanitizePathToken("the  quick   fox") == "the-quick-fox");

    QNP_CHECK(sanitizePathToken("a<<>>b") == "a-b");
    QNP_CHECK(sanitizePathToken("C:\\evil\\..\\x") == "C-evil-..-x");
    QNP_CHECK(sanitizePathToken("../..") == "");

    QNP_CHECK(sanitizePathToken("  -name.  ") == "name");

    const std::string japanese = "\xE3\x81\x95\xE3\x81\x8F\xE3\x82\x89";
    QNP_CHECK(sanitizePathToken(japanese) == japanese);

    const std::string many = japanese + japanese + japanese + japanese;
    const std::string cut = sanitizePathToken(many, 8u);
    QNP_CHECK(cut.size() <= 8u);
    QNP_CHECK(cut == japanese.substr(0u, 6u));

    QNP_CHECK(sanitizePathToken("///") == "");
}

QNP_TEST(replay_set_naming_reads_the_date_the_recording_carries) {
    int month = 0;
    int day = 0;
    civilMonthDayFromUnix(kNoon, 0, month, day);
    QNP_CHECK(month == 9);
    QNP_CHECK(day == 20);

    civilMonthDayFromUnix(kNoon, 13 * 60, month, day);
    QNP_CHECK(month == 9);
    QNP_CHECK(day == 21);
    civilMonthDayFromUnix(kNoon - 13 * 3600, -11 * 60, month, day);
    QNP_CHECK(month == 9);
    QNP_CHECK(day == 19);

    civilMonthDayFromUnix(-1, 0, month, day);
    QNP_CHECK(month == 12);
    QNP_CHECK(day == 31);
}

QNP_TEST(replay_set_naming_builds_a_folder_per_set) {
    const GameManifestV2 manifest =
        manifestWith("Alice", "Bob", 0u, 1u, kNoon, 1u,
                     0x1A2B3C4D00000000ull);
    QNP_CHECK(setFolderNameFor(manifest) == "09-20_Alice-vs-Bob_1A2B3C4D");

    GameManifestV2 later = manifest;
    later.display.seriesUuidHigh = 0x99887766FFFFFFFFull;
    QNP_CHECK(setFolderNameFor(later) == "09-20_Alice-vs-Bob_99887766");
    QNP_CHECK(setFolderNameFor(later) != setFolderNameFor(manifest));

    const std::string matchup =
        gameNameHintFor(manifest, "").substr(4u);
    GameManifestV2 offline = manifest;
    setReplayTextV2(offline.lanes[1].name, "");
    QNP_CHECK(setFolderNameFor(offline)
              == "09-20_" + matchup + "_1A2B3C4D");

    GameManifestV2 anonymous = offline;
    setReplayTextV2(anonymous.lanes[0].name, "");
    QNP_CHECK(setFolderNameFor(anonymous)
              == "09-20_" + matchup + "_1A2B3C4D");
    QNP_CHECK(setFolderNameFor(manifest).find(matchup) == std::string::npos);

    GameManifestV2 hostile = manifest;
    setReplayTextV2(hostile.lanes[0].name, "..\\..\\Windows");
    const std::string folder = setFolderNameFor(hostile);
    QNP_CHECK(folder.find("..\\") == std::string::npos);
    QNP_CHECK(folder.find('/') == std::string::npos);
}

QNP_TEST(replay_set_naming_puts_the_characters_in_the_file_name) {
    const GameManifestV2 manifest =
        manifestWith("Alice", "Bob", 0u, 1u, kNoon, 3u, 0ull);
    const std::string hint = gameNameHintFor(manifest, "netplay");

    QNP_CHECK(hint.compare(0, 8, "netplay_") == 0);
    QNP_CHECK(hint.find("G04_") != std::string::npos);
    QNP_CHECK(hint.find("-vs-") != std::string::npos);

    GameManifestV2 first = manifest;
    first.display.gameOrdinalInSeries = 0u;
    QNP_CHECK(gameNameHintFor(first, "local").find("G01_")
              != std::string::npos);
    GameManifestV2 second = manifest;
    second.display.gameOrdinalInSeries = 1u;
    QNP_CHECK(gameNameHintFor(second, "local").find("G02_")
              != std::string::npos);
    GameManifestV2 tenth = manifest;
    tenth.display.gameOrdinalInSeries = 9u;
    QNP_CHECK(gameNameHintFor(tenth, "local").find("G10_")
              != std::string::npos);

    GameManifestV2 future = manifest;
    future.slots[1].identity.nativeCharacterId = 200u;
    const std::string odd = gameNameHintFor(future, "netplay");
    QNP_CHECK(!odd.empty());
    QNP_CHECK(odd.find('#') == std::string::npos);
}
