#include "TestFramework.hpp"
#include "game/DirectBoot.h"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string projectRoot() {
    const std::string here = __FILE__;
    const std::size_t source = here.find("src");
    return source == std::string::npos ? std::string{}
                                       : here.substr(0, source);
}

std::string readSource(const char* relativePath) {
    std::ifstream file(projectRoot() + relativePath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size()
            && text[i + 1] == '\n') {
            continue;
        }
        normalized.push_back(text[i]);
    }
    return normalized;
}

std::string slice(const std::string& source, const char* beginMarker,
                  const char* endMarker) {
    const std::size_t begin = source.find(beginMarker);
    if (begin == std::string::npos) return {};
    const std::size_t end = source.find(endMarker, begin);
    if (end == std::string::npos || end <= begin) return {};
    return source.substr(begin, end - begin);
}

std::size_t countOccurrences(const std::string& source, const char* needle) {
    std::size_t count = 0;
    const std::string what = needle;
    if (what.empty()) return 0;
    for (std::size_t at = source.find(what); at != std::string::npos;
         at = source.find(what, at + what.size()))
        ++count;
    return count;
}

}

TEST(DirectBootNeverWritesTheArcadeFormationHandoff) {
    const std::string boot = readSource("src/game/DirectBoot.cpp");
    CHECK(!boot.empty());

    CHECK(boot.find("wr32(kMatchFormationRow, 0)") == std::string::npos);
    CHECK(boot.find("wr32(kVsFormationApplied, 0)") == std::string::npos);
    CHECK(boot.find("kMatchFormationMode") == std::string::npos);
    CHECK(boot.find("kCharacterSelectPolicy") == std::string::npos);

    CHECK(boot.find("wr32(kMatchFormationRow, 1)") != std::string::npos);

    CHECK(boot.find("void selectVsModeFromMainMenu()") == std::string::npos);
    CHECK(boot.find("void jumpToCharSelect()") == std::string::npos);
    CHECK(boot.find("\n    (void)&selectVsModeFromMainMenu;")
          == std::string::npos);
    CHECK(boot.find("\n    (void)sub48;\n}") != std::string::npos);
}

TEST(TrainingDirectBootUsesTheCommonNativeMenuInitializationPath) {
    const std::string boot = readSource("src/game/DirectBoot.cpp");
    CHECK(!boot.empty());
    const std::string menu = slice(boot, "case P_Menu:", "case P_VsList:");
    CHECK(!menu.empty());

    CHECK(menu.find("wr32(kSub48, 3)") == std::string::npos);
    CHECK(menu.find("training: title -> native menu initializer")
          == std::string::npos);
    CHECK(menu.find("if (scene == 1 && sub48 == 2)")
          != std::string::npos);
    CHECK(menu.find("training: main-menu bootstrap TIMEOUT")
          != std::string::npos);
}

TEST(TrainingStageSelectionConsumesEachP1DirectionOnce) {
    const std::string policy =
        readSource("src/game/DirectBoot.h");
    const std::string runtime =
        readSource("src/training/TrainingGameIntegration.inl");
    CHECK(!policy.empty());
    CHECK(!runtime.empty());

    const std::string selection = slice(
        policy, "class TrainingSelectionPolicy",
        "inline const char* trainingSelectionFailureText");
    CHECK(!selection.empty());
    CHECK(policy.find("bool stageSelectionActive = false")
          != std::string::npos);
    CHECK(selection.find("if (sample.stageSelectionActive)\n"
                         "            decision.p2InputSource = 1;")
          != std::string::npos);

    const std::string override = slice(
        runtime, "bool trainingOverrideActive(int player)",
        "\nvoid trainingObserveOfflinePhysicalSnapshot(");
    CHECK(!override.empty());
    CHECK(override.find(
              "if (player == 1 && trainingInStageSelectionFrontend())")
          != std::string::npos);
    CHECK(override.find(
              "if (player == 1 && trainingInStageSelectionFrontend())\n"
              "        return 0;")
          != std::string::npos);
}

TEST(TrainingReturnResetsOnlyTheFrontendStageSelector) {
    const std::string boot = readSource("src/game/DirectBoot.cpp");
    CHECK(!boot.empty());

    const std::string returned = slice(
        boot, "if (g_trainingAwaitReturnCharSelect) {",
        "\n                if (scene == 3 && s3sub == 1) {");
    CHECK(!returned.empty());
    CHECK(returned.find("decideTrainingReturnedStageReset")
          != std::string::npos);
    CHECK(returned.find("wr32(kStageSelectionKind,") != std::string::npos);
    CHECK(returned.find("wr32(kStageSelectionValue,") != std::string::npos);
    CHECK(returned.find("wr32(kLogicalStage,") == std::string::npos);
    CHECK(returned.find("scene == 3 && s3sub == 2") != std::string::npos);
}

