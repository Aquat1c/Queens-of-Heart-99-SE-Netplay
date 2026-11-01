#include "TestFramework.hpp"
#include "qoh99/AddressMap.hpp"
#include "qoh99_game.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace qoh99;

TEST(ProductionAddressesMatchReModel) {
    struct { uintptr_t prod; const char* sem; } g[] = {
        { kInputWordAddr,    "sessionInputWord" },
        { kKeyStateAddr,     "keyState" },
        { kSceneStateAddr,   "sceneCurrent" },
        { kSceneNextAddr,    "sceneNext" },
        { kMatchSubAddr,     "matchSubstate" },
        { kInMatchNetAddr,   "netInMatchFlag" },
        { kBlock0Addr,       "arenaBlock0" },
        { kBlock1Addr,       "arenaBlock1" },
        { kActiveBlockPtr,   "arenaActivePtr" },
        { kBlockSelector,    "arenaSelector" },
        { kFighterP1Addr,    "fighterP1" },
        { kFighterP2Addr,    "fighterP2" },
        { kFighterTableAddr, "fighterPtrTable" },
        { kRngSeedGameplay,  "rngSeedGameplay" },
        { kRngSeedCosmetic,  "rngSeedCosmetic" },
        { kRngSelector,      "rngSelector" },
        { kHealthBase,       "healthArray" },
        { kScoreBase,        "scoreArray" },
        { kNetDispatchPtr,   "netDispatchPtr" },
        { kNetConnectCbPtr,  "netOnMatchStartPtr" },
        { kNetLocalIdAddr,   "netSessionId" },
        { kNetSideAddr,      "netLocalSide" },
        { kNetReadyAddr,     "netReadyFlag" },
        { kNetClientGotAddr, "netClientGotFlag" },
        { kNetIsNetworkAddr, "netIsNetworkSession" },
        { kSub48StateAddr,   "mainMenuSubstate" },
        { kMenuCursorAddr,   "mainMenuCursor" },
        { kVsModeRowAddr,    "scene3Cursor" },
        { kVsModeConfirm,    "scene3ConfirmFlag" },
        { kTeamSize,         "vsFormationRow" },
        { kHumanFlags,       "charSelConfirmP1" },
    };
    for (auto& e : g) {
        const AddressEntry* a = AddressMap::find(e.sem);
        if (!a) { testfw::reportFail(__FILE__, __LINE__, std::string("no AddressMap entry: ") + e.sem); continue; }
        if (e.prod != a->va)
            testfw::reportFail(__FILE__, __LINE__,
                std::string("production ") + e.sem + " diverges from AddressMap");
    }
    CHECK_EQ(kFrameDispatchFn, (uintptr_t)fnaddr::kSceneMatchDispatch);
    CHECK_EQ(kNetSetupFn,      (uintptr_t)fnaddr::kNetSetupTransport);
    CHECK_EQ(kNetConnectFn,    (uintptr_t)fnaddr::kNetConnectDialogs);
    CHECK_EQ(kNetSendFn,       (uintptr_t)fnaddr::kNetSendMsg);
    CHECK_EQ(kInputSetKeyFn,   (uintptr_t)fnaddr::kInputInjectKey);
    CHECK_EQ(kVsModeSetupFn,   (uintptr_t)fnaddr::kApplyVsModeRow);
    CHECK_EQ(kBlockSize, (size_t)addr::kArenaBlockSize);
}

TEST(ProductionFunctionAliasesAreIdentityLayoutOnly) {
    constexpr uintptr_t kIdentityLayoutLastEntryVa = 0x472B80u;
    const uintptr_t fns[] = {
        kFrameDispatchFn, kNetSetupFn, kNetConnectFn, kNetSendFn,
        kInputSetKeyFn,   kVsModeSetupFn,
    };
    for (uintptr_t va : fns)
        CHECK(va <= kIdentityLayoutLastEntryVa);
}

