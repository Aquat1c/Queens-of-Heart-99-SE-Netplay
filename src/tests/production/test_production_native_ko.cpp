#include "TestFramework.hpp"
#include <fstream>
#include <iterator>
#include <string>

static std::string readNativeKoSource() {
    std::string here = __FILE__;
    const std::size_t pos = here.find("src");
    if (pos == std::string::npos) return {};
    std::ifstream file(here.substr(0, pos) + "src/GameHooks.cpp");
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

static std::string nativeKoFallbackBody(const std::string& source) {
    const std::size_t begin = source.find(
        "if (segmentAge >= forceKoAt && life > 0 && resultState == 0u)");
    const std::size_t end = source.find(
        "\nstatic uint64_t g_ovrLogged", begin);
    if (begin == std::string::npos || end == std::string::npos) return {};
    return source.substr(begin, end - begin);
}

TEST(NativeKoFallbackIsExactTestOnlyOptIn) {
    const std::string source = readNativeKoSource();
    CHECK(!source.empty());
    CHECK(source.find(
              "std::strcmp(optIn, \"live-e2e:native-rematch\") == 0")
          != std::string::npos);
    CHECK(source.find("if (!g_netAutomatchTest) return;")
          != std::string::npos);
    CHECK(source.find(
              "if (g_netAutomatchForceKoEnabled\n"
              "        && segmentAge >= lowLifeAt && life > 300)")
          != std::string::npos);
    CHECK(source.find("if (!g_netAutomatchForceKoEnabled) return;")
          != std::string::npos);
}

TEST(SpectatorConfirmedTransitionDriverIsExactAndFrontendOnly) {
    const std::string source = readNativeKoSource();
    CHECK(!source.empty());
    CHECK(source.find(
              "std::strcmp(\n"
              "               optIn, "
              "\"live-e2e:spectator-confirmed-transition\") == 0")
          != std::string::npos);

    const std::size_t inputBegin = source.find(
        "int netAutomatchLocalInput(uint32_t frame)");
    const std::size_t inputEnd = source.find(
        "\nvoid netAutomatchPrepareCanonicalFrame(", inputBegin);
    CHECK(inputBegin != std::string::npos);
    CHECK(inputEnd != std::string::npos);
    if (inputBegin != std::string::npos
        && inputEnd != std::string::npos) {
        const std::string input =
            source.substr(inputBegin, inputEnd - inputBegin);
        CHECK(input.find(
                  "if (g_netAutomatchFrontendOnly && scene != 3) return 0;")
              != std::string::npos);
    }

    const std::size_t prepareBegin = source.find(
        "void netAutomatchPrepareCanonicalFrame(");
    const std::size_t prepareEnd = source.find(
        "\nstatic uint64_t g_ovrLogged", prepareBegin);
    CHECK(prepareBegin != std::string::npos);
    CHECK(prepareEnd != std::string::npos);
    if (prepareBegin != std::string::npos
        && prepareEnd != std::string::npos) {
        const std::string prepare =
            source.substr(prepareBegin, prepareEnd - prepareBegin);
        CHECK(prepare.find(
                  "const bool forceCharacterCursors =\n"
                  "        !g_netAutomatchFrontendOnly")
              != std::string::npos);
        CHECK(prepare.find(
                  "if (g_netAutomatchFrontendOnly\n"
                  "        || !gameplay")
              != std::string::npos);
    }

    CHECK(source.find(
              "g_netAutomatchFrontendOnly = spectatorTransition;")
          != std::string::npos);
    CHECK(source.find("g_netAutomatchFrontendOnly = false;")
          != std::string::npos);
}

TEST(AutomatchHarnessPauseIsCooperativeAndExactOptIn) {
    const std::string source = readNativeKoSource();
    CHECK(!source.empty());

    const std::size_t begin = source.find("int doNetplay(FrameDispatchFn orig)");
    const std::size_t end = source.find(
        "\nint doSpectator(FrameDispatchFn orig)", begin);
    CHECK(begin != std::string::npos);
    CHECK(end != std::string::npos);
    const std::string body = source.substr(begin, end - begin);

    const std::size_t transportPump =
        body.find("!netDrainRemoteInputs()");
    const std::size_t pauseRequest =
        body.find("const bool testPauseRequested");
    const std::size_t phaseDelay =
        body.find("if (!netApplyPhaseInputDelay())");
    CHECK(transportPump != std::string::npos);
    CHECK(pauseRequest != std::string::npos);
    CHECK(phaseDelay != std::string::npos);
    CHECK(transportPump < pauseRequest);
    CHECK(pauseRequest < phaseDelay);

    const std::size_t pauseEnd = body.find(
        "\n    InterlockedExchange(&g_netTestPauseAcknowledged, 0);",
        pauseRequest);
    CHECK(pauseEnd != std::string::npos);
    const std::string pause = body.substr(
        pauseRequest, pauseEnd - pauseRequest);
    CHECK(pause.find("g_netAutomatchHarnessEnabled")
          != std::string::npos);
    CHECK(pause.find("&g_netTestPauseRequest")
          != std::string::npos);
    CHECK(pause.find(
              "InterlockedExchange(&g_netTestPauseAcknowledged, 1)")
          != std::string::npos);
    CHECK(pause.find("netPaceSemanticHold()")
          != std::string::npos);
    CHECK(pause.find("return kNetHoldFrameRet;")
          != std::string::npos);
    CHECK(pause.find("orig()") == std::string::npos);
}

TEST(NativeKoFallbackUsesVerifiedNativeFatalPath) {
    const std::string source = readNativeKoSource();
    const std::string fallback = nativeKoFallbackBody(source);
    CHECK(!fallback.empty());

    CHECK(source.find("FunctionId::NativeDamage") != std::string::npos);
    CHECK(source.find("FunctionId::NativeReactionGate")
          != std::string::npos);
    CHECK(source.find("g_nativeDamageFn") != std::string::npos);
    CHECK(source.find("g_nativeReactionGateFn") != std::string::npos);
    CHECK(source.find("kNativeDamageFn") == std::string::npos);
    CHECK(source.find("kNativeReactionGateFn") == std::string::npos);
    CHECK(source.find("constexpr uint32_t kNativeFatalAction = 58u;")
          != std::string::npos);
    CHECK(source.find("constexpr uint32_t kNativeGenericKoCause = 129u;")
          != std::string::npos);
    CHECK(source.find("netAutomatchNativeKoCodeMatches()")
          != std::string::npos);

    CHECK(fallback.find("rollbackEligible()") != std::string::npos);
    CHECK(fallback.find("rd32(kSceneStateAddr) == 2u")
          != std::string::npos);
    CHECK(fallback.find("rd32(kMatchSubAddr) == 0u")
          != std::string::npos);
    CHECK(fallback.find("rd32(addr::kVsFormationRow) <= 1u")
          != std::string::npos);
    CHECK(fallback.find("rd32(addr::kTeamSize) == 0u")
          == std::string::npos);
    CHECK(fallback.find("rd32(kRoundResultStateAddr) == 0u")
          != std::string::npos);
    CHECK(fallback.find(
              "rd32(kNativeFatalHitModeAddr) == 0xFFFFFFFFu")
          != std::string::npos);
    CHECK(fallback.find("rd32(kNativeFatalHitDisableAddr) == 0u")
          != std::string::npos);
    CHECK(fallback.find("loserReadable && winnerReadable && mirrorReadable")
          != std::string::npos);
    CHECK(fallback.find(
              "rd8(loserActive + kFighterPlayerSlotOffset) == loser")
          != std::string::npos);
    CHECK(fallback.find(
              "rd8(winnerActive + kFighterPlayerSlotOffset) == winner")
          != std::string::npos);

    const std::size_t precheck = fallback.find("reactionGate(");
    const std::size_t damage = fallback.find("(void)applyDamage(");
    const std::size_t pending = fallback.find(
        "wr32(loserActive + kFighterPendingActionOffset");
    const std::size_t variant = fallback.find(
        "loserActive + kFighterReactionVariantOffset) = 2u");
    CHECK(precheck != std::string::npos);
    CHECK(damage != std::string::npos);
    CHECK(pending != std::string::npos);
    CHECK(variant != std::string::npos);
    CHECK(precheck < damage);
    CHECK(damage < pending);
    CHECK(pending < variant);
}

TEST(NativeKoFallbackLeavesResultLifecycleToQoh) {
    const std::string source = readNativeKoSource();
    const std::string fallback = nativeKoFallbackBody(source);
    CHECK(!fallback.empty());

    CHECK(fallback.find("wr32(kRoundResultStateAddr")
          == std::string::npos);
    CHECK(fallback.find("wr32(0x625628u")
          == std::string::npos);
    CHECK(fallback.find("wr32(0x62562Cu")
          == std::string::npos);
    CHECK(fallback.find(
              "wr32(loserActive + kFighterRoundLostOffset")
          == std::string::npos);
    CHECK(fallback.find(
              "wr32(loserActive + kFighterRoundEndOffset")
          == std::string::npos);

    CHECK(fallback.find(
              "g_netAutomatchNativeKoLoggedMask |= roundBit;")
          != std::string::npos);
    CHECK(fallback.find("forced native KO fallback")
          != std::string::npos);
    CHECK(fallback.find(
              "forced native KO fallback P%u")
          != std::string::npos);
    CHECK(fallback.find("FORCED NATIVE KO fallback")
          == std::string::npos);
}