TEST(DirectBootOfflineCharacterSelectObservesInsteadOfOverwriting) {
    const std::string boot = readSource("src/game/DirectBoot.cpp");
    const std::string handoff =
        slice(boot, "if (g_stopAtCharSel) {", "if (g_replaySelection) {");
    CHECK(!handoff.empty());

    CHECK(handoff.find("observeNetworkTwoHumanCharacterSelect")
          != std::string::npos);
    CHECK(handoff.find("observeTwoFighterCharacterSelect")
          != std::string::npos);
    CHECK(handoff.find("decideTwoFighterCharacterSelectHandoff")
          != std::string::npos);
    CHECK(handoff.find("wr32(") == std::string::npos);
}

TEST(DirectBootUnattendedLaunchVerifiesTheSceneTwoBoundary) {
    const std::string boot = readSource("src/game/DirectBoot.cpp");
    CHECK(!boot.empty());

    const std::string charsel = slice(
        boot, "if (g_replaySelection) {", "\n            if (scene == 2) {");
    CHECK(!charsel.empty());
    CHECK(charsel.find("wr32(kMatchStart, 1);") != std::string::npos);
    CHECK(charsel.find("wr32(kNativeConfirmationLatch, 30);")
          != std::string::npos);
    CHECK(charsel.find("reinterpret_cast<VoidRetFn>(kSub_434710)();")
          != std::string::npos);
    CHECK_EQ(countOccurrences(boot, "wr32(kMatchStart"), std::size_t{1});
    CHECK_EQ(countOccurrences(boot, "wr32(kNativeConfirmationLatch"),
             std::size_t{1});
    CHECK_EQ(countOccurrences(boot,
                              "reinterpret_cast<VoidRetFn>(kSub_434710)();"),
             std::size_t{1});
    CHECK(boot.find("wr32(kSceneNext, 2);") == std::string::npos);
    CHECK(charsel.find("wr32(kP1CharacterSelectState, h0 + 1);")
          != std::string::npos);
    CHECK_EQ(countOccurrences(boot, "wr32(kP1CharacterSelectState"),
             std::size_t{1});
    const std::string wait = slice(
        boot, "if (((g_frame - g_phaseStart) % 300) == 0)",
        "\n            if (scene == 2) {");
    CHECK(!wait.empty());
    CHECK(wait.find("wr32(") == std::string::npos);
    CHECK(wait.find("kSub_434710") == std::string::npos);

    const std::string reached =
        slice(boot,
              "if (scene == 2) {\n                bootLog(\"MATCH reached\");",
              "\n            else if (g_frame - g_phaseStart > 1800)");
    CHECK(!reached.empty());
    CHECK(reached.find("observeTwoFighterMatchEntry") != std::string::npos);
    CHECK(reached.find("TwoFighterMatchEntryVerified") != std::string::npos);
    CHECK(reached.find("g_failed = true;") != std::string::npos);
    CHECK(reached.find("rd32(kMatchCommittedCpuMask)") != std::string::npos);
    CHECK(reached.find("wr32(") == std::string::npos);

    CHECK(boot.find("static uint64_t s_confAt") == std::string::npos);
    CHECK(boot.find("uint64_t g_forcedLaunchAt = 0;") != std::string::npos);
    CHECK(charsel.find("g_forcedLaunchAt") != std::string::npos);

    const std::string hooks = readSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    const std::string inputRead = slice(
        hooks, "int __fastcall hkInputRead(int player)", "\nstruct InputPhaseSample { int32_t x, y, act, facing; uint32_t ringHash; };");
    CHECK(!inputRead.empty());
    const std::size_t netGate = inputRead.find("g_netActive && (player");
    const std::size_t replayGate =
        inputRead.find("g_replayRoleActive && player");
    const std::size_t trainingGate =
        inputRead.find("trainingOverrideActive(player)");
    const std::size_t frontendGate =
        inputRead.find("qgame::directBootUnattendedFrontendActive()");
    CHECK(netGate != std::string::npos);
    CHECK(replayGate != std::string::npos);
    CHECK(trainingGate != std::string::npos);
    CHECK(frontendGate != std::string::npos);
    CHECK(netGate < frontendGate);
    CHECK(replayGate < frontendGate);
    CHECK(trainingGate < frontendGate);
    CHECK(inputRead.find("return g_frontendInject[player];")
          != std::string::npos);

    const std::string gate = slice(
        boot, "bool directBootUnattendedFrontendActive() {", "\n}");
    CHECK(!gate.empty());
    CHECK(gate.find("g_unattendedRematch") != std::string::npos);
    CHECK(gate.find("!g_networkDriven") != std::string::npos);
    CHECK(gate.find("!g_done") != std::string::npos);
}