TEST(NoSourceLiteralSitsAboveTheDecemberLayoutSplit) {
    constexpr std::uintptr_t kSplitVa = 0x470510u;
    constexpr std::uintptr_t kCodeDataBoundary = 0x499000u;

    std::string here = __FILE__;
    const std::size_t pos = here.find("src");
    if (pos == std::string::npos) {
        testfw::reportFail(__FILE__, __LINE__,
                           "cannot locate src/ from __FILE__");
        return;
    }
    const std::filesystem::path srcRoot =
        std::filesystem::path(here.substr(0, pos)) / "src";

    std::size_t scanned = 0;
    std::vector<std::string> offenders;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(srcRoot)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".inl")
            continue;
        const std::string full = entry.path().generic_string();
        if (full.find("training") != std::string::npos) continue;
        if (full.find("/tests/") != std::string::npos
            || entry.path().filename().string().rfind("test_", 0) == 0)
            continue;
        std::ifstream file(entry.path());
        if (!file) continue;
        ++scanned;
        std::string line;
        bool inBlockComment = false;
        for (std::size_t lineNo = 1; std::getline(file, line); ++lineNo) {
            std::string code;
            for (std::size_t i = 0; i < line.size(); ++i) {
                if (inBlockComment) {
                    if (line[i] == '*' && i + 1 < line.size()
                        && line[i + 1] == '/') {
                        inBlockComment = false;
                        ++i;
                    }
                    continue;
                }
                if (line[i] == '/' && i + 1 < line.size()) {
                    if (line[i + 1] == '/') break;
                    if (line[i + 1] == '*') { inBlockComment = true; ++i; continue; }
                }
                if (line[i] == '"') {
                    for (++i; i < line.size(); ++i) {
                        if (line[i] == '\\') { ++i; continue; }
                        if (line[i] == '"') break;
                    }
                    continue;
                }
                code.push_back(line[i]);
            }
            if (code.find("static_assert") != std::string::npos) continue;
            line = code;

            for (std::size_t i = 0; i + 7 < line.size(); ++i) {
                if (line[i] != '0' || (line[i + 1] != 'x' && line[i + 1] != 'X'))
                    continue;
                std::size_t j = i + 2;
                std::string digits;
                while (j < line.size() && std::isxdigit(
                           static_cast<unsigned char>(line[j]))) {
                    digits.push_back(line[j]);
                    ++j;
                }
                if (digits.size() == 8 && digits.compare(0, 2, "00") == 0)
                    digits.erase(0, 2);
                if (digits.size() != 6 || digits[0] != '4') continue;
                const std::uintptr_t va =
                    std::stoul(digits, nullptr, 16);
                if (va <= kSplitVa || va >= kCodeDataBoundary) continue;
                offenders.push_back(
                    entry.path().filename().string() + ":"
                    + std::to_string(lineNo) + " 0x" + digits);
                i = j - 1;
            }
        }
    }
    CHECK(scanned > 0);
    for (const std::string& offender : offenders) {
        testfw::reportFail(
            __FILE__, __LINE__,
            "code literal above the December layout split (November-only "
            "address; resolve it through the profile seam instead): "
            + offender);
    }
}

TEST(PlayerFacingFailureTextCarriesNoImplementationJargon) {
    std::string here = __FILE__;
    const std::size_t pos = here.find("src");
    if (pos == std::string::npos) {
        testfw::reportFail(__FILE__, __LINE__, "cannot locate src/");
        return;
    }
    std::ifstream file(std::filesystem::path(here.substr(0, pos))
                       / "src" / "client" / "Main.cpp");
    if (!file) {
        testfw::reportFail(__FILE__, __LINE__, "cannot read client/Main.cpp");
        return;
    }
    static const char* const kBanned[] = {
        "WSAStartup", "WSAGetLastError", "socket", "Socket",
        "SHA-256", "sha256", "DLL not found", "GAME.EXE",
        "endpoint", "payload", "codec", "opcode", "struct ",
        "nullptr", "errno", "HRESULT",
    };
    std::string line;
    std::size_t examined = 0;
    for (std::size_t lineNo = 1; std::getline(file, line); ++lineNo) {
        const std::size_t call = line.find("fail(con,");
        if (call == std::string::npos) continue;
        if (line.find("int fail(") != std::string::npos) continue;
        ++examined;
        for (const char* banned : kBanned) {
            if (line.find(banned) == std::string::npos) continue;
            testfw::reportFail(
                __FILE__, static_cast<int>(lineNo),
                std::string("player-facing failure text contains \"") + banned
                + "\" - say what happened and what to do, and move this to the "
                  "technical half: " + line.substr(call, 110));
        }
    }
    CHECK(examined > 50u);
}

TEST(ProductionAddressesPinnedValues) {
    CHECK_EQ(kFrameDispatchFn, 0x428430u);
    CHECK_EQ(kMatchSubAddr,    0x8719A4u);
    CHECK_EQ(kFighterP1Addr,   0x623E00u);
    CHECK_EQ(kActiveBlockPtr,  0x6234E8u);
    CHECK_EQ(kVsModeConfirm,   0x4AA450u);
    CHECK_EQ(kBlockSize,       0x1CED0u);
}
