#include "TestHarness.h"

#include <cstdio>
#include <string>

#include "game/GameErrorJournal.h"

QNP_TEST(game_error_journal_splits_lines_and_decodes_shift_jis) {
    const std::string bytes =
        "error:.\\System\\title\\wall.img\r\n"
        "\r\n"
        "error:Title\x82\xCC\x8F\x89\x8A\xFA\x89\xBB\x82\xC9\x8E\xB8\x94\x73\x82\xB5\x82\xDC\x82\xB5\x82\xBD\x81\x42\n";
    const std::vector<std::string> lines = qgame::splitGameErrorJournal(bytes);
    QNP_REQUIRE(lines.size() == 2u);
    QNP_CHECK(lines[0] == "error:.\\System\\title\\wall.img");
    const std::string utf8 = qgame::gameErrorTextUtf8(lines[1].c_str());
    QNP_CHECK(utf8.find("error:Title") == 0u);
    QNP_CHECK(utf8.find("\xE5\x88\x9D\xE6\x9C\x9F\xE5\x8C\x96") != std::string::npos);
    QNP_CHECK(qgame::gameErrorTextUtf8("plain\r\n") == "plain");
    QNP_CHECK(qgame::gameErrorTextUtf8(nullptr) == "(null)");
}

QNP_TEST(game_error_journal_reads_only_this_runs_file) {
    char dir[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, dir);
    const std::string root = std::string(dir) + "qoh99_journal_test_" + std::to_string(GetCurrentProcessId());
    CreateDirectoryA(root.c_str(), nullptr);
    CreateDirectoryA((root + "\\SYSTEM").c_str(), nullptr);
    const std::vector<std::string> candidates = qgame::gameErrorJournalCandidates(root);
    QNP_REQUIRE(!candidates.empty());
    QNP_CHECK(candidates[0] == root + "\\SYSTEM\\QOHerror.txt");

    qgame::GameErrorJournalFile journal = qgame::readGameErrorJournalFile(candidates[0], 0ull);
    QNP_CHECK(!journal.present);

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const unsigned long long before =
        ((static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime) - 10000000ull;
    {
        FILE* f = std::fopen(candidates[0].c_str(), "wb");
        QNP_REQUIRE(f != nullptr);
        std::fputs("error:.\\System\\title\\LOGO1.img\r\nerror:second\r\n", f);
        std::fclose(f);
    }
    journal = qgame::readGameErrorJournalFile(candidates[0], before);
    QNP_CHECK(journal.present && journal.fromThisRun);
    QNP_REQUIRE(journal.lines.size() == 2u);
    QNP_CHECK(journal.lines[0] == "error:.\\System\\title\\LOGO1.img");

    const unsigned long long future = before + 3600ull * 10000000ull;
    journal = qgame::readGameErrorJournalFile(candidates[0], future);
    QNP_CHECK(journal.present && !journal.fromThisRun && journal.lines.empty());

    DeleteFileA(candidates[0].c_str());
    RemoveDirectoryA((root + "\\SYSTEM").c_str());
    RemoveDirectoryA(root.c_str());
}

#include "game/SessionLogHighlights.h"

QNP_TEST(session_log_highlights_quote_only_the_current_launch) {
    const std::string log =
        "[10:00:00.000]   qoh99_netplay.dll  built Sep  5 2026  pid=100\r\n"
        "[10:00:01.000] [hooks] REFUSING startup: prologue MISMATCH at sub_1\r\n"
        "[10:00:02.000] [CRASH] code=0xC0000005 (ACCESS_VIOLATION) addr=00401000\r\n"
        "[10:05:00.000]   qoh99_netplay.dll  built Sep  5 2026  pid=200\r\n"
        "[10:05:01.000] [input-v2] launch configuration accepted\r\n"
        "[10:05:02.000] [presentation] FAILED to hook palette sub_45B590\r\n"
        "[10:05:03.000] [hooks] mandatory gameplay hooks incomplete - rolling back startup\r\n";
    static const char* const patterns[] = { "REFUS", "FAILED", "MISMATCH", "rolling back", "[CRASH]" };
    const std::vector<std::string> lines = qgame::selectHighlightLines(
        log, qgame::kDllLogLaunchBanner, patterns, 5u, 6u);
    QNP_REQUIRE(lines.size() == 2u);
    QNP_CHECK(lines[0].find("FAILED to hook palette") != std::string::npos);
    QNP_CHECK(lines[1].find("rolling back") != std::string::npos);
    QNP_CHECK(lines[1].back() != '\r');

    const std::vector<std::string> capped = qgame::selectHighlightLines(log, nullptr, patterns, 5u, 1u);
    QNP_REQUIRE(capped.size() == 1u);
    QNP_CHECK(capped[0].find("rolling back") != std::string::npos);
    const std::vector<std::string> all = qgame::selectHighlightLines(log, nullptr, patterns, 5u, 0u);
    QNP_CHECK(all.size() == 4u);
    QNP_CHECK(qgame::selectHighlightLines("", qgame::kDllLogLaunchBanner, patterns, 5u, 6u).empty());
    const std::string quietLaunch = log +
        "[logger] process-start pid=300 built Oct 24 2026\n"
        "[12:00:00.000] [CRASH] new failure after bounded history rolled\n";
    const auto current = qgame::selectHighlightLines(
        quietLaunch, qgame::kDllLogLaunchBanner, patterns, 5u, 6u);
    QNP_REQUIRE(current.size() == 1u);
    QNP_CHECK(current[0].find("new failure") != std::string::npos);
}