TEST(DirectBootUnattendedReEntersInsteadOfSuppressingTheSceneThreeReturn) {
    const std::string boot = readSource("src/game/DirectBoot.cpp");

    CHECK(boot.find("enum Phase { P_Menu, P_VsList, P_CharSel, P_Match, "
                    "P_Done };")
          != std::string::npos);
    CHECK(boot.find("case P_Match:") != std::string::npos);

    const std::string reached =
        slice(boot, "if (scene == 2) {\n                bootLog(\"MATCH reached\");",
              "\n            else if (g_frame - g_phaseStart > 1800)");
    CHECK(!reached.empty());
    CHECK(reached.find("if (g_unattendedRematch) {") != std::string::npos);
    CHECK(reached.find("g_phase = P_Match;") != std::string::npos);
    CHECK(reached.find("g_phaseStart = g_frame;") != std::string::npos);

    const std::string matchPhase =
        slice(boot, "case P_Match:", "\n        default: break;");
    CHECK(!matchPhase.empty());
    CHECK(matchPhase.find("wr32(") == std::string::npos);
    CHECK(matchPhase.find("g_phase = P_Menu") == std::string::npos);
    CHECK(matchPhase.find("g_phase = P_VsList") == std::string::npos);
    CHECK(matchPhase.find("g_phase = P_CharSel;") != std::string::npos);
    CHECK(matchPhase.find("scene == 3 && s3sub == 2") != std::string::npos);

    CHECK(boot.find("g_unattendedRematch = true;") != std::string::npos);
    const std::string stopAtCharSel =
        slice(boot, "bool startDirectBootToCharSelect(int vsRow) {", "\n}");
    CHECK(!stopAtCharSel.empty());
    CHECK(stopAtCharSel.find("g_unattendedRematch = false;")
          != std::string::npos);
}

TEST(DirectBootRenderFallbackIsNotPermanentlyDisabled) {
    const std::string boot = readSource("src/game/DirectBoot.cpp");

    CHECK(boot.find("bool g_offlineDispatchDriven") == std::string::npos);
    CHECK(boot.find("g_offlineDispatchDriven =") == std::string::npos);
    CHECK(boot.find("!qgame::g_offlineDispatchDriven") == std::string::npos);

    const std::string fallback =
        slice(boot, "void qoh99np_boot_tick()", "\n}");
    CHECK(!fallback.empty());
    CHECK(fallback.find("qgame::g_offlineDispatchTicked = false;")
          != std::string::npos);
    CHECK(fallback.find("qgame::tick();") != std::string::npos);
}

TEST(SceneDispatchHooksTickDirectBootForEveryUnattendedMode) {
    const std::string hooks = readSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());

    for (const char* fn : { "int hkScene1Dispatch() {",
                            "int hkScene3Dispatch() {" }) {
        const std::string body = slice(hooks, fn, "\n}");
        CHECK(!body.empty());
        CHECK(body.find("g_mode == RunMode::Observe || g_mode == "
                        "RunMode::SelfTest")
              != std::string::npos);
        CHECK(body.find("qgame::directBootOfflineTick();")
              != std::string::npos);
        CHECK(body.find("if (g_mode == RunMode::Observe) "
                        "netOfflineFrameTick();")
              != std::string::npos);
    }
}

TEST(DirectBootFormationModelIsRowDerivedNotHardcoded) {
    using namespace qgame::directboot;
    CHECK(isTwoFighterDirectBootRow(0));
    CHECK(isTwoFighterDirectBootRow(1));
    CHECK(!isTwoFighterDirectBootRow(2));
    CHECK(!isTwoFighterDirectBootRow(-1));

    CHECK_EQ(expectedTwoFighterCharacterSelect(0u).cpuSlotMask, 0u);
    CHECK_EQ(expectedTwoFighterCharacterSelect(1u).cpuSlotMask, 0u);
    CHECK_EQ(vsRowCpuSlotMaskAtMatchEntry(0u), 2u);
    CHECK_EQ(vsRowCpuSlotMaskAtMatchEntry(1u), 0u);
}
