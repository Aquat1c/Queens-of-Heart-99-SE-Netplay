#include "ipc/IpcConfig.h"
#include "game/RuntimeDelayControl.h"
#include "net/PacingBudget.h"
#include "net/Protocol.h"
#include "training/TrainingTypes.h"
#include "input/InputDisplayTypes.h"
#include "training/TrainingHitboxOverlayBridge.h"
#include "qoh99/generated/NativeSymbolCatalog.hpp"
#include "render_dx9_imgui.h"
#include "TestFramework.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string qohProjectRoot() {
    const std::string here = __FILE__;
    const std::size_t source = here.find("src");
    return source == std::string::npos
        ? std::string{}
        : here.substr(0, source);
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    const std::istreambuf_iterator<char> begin(file);
    const std::istreambuf_iterator<char> end;
    std::string text(begin, end);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

std::string readQohSource(const char* relativePath) {
    return readFile(qohProjectRoot() + relativePath);
}

std::string readWorkspaceSource(const char* relativePath) {
    return readFile(qohProjectRoot() + relativePath);
}

std::string sourceSlice(const std::string& source,
                        const char* beginMarker,
                        const char* endMarker) {
    const std::size_t begin = source.find(beginMarker);
    if (begin == std::string::npos) return {};
    const std::size_t end = source.find(endMarker, begin);
    if (end == std::string::npos || end <= begin) return {};
    return source.substr(begin, end - begin);
}

std::string withoutWhitespace(const std::string& source) {
    std::string result;
    result.reserve(source.size());
    for (const unsigned char byte : source) {
        if (!std::isspace(byte))
            result.push_back(static_cast<char>(byte));
    }
    return result;
}

std::size_t countOccurrences(const std::string& source,
                             const char* needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = source.find(needle, offset))
           != std::string::npos) {
        ++count;
        offset += std::char_traits<char>::length(needle);
    }
    return count;
}

qipc::IpcConfig validTrainingLaunch() {
    qipc::IpcConfig config{};
    config.mode = qipc::MODE_OBSERVE;
    config.isHost = 1;
    config.localPlayer = 1;
    config.directBoot = 1;
    config.vsRow = qipc::kVsRow1v1;
    config.trainingEnabled = 1;
    return config;
}

}

TEST(TrainingIpcV12RoleIsExplicitAndFailClosed) {
    CHECK_EQ(qipc::kIpcVersion, 25u);
    CHECK_EQ(sizeof(qipc::IpcConfig::replayPresentationFlags), 2u);
    CHECK_EQ(offsetof(qipc::IpcConfig, modeControls)
                 + sizeof(qipc::IpcConfig::modeControls),
               sizeof(qipc::IpcConfig));
    CHECK_EQ(offsetof(qipc::IpcConfig, compatibilityIdentity)
                 + sizeof(qipc::IpcConfig::compatibilityIdentity),
             offsetof(qipc::IpcConfig, configurationProvenance));
    CHECK_EQ(offsetof(qipc::IpcConfig, configurationProvenance)
                  + sizeof(qipc::IpcConfig::configurationProvenance),
              offsetof(qipc::IpcConfig, spectatorTopologyEnabled));
    CHECK_EQ(offsetof(qipc::IpcConfig, spectatorSequenceBase)
                  + sizeof(qipc::IpcConfig::spectatorSequenceBase),
              offsetof(qipc::IpcConfig, spectatorClientPublisherEnabled));
    CHECK(offsetof(qipc::IpcConfig, trainingEnabled)
          < offsetof(qipc::IpcConfig, compatFlags));

    qipc::IpcConfig ordinary{};
    ordinary.mode = qipc::MODE_OBSERVE;
    CHECK(qipc::validateTrainingLaunch(ordinary) == nullptr);

    const qipc::IpcConfig valid = validTrainingLaunch();
    CHECK(qipc::validateTrainingLaunch(valid) == nullptr);

    qipc::IpcConfig invalid = valid;
    invalid.trainingEnabled = 2;
    CHECK(qipc::validateTrainingLaunch(invalid) != nullptr);
    invalid = valid;
    invalid.mode = qipc::MODE_NETPLAY;
    CHECK(qipc::validateTrainingLaunch(invalid) != nullptr);
    invalid = valid;
    invalid.isHost = 0;
    CHECK(qipc::validateTrainingLaunch(invalid) != nullptr);
    invalid = valid;
    invalid.localPlayer = 2;
    CHECK(qipc::validateTrainingLaunch(invalid) != nullptr);
    invalid = valid;
    invalid.directBoot = 0;
    CHECK(qipc::validateTrainingLaunch(invalid) != nullptr);
    invalid = valid;
    invalid.vsRow = qipc::kVsRow1vCpu;
    CHECK(qipc::validateTrainingLaunch(invalid) != nullptr);
    invalid = valid;
    invalid.probeMode = 1;
    CHECK(qipc::validateTrainingLaunch(invalid) != nullptr);
    invalid = valid;
    invalid.hasSocket = 1;
    CHECK(qipc::validateTrainingLaunch(invalid) != nullptr);
    invalid = valid;
    invalid.hasSpectatorSocket = 1;
    CHECK(qipc::validateTrainingLaunch(invalid) != nullptr);
    invalid = valid;
    invalid.serveSpectators = 1;
    CHECK(qipc::validateTrainingLaunch(invalid) != nullptr);
    invalid = valid;
    invalid.offlineExtraPresent[0] = 1;
    CHECK(qipc::validateTrainingLaunch(invalid) != nullptr);
}

TEST(TrainingReturnDiscardsStaleCasterInputWithoutWideningOtherLaunches) {
    const std::string caster = readQohSource("src/client/Main.cpp");
    const std::string console = readQohSource("src/client/ConsoleUi.cpp");
    CHECK(!caster.empty());
    CHECK(!console.empty());

    const std::string offline = sourceSlice(
        caster, "int runOffline(", "\nuint8_t probeModeFromName(");
    CHECK(!offline.empty());
    const std::string compact = withoutWhitespace(offline);
    CHECK(compact.find(
              "constintresult=launch(s,cfg,INVALID_SOCKET,con,INFINITE,"
              "hiddenDesktop);")
          != std::string::npos);
    CHECK(compact.find(
              "if(trainingEnabled&&con)con->discardPendingInput();"
              "returnresult;")
          != std::string::npos);
    CHECK_EQ(countOccurrences(caster, "discardPendingInput()"), 1u);
    CHECK(console.find("FlushConsoleInputBuffer(_in)")
          != std::string::npos);
}

TEST(TrainingLaunchIsArmedOnlyFromValidatedIpc) {
    const std::string dllMain = readQohSource("src/DllMain.cpp");
    const std::string directBoot =
        readQohSource("src/game/DirectBoot.cpp");
    CHECK(!dllMain.empty());
    CHECK(!directBoot.empty());

    const std::string compactDll = withoutWhitespace(dllMain);
    CHECK(compactDll.find(
              "qoh99::armTraining(haveIpc&&cfg.trainingEnabled!=0);")
          != std::string::npos);

    const std::string selection = sourceSlice(
        directBoot, "void trainingSelectionTick(", "\nvoid tick()");
    CHECK(!selection.empty());
    const std::string compactSelection =
        withoutWhitespace(selection);
    CHECK(compactSelection.find(
              "constfn::ReadLogicalFnreadLogical=fn::readLogical();")
          != std::string::npos);
    CHECK(compactSelection.find("if(!readLogical){") != std::string::npos);
    CHECK(compactSelection.find(
              "static_cast<uint32_t>(readLogical(0))")
          != std::string::npos);
    CHECK(selection.find("kInputReadFn") == std::string::npos);
}

TEST(TrainingLogicalReadIsCacheOnlyAndMenuCaptureNeutralizes) {
    const std::string hooks =
        readQohSource("src/GameHooks.cpp");
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!hooks.empty());
    CHECK(!runtime.empty());
    CHECK(hooks.find(
              "#include \"training/TrainingGameIntegration.inl\"")
          != std::string::npos);

    const std::string inputRead = sourceSlice(
        hooks, "int __fastcall hkInputRead(int player)",
        "\nstruct InputPhaseSample { int32_t x, y, act, facing; uint32_t ringHash; };");
    CHECK(!inputRead.empty());
    const std::size_t trainingGate =
        inputRead.find("trainingOverrideActive(player)");
    const std::size_t offlineGate =
        inputRead.find("netOfflineOverrideActive(player)");
    CHECK(trainingGate != std::string::npos);
    CHECK(offlineGate != std::string::npos);
    CHECK(trainingGate < offlineGate);
    CHECK(inputRead.find("return trainingOverrideMask(player)")
          != std::string::npos);
    CHECK(inputRead.find("GetAsyncKeyState") == std::string::npos);
    CHECK(inputRead.find("SDL_") == std::string::npos);
    CHECK(inputRead.find("joyGet") == std::string::npos);

    const std::string override = sourceSlice(
        runtime, "bool trainingOverrideActive(int player)",
        "\nvoid trainingObserveOfflinePhysicalSnapshot(");
    CHECK(!override.empty());
    const std::string compactOverride =
        withoutWhitespace(override);
    CHECK(compactOverride.find(
              "InterlockedCompareExchange"
              "(&g_trainingMenuCaptured,0,0)!=0")
          != std::string::npos);
    CHECK(compactOverride.find(
              "inttrainingOverrideMask(intplayer)")
          != std::string::npos);
    CHECK(compactOverride.find("return0;")
          != std::string::npos);
    CHECK(compactOverride.find("g_trainingFinalInput[player]")
          != std::string::npos);
}

TEST(TrainingGuardFacingIsDecidedWhenQohReadsTheLane) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());
    const std::string read = sourceSlice(
        runtime, "static uint32_t trainingGuardAwayAtRead(",
        "\nint trainingOverrideMask(int player)");
    CHECK(!read.empty());
    const std::string compactRead = withoutWhitespace(read);
    CHECK(compactRead.find("rd8(fighter+kTrainingFighterFacingOffset)")
          != std::string::npos);
    CHECK(compactRead.find("(mask&kHorizontal)!=arm.awayBit")
          != std::string::npos);

    const std::string mask = sourceSlice(
        runtime, "int trainingOverrideMask(int player)",
        "\nvoid trainingObserveOfflinePhysicalSnapshot(");
    CHECK(withoutWhitespace(mask).find(
              "trainingGuardAwayAtRead(player,g_trainingFinalInput[player])")
          != std::string::npos);

    const std::string decision = sourceSlice(
        runtime, "static qtraining::GuardDecision trainingGuardDecision(",
        "\n}\n");
    CHECK(!decision.empty());
    CHECK(decision.find("input.defenderFacing = defenderFacing;")
          != std::string::npos);
    CHECK(decision.find("kTrainingFighterYOffset") != std::string::npos);
    CHECK(decision.find("kTrainingFighterXOffset") == std::string::npos);
    CHECK(withoutWhitespace(decision).find(
              "input.defenderInTechResult="
              "qtraining::combatActionIsGroundRecovery(")
          != std::string::npos);
}

TEST(TrainingFrontendInputGateIsIsolatedAndPhysicallyRearmed) {
    const std::string hooks =
        readQohSource("src/GameHooks.cpp");
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!hooks.empty());
    CHECK(!runtime.empty());
    const std::string compactRuntime = withoutWhitespace(runtime);

    CHECK(compactRuntime.find(
              "staticvolatileuint32_t"
              "g_trainingLiveE2ELogicalMask=0u;")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "GetEnvironmentVariableA("
              "\"QOH99_TRAINING_LIVE_E2E\"")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "trainingLiveE2EEnvironmentValueExact(")
          != std::string::npos);

    const std::string inputRead = sourceSlice(
        hooks, "int __fastcall hkInputRead(int player)",
        "\nstruct InputPhaseSample { int32_t x, y, act, facing; uint32_t ringHash; };");
    CHECK(!inputRead.empty());
    const std::size_t network =
        inputRead.find("if (g_netActive");
    const std::size_t training =
        inputRead.find("trainingOverrideActive(player)");
    const std::size_t offline =
        inputRead.find("netOfflineOverrideActive(player)");
    CHECK(network != std::string::npos);
    CHECK(training != std::string::npos);
    CHECK(offline != std::string::npos);
    CHECK(network < training);
    CHECK(training < offline);

    const std::string override = sourceSlice(
        runtime, "bool trainingOverrideActive(int player)",
        "\nvoid trainingObserveOfflinePhysicalSnapshot(");
    CHECK(!override.empty());
    const std::string compactOverride =
        withoutWhitespace(override);
    CHECK(compactOverride.find(
              "player==0&&trainingInCharacterSelectFrontend()")
          != std::string::npos);
    CHECK(compactOverride.find(
              "return!armed||g_trainingLiveE2EEnabled;")
          != std::string::npos);
    CHECK(compactOverride.find(
              "g_trainingLiveE2ELogicalMask&1u")
          != std::string::npos);
    CHECK(override.find("GetEnvironmentVariable")
          == std::string::npos);
    CHECK(override.find("SDL_") == std::string::npos);
    CHECK(override.find("GetAsyncKeyState")
          == std::string::npos);

    const std::string observation = sourceSlice(
        runtime,
        "void trainingObserveOfflinePhysicalSnapshot(",
        "\nstatic void trainingArmInternal(");
    CHECK(!observation.empty());
    const std::string compactObservation =
        withoutWhitespace(observation);
    CHECK(compactObservation.find(
              "sample.managedRuntimeArmed="
              "g_offlineManagedArmed;")
          != std::string::npos);
    CHECK(compactObservation.find(
              "sample.physicalNeutralBeforeSocd="
              "p1PhysicalNeutralBeforeSocd;")
          != std::string::npos);
    CHECK(compactObservation.find(
              "g_trainingFrontendInputGate.advance(sample)")
          != std::string::npos);
    CHECK(compactObservation.find("p1Mask==0")
          == std::string::npos);

    const std::string stage = sourceSlice(
        hooks, "if (s_stage == 6)",
        "\nbool netRecorderPublishFrame(");
    CHECK(!stage.empty());
    const std::string compactStage = withoutWhitespace(stage);
    CHECK(compactStage.find(
              "constbooltrainingP1RuntimeReady=")
          != std::string::npos);
    CHECK(compactStage.find(
              "g_offlineManagedArmed="
              "s_started>0&&stageSucceeded;")
          != std::string::npos);
    CHECK(stage.find("qipc::ipcInputReadyEventName")
          != std::string::npos);
    CHECK(stage.find("qipc::ipcInputFailedEventName")
          != std::string::npos);
}

TEST(TrainingDirectBootFailureAndFirstReturnTickAreSurfaced) {
    const std::string directBoot =
        readQohSource("src/game/DirectBoot.cpp");
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!directBoot.empty());
    CHECK(!runtime.empty());

    const std::string returned = sourceSlice(
        directBoot, "if (g_trainingAwaitReturnCharSelect)",
        "\n                if (scene == 3 && s3sub == 1) {");
    CHECK(!returned.empty());
    const std::size_t reset =
        returned.find("g_trainingSelectionPolicy.reset()");
    const std::size_t tick =
        returned.find("trainingSelectionTick(scene, s3sub)");
    CHECK(reset != std::string::npos);
    CHECK(tick != std::string::npos);
    CHECK(reset < tick);

    CHECK(directBoot.find("bool directBootOfflineFailed()")
          != std::string::npos);
    const std::string fallback = sourceSlice(
        directBoot,
        "extern \"C\" __declspec(dllexport) void qoh99np_boot_tick()",
        "\n}");
    CHECK(!fallback.empty());
    CHECK(fallback.find("qgame::g_failed = true")
          != std::string::npos);
    CHECK(runtime.find("qgame::directBootOfflineFailed()")
          != std::string::npos);
    CHECK(runtime.find(
              "\"Training DirectBoot failed before selector completion\"")
          != std::string::npos);
}

TEST(TrainingReturnLatchBlocksStepOwnershipUntilSceneThreeGeneration) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());

    const std::string gate = sourceSlice(
        runtime, "static int __cdecl hkTrainingStepGate()",
        "\nstatic int __cdecl hkTrainingMatchScope()");
    CHECK(!gate.empty());
    const std::size_t latch =
        gate.find("if (g_trainingReturningToSelect)");
    const std::size_t pending =
        gate.find("if (g_trainingStepPending)");
    const std::size_t overlay =
        gate.find("(void)trainingApplyConfigOverlay()");
    const std::size_t nativeGate =
        gate.find("g_trainingOrigStepGate");
    const std::size_t admission =
        gate.find("trainingAdmitAfterNativeGate(nativeResult)");
    CHECK(latch != std::string::npos);
    CHECK(pending != std::string::npos);
    CHECK(overlay != std::string::npos);
    CHECK(nativeGate != std::string::npos);
    CHECK(admission != std::string::npos);
    CHECK(latch < pending);
    CHECK(latch < overlay);
    CHECK(latch < nativeGate);
    CHECK(latch < admission);

    const std::string latchedGate = sourceSlice(
        gate, "if (g_trainingReturningToSelect)",
        "\n    if (g_trainingStepPending)");
    CHECK(!latchedGate.empty());
    const std::string compactLatchedGate =
        withoutWhitespace(latchedGate);
    CHECK(compactLatchedGate.find(
              "g_trainingFinalInput[0]=0u;")
          != std::string::npos);
    CHECK(compactLatchedGate.find(
              "g_trainingFinalInput[1]=0u;")
          != std::string::npos);
    CHECK(compactLatchedGate.find(
              "g_trainingLastNativeReturn=0u;")
          != std::string::npos);
    CHECK(compactLatchedGate.find(
              "g_trainingLastAdmission="
              "StepAdmissionResult::NativeReturnedZero;")
          != std::string::npos);
    CHECK(compactLatchedGate.find("return0;")
          != std::string::npos);
    CHECK(latchedGate.find("trainingApplyConfigOverlay")
          == std::string::npos);
    CHECK(latchedGate.find("g_trainingOrigStepGate")
          == std::string::npos);
    CHECK(latchedGate.find("trainingAdmitAfterNativeGate")
          == std::string::npos);
    CHECK(latchedGate.find("trainingDrainCommands")
          == std::string::npos);
    CHECK(latchedGate.find("kSceneStateAddr")
          == std::string::npos);

    const std::string admissionBody = sourceSlice(
        runtime, "static int trainingAdmitAfterNativeGate(",
        "\nstatic int __cdecl hkTrainingStepGate()");
    CHECK(!admissionBody.empty());
    CHECK(admissionBody.find("trainingDrainCommands(")
          != std::string::npos);

    const std::string override = sourceSlice(
        runtime, "bool trainingOverrideActive(int player)",
        "\nint trainingOverrideMask(int player)");
    CHECK(!override.empty());
    const std::string compactOverride =
        withoutWhitespace(override);
    const std::size_t inputLatch = compactOverride.find(
        "if(g_trainingReturningToSelect)returnfalse;");
    const std::size_t characterSelect = compactOverride.find(
        "trainingInCharacterSelectFrontend()");
    const std::size_t menuCapture = compactOverride.find(
        "InterlockedCompareExchange"
        "(&g_trainingMenuCaptured,0,0)!=0");
    const std::size_t pendingOverride =
        compactOverride.find("returng_trainingStepPending");
    CHECK(inputLatch != std::string::npos);
    CHECK(characterSelect != std::string::npos);
    CHECK(menuCapture != std::string::npos);
    CHECK(pendingOverride != std::string::npos);
    CHECK(inputLatch < characterSelect);
    CHECK(inputLatch < menuCapture);
    CHECK(inputLatch < pendingOverride);

    const std::string liveIngress = sourceSlice(
        runtime, "static void trainingLiveE2EPollCommand(",
        "\nstatic void trainingLiveE2EMirrorStatus(");
    CHECK(!liveIngress.empty());
    CHECK(withoutWhitespace(liveIngress).find(
              "scene==2u&&matchSub==0u"
              "&&!g_trainingReturningToSelect")
          != std::string::npos);

    const std::string newGeneration = sourceSlice(
        runtime, "static void trainingBeginNewGeneration()",
        "\nstatic void trainingOuterTick()");
    CHECK(!newGeneration.empty());
    CHECK(newGeneration.find(
              "g_trainingReturningToSelect = false;")
          != std::string::npos);

    const std::string outerTick = sourceSlice(
        runtime, "static void trainingOuterTick()",
        "\nstatic bool trainingInCharacterSelectFrontend()");
    CHECK(!outerTick.empty());
    const std::string compactOuterTick =
        withoutWhitespace(outerTick);
    CHECK(compactOuterTick.find(
              "if(!g_trainingReturningToSelect){"
              "(void)trainingApplyConfigOverlay();")
          != std::string::npos);
    const std::size_t sceneThree =
        compactOuterTick.find("elseif(scene==3u)");
    CHECK(compactOuterTick.find(
              "elseif(scene==3u){"
              "if(g_trainingHadActiveMatch"
              "||g_trainingReturningToSelect){"
              "trainingBeginNewGeneration();")
          != std::string::npos);
    const std::size_t generation = compactOuterTick.find(
        "trainingBeginNewGeneration()", sceneThree);
    const std::size_t characterSelectLifecycle = compactOuterTick.find(
        "TrainingLifecycleState::CharacterSelect", sceneThree);
    CHECK(sceneThree != std::string::npos);
    CHECK(generation != std::string::npos);
    CHECK(characterSelectLifecycle != std::string::npos);
    CHECK(sceneThree < generation);
    CHECK(generation < characterSelectLifecycle);
}

TEST(TrainingCharacterSelectClearsEveryPlayerFacingSetting) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());

    const std::string settings = sourceSlice(
        runtime, "static void trainingResetPlayerSettings()",
        "\nstatic void trainingBeginNewGeneration()");
    CHECK(!settings.empty());

    static const char* const kMustReset[] = {
        "g_trainingSelectedStateSlot = 0u;",
        "g_trainingGuardMode = GuardMode::Off;",
        "g_trainingGuardDuringPlayback = false;",
        "g_trainingDummyStance = DummyStance::Stand;",
        "g_trainingDummyTech = DummyTech::Off;",
        "g_trainingHitboxMode = qtraining::HitboxDisplayMode::Off;",
        "g_trainingHitboxRenderer = qtraining::kHitboxDefaultRenderer;",
        "g_trainingMeterChargeTarget = TrainingRefillTarget::Both;",
        "g_trainingRefillRequest = 0u;",
        "g_trainingExRestoreTarget = qtraining::ExRestoreTarget{};",
        "g_trainingContactPolicyRequested = "
            "qtraining::TrainingContactPolicySet{};",
        "g_trainingContactPolicyLatched = "
            "qtraining::TrainingContactPolicySet{};",
        "g_trainingResourcePolicies = qtraining::ResourcePolicySet{};",
        "trainingResetRecoverySettings();",
        "trainingResetCombatToolSettings();",
    };
    for (const char* entry : kMustReset)
        CHECK(settings.find(entry) != std::string::npos);

    const std::string newGeneration = sourceSlice(
        runtime, "static void trainingBeginNewGeneration()",
        "\nstatic void trainingOuterTick()");
    CHECK(!newGeneration.empty());
    CHECK(newGeneration.find("trainingResetPlayerSettings();")
          != std::string::npos);

    const std::string arm = sourceSlice(
        runtime, "static void trainingArmInternal(bool enabled)",
        "\n    QLOG(\"[training] explicit offline training armed \"");
    CHECK(!arm.empty());
    CHECK(arm.find("trainingResetPlayerSettings();") != std::string::npos);

    static const char* const kOwnedByTheSharedList[] = {
        "g_trainingExRestoreTarget =",
        "g_trainingContactPolicyRequested =",
        "g_trainingDummyStance =",
        "g_trainingGuardMode =",
        "g_trainingResourcePolicies =",
    };
    for (const char* owned : kOwnedByTheSharedList) {
        CHECK(newGeneration.find(owned) == std::string::npos);
        CHECK(arm.find(owned) == std::string::npos);
    }
}

TEST(TrainingLiveHarnessUsesFiveBoundedNativeConfirmEdges) {
    const std::string harness =
        readQohSource("src/netplay/tests/live_training_e2e.py");
    CHECK(!harness.empty());

    CHECK(harness.find(
              "environment[\"QOH99_TRAINING_LIVE_E2E\"] = \"1\"")
          != std::string::npos);
    CHECK(harness.find(
              "\"live_input_mask\": "
              "\"?g_trainingLiveE2ELogicalMask@\"")
          != std::string::npos);
    CHECK(harness.find(
              "\"frontend_input_armed\": "
              "\"?g_trainingFrontendInputArmed@\"")
          != std::string::npos);
    CHECK(harness.find(
              "\"stage_input_armed\": "
              "\"?g_trainingStageInputArmed@\"")
          != std::string::npos);
    CHECK(countOccurrences(
              harness, "self.set_live_input_mask(1)")
          == 5u);
    CHECK(harness.find(
              "memory.write_u32(ADDR[\"p1_confirm\"]")
          == std::string::npos);
    CHECK(harness.find(
              "memory.write_u32(ADDR[\"p2_confirm\"]")
          == std::string::npos);
    CHECK(harness.find("def match_start_ready()")
          != std::string::npos);
    CHECK(harness.find(
              "\"native P1 first confirm edge state 1->2\"")
          != std::string::npos);
    CHECK(harness.find(
              "\"native P1 neutral history edge while selector remains state 2\"")
          != std::string::npos);
    CHECK(harness.find(
              "\"native P1 second confirm edge state 2->4 and \"")
          != std::string::npos);
    CHECK(harness.find(
              "\"native P2 first confirm edge state 1->2\"")
          != std::string::npos);
    CHECK(harness.find(
              "\"native P2 neutral history edge while selector remains state 2\"")
          != std::string::npos);
    CHECK(harness.find(
              "\"native P2 second confirm edge state 2->4\"")
          != std::string::npos);
    CHECK(harness.find(
              "\"released stage-confirm edge, scene 2, and P2 normalization\"")
          != std::string::npos);
    CHECK(harness.find(
              "\"one native Right edge selecting stage 2\"")
          != std::string::npos);
    CHECK(harness.find(
              "stage_right_state[\"stage_cursor\"] != 1")
          != std::string::npos);
    CHECK(harness.find("\"native_confirm_pulses\": 5")
          != std::string::npos);
    CHECK(harness.find("\"native_press_release_edges\": 5")
          != std::string::npos);
    CHECK(harness.find("\"native_stage_direction_edges\": 1")
          != std::string::npos);
    CHECK(harness.find("SELECTOR_INPUT_HISTORY = (")
          != std::string::npos);
    CHECK(harness.find("def selector_input_history(")
          != std::string::npos);
    CHECK(harness.find("\"target_hold_dispatch_frames\": 2")
          != std::string::npos);
    CHECK(harness.find("\"released_before_terminal_wait\": True")
          != std::string::npos);
    CHECK(harness.find("selection_timeout = 15.0")
          != std::string::npos);
    CHECK(harness.find(
              "[training-select] held P2 inactive after native "
              "same-call handoff")
          != std::string::npos);

    const std::string selection = sourceSlice(
        harness, "    def drive_character_select(",
        "\n    def fighter_snapshot(");
    CHECK(!selection.empty());
    const std::size_t p1FirstAssert =
        selection.find("self.set_live_input_mask(1)");
    const std::size_t p1StateTwo =
        selection.find(
            "\"native P1 first confirm edge state 1->2\"",
            p1FirstAssert);
    const std::size_t p1FirstRelease =
        selection.find("self.set_live_input_mask(0)", p1StateTwo);
    const std::size_t p1Neutral = selection.find(
        "\"native P1 neutral history edge while selector remains state 2\"",
        p1FirstRelease);
    const std::size_t p1SecondAssert =
        selection.find("self.set_live_input_mask(1)", p1Neutral);
    const std::size_t p1StateFour = selection.find(
        "\"native P1 second confirm edge state 2->4 and \"",
        p1SecondAssert);
    const std::size_t p1SecondRelease =
        selection.find("self.set_live_input_mask(0)", p1StateFour);
    const std::size_t p2FirstAssert =
        selection.find("self.set_live_input_mask(1)", p1SecondRelease);
    const std::size_t p2StateTwo =
        selection.find(
            "\"native P2 first confirm edge state 1->2\"",
            p2FirstAssert);
    const std::size_t p2FirstRelease =
        selection.find("self.set_live_input_mask(0)", p2StateTwo);
    const std::size_t p2Neutral = selection.find(
        "\"native P2 neutral history edge while selector remains state 2\"",
        p2FirstRelease);
    const std::size_t p2SecondAssert =
        selection.find("self.set_live_input_mask(1)", p2Neutral);
    const std::size_t p2StateFour = selection.find(
        "\"native P2 second confirm edge state 2->4\"",
        p2SecondAssert);
    const std::size_t p2SecondRelease =
        selection.find("self.set_live_input_mask(0)", p2StateFour);
    const std::size_t stageAssert =
        selection.find("self.set_live_input_mask(1)", p2SecondRelease);
    CHECK(p1FirstAssert != std::string::npos);
    CHECK(p1StateTwo != std::string::npos);
    CHECK(p1FirstRelease != std::string::npos);
    CHECK(p1Neutral != std::string::npos);
    CHECK(p1SecondAssert != std::string::npos);
    CHECK(p1StateFour != std::string::npos);
    CHECK(p1SecondRelease != std::string::npos);
    CHECK(p2FirstAssert != std::string::npos);
    CHECK(p2StateTwo != std::string::npos);
    CHECK(p2FirstRelease != std::string::npos);
    CHECK(p2Neutral != std::string::npos);
    CHECK(p2SecondAssert != std::string::npos);
    CHECK(p2StateFour != std::string::npos);
    CHECK(p2SecondRelease != std::string::npos);
    CHECK(stageAssert != std::string::npos);
    CHECK(p1FirstAssert < p1StateTwo);
    CHECK(p1StateTwo < p1FirstRelease);
    CHECK(p1FirstRelease < p1Neutral);
    CHECK(p1Neutral < p1SecondAssert);
    CHECK(p1SecondAssert < p1StateFour);
    CHECK(p1StateFour < p1SecondRelease);
    CHECK(p1SecondRelease < p2FirstAssert);
    CHECK(p2FirstAssert < p2StateTwo);
    CHECK(p2StateTwo < p2FirstRelease);
    CHECK(p2FirstRelease < p2Neutral);
    CHECK(p2Neutral < p2SecondAssert);
    CHECK(p2SecondAssert < p2StateFour);
    CHECK(p2StateFour < p2SecondRelease);
    CHECK(p2SecondRelease < stageAssert);

    const std::string p1Confirmed = sourceSlice(
        selection,
        "        def p1_confirmed() -> dict[str, int] | None:",
        "\n        self.set_live_input_mask(");
    const std::string p2Confirmed = sourceSlice(
        selection,
        "        def p2_confirmed() -> dict[str, int] | None:",
        "\n        self.set_live_input_mask(1)");
    CHECK(!p1Confirmed.empty());
    CHECK(!p2Confirmed.empty());
    CHECK(p1Confirmed.find(
              "state[\"live_input_mask\"] == 1")
          != std::string::npos);
    CHECK(p2Confirmed.find(
              "state[\"live_input_mask\"] == 1")
          != std::string::npos);
}

TEST(TrainingNativeSeamsAreQualifiedBeforeTransactionalHooks) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());
    const std::string compact = withoutWhitespace(runtime);

    CHECK(compact.find(
              "kTrainingMatchScopeFn=0x421EE0u")
          != std::string::npos);
    CHECK(compact.find(
              "kTrainingStepGateFn=0x41A0E0u")
          != std::string::npos);
    CHECK(compact.find(
              "kTrainingDamageFn=0x42D760u")
          != std::string::npos);
    CHECK(compact.find(
              "kTrainingSelectorConfirmFn=0x434410u")
          != std::string::npos);
    CHECK(compact.find(
              "kTrainingResetFn=0x40AAD0u")
          != std::string::npos);
    CHECK(compact.find(
              "kTrainingThreatNowFn=0x4428B0u")
          != std::string::npos);
    CHECK(compact.find(
              "kTrainingThreatNextFn=0x442AB0u")
          != std::string::npos);

    const std::string verification = sourceSlice(
        runtime, "static bool trainingVerifyNativeSeams()",
        "\nstatic void trainingRemoveCreatedHook(");
    CHECK(!verification.empty());
    CHECK(verification.find("kMatchScopeSig") != std::string::npos);
    CHECK(verification.find("kStepGateSig") != std::string::npos);
    CHECK(verification.find("kDamageSig") != std::string::npos);
    CHECK(verification.find("kSelectorConfirmSig")
          != std::string::npos);
    CHECK(verification.find("kResetSig") != std::string::npos);
    CHECK(verification.find("kCollisionSig") != std::string::npos);
    CHECK(verification.find("kFacingSig") != std::string::npos);
    CHECK(verification.find("kClampSig") != std::string::npos);
    CHECK(verification.find("kThreatNowSig") != std::string::npos);
    CHECK(verification.find("kThreatNextSig") != std::string::npos);
    const std::string compactVerification =
        withoutWhitespace(verification);
    CHECK(compactVerification.find(
              "kSelectorConfirmSig[]={"
              "0x83,0xEC,0x1C,0x53,0x55,0x56,0x8B,0xF1,"
              "0x33,0xD2,0x57,0x8D,0x04,0xF5,"
              "0x00,0x00,0x00,0x00,};")
          != std::string::npos);
    CHECK(countOccurrences(verification, "se::verifyCodeBytes(")
          >= 10u);

    const std::string install = sourceSlice(
        runtime, "static bool installTrainingHooksOnce()",
        "\nstatic void trainingBeginNewGeneration()");
    CHECK(!install.empty());
    CHECK(install.find(
              "g_trainingHooksSupported = trainingVerifyNativeSeams()")
          != std::string::npos);
    const std::size_t verify =
        install.find("trainingVerifyNativeSeams()");
    const std::size_t create =
        install.find("MH_CreateHook(");
    CHECK(verify != std::string::npos);
    CHECK(create != std::string::npos);
    CHECK(verify < create);
    CHECK(install.find(
              "reinterpret_cast<void*>(kTrainingSelectorConfirmFn)")
          != std::string::npos);
    CHECK(install.find(
              "reinterpret_cast<void*>(&hkTrainingSelectorConfirm)")
          != std::string::npos);
    CHECK(install.find(
              "&g_trainingOrigSelectorConfirm")
          != std::string::npos);
    CHECK(countOccurrences(install, "kTrainingSelectorConfirmFn")
          >= 4u);
}

TEST(TrainingSelectorSameCallSeamIsPostNativeAndTrainingOnly) {
    const std::string hooks =
        readQohSource("src/GameHooks.cpp");
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!hooks.empty());
    CHECK(!runtime.empty());

    const std::string selector = sourceSlice(
        runtime,
        "static int __fastcall hkTrainingSelectorConfirm(",
        "\nstatic int __fastcall hkTrainingDamage(");
    CHECK(!selector.empty());
    CHECK(countOccurrences(
              selector, "g_trainingOrigSelectorConfirm(")
          == 1u);
    const std::size_t nativeCall =
        selector.find("g_trainingOrigSelectorConfirm(selectorPlayer)");
    const std::size_t decision = selector.find(
        "decideTrainingSameCallConfirm(sample)");
    const std::size_t hold =
        selector.find("wr32(kTrainingP2SelectStateAddr, 0u)");
    CHECK(nativeCall != std::string::npos);
    CHECK(decision != std::string::npos);
    CHECK(hold != std::string::npos);
    CHECK(nativeCall < decision);
    CHECK(decision < hold);
    CHECK(selector.find(
              "sample.explicitTraining = g_trainingLaunchRequested")
          != std::string::npos);
    CHECK(selector.find(
              "TrainingSameCallConfirmAction::FailClosed")
          != std::string::npos);
    CHECK(selector.find("trainingFailMutation(")
          != std::string::npos);

    const std::string installHooks = sourceSlice(
        hooks, "bool installHooks(RunMode mode)",
        "\nvoid armTraining(");
    CHECK(!installHooks.empty());
    const std::string compactInstall =
        withoutWhitespace(installHooks);
    CHECK(compactInstall.find(
              "if(explicitTrainingMode"
              "&&!installTrainingHooksOnce())")
          != std::string::npos);
    CHECK(compactInstall.find(
              "if(explicitTrainingMode"
              "&&!installTrainingHooksOnce()){")
          != std::string::npos);
}

TEST(TrainingStepHasMandatoryScratchAndDamageFloorsAtZero) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());

    const std::string capture = sourceSlice(
        runtime, "static bool trainingCaptureSnapshot(",
        "\nstatic bool trainingSlotCompatible(");
    CHECK(!capture.empty());
    const std::size_t snapshot =
        capture.find("g_rb.snapshotTo(slot.bytes.data())");
    const std::size_t fpu = capture.find("fxSaveTo(slot.fpu.bytes)");
    CHECK(snapshot != std::string::npos);
    CHECK(fpu != std::string::npos);
    CHECK(snapshot < fpu);

    const std::string admission = sourceSlice(
        runtime, "static int trainingAdmitAfterNativeGate(",
        "\nstatic int __cdecl hkTrainingStepGate()");
    CHECK(!admission.empty());
    const std::size_t mandatory =
        admission.rfind("trainingCaptureSnapshot(g_trainingScratch)");
    const std::size_t compose =
        admission.find("trainingComposeInputForStep()");
    const std::size_t pending =
        admission.find("g_trainingStepPending = true");
    CHECK(mandatory != std::string::npos);
    CHECK(compose != std::string::npos);
    CHECK(pending != std::string::npos);
    CHECK(mandatory < compose);
    CHECK(compose < pending);

    const std::string damage = sourceSlice(
        runtime, "static int __fastcall hkTrainingDamage(",
        "\nstatic bool trainingVerifyNativeSeams()");
    CHECK(!damage.empty());
    const std::string compactDamage =
        withoutWhitespace(damage);
    CHECK(compactDamage.find("slot>=4u||!active||!mirror")
          != std::string::npos);
    CHECK(damage.find(
              "rd8(active + kFighterPlayerSlotOffset)")
          == std::string::npos);
    CHECK(damage.find(
              "rd8(mirror + kFighterPlayerSlotOffset)")
          == std::string::npos);
    CHECK(compactDamage.find("rangeReadable(life,16u)")
          != std::string::npos);
    CHECK(compactDamage.find(
              "constint64_tpreEffective=total-pending;")
          != std::string::npos);
    CHECK(compactDamage.find("preEffective<0")
          != std::string::npos);
    const std::size_t original =
        compactDamage.find("constintresult=g_trainingOrigDamage");
    const std::size_t floor =
        compactDamage.find("if(effective<0)");
    const std::size_t zeroEffective = compactDamage.find(
        "wr32(life+kBattleLifeDamageOffset,"
        "static_cast<uint32_t>(nativeTotal));");
    const std::size_t restoreFatal = compactDamage.find(
        "wr32(active+kTrainingFighterFatalCauseOffset,"
        "activeFatalCause);");
    CHECK(original != std::string::npos);
    CHECK(floor != std::string::npos);
    CHECK(zeroEffective != std::string::npos);
    CHECK(restoreFatal != std::string::npos);
    CHECK(original < floor);
    CHECK(floor < zeroEffective);
    CHECK(zeroEffective < restoreFatal);
    CHECK(compactDamage.rfind("returnresult;")
          != std::string::npos);
}

TEST(TrainingPositionResetRestoresRoundCharacterStateThenCommitsPlacement) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());

    const std::string collision = sourceSlice(
        runtime,
        "static bool trainingCurrentCollisionRectangle(",
        "\nstatic bool trainingCurrentHorizontalBounds(");
    CHECK(!collision.empty());
    const std::string compactCollision =
        withoutWhitespace(collision);
    CHECK(compactCollision.find(
              "constuintptr_tcollisionTable=rd32(fighter+32u);")
          != std::string::npos);
    CHECK(compactCollision.find(
              "trainingCheckedScaledPointer("
              "collisionTable,collisionIndex,8u,8u,"
              "collisionRecord)")
          != std::string::npos);
    CHECK(compactCollision.find(
              "collision(static_cast<int>(fighter),"
              "static_cast<int>(collisionIndex),"
              "nativeRectangle)!=1")
          != std::string::npos);

    const std::string clamp = sourceSlice(
        runtime, "static bool trainingClampFighterToWorld(",
        "\nstatic bool trainingFighterPositionMatches(");
    CHECK(!clamp.empty());
    const std::string compactClamp = withoutWhitespace(clamp);
    CHECK(compactClamp.find(
              "iteration<kTrainingWorldClampIterationCap")
          != std::string::npos);
    CHECK(compactClamp.find(
              "nativeResult!=1&&nativeResult!=2")
          != std::string::npos);
    CHECK(compactClamp.find(
              "after!=static_cast<int32_t>(expected)")
          != std::string::npos);
    CHECK(compactClamp.rfind("returnfalse;")
          != std::string::npos);

    CHECK(runtime.find("trainingApplyNativePositionReset")
          == std::string::npos);
    CHECK(runtime.find("trainingRejectPositionMutation")
          == std::string::npos);
    CHECK(runtime.find("using TrainingResetFn") == std::string::npos);
    CHECK(runtime.find("reinterpret_cast<TrainingResetFn>")
          == std::string::npos);

    const std::string commit = sourceSlice(
        runtime, "static bool trainingPositionCommitMatches(",
        "\nstatic qtraining::PositionMathConfig");
    CHECK(!commit.empty());
    CHECK(countOccurrences(
              commit, "trainingFighterPositionMatches(")
          == 4u);
    CHECK(commit.find("trainingCollisionPairSeparated(")
          != std::string::npos);
    CHECK(commit.find("kTrainingCameraPreviousAddr")
          != std::string::npos);
    CHECK(commit.find("kTrainingCameraParityAddr")
          != std::string::npos);
    CHECK(commit.find("kTrainingCameraYAddr")
          != std::string::npos);

    const std::string fighter = sourceSlice(
        runtime, "static bool trainingFighterPositionMatches(",
        "\nstatic bool trainingCollisionPairSeparated(");
    CHECK(!fighter.empty());
    CHECK(fighter.find("kTrainingFighterPreviousXOffset")
          != std::string::npos);
    CHECK(fighter.find("kTrainingFighterFacingOffset")
          != std::string::npos);
    CHECK(fighter.find("kTrainingFighterFacingSnapshotOffset")
          != std::string::npos);

    CHECK(commit.find("absoluteSeparation != plan.anchorSeparation")
          != std::string::npos);
    CHECK(commit.find("config.anchorSeparation") == std::string::npos);

    const std::string apply = sourceSlice(
        runtime, "static bool trainingApplyPositionPreset(",
        "\nstatic qtraining::GuardDecision trainingGuardDecision(");
    CHECK(!apply.empty());
    const std::size_t setFacing = apply.find("trainingSetFacing(");
    const std::size_t bounds =
        apply.find("trainingCurrentHorizontalBounds(");
    const std::size_t planned = apply.find("planPositionPreset(");
    const std::size_t write = apply.find("trainingWritePosition(");
    const std::size_t clampCall =
        apply.find("trainingClampFighterToWorld(");
    const std::size_t camera = apply.find("wr32(kTrainingCamera");
    const std::size_t verify =
        apply.find("trainingPositionCommitMatches(");
    CHECK(setFacing != std::string::npos);
    CHECK(bounds != std::string::npos);
    CHECK(planned != std::string::npos);
    CHECK(write != std::string::npos);
    CHECK(clampCall != std::string::npos);
    CHECK(camera != std::string::npos);
    CHECK(verify != std::string::npos);
    CHECK(setFacing < bounds);
    CHECK(bounds < planned);
    CHECK(planned < write);
    CHECK(write < clampCall);
    CHECK(clampCall < camera);
    CHECK(camera < verify);
    CHECK(countOccurrences(apply, "trainingWritePosition(") == 4u);
    CHECK(countOccurrences(apply, "wr32(kTrainingCamera") == 4u);
    CHECK(apply.find("kTrainingRoundCounterAddr") == std::string::npos);
    CHECK(apply.find("kTrainingWins") == std::string::npos);
    CHECK(apply.find("kBattleLife") == std::string::npos);

    const std::string pairReset = sourceSlice(
        runtime, "static bool trainingResetPairToPreset(PositionPreset preset) {",
        "\n}");
    CHECK(!pairReset.empty());
    const std::size_t restorable =
        pairReset.find("trainingRoundCharacterStateRestorable()");
    const std::size_t captureResources =
        pairReset.find("trainingCaptureResetResourceState(resources)");
    const std::size_t despawn =
        pairReset.find("trainingDespawnTransientActors()");
    const std::size_t restoreActors =
        pairReset.find("trainingRestoreRoundCharacterActors()");
    const std::size_t clearCombo =
        pairReset.find("trainingClearComboReadout()");
    const std::size_t restoreRegions =
        pairReset.find("trainingRestoreRoundCharacterRegions()");
    const std::size_t place =
        pairReset.find("trainingApplyPositionPreset(preset)");
    const std::size_t applyResources =
        pairReset.find("trainingApplyResetResourceState(resources)");
    CHECK(restorable != std::string::npos);
    CHECK(captureResources != std::string::npos);
    CHECK(clearCombo != std::string::npos);
    CHECK(despawn != std::string::npos);
    CHECK(restoreActors != std::string::npos);
    CHECK(restoreRegions != std::string::npos);
    CHECK(place != std::string::npos);
    CHECK(applyResources != std::string::npos);
    CHECK(restorable < captureResources);
    CHECK(captureResources < despawn);
    CHECK(despawn < restoreActors);
    CHECK(restoreActors < clearCombo);
    CHECK(clearCombo < restoreRegions);
    CHECK(restoreRegions < place);
    CHECK(place < applyResources);
    CHECK(apply.find("trainingForceFighterIdle(") == std::string::npos);

    const std::string edges = sourceSlice(
        runtime, "static bool trainingApplyActionEdges(",
        "\nstatic bool trainingAdmitPausedFrame()");
    CHECK(!edges.empty());
    const std::size_t reset = edges.find(
        "fired(qtraining::TrainingAction::ResetPosition)");
    const std::size_t returnToSelect = edges.find(
        "fired(qtraining::TrainingAction::ReturnToCharacterSelect)");
    const std::size_t request = edges.find("trainingResetPairToPreset(");
    CHECK(reset != std::string::npos);
    CHECK(returnToSelect != std::string::npos);
    CHECK(request != std::string::npos);
    CHECK(reset < returnToSelect);
    CHECK(request < returnToSelect);
}

TEST(TrainingConfigSaveAndUiProviderPreserveOwnershipBoundaries) {
    const std::string hooks =
        readQohSource("src/GameHooks.cpp");
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string exports =
        readWorkspaceSource("ddraw_wrapper/ddraw.def");
    CHECK(!hooks.empty());
    CHECK(!runtime.empty());
    CHECK(!wrapper.empty());
    CHECK(!exports.empty());

    const std::string configSave = sourceSlice(
        hooks, "unsigned int __fastcall hkConfigSave(",
        "\nbool netReadActiveMatchConfig(");
    CHECK(!configSave.empty());
    const std::size_t stage = configSave.find(
        "trainingStageOriginalConfigForSave(trainingRuntime)");
    const std::size_t nativeSave =
        configSave.find("g_origConfigSave(path, edx)");
    const std::size_t restore = configSave.find(
        "trainingRestoreRuntimeConfigAfterSave(trainingRuntime)");
    CHECK(stage != std::string::npos);
    CHECK(nativeSave != std::string::npos);
    CHECK(restore != std::string::npos);
    CHECK(stage < nativeSave);
    CHECK(nativeSave < restore);

    const std::string provider = sourceSlice(
        runtime, "static void trainingTryRegisterUi(",
        "\nstatic bool trainingVerifyNativeSeams()");
    CHECK(!provider.empty());
    CHECK(provider.find(
              "\"EFZDX9_RegisterTrainingUiProvider\"")
          != std::string::npos);
    CHECK(provider.find("&trainingStatusProvider")
          != std::string::npos);
    CHECK(provider.find("&trainingCommandEnqueue")
          != std::string::npos);
    CHECK(provider.find("&trainingMenuCapture")
          != std::string::npos);
    CHECK(provider.find("trainingPublishNativeHudOnlyStatus();")
          != std::string::npos);
    CHECK(provider.find("trainingPublishStatus();") != std::string::npos);

    const std::string replayHud = sourceSlice(
        runtime, "static void trainingReplayNativeHudTick()",
        "\nstatic void trainingOuterTick()");
    CHECK(!replayHud.empty());
    CHECK(replayHud.find("trainingPublishNativeHudOnlyStatus();")
          != std::string::npos);
    CHECK(replayHud.find("trainingPublishStatus();") == std::string::npos);
    CHECK(replayHud.find("!= TrainingCommandKind::SetNativeHudMask")
          != std::string::npos);
    CHECK(replayHud.find("CommandAckResult::RejectedUnavailable")
          != std::string::npos);
    CHECK(replayHud.find("trainingOuterTick") == std::string::npos);
    CHECK(replayHud.find("trainingCaptureSnapshot") == std::string::npos);
    CHECK(replayHud.find("trainingApplyCommand") == std::string::npos);
    CHECK(replayHud.find("trainingDrainCommands") == std::string::npos);

    CHECK(!hooks.empty());
    CHECK(hooks.find("else trainingReplayNativeHudTick();")
          != std::string::npos);
    CHECK_EQ(countOccurrences(hooks, "trainingReplayNativeHudTick();"), 2u);

    CHECK(wrapper.find(
              "extern \"C\" void __stdcall "
              "EFZDX9_RegisterTrainingUiProvider(")
          != std::string::npos);
    CHECK(exports.find(
              "EFZDX9_RegisterTrainingUiProvider @15")
          != std::string::npos);
}

TEST(TrainingNoResultInvariantIncludesBothOutcomeScalars) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());
    const std::string compact = withoutWhitespace(runtime);

    CHECK(compact.find(
              "kTrainingOutcomeP1Addr=0x625620u")
          != std::string::npos);
    CHECK(compact.find(
              "kTrainingOutcomeP2Addr=0x625624u")
          != std::string::npos);
    CHECK(compact.find(
              "g_trainingBaseline.outcomeP1="
              "rd32(kTrainingOutcomeP1Addr);")
          != std::string::npos);
    CHECK(compact.find(
              "g_trainingBaseline.outcomeP2="
              "rd32(kTrainingOutcomeP2Addr);")
          != std::string::npos);

    const std::string invariants = sourceSlice(
        runtime, "static bool trainingInvariantsHold()",
        "\nstatic bool trainingPrepareRollbackLayout()");
    CHECK(!invariants.empty());
    const std::string compactInvariants =
        withoutWhitespace(invariants);
    CHECK(compactInvariants.find(
              "rd32(kTrainingOutcomeP1Addr)=="
              "g_trainingBaseline.outcomeP1")
          != std::string::npos);
    CHECK(compactInvariants.find(
              "rd32(kTrainingOutcomeP2Addr)=="
              "g_trainingBaseline.outcomeP2")
          != std::string::npos);

    const std::string status = sourceSlice(
        runtime, "static void trainingPublishStatus()",
        "\nstatic BOOL __stdcall trainingStatusProvider(");
    CHECK(!status.empty());
    const std::string compactStatus = withoutWhitespace(status);
    CHECK(compactStatus.find(
              "status.endlessRoundEffective="
              "status.inMatchReady!=0u&&trainingInvariantsHold()")
          != std::string::npos);
}

TEST(TrainingRestoreIsTransactionalAudioSafeAndFpuFinal) {
    const std::string hooks =
        readQohSource("src/GameHooks.cpp");
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!hooks.empty());
    CHECK(!runtime.empty());

    const std::string stop = sourceSlice(
        runtime, "static bool trainingStopTransientSfx()",
        "\nstatic bool trainingApplyConfigOverlay()");
    CHECK(!stop.empty());
    CHECK(countOccurrences(
              stop, "callNativeSoundStopPreservingFpu(")
          == 2u);
    CHECK(stop.find("g_origSoundStop(endpoint")
          == std::string::npos);
    CHECK(stop.find("g_origSoundStop(sample")
          == std::string::npos);

    const std::string finish = sourceSlice(
        runtime, "static bool trainingFinishRestoredImage(",
        "\nstatic bool trainingRestoreSnapshot(");
    CHECK(!finish.empty());
    const std::size_t timelineReset =
        finish.find("trainingResetTimelineMachines(true);");
    const std::size_t macroStop =
        finish.find("g_trainingMacros.requestStop();");
    const std::size_t postwriteValidation =
        finish.rfind("g_rb.validateLive(");
    const std::size_t audio =
        finish.find("trainingStopTransientSfx()");
    const std::size_t fpu = finish.rfind("fxRestoreFrom(");
    CHECK(countOccurrences(finish, "g_rb.validateLive(") == 2u);
    CHECK(timelineReset != std::string::npos);
    CHECK(macroStop != std::string::npos);
    CHECK(postwriteValidation != std::string::npos);
    CHECK(audio != std::string::npos);
    CHECK(fpu != std::string::npos);
    CHECK(timelineReset < macroStop);
    CHECK(macroStop < postwriteValidation);
    CHECK(postwriteValidation < audio);
    CHECK(audio < fpu);

    const std::string load = sourceSlice(
        runtime, "static CommandAckResult trainingLoadState(",
        "\nstatic CommandAckResult trainingBeginReturnToCharacterSelect()");
    CHECK(!load.empty());
    CHECK(load.find("trainingRestoreScratchOrFailClosed(")
          != std::string::npos);

    const std::string drain = sourceSlice(
        runtime,
        "static bool trainingDrainCommands(",
        "\nstatic bool trainingDrainFailedRecoveryCommands(");
    CHECK(!drain.empty());
    CHECK(drain.find("g_trainingMutationFailed")
          != std::string::npos);
    const std::size_t dequeue =
        drain.find("g_trainingCommands.tryDequeue(command)");
    const std::size_t capture =
        drain.find("trainingCaptureSnapshot(g_trainingScratch)");
    CHECK(dequeue != std::string::npos);
    CHECK(capture != std::string::npos);
    CHECK(dequeue < capture);
    const std::size_t applyCommand =
        drain.find("trainingApplyCommand(command)");
    const std::size_t acknowledge =
        drain.find("trainingAcknowledgeCommand(command, result)");
    const std::size_t classify =
        drain.find("commandOwnsNativeImageTransaction(");
    const std::size_t consumed =
        drain.find("imageTransactionConsumed = true");
    const std::size_t stopBatch =
        drain.find("break;", consumed);
    CHECK(applyCommand != std::string::npos);
    CHECK(acknowledge != std::string::npos);
    CHECK(classify != std::string::npos);
    CHECK(consumed != std::string::npos);
    CHECK(stopBatch != std::string::npos);
    CHECK(applyCommand < acknowledge);
    CHECK(acknowledge < classify);
    CHECK(classify < consumed);
    CHECK(consumed < stopBatch);

    const std::string admission = sourceSlice(
        runtime, "static int trainingAdmitAfterNativeGate(",
        "\nstatic int __cdecl hkTrainingStepGate()");
    CHECK(!admission.empty());
    const std::size_t pollCall =
        admission.find("g_trainingInput.pollActions(");
    const std::size_t mutatingGuard =
        admission.find("trainingImageMutatingActionMask()");
    const std::size_t drainCall =
        admission.find("trainingDrainCommands(");
    const std::size_t actionTable =
        admission.find("trainingApplyActionEdges(actions,");
    const std::size_t postCommandSnapshot =
        admission.rfind(
            "trainingCaptureSnapshot(g_trainingScratch)");
    const std::size_t pauseHold =
        admission.find("trainingAdmitPausedFrame()");
    CHECK(pollCall != std::string::npos);
    CHECK(mutatingGuard != std::string::npos);
    CHECK(drainCall != std::string::npos);
    CHECK(actionTable != std::string::npos);
    CHECK(postCommandSnapshot != std::string::npos);
    CHECK(pauseHold != std::string::npos);
    CHECK(pollCall < mutatingGuard);
    CHECK(mutatingGuard < drainCall);
    CHECK(drainCall < actionTable);
    CHECK(actionTable < postCommandSnapshot);
    CHECK(postCommandSnapshot < pauseHold);
    CHECK(admission.find("wr8(kTrainingPauseAddr") == std::string::npos);
    CHECK(admission.find("g_trainingResetHeldPrevious")
          == std::string::npos);
    CHECK(admission.find("g_trainingPhysical.heldActions")
          != std::string::npos);
    CHECK(admission.find("g_trainingCommands.approximateDepth()")
          == std::string::npos);

    const std::string scratch = sourceSlice(
        runtime, "static bool trainingRestoreScratchFailSafe()",
        "\nstatic bool trainingFinalizePendingStep()");
    CHECK(!scratch.empty());
    CHECK(scratch.find("trainingFinishRestoredImage(")
          != std::string::npos);

    const std::string install = sourceSlice(
        hooks, "bool installHooks(RunMode mode)",
        "\nvoid armTraining(");
    CHECK(!install.empty());
    const std::string compactInstall = withoutWhitespace(install);
    CHECK(compactInstall.find(
              "explicitTrainingMode&&"
              "(!g_configSaveHooked||!g_soundStopHooked||"
              "g_origSoundStop==nullptr)")
          != std::string::npos);
}

TEST(TrainingLiveMailboxUsesRealQueueAndPhysicalSnapshotBoundary) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    const std::string harness =
        readQohSource("src/netplay/tests/live_training_e2e.py");
    CHECK(!runtime.empty());
    CHECK(!harness.empty());

    const std::string compactRuntime = withoutWhitespace(runtime);
    CHECK(compactRuntime.find(
              "structTrainingLiveE2EMailboxV1{")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "static_assert(sizeof(TrainingLiveE2EMailboxV1)==112u")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "returng_trainingLiveE2EEnabled"
              "&&g_trainingLaunchRequested&&!g_netActive"
              "&&g_mode==RunMode::Observe;")
          != std::string::npos);

    const std::string poll = sourceSlice(
        runtime, "static void trainingLiveE2EPollCommand(",
        "\nstatic void trainingLiveE2EMirrorStatus(");
    CHECK(!poll.empty());
    const std::string compactPoll = withoutWhitespace(poll);
    CHECK(compactPoll.find(
              "TrainingCommandV1command{};")
          != std::string::npos);
    CHECK(compactPoll.find(
              "command.trainingGeneration=g_trainingGeneration;")
          != std::string::npos);
    CHECK(compactPoll.find(
              "g_trainingCommands.tryEnqueue(&command,sizeof(command))")
          != std::string::npos);
    CHECK(compactPoll.find(
              "g_trainingGeneration=="
              "g_trainingLiveE2EMailbox.ingressGeneration")
          != std::string::npos);
    CHECK(poll.find("trainingApplyCommand(") == std::string::npos);

    const std::string observation = sourceSlice(
        runtime,
        "void trainingObserveOfflinePhysicalSnapshot(",
        "\nstatic void trainingArmInternal(");
    CHECK(!observation.empty());
    const std::string compactObservation =
        withoutWhitespace(observation);
    CHECK(compactObservation.find(
              "liveGameplayInput"
              "?g_trainingLiveE2EMailbox.gameplayP1Mask:0u")
          != std::string::npos);
    CHECK(compactObservation.find(
              "g_trainingPhysical.p1Mask=effectiveP1Mask;")
          != std::string::npos);
    CHECK(compactObservation.find(
              "sample.physicalNeutralBeforeSocd="
              "effectiveP1PhysicalNeutralBeforeSocd;")
          != std::string::npos);

    const std::string publish = sourceSlice(
        harness, "def send_training_command(", "\n    def wait_for_active_training(");
    CHECK(!publish.empty());
    const std::size_t suspend = publish.find("memory.suspend()");
    const std::size_t payload = publish.find(
        "for field, field_value in payload.items():");
    const std::size_t request = publish.rfind(
        "base + MAILBOX_OFFSET[\"request_id\"], command_id");
    const std::size_t resume = publish.find("memory.resume()");
    CHECK(suspend != std::string::npos);
    CHECK(payload != std::string::npos);
    CHECK(request != std::string::npos);
    CHECK(resume != std::string::npos);
    CHECK(suspend < payload);
    CHECK(payload < request);
    CHECK(request < resume);

    CHECK(harness.find("\"save-slot-0\"") != std::string::npos);
    CHECK(harness.find("\"reset-center\"") != std::string::npos);
    CHECK(harness.find("\"load-slot-0\"") != std::string::npos);
    CHECK(harness.find("\"hitbox-on\"") != std::string::npos);
    CHECK(harness.find("\"guard-always-stand\"")
          != std::string::npos);
    CHECK(harness.find("\"guard-always-crouch\"")
          != std::string::npos);
    CHECK(harness.find("\"macro-record-start\"")
          != std::string::npos);
    CHECK(harness.find("\"macro-play-start\"")
          != std::string::npos);
    CHECK(harness.find("\"return-to-character-select\"")
          != std::string::npos);
    CHECK(harness.find("self.drive_character_select(cycle=2)")
          != std::string::npos);
    CHECK(harness.find("\"outcome_p1\": 0x625620")
          != std::string::npos);
    CHECK(harness.find("\"outcome_p2\": 0x625624")
          != std::string::npos);
}

TEST(TrainingLiveLethalProbeTraversesQualifiedDamageDetour) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    const std::string harness =
        readQohSource("src/netplay/tests/live_training_e2e.py");
    CHECK(!runtime.empty());
    CHECK(!harness.empty());

    const std::string probe = sourceSlice(
        runtime,
        "static bool trainingLiveE2EApplyLethalDamageProbe()",
        "\nstatic bool trainingFinalizePendingStep()");
    CHECK(!probe.empty());
    const std::string compactProbe = withoutWhitespace(probe);
    CHECK(compactProbe.find(
              "constexprint64_tkMaximumProbeDamage=1000000;")
          != std::string::npos);
    CHECK(compactProbe.find(
              "constuintptr_tvictim=trainingFighter(trainingDummySlot(),false);")
          != std::string::npos);
    CHECK(compactProbe.find(
              "constuintptr_tmirror=trainingFighter(trainingDummySlot(),true);")
          != std::string::npos);
    CHECK(probe.find(
              "rd8(mirror + kFighterPlayerSlotOffset)")
          == std::string::npos);
    CHECK(compactProbe.find(
              "reinterpret_cast<TrainingDamageFn>(kTrainingDamageFn)")
          != std::string::npos);
    CHECK(compactProbe.find(
              "(void)damageFn(0,static_cast<int>(victim),0,"
              "static_cast<int>(damage));")
          != std::string::npos);
    CHECK(compactProbe.find(
              "postCurrent+postReserve-postPending!=0")
          != std::string::npos);
    CHECK(compactProbe.find("!fatalRestored")
          != std::string::npos);
    CHECK(compactProbe.find("!trainingInvariantsHold()")
          != std::string::npos);
    CHECK(probe.find("trainingRestoreScratchFailSafe()")
          != std::string::npos);

    const std::string admission = sourceSlice(
        runtime, "static int trainingAdmitAfterNativeGate(",
        "\nstatic int __cdecl hkTrainingStepGate()");
    CHECK(!admission.empty());
    const std::size_t pending =
        admission.find("g_trainingStepPending = true");
    const std::size_t liveProbe =
        admission.find("trainingLiveE2EApplyLethalDamageProbe()");
    CHECK(pending != std::string::npos);
    CHECK(liveProbe != std::string::npos);
    CHECK(pending < liveProbe);

    const std::string damage = sourceSlice(
        runtime, "static int __fastcall hkTrainingDamage(",
        "\nstatic TrainingDisableReason trainingCurrentDisableReason()");
    CHECK(!damage.empty());
    CHECK(damage.find(
              "g_trainingLiveE2EMailbox.lethalDamageDetourCount")
          != std::string::npos);

    CHECK(harness.find(
              "def run_lethal_damage_detour_probe(self)")
          != std::string::npos);
    CHECK(harness.find(
              "def run_native_combat_hit_probe(self)")
          != std::string::npos);
    CHECK(harness.find(
              "\"continued Training admission after genuine native hits\"")
          != std::string::npos);
    CHECK(harness.find(
              "\"native_collision_path\": True")
          != std::string::npos);
    CHECK(harness.find(
              "\"native_combat_hit\": native_combat_hit")
          != std::string::npos);
    CHECK(harness.find(
              "\"qualified 0x42D760 lethal-damage detour probe\"")
          != std::string::npos);
    CHECK(harness.find(
              "\"fatal_metadata_restored\": after_fatal == before_fatal")
          != std::string::npos);
    CHECK(harness.find(
              "\"lethal_damage_detour\": lethal_damage_probe")
          != std::string::npos);
    CHECK(harness.find("if slot != player:")
          == std::string::npos);
    CHECK(harness.find("fighter slot mismatch")
          == std::string::npos);
    CHECK(harness.find(
              "if int(item[\"active_character\"]) "
              "!= expected_character:")
          != std::string::npos);
    CHECK(harness.find(
              "if int(item[character_key]) != expected_character:")
          == std::string::npos);
}

TEST(TrainingWrapperAbiExactlyMirrorsRuntimePod) {
    CHECK_EQ(sizeof(Qoh99TrainingCommandV1), 32u);
    CHECK_EQ(sizeof(qtraining::TrainingCommandV1), 32u);
    CHECK_EQ(sizeof(Qoh99TrainingCommandV1),
             sizeof(qtraining::TrainingCommandV1));
    CHECK_EQ(offsetof(Qoh99TrainingCommandV1, commandKind),
             offsetof(qtraining::TrainingCommandV1, commandKind));
    CHECK_EQ(offsetof(Qoh99TrainingCommandV1, flags),
             offsetof(qtraining::TrainingCommandV1, flags));

    CHECK_EQ(sizeof(Qoh99TrainingStatusV1), 1888u);
    CHECK_EQ(sizeof(qtraining::TrainingStatusV1), 1888u);
    CHECK_EQ(sizeof(qtraining::TrainingStatusV1),
             1104u
                 + ((25u + 3u * qtraining::kTrainingTriggerEventCount)
                    * sizeof(uint32_t))
                 + ((2u + qtraining::kTrainingTriggerEventCount)
                    * sizeof(uint32_t))
                 + 16u * sizeof(uint32_t)
                 + 4u * sizeof(uint32_t)
                 + (3u + 3u * qtraining::kTrainingActorReportCapacity)
                       * sizeof(uint32_t)
                 + 7u * sizeof(uint32_t)
                 + 3u * sizeof(uint32_t)
                 + 4u * sizeof(uint32_t)
                 + 2u * sizeof(uint32_t)
                 + 30u * sizeof(uint32_t)
                 + 3u * sizeof(uint32_t)
                 + 1u * sizeof(uint32_t)
                 + 7u * sizeof(uint32_t)
                 + 3u * sizeof(uint32_t)
                 + 1u * sizeof(uint32_t)
                 + 5u * sizeof(uint32_t));
    CHECK_EQ(sizeof(Qoh99TrainingStatusV1),
             sizeof(qtraining::TrainingStatusV1));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_STATUS_V1_BASE_SIZE),
             static_cast<unsigned>(qtraining::kTrainingStatusV1BaseSize));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_STATUS_V1_COMBAT_TIER),
             static_cast<unsigned>(qtraining::kTrainingStatusV1CombatTier));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_STATUS_V1_FIGHTER_TIER),
             static_cast<unsigned>(qtraining::kTrainingStatusV1FighterTier));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_STATUS_V1_RECOVERY_TIER),
             static_cast<unsigned>(qtraining::kTrainingStatusV1RecoveryTier));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_STATUS_V1_WALL_TIER),
             static_cast<unsigned>(qtraining::kTrainingStatusV1WallTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_RECOVERY_DIAG_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1RecoveryDiagTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_METER_CHARGE_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1MeterChargeTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1CombatToolsTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_ACTION_TRIGGERS_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1ActionTriggersTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_DEBUG_OVERLAY_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1DebugOverlayTier));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_STATUS_V1_RECOVERY_TIER),
             1072u);
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_STATUS_V1_WALL_TIER), 1080u);
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_RECOVERY_DIAG_TIER), 1096u);
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_METER_CHARGE_TIER), 1104u);
    CHECK_EQ(static_cast<unsigned>(
                 offsetof(Qoh99TrainingStatusV1, p1WallFlags)),
             static_cast<unsigned>(QOH99_TRAINING_STATUS_V1_RECOVERY_TIER));
    CHECK_EQ(static_cast<unsigned>(
                 offsetof(qtraining::TrainingStatusV1, p1WallFlags)),
             static_cast<unsigned>(
                 offsetof(Qoh99TrainingStatusV1, p1WallFlags)));
    CHECK_EQ(static_cast<unsigned>(
                 offsetof(qtraining::TrainingStatusV1, p2WallFlags)),
             static_cast<unsigned>(
                 offsetof(Qoh99TrainingStatusV1, p2WallFlags)));
    CHECK_EQ(static_cast<unsigned>(
                 offsetof(Qoh99TrainingStatusV1, recoveryEvaluatorReason)),
             static_cast<unsigned>(QOH99_TRAINING_STATUS_V1_WALL_TIER));
    CHECK_EQ(static_cast<unsigned>(
                 offsetof(qtraining::TrainingStatusV1,
                          recoveryEvaluatorReason)),
             static_cast<unsigned>(
                 offsetof(Qoh99TrainingStatusV1, recoveryEvaluatorReason)));
    CHECK_EQ(static_cast<unsigned>(
                 offsetof(Qoh99TrainingStatusV1, recoveryStandDownMask)),
             1084u);
    CHECK_EQ(static_cast<unsigned>(
                 offsetof(qtraining::TrainingStatusV1, recoveryStandDownMask)),
             static_cast<unsigned>(
                 offsetof(Qoh99TrainingStatusV1, recoveryStandDownMask)));
    CHECK_EQ(static_cast<unsigned>(
                 offsetof(Qoh99TrainingStatusV1, recoveryArmableMask)), 1088u);
    CHECK_EQ(static_cast<unsigned>(
                 offsetof(qtraining::TrainingStatusV1, recoveryArmableMask)),
             static_cast<unsigned>(
                 offsetof(Qoh99TrainingStatusV1, recoveryArmableMask)));
    CHECK_EQ(static_cast<unsigned>(
                 offsetof(Qoh99TrainingStatusV1, recoveryTapsEmitted)), 1092u);
    CHECK_EQ(static_cast<unsigned>(
                 offsetof(qtraining::TrainingStatusV1, recoveryTapsEmitted)),
             static_cast<unsigned>(
                 offsetof(Qoh99TrainingStatusV1, recoveryTapsEmitted)));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_RECOVERY_EVALUATOR_NOT_ATTEMPTED),
             static_cast<unsigned>(
                 qtraining::RecoveryEvaluatorReason::NotAttempted));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_RECOVERY_EVALUATOR_PROLOGUE_MISMATCH),
             static_cast<unsigned>(
                 qtraining::RecoveryEvaluatorReason::PrologueMismatch));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_RECOVERY_EVALUATOR_COUNT),
             static_cast<unsigned>(
                 qtraining::RecoveryEvaluatorReason::Count));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_RECOVERY_STAND_DOWN_MENU_CAPTURED),
             static_cast<unsigned>(qtraining::RecoveryStandDownMenuCaptured));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_RECOVERY_STAND_DOWN_KO_LATCH),
             static_cast<unsigned>(qtraining::RecoveryStandDownKoLatch));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_RECOVERY_STAND_DOWN_DIRECTION_SUPPRESSED),
             static_cast<unsigned>(
                 qtraining::RecoveryStandDownDirectionSuppressed));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_FRAME_ADVANTAGE_GAP_TIER),
             static_cast<unsigned>(
                 offsetof(qtraining::TrainingStatusV1, faultlessPolicy)));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_CONTACT_POLICY_TIER),
             static_cast<unsigned>(
                 offsetof(qtraining::TrainingStatusV1, exRestorePinned)));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_EX_RESTORE_TARGET_TIER),
             static_cast<unsigned>(
                 offsetof(qtraining::TrainingStatusV1, macroAfterReset)));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_EX_RESTORE_TARGET_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1ExRestoreTargetTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_STATUS_MESSAGE_TIER),
             static_cast<unsigned>(sizeof(qtraining::TrainingStatusV1)));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_STATUS_MESSAGE_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1StatusMessageTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_PLAYER_SWAP_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1PlayerSwapTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_PLAYER_SWAP_TIER),
             static_cast<unsigned>(
                 offsetof(qtraining::TrainingStatusV1, statusEventKind)));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_REDUCE_POLICY_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1ReducePolicyTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_GUARD_TIMING_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1GuardTimingTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_MACRO_AFTER_RESET_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1MacroAfterResetTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_FRAME_ADVANTAGE_GAP_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1FrameAdvantageGapTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_FRAME_TRACE_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1FrameTraceTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_NATIVE_HUD_REQUESTED_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1NativeHudRequestedTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_NATIVE_HUD_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1NativeHudTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_ACTOR_POOL_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1ActorPoolTier));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_STATE_LOG_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1StateLogTier));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_ACTOR_REPORT_CAPACITY),
             static_cast<unsigned>(
                 qtraining::kTrainingActorReportCapacity));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_STATUS_V1_DEBUG_OVERLAY_TIER),
             static_cast<unsigned>(
                 qtraining::kTrainingStatusV1DebugOverlayTier));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_COUNT),
             static_cast<unsigned>(qtraining::TrainingCommandKind::Count));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_COUNT), 49u);
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_PLAYER_SWAP),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetPlayerSwap));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_MACRO_AFTER_RESET),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetMacroAfterReset));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_GUARD_AUTO_HEURISTIC),
             static_cast<unsigned>(qtraining::GuardMode::AutoHeuristic));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_GUARD_ALWAYS_AIR),
             static_cast<unsigned>(qtraining::GuardMode::AlwaysAir));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_TRIGGER_ON_RESET_ONLY),
             static_cast<unsigned>(qtraining::ResourceTrigger::OnResetOnly));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_RECOVERY_LATE),
             static_cast<unsigned>(qtraining::ManualRecoveryMode::Late));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_RECOVERY_SLOT_COUNT),
             static_cast<unsigned>(qtraining::RecoveryPolicySlot::Count));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_RECOVERY_BRANCH_COUNT),
             static_cast<unsigned>(qtraining::kRecoveryBranchCount));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_TERMINAL_RANDOM),
             static_cast<unsigned>(qtraining::TerminalRecoveryChoice::Random));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_HOLD_MENU),
             static_cast<unsigned>(qtraining::TrainingHoldMenu));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_POSTURE_UNREADABLE),
             static_cast<unsigned>(qtraining::PostureClass::Unreadable));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_GUARD_GAUGE_FULL),
             static_cast<unsigned>(qtraining::kGuardGaugeFull));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_EX_STOCKS_FULL),
             static_cast<unsigned>(qtraining::kExStocksFull));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_EX_FRACTION_MAX),
             static_cast<unsigned>(qtraining::kExFractionMax));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_RECOVERY_DELAY_STEPS_MAX),
             static_cast<unsigned>(qtraining::kRecoveryDelayStepsMax));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_TRIGGER_EVENT_COUNT), 8u);
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_TRIGGER_EVENT_ON_HIT),
             static_cast<unsigned>(qtraining::TrainingTriggerEvent::OnHit));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_TRIGGER_EVENT_AFTER_BLOCK),
             static_cast<unsigned>(
                 qtraining::TrainingTriggerEvent::AfterBlock));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_TRIGGER_EVENT_AFTER_HITSTUN),
             static_cast<unsigned>(
                 qtraining::TrainingTriggerEvent::AfterHitstun));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_TRIGGER_EVENT_ON_WAKEUP),
             static_cast<unsigned>(
                 qtraining::TrainingTriggerEvent::OnWakeup));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_TRIGGER_EVENT_AFTER_AIR_RECOVERY),
             static_cast<unsigned>(
                 qtraining::TrainingTriggerEvent::AfterAirRecovery));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_TRIGGER_EVENT_AFTER_GROUND_RECOVERY),
             static_cast<unsigned>(
                 qtraining::TrainingTriggerEvent::AfterGroundRecovery));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_TRIGGER_EVENT_AFTER_THROW_TECH),
             static_cast<unsigned>(
                 qtraining::TrainingTriggerEvent::AfterThrowTech));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_TRIGGER_EVENT_GUARD_CANCEL),
             static_cast<unsigned>(
                 qtraining::TrainingTriggerEvent::GuardCancel));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_TRIGGER_MAX_DELAY_STEPS),
             static_cast<unsigned>(
                 qtraining::kTrainingTriggerMaximumDelaySteps));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_TRIGGER_MAX_CHANCE_PERCENT),
             static_cast<unsigned>(
                 qtraining::kTrainingTriggerMaximumChancePercent));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_LIFE_VALUE),
             static_cast<unsigned>(qtraining::TrainingCommandKind::SetLifeValue));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_EX_METER),
             static_cast<unsigned>(qtraining::TrainingCommandKind::SetExMeter));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_GUARD_GAUGE_VALUE),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetGuardGaugeValue));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_HEALTH_POLICY),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetHealthPolicy));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_EX_POLICY),
             static_cast<unsigned>(qtraining::TrainingCommandKind::SetExPolicy));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_GUARD_GAUGE_POLICY),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetGuardGaugePolicy));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_RESET_ROUND_START_NEUTRAL),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::ResetRoundStartNeutral));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_RECOVERY_POLICY),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetRecoveryPolicy));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_RECOVERY_TIMING),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetRecoveryTiming));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_TERMINAL_RECOVERY),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetTerminalRecoveryChoice));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_TOGGLE_PAUSE),
             static_cast<unsigned>(qtraining::TrainingCommandKind::TogglePause));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_FRAME_STEP),
             static_cast<unsigned>(qtraining::TrainingCommandKind::FrameStep));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_THROW_TECH_CHANCE),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetThrowTechChance));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_TRIGGER_CONFIG),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetTriggerConfig));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_FRAME_ADVANTAGE_ENABLED),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetFrameAdvantageEnabled));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1,
                      explicitTrainingPresent),
             offsetof(qtraining::TrainingStatusV1,
                      explicitTrainingPresent));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, p1FinalLogicalMask),
             offsetof(qtraining::TrainingStatusV1,
                      p1FinalLogicalMask));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, macroFrameCounts),
             offsetof(qtraining::TrainingStatusV1,
                      macroFrameCounts));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, disableText),
             offsetof(qtraining::TrainingStatusV1, disableText));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, resultText),
             offsetof(qtraining::TrainingStatusV1, resultText));

    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, combatReadoutValid),
             static_cast<std::size_t>(QOH99_TRAINING_STATUS_V1_BASE_SIZE));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, combatReadoutValid),
             offsetof(qtraining::TrainingStatusV1, combatReadoutValid));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, dummyStance),
             offsetof(qtraining::TrainingStatusV1, dummyStance));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, dummyTech),
             offsetof(qtraining::TrainingStatusV1, dummyTech));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, lifeMax),
             offsetof(qtraining::TrainingStatusV1, lifeMax));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, lastHitCause),
             offsetof(qtraining::TrainingStatusV1, lastHitCause));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, lastHitCause), 640u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, p2MaxComboDamageDealt),
             offsetof(qtraining::TrainingStatusV1,
                      p2MaxComboDamageDealt));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, p2MaxComboDamageDealt), 692u);

    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, p1LifeCurrent),
             static_cast<std::size_t>(QOH99_TRAINING_STATUS_V1_COMBAT_TIER));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, p1LifeCurrent),
             offsetof(qtraining::TrainingStatusV1, p1LifeCurrent));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, fighterReadoutValid),
             offsetof(qtraining::TrainingStatusV1, fighterReadoutValid));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, fighterReadoutValid), 728u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, p1ActionState),
             offsetof(qtraining::TrainingStatusV1, p1ActionState));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, p1ActionState), 732u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, p2ActionState),
             offsetof(qtraining::TrainingStatusV1, p2ActionState));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, p2PositionY),
             offsetof(qtraining::TrainingStatusV1, p2PositionY));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, p2PositionY), 808u);

    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, holdReasons),
             static_cast<std::size_t>(QOH99_TRAINING_STATUS_V1_FIGHTER_TIER));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, holdReasons),
             offsetof(qtraining::TrainingStatusV1, holdReasons));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1,
                      guardHeuristicPostureDivergences),
             offsetof(qtraining::TrainingStatusV1,
                      guardHeuristicPostureDivergences));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, p1PostureClass),
             offsetof(qtraining::TrainingStatusV1, p1PostureClass));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, p1PostureClass), 828u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, resourcePolicyValid),
             offsetof(qtraining::TrainingStatusV1, resourcePolicyValid));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, resourcePolicyValid), 836u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, bothFightersNeutral),
             offsetof(qtraining::TrainingStatusV1, bothFightersNeutral));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, bothFightersNeutral), 864u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, lifeSetMax),
             offsetof(qtraining::TrainingStatusV1, lifeSetMax));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, lifeSetMax), 948u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, recoveryReadoutValid),
             offsetof(qtraining::TrainingStatusV1, recoveryReadoutValid));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, recoveryReadoutValid), 976u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, recoveryState),
             offsetof(qtraining::TrainingStatusV1, recoveryState));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, recoveryState), 1016u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, recoveryEligibleMask),
             offsetof(qtraining::TrainingStatusV1, recoveryEligibleMask));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, recoveryContactTestUnknown),
             offsetof(qtraining::TrainingStatusV1,
                      recoveryContactTestUnknown));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, recoveryContactTestUnknown),
             1068u);

    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, combatToolsReadoutValid), 1104u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, combatToolsReadoutValid),
             offsetof(qtraining::TrainingStatusV1,
                      combatToolsReadoutValid));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, throwTechChancePercent), 1108u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, throwTechChancePercent),
             offsetof(qtraining::TrainingStatusV1,
                      throwTechChancePercent));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, throwTechResultObserved), 1124u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, throwTechResultObserved),
             offsetof(qtraining::TrainingStatusV1,
                      throwTechResultObserved));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerMacroSlot), 1128u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerMacroSlot),
             offsetof(qtraining::TrainingStatusV1, triggerMacroSlot));
    CHECK_EQ(sizeof(((Qoh99TrainingStatusV1*)nullptr)->triggerMacroSlot),
             8u * sizeof(DWORD));
    CHECK_EQ(sizeof(((Qoh99TrainingStatusV1*)nullptr)->triggerMacroSlot),
             sizeof(((qtraining::TrainingStatusV1*)nullptr)
                        ->triggerMacroSlot));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerDelaySteps), 1160u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerDelaySteps),
             offsetof(qtraining::TrainingStatusV1, triggerDelaySteps));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerChancePercent), 1192u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerChancePercent),
             offsetof(qtraining::TrainingStatusV1,
                      triggerChancePercent));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerPendingCount), 1224u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerPendingCount),
             offsetof(qtraining::TrainingStatusV1,
                      triggerPendingCount));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerLastOutcome), 1248u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerLastOutcome),
             offsetof(qtraining::TrainingStatusV1,
                      triggerLastOutcome));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, frameAdvantageEnabled), 1252u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, frameAdvantageEnabled),
             offsetof(qtraining::TrainingStatusV1,
                      frameAdvantageEnabled));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, frameAdvantageValue), 1276u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, frameAdvantageValue),
             offsetof(qtraining::TrainingStatusV1,
                      frameAdvantageValue));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1,
                      frameAdvantageAttackerReadyDelay), 1280u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1,
                      frameAdvantageDefenderReadyDelay), 1284u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1,
                      frameAdvantageCompletedCount), 1288u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, frameAdvantageTradeCount), 1292u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, frameAdvantageTimeoutCount),
             1296u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, frameAdvantageTimeoutCount),
             offsetof(qtraining::TrainingStatusV1,
                      frameAdvantageTimeoutCount));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerMoveCatalogId), 1300u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerMoveCatalogId),
             offsetof(qtraining::TrainingStatusV1, triggerMoveCatalogId));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerMoveIndex), 1304u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerMoveIndex),
             offsetof(qtraining::TrainingStatusV1, triggerMoveIndex));
    CHECK_EQ(sizeof(((Qoh99TrainingStatusV1*)nullptr)->triggerMoveIndex),
             8u * sizeof(DWORD));
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerLastMoveIndex), 1336u);
    CHECK_EQ(offsetof(Qoh99TrainingStatusV1, triggerLastMoveIndex),
             offsetof(qtraining::TrainingStatusV1,
                      triggerLastMoveIndex));
}

TEST(InputDisplayWrapperAbiExactlyMirrorsRuntimePod) {
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_API_VERSION),
             static_cast<unsigned>(qinput::kInputDisplayApiVersion));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_LANES),
             static_cast<unsigned>(qinput::kInputDisplayLaneCount));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_HISTORY),
             static_cast<unsigned>(qinput::kInputDisplayHistoryCapacity));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_HOLD_CAP),
             static_cast<unsigned>(qinput::kInputDisplayHoldCap));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_NO_LANE),
             static_cast<unsigned>(qinput::kInputDisplayNoLane));

    CHECK_EQ(sizeof(Qoh99InputDisplayEntryV1),
             sizeof(qinput::InputDisplayEntryV1));
    CHECK_EQ(sizeof(Qoh99InputDisplayLaneV1),
             sizeof(qinput::InputDisplayLaneV1));
    CHECK_EQ(sizeof(Qoh99InputDisplaySnapshotV1),
             sizeof(qinput::InputDisplaySnapshotV1));
    CHECK_EQ(sizeof(Qoh99InputDisplaySnapshotV1), 1128u);
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_INPUT_DISPLAY_SNAPSHOT_V1_BASE_SIZE),
             static_cast<unsigned>(qinput::kInputDisplaySnapshotV1BaseSize));

    CHECK_EQ(offsetof(Qoh99InputDisplayLaneV1, entries),
             offsetof(qinput::InputDisplayLaneV1, entries));
    CHECK_EQ(offsetof(Qoh99InputDisplaySnapshotV1, mode),
             offsetof(qinput::InputDisplaySnapshotV1, mode));
    CHECK_EQ(offsetof(Qoh99InputDisplaySnapshotV1, drivenLane),
             offsetof(qinput::InputDisplaySnapshotV1, drivenLane));
    CHECK_EQ(offsetof(Qoh99InputDisplaySnapshotV1, historyCapacity),
             offsetof(qinput::InputDisplaySnapshotV1, historyCapacity));
    CHECK_EQ(offsetof(Qoh99InputDisplaySnapshotV1, lanes),
             offsetof(qinput::InputDisplaySnapshotV1, lanes));

    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_MODE_NONE), 0u);
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_MODE_NONE),
             static_cast<unsigned>(qinput::InputDisplayMode::None));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_MODE_TRAINING),
             static_cast<unsigned>(qinput::InputDisplayMode::Training));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_MODE_REPLAY),
             static_cast<unsigned>(qinput::InputDisplayMode::ReplayPlayback));

    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_DIR_NEUTRAL),
             static_cast<unsigned>(qinput::InputDisplayDirection::Neutral));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_DIR_RIGHT),
             static_cast<unsigned>(qinput::InputDisplayDirection::Right));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_DIR_DOWN),
             static_cast<unsigned>(qinput::InputDisplayDirection::Down));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_DIR_LEFT),
             static_cast<unsigned>(qinput::InputDisplayDirection::Left));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_DIR_UP),
             static_cast<unsigned>(qinput::InputDisplayDirection::Up));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_DIR_UP_RIGHT),
             static_cast<unsigned>(qinput::InputDisplayDirection::UpRight));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_DISPLAY_DIR_COUNT),
             static_cast<unsigned>(qinput::kInputDisplayDirectionCount));

    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_LOGICAL_FIGHT_MASK),
             static_cast<unsigned>(qinput::kLogicalFightMask));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_LOGICAL_UP),
             static_cast<unsigned>(qinput::kLogicalUp));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_LOGICAL_DOWN),
             static_cast<unsigned>(qinput::kLogicalDown));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_LOGICAL_LEFT),
             static_cast<unsigned>(qinput::kLogicalLeft));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_LOGICAL_RIGHT),
             static_cast<unsigned>(qinput::kLogicalRight));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_LOGICAL_A),
             static_cast<unsigned>(qinput::logicalBit(qinput::FightAction::A)));
    CHECK_EQ(static_cast<unsigned>(QOH99_INPUT_LOGICAL_D),
             static_cast<unsigned>(qinput::logicalBit(qinput::FightAction::D)));
}

TEST(InputDisplayWrapperConsumerIsRegisteredGatedAndStateless) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string exports =
        readWorkspaceSource("ddraw_wrapper/ddraw.def");
    CHECK(!wrapper.empty());
    CHECK(!exports.empty());

    CHECK(exports.find("EFZDX9_RegisterTrainingUiProvider @15")
          != std::string::npos);
    CHECK(exports.find("EFZDX9_RegisterInputDisplayProvider @16")
          != std::string::npos);
    CHECK(wrapper.find(
              "extern \"C\" void __stdcall "
              "EFZDX9_RegisterInputDisplayProvider(")
          != std::string::npos);

    const std::string gate = sourceSlice(
        wrapper, "bool PresenterNeedsImGuiPath()", "\nbool PresenterNeedsAnyOverlay()");
    CHECK(!gate.empty());
    CHECK(gate.find("g_presenter.inputDisplayProvider != nullptr")
          != std::string::npos);

    const std::string draw = sourceSlice(
        wrapper, "void DrawInputDisplay(const EfzDx9PresentParams &params)",
        "\nvoid DrawTexturedQuad(");
    CHECK(!draw.empty());
    CHECK(draw.find("std::memset(&snapshot, 0, sizeof(snapshot))")
          != std::string::npos);
    CHECK(draw.find("InputDisplaySnapshotRenderable(snapshot)")
          != std::string::npos);
    CHECK(draw.find("static Qoh99InputDisplaySnapshotV1")
          == std::string::npos);
    CHECK(wrapper.find("g_inputDisplaySnapshot") == std::string::npos);
    CHECK(wrapper.find("g_inputDisplayCached") == std::string::npos);

    const std::string renderable = sourceSlice(
        wrapper,
        "bool InputDisplaySnapshotRenderable(const Qoh99InputDisplaySnapshotV1 "
        "&snapshot)",
        "\nDWORD InputDisplayLaneSlotAt(");
    CHECK(!renderable.empty());
    CHECK(renderable.find("QOH99_INPUT_DISPLAY_MODE_TRAINING")
          != std::string::npos);
    CHECK(renderable.find("QOH99_INPUT_DISPLAY_MODE_REPLAY")
          != std::string::npos);
    CHECK(renderable.find("snapshot.drivenLane >= snapshot.laneCount")
          != std::string::npos);

    const std::string ddraw =
        readWorkspaceSource("ddraw_wrapper/ddraw.c");
    CHECK(ddraw.find("\"ToggleInputViewer\"") != std::string::npos);
    CHECK(ddraw.find("WRAPPER_HOTKEY_TOGGLE_INPUT_VIEWER")
          != std::string::npos);
    CHECK(ddraw.find("EFZDX9_Presenter_ToggleInputDisplay()")
          != std::string::npos);
    CHECK(ddraw.find("wParam == VK_BACK") == std::string::npos);
    const std::string inputDisplay = sourceSlice(
        wrapper, "void DrawInputDisplay(", "\n}");
    CHECK(!inputDisplay.empty());
    CHECK(inputDisplay.find("IsKeyPressed") == std::string::npos);
    CHECK(inputDisplay.find("GetAsyncKeyState") == std::string::npos);
}

TEST(WrapperPresentationModesAndExperimentalFlipExAreTypedEndToEnd) {
    const std::string policy =
        readWorkspaceSource("ddraw_wrapper/qoh99_presentation_policy.h");
    const std::string ddraw =
        readWorkspaceSource("ddraw_wrapper/ddraw.c");
    const std::string presenter =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string presenterAbi =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.h");
    const std::string hooks = readQohSource("src/GameHooks.cpp");
    CHECK(!policy.empty());
    CHECK(!ddraw.empty());
    CHECK(!presenter.empty());
    CHECK(!presenterAbi.empty());
    CHECK(!hooks.empty());

    CHECK(policy.find("QOH_PRESENTATION_EXCLUSIVE = 2")
          != std::string::npos);
    CHECK(policy.find("QOH_RENDERER_D3D9EX_FLIPEX = 2")
          != std::string::npos);
    CHECK(policy.find("qoh99_presentation_mode_from_ini")
          != std::string::npos);
    CHECK(ddraw.find("\"PresentationMode\"") != std::string::npos);
    CHECK(ddraw.find("qoh99_presentation_legacy_borderless_mirror(mode)")
          != std::string::npos);
    CHECK(presenterAbi.find("QohPresentationMode requestedMode;")
          != std::string::npos);
    CHECK(presenterAbi.find("QohPresentationMode effectiveMode;")
          != std::string::npos);
    CHECK(presenterAbi.find("QohRendererBackend requestedBackend;")
          != std::string::npos);
    CHECK(presenterAbi.find("QohRendererBackend effectiveBackend;")
          != std::string::npos);

    const std::string modeConfig = sourceSlice(
        presenter, "bool BuildModeConfig(",
        "\nusing Direct3DCreate9ExFn");
    CHECK(!modeConfig.empty());
    CHECK(modeConfig.find("GetAdapterMonitor") != std::string::npos);
    CHECK(modeConfig.find("MonitorFromWindow") != std::string::npos);
    CHECK(modeConfig.find(
              "mode == QOH_PRESENTATION_EXCLUSIVE ? FALSE : TRUE")
          != std::string::npos);
    CHECK(modeConfig.find("FullScreen_RefreshRateInHz")
          != std::string::npos);

    const std::string exCreate = sourceSlice(
        presenter, "bool TryCreateD3D9ExDevice(", "\nbool CreateDevice(");
    CHECK(!exCreate.empty());
    CHECK(exCreate.find("Direct3DCreate9Ex") != std::string::npos);
    CHECK(exCreate.find("CreateDeviceEx") != std::string::npos);
    CHECK(exCreate.find("BackBufferCount = 2u") != std::string::npos);
    CHECK(exCreate.find("D3DSWAPEFFECT_FLIPEX") != std::string::npos);
    CHECK(exCreate.find("SetMaximumFrameLatency(1u)")
          != std::string::npos);
    CHECK(presenter.find("deviceEx->ResetEx(") != std::string::npos);
    CHECK(presenter.find("deviceEx->PresentEx(") != std::string::npos);
    CHECK(presenter.find("D3DPOOL_DEFAULT : D3DPOOL_MANAGED")
          != std::string::npos);
    CHECK(presenter.find("D3DLOCK_DISCARD") != std::string::npos);

    CHECK(policy.find("effective_backend != QOH_RENDERER_D3D9EX_FLIPEX")
          != std::string::npos);
    CHECK_EQ(countOccurrences(
                 ddraw, "qoh99_presentation_backend_allows_gdi("),
             std::size_t{2});
    CHECK(presenter.find("exDisabledAfterHardFailure = true")
          != std::string::npos);
    CHECK(presenter.find(
              "D3D9Ex FlipEx unavailable/failed") != std::string::npos);

    CHECK(ddraw.find("WM_QOH99_D3D_DEVICE_WORK") != std::string::npos);
    CHECK(presenter.find("GetWindowThreadProcessId(window, nullptr)")
          != std::string::npos);
    CHECK(presenter.find("SendMessageTimeoutA(") != std::string::npos);
    CHECK(hooks.find("if (!netPrepareOverlayHud())")
          != std::string::npos);
    CHECK(hooks.find("explicitTrainingMode && !netPrepareOverlayHud()")
          == std::string::npos);

    const std::string runtimeModeChange = sourceSlice(
        ddraw, "static void request_presentation_mode(",
        "\nstatic void toggle_window_mode(");
    const std::string startupRestore = sourceSlice(
        ddraw, "static void apply_display_restore(MyDDraw *dd)",
        "\nstatic void qoh99_display_restore_tick(");
    CHECK(!runtimeModeChange.empty());
    CHECK(!startupRestore.empty());
    CHECK(runtimeModeChange.find("win_state_save_client_size(dd->hWnd);")
          != std::string::npos);
    CHECK(startupRestore.find("win_state_save_client_size")
          == std::string::npos);
    CHECK(startupRestore.find("build_windowed_restore_rect(")
          != std::string::npos);
    CHECK(startupRestore.find("dd->savedWinRect = windowedRect;")
          != std::string::npos);
    CHECK(startupRestore.find("dd->hasSavedWinRect = TRUE;")
          != std::string::npos);
}

TEST(InputViewerLayersUnderTheMenuAndOverTheGame) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string imgui = readQohSource("third_party/imgui/imgui.cpp");
    const std::string backend =
        readQohSource("third_party/imgui/backends/imgui_impl_dx9.cpp");
    CHECK(!wrapper.empty());
    CHECK(!imgui.empty());
    CHECK(!backend.empty());

    const std::string layerFn = withoutWhitespace(sourceSlice(
        wrapper, "ImDrawList* WrapperOverlayDrawList()",
        "\nbool ReadTrainingStatus("));
    CHECK(!layerFn.empty());
    CHECK(layerFn.find("g_presenter.menuVisible"
                       "?ImGui::GetBackgroundDrawList()"
                       ":ImGui::GetForegroundDrawList();")
          != std::string::npos);

    const std::string render = sourceSlice(
        imgui, "void ImGui::Render()",
        "\n// Calculate text size. Text can be multi-line.");
    CHECK(!render.empty());
    const std::size_t bgLayer = render.find("GetBackgroundDrawList(viewport)");
    const std::size_t windowLayer = render.find("AddRootWindowToDrawData(window)");
    const std::size_t fgLayer = render.find("GetForegroundDrawList(viewport)");
    CHECK(bgLayer != std::string::npos);
    CHECK(windowLayer != std::string::npos);
    CHECK(fgLayer != std::string::npos);
    CHECK(bgLayer < windowLayer);
    CHECK(windowLayer < fgLayer);

    CHECK(backend.find("SetRenderState(D3DRS_ZENABLE, FALSE)")
          != std::string::npos);

    const std::string present = sourceSlice(
        wrapper, "BOOL PresentFrameLocked(const EfzDx9PresentParams &params)",
        "\nvoid FlushPendingMenuCapture(");
    CHECK(!present.empty());
    const std::size_t quad = present.find("RenderFrameQuad(params);");
    const std::size_t viewer = present.find("DrawInputDisplay(params);");
    const std::size_t menu = present.find("BuildDebugMenu(params);");
    const std::size_t flush =
        present.find("ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());");
    CHECK(quad != std::string::npos);
    CHECK(viewer != std::string::npos);
    CHECK(menu != std::string::npos);
    CHECK(flush != std::string::npos);
    CHECK(quad < viewer);
    CHECK(quad < menu);
    CHECK(viewer < flush);
    CHECK(menu < flush);
    CHECK_EQ(static_cast<unsigned>(countOccurrences(
                 present, "ImGui_ImplDX9_RenderDrawData(")),
             1u);

    const std::string overlayBlock = withoutWhitespace(sourceSlice(
        present, "DrawNetplayHud(params);", "\n        ImGui::EndFrame();"));
    CHECK(!overlayBlock.empty());
    CHECK(overlayBlock.find("DrawInputDisplay(params);") != std::string::npos);
    CHECK(overlayBlock.find("if(g_presenter.menuVisible){BuildDebugMenu(params);}")
          != std::string::npos);
    CHECK_EQ(static_cast<unsigned>(countOccurrences(
                 overlayBlock, "g_presenter.menuVisible")),
             1u);

    const std::string gate = sourceSlice(
        wrapper, "bool PresenterNeedsImGuiPath()",
        "\nbool PresenterNeedsAnyOverlay()");
    CHECK(!gate.empty());
    CHECK(gate.find("return g_presenter.menuVisible") != std::string::npos);

    const std::string draw = sourceSlice(
        wrapper, "void DrawInputDisplay(const EfzDx9PresentParams &params)",
        "\nvoid DrawTrainingFrameAdvantageOverlay(");
    CHECK(!draw.empty());
    CHECK(draw.find("if (!g_inputDisplayVisible)") != std::string::npos);
    CHECK(draw.find("menuVisible") == std::string::npos);
}

TEST(InputDisplayProviderIsHandedToTheWrapperByBothProducers) {
    const std::string bridge =
        readQohSource("src/input/InputDisplayOverlayBridge.h");
    CHECK(!bridge.empty());

    CHECK(bridge.find("\"EFZDX9_RegisterInputDisplayProvider\"")
          != std::string::npos);
    CHECK(bridge.find("L\"ddraw.dll\"") != std::string::npos);
    CHECK(bridge.find("int __stdcall inputDisplayOverlayProviderThunkV1(")
          != std::string::npos);
    CHECK(bridge.find("inputDisplay().read(snapshot)") != std::string::npos);
    CHECK(bridge.find("inputDisplayTierFor(") != std::string::npos);
    CHECK(bridge.find("if (!inputDisplay().read(snapshot)) return 0;")
          != std::string::npos);
    CHECK(bridge.find("registerProvider(kInputDisplayApiVersion, nullptr, "
                      "nullptr);")
          != std::string::npos);

    const std::string training =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!training.empty());
    CHECK(training.find("input/InputDisplayOverlayBridge.h")
          != std::string::npos);
    CHECK(training.find("qinput::inputDisplayTryRegisterOverlay()")
          != std::string::npos);
    const std::string outerTick = sourceSlice(
        training, "static void trainingOuterTick() {", "\n    if (scene != 3u");
    CHECK(!outerTick.empty());
    CHECK(outerTick.find("trainingTryRegisterInputDisplay();")
          != std::string::npos);
    const std::string disarm = sourceSlice(
        training, "static void trainingArmInternal(bool enabled) {",
        "\n    g_trainingLaunchRequested = true;");
    CHECK(!disarm.empty());
    CHECK(disarm.find("qinput::inputDisplay().publishIdle();")
          < disarm.find("qinput::inputDisplayReleaseOverlay();"));

    const std::string replay =
        readQohSource("src/game/replay/ReplayRuntime.cpp");
    CHECK(!replay.empty());
    CHECK(replay.find("input/InputDisplayOverlayBridge.h") != std::string::npos);
    CHECK(replay.find("qinput::inputDisplayTryRegisterOverlay()")
          != std::string::npos);
    const std::string outerSlot = sourceSlice(
        replay, "ReplayTickPlan replayPlanOuterSlot() {", "\n    if (s.playbackV3) {");
    CHECK(!outerSlot.empty());
    CHECK(outerSlot.find("tryRegisterInputDisplayProvider(s);")
          != std::string::npos);
    const std::string stop = sourceSlice(
        replay, "void stopReplayRuntime() {", "\nbool replayRuntimeActive()");
    CHECK(!stop.empty());
    CHECK(stop.find("qinput::inputDisplay().publishIdle();")
          < stop.find("qinput::inputDisplayReleaseOverlay();"));
}

TEST(ReplayTransportProviderIsExportedAndMirroredOnBothSides) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string wrapperAbi =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.h");
    const std::string exports =
        readWorkspaceSource("ddraw_wrapper/ddraw.def");
    const std::string runtimeAbi =
        readQohSource("src/game/replay/ReplayRuntime.h");
    CHECK(!wrapper.empty());
    CHECK(!wrapperAbi.empty());
    CHECK(!exports.empty());
    CHECK(!runtimeAbi.empty());

    CHECK(exports.find("EFZDX9_RegisterInputDisplayProvider @16")
          != std::string::npos);
    CHECK(exports.find("EFZDX9_RegisterReplayTransportProvider @17")
          != std::string::npos);
    CHECK(wrapper.find("extern \"C\" void __stdcall "
                       "EFZDX9_RegisterReplayTransportProvider(")
          != std::string::npos);
    CHECK(runtimeAbi.find("\"EFZDX9_RegisterReplayTransportProvider\"")
          != std::string::npos);

    CHECK_EQ(static_cast<unsigned>(sizeof(Qoh99ReplayTransportStatusV2)), 84u);
    CHECK(runtimeAbi.find("struct ReplayTransportStatusV2 {")
          != std::string::npos);
    CHECK(wrapperAbi.find("typedef struct Qoh99ReplayTransportStatusV2 {")
          != std::string::npos);

    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TRANSPORT_TOGGLE_PAUSE), 3u);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TRANSPORT_STEP), 4u);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TRANSPORT_RESTART), 9u);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TRANSPORT_STEP_BACK), 23u);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TRANSPORT_SPEED_SLOWER), 13u);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TRANSPORT_SPEED_FASTER), 14u);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TAKEOVER_EXIT), 15u);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TAKEOVER_P1), 16u);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TAKEOVER_P2), 17u);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TAKEOVER_RETRY), 18u);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TAKEOVER_DELAY_DECREASE), 19u);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TAKEOVER_DELAY_INCREASE), 20u);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_TAKEOVER_SET_DELAY), 21u);
    CHECK(runtimeAbi.find("TogglePause = 3,") != std::string::npos);
    CHECK(runtimeAbi.find("Step = 4,") != std::string::npos);
    CHECK(runtimeAbi.find("SpeedStepSlower = 13,") != std::string::npos);
    CHECK(runtimeAbi.find("SpeedStepFaster = 14,") != std::string::npos);
    CHECK(runtimeAbi.find("TakeoverExit = 15,") != std::string::npos);
    CHECK(runtimeAbi.find("TakeoverP1 = 16,") != std::string::npos);
    CHECK(runtimeAbi.find("TakeoverP2 = 17,") != std::string::npos);
    CHECK(runtimeAbi.find("TakeoverRetry = 18,") != std::string::npos);
    CHECK(runtimeAbi.find("TakeoverDelayDecrease = 19,")
          != std::string::npos);
    CHECK(runtimeAbi.find("TakeoverDelayIncrease = 20,")
          != std::string::npos);
    CHECK(runtimeAbi.find("TakeoverSetDelay = 21,") != std::string::npos);

    CHECK(wrapper.find("g_replayTransportPaused") == std::string::npos);
    CHECK(wrapper.find("static Qoh99ReplayTransportStatusV2")
          == std::string::npos);
    const std::string poll = sourceSlice(
        wrapper, "bool ReplayTransportPoll(Qoh99ReplayTransportStatusV2 &out)",
        "\nvoid ReplayTransportSubmit(");
    CHECK(!poll.empty());
    CHECK(poll.find("std::memset(&out, 0, sizeof(out))") != std::string::npos);
    CHECK(poll.find("return out.active != 0u;") != std::string::npos);

    CHECK(wrapper.find("status.generation, command, argument")
          != std::string::npos);
    const std::string submit = sourceSlice(
        wrapper, "void ReplayTransportSubmit(", "\nvoid FormatReplaySpeed(");
    CHECK(!submit.empty());
    CHECK(submit.find("while") == std::string::npos);
    CHECK(submit.find("for (") == std::string::npos);
}

TEST(ReplayTakeoverUsesOneExternalReplaySnapshotAndNoSharedSlotPool) {
    const std::string hooks = readQohSource("src/GameHooks.cpp");
    const std::string runtime =
        readQohSource("src/game/replay/ReplayRuntime.cpp");
    const std::string dll = readQohSource("src/DllMain.cpp");
    CHECK(!hooks.empty());
    CHECK(!runtime.empty());
    CHECK(!dll.empty());

    const std::string capture = sourceSlice(
        hooks, "static bool replayTakeoverCaptureSnapshot(",
        "\nstatic bool replayTakeoverRestoreSnapshot(");
    const std::string restore = sourceSlice(
        hooks, "static bool replayTakeoverRestoreSnapshot(",
        "\nstatic int replayRunBootstrapTick(");
    CHECK(!capture.empty());
    CHECK(!restore.empty());
    CHECK(capture.find("g_rb.snapshotTo(snapshot.regions.data())")
          != std::string::npos);
    CHECK(restore.find("g_rb.loadFrom(snapshot.regions.data())")
          != std::string::npos);
    CHECK(capture.find("RawStructuralOnly") != std::string::npos);
    CHECK(restore.find("RawStructuralOnly") != std::string::npos);
    CHECK(capture.find("saveToSlot") == std::string::npos);
    CHECK(capture.find("loadFromSlot") == std::string::npos);
    CHECK(restore.find("saveToSlot") == std::string::npos);
    CHECK(restore.find("loadFromSlot") == std::string::npos);
    CHECK(capture.find("qtraining::") == std::string::npos);
    CHECK(restore.find("qtraining::") == std::string::npos);

    const std::string replayDriver = sourceSlice(
        hooks, "static int doReplayV3(FrameDispatchFn orig)",
        "\nint doReplay(FrameDispatchFn orig)");
    const std::string branch = sourceSlice(
        replayDriver, "if (plan.takeover && !plan.paused)",
        "\n    if (plan.paused");
    CHECK(!replayDriver.empty());
    CHECK(!branch.empty());
    CHECK(branch.find("replayTakeoverBeginFrame(") != std::string::npos);
    CHECK(branch.find("replayTakeoverCommitFrame()") != std::string::npos);
    CHECK(branch.find("replayPeekFrameV2(") == std::string::npos);
    CHECK(branch.find("replayBeginFrameV2(") == std::string::npos);
    CHECK(branch.find("replayCommitFrameV2(") == std::string::npos);
    CHECK(replayDriver.find("replayPeekFrameV2(") != std::string::npos);
    CHECK(replayDriver.find("replayBeginFrameV2(") != std::string::npos);
    CHECK(replayDriver.find("replayCommitFrameV2(") != std::string::npos);

    const std::string planner = sourceSlice(
        runtime, "ReplayTickPlan replayPlanOuterSlot()",
        "\nuint32_t replayPresentationGeneration()");
    CHECK(!planner.empty());
    const std::size_t v3 = planner.find("if (s.playbackV3)");
    const std::size_t takeover = planner.find("if (s.takeover.active())");
    CHECK(v3 != std::string::npos);
    CHECK(takeover != std::string::npos);
    CHECK(v3 < takeover);
    CHECK(planner.find("plan.takeoverCountdownHold = true;")
          != std::string::npos);

    CHECK(replayDriver.find("if (!captured || !netPaceIdleSlot())")
          != std::string::npos);
    CHECK(replayDriver.find("plan.takeoverCountdownHold")
          != std::string::npos);

    const std::string replayLaunch = sourceSlice(
        dll, "if (haveIpc && cfg.mode == qipc::MODE_REPLAY)",
        "\n    if (haveIpc) {");
    CHECK(!replayLaunch.empty());
    CHECK(replayLaunch.find("setReplayTakeoverInputConfig(cfg.inputConfig)")
          != std::string::npos);
    CHECK(replayLaunch.find("setInputLaunchConfig(") == std::string::npos);
    CHECK(replayLaunch.find("setModeControls(") == std::string::npos);
    CHECK(replayLaunch.find("armNetplay(") == std::string::npos);
    CHECK(replayLaunch.find("armSpectator(") == std::string::npos);
    CHECK(replayLaunch.find("armTraining(") == std::string::npos);

    const std::string inputValidation = sourceSlice(
        dll, "if (inputWhy != nullptr", "\n    qgame::storeCanonicalPlayerName");
    CHECK(!inputValidation.empty());
    CHECK(inputValidation.find("cfg.mode == qipc::MODE_REPLAY")
          != std::string::npos);
}

TEST(ReplayTakeoverWrapperDefaultsAndHudMatchTheReferenceSurface) {
    const std::string wrapper = readWorkspaceSource("ddraw_wrapper/ddraw.c");
    const std::string renderer =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string caster = readQohSource("src/client/Main.cpp");
    CHECK(!wrapper.empty());
    CHECK(!renderer.empty());
    CHECK(!caster.empty());

    const std::string compactWrapper = withoutWhitespace(wrapper);
    CHECK(compactWrapper.find(
        "{0x10,0,0,0},{0x21,0,0,0},{0x22,0,0,0},"
        "{0x13,0,0,0},{0x0A,0,0,0},{0x0B,0,0,0}")
          != std::string::npos);
    CHECK(wrapper.find("\"Replay\", \"TakeoverDelay\"")
          != std::string::npos);
    CHECK(wrapper.find("Takeover input delay: 0-9.")
          != std::string::npos);

    const std::string hud = sourceSlice(
        renderer, "void DrawReplayTakeoverHud(",
        "\nvoid DrawReplayTransport(");
    CHECK(!hud.empty());
    CHECK(hud.find("10.0f") != std::string::npos);
    CHECK(hud.find("466.0f") != std::string::npos);
    CHECK(hud.find("416.0f : 224.0f") != std::string::npos);
    CHECK(hud.find("centerY = 428.0f") != std::string::npos);
    CHECK(hud.find("centerX - 60.0f") != std::string::npos);
    CHECK(hud.find("centerX - 58.0f") != std::string::npos);
    CHECK(hud.find("IM_COL32(255, 0, 0, 255)") != std::string::npos);
    CHECK(hud.find("IM_COL32(255, 255, 0, 255)") != std::string::npos);
    CHECK(hud.find("IM_COL32(0, 0, 255, 255)") != std::string::npos);
    CHECK(hud.find("IM_COL32(0, 255, 0, 255)") != std::string::npos);

    CHECK(caster.find("Exit takeover.") != std::string::npos);
    CHECK(caster.find("Take over Player 1.") != std::string::npos);
    CHECK(caster.find("Take over Player 2.") != std::string::npos);
    CHECK(caster.find("Retry takeover.") != std::string::npos);
    CHECK(caster.find("Lower takeover delay.") != std::string::npos);
    CHECK(caster.find("Raise takeover delay.") != std::string::npos);
}

TEST(ReplayFrameStepAndCollisionBoxesStayPresentationOnly) {
    const std::string hooks = readQohSource("src/GameHooks.cpp");
    const std::string runtime =
        readQohSource("src/game/replay/ReplayRuntime.cpp");
    const std::string renderer =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/ddraw.c");
    const std::string caster = readQohSource("src/client/Main.cpp");
    CHECK(!hooks.empty());
    CHECK(!runtime.empty());
    CHECK(!renderer.empty());
    CHECK(!wrapper.empty());
    CHECK(!caster.empty());

    const std::string runtimeToggle = sourceSlice(
        runtime, "case PresentationCommand::ToggleCollisionDisplay:",
        "\n            case PresentationCommand::Exit:");
    CHECK(!runtimeToggle.empty());
    CHECK(runtimeToggle.find("s.collisionDisplayV3 = !s.collisionDisplayV3")
          != std::string::npos);
    CHECK(runtimeToggle.find("record") == std::string::npos);
    CHECK(runtimeToggle.find("hash") == std::string::npos);

    const std::string replayV3 = sourceSlice(
        hooks, "static int doReplayV3(FrameDispatchFn orig)",
        "\nint doReplay(FrameDispatchFn orig)");
    const std::string replayDriver = sourceSlice(
        hooks, "int doReplay(FrameDispatchFn orig)",
        "\nint __cdecl hkFrameDispatch()");
    CHECK(!replayV3.empty());
    CHECK(!replayDriver.empty());
    const std::size_t roleGate = replayDriver.find("if (!g_replayRoleActive)");
    const std::size_t overlay = replayDriver.find("replaySetCustomHitboxes(");
    CHECK(roleGate != std::string::npos);
    CHECK(overlay != std::string::npos);
    CHECK(roleGate < overlay);
    CHECK(replayV3.find("replayPlanOuterSlot()") != std::string::npos);
    CHECK(replayV3.find("replayCollisionDisplayRequested()")
          != std::string::npos);
    CHECK(replayV3.find("replaySetCustomHitboxes(") != std::string::npos);
    CHECK(replayV3.find("replayReleaseCustomHitboxes()") != std::string::npos);
    CHECK(replayDriver.find("replayCollisionDisplayRequested()")
          != std::string::npos);
    CHECK(replayDriver.find("qtraining::") == std::string::npos);

    const std::string keys = sourceSlice(
        renderer, "void DrawNetplayKeysTab()",
        "\nvoid DrawNetplayHelpTab()");
    CHECK(!keys.empty());
    CHECK(keys.find("ImGui::Button(\"Frame step\")")
          != std::string::npos);
    CHECK(keys.find("QOH99_REPLAY_TRANSPORT_STEP")
          != std::string::npos);
    CHECK(keys.find("Toggle collision boxes") != std::string::npos);
    CHECK(keys.find("QOH99_REPLAY_TOGGLE_COLLISION_DISPLAY")
          != std::string::npos);
    CHECK(wrapper.find("WRAPPER_HOTKEY_REPLAY_TOGGLE_COLLISION_DISPLAY")
          != std::string::npos);
    CHECK(caster.find("ModeControlAction::ReplayToggleCollisionDisplay")
          != std::string::npos);
}

TEST(NetplayHudNamesAreUtf8AndTheDebugEditorsStayPreviewOnly) {
    const std::string renderer =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string wrapperAbi =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.h");
    const std::string hooks = readQohSource("src/GameHooks.cpp");
    CHECK(!renderer.empty());
    CHECK(!wrapperAbi.empty());
    CHECK(!hooks.empty());

    const std::string fonts = sourceSlice(
        renderer, "void ConfigureOverlayFonts(ImGuiIO &io)",
        "\nbool InitializeImGui(HWND window)");
    CHECK(!fonts.empty());
    CHECK(fonts.find("GetGlyphRangesCyrillic()") != std::string::npos);
    CHECK(fonts.find("GetGlyphRangesJapanese()") != std::string::npos);
    CHECK(fonts.find("MergeMode = true") != std::string::npos);
    CHECK(fonts.find("NotoSansCJKjp-Regular.ttf") != std::string::npos);
    CHECK(fonts.find("YuGothM.ttc") != std::string::npos);
    CHECK(fonts.find("msgothic.ttc") != std::string::npos);
    CHECK(fonts.find("AddInstalledGdiFont(") != std::string::npos);
    CHECK(renderer.find("GetFontData(dc, 0u, 0u") != std::string::npos);
    CHECK(renderer.find("ConfigureOverlayFonts(io);") != std::string::npos);

    CHECK(wrapperAbi.find(
        "Every text field in this presentation ABI is UTF-8")
          != std::string::npos);
    CHECK(hooks.find("qgame::playerNameForUtf8Display(")
          != std::string::npos);
    CHECK(hooks.find(
        "netHudStorePlayerIdentity(outState->playerOneName, 0u);")
          != std::string::npos);
    CHECK(hooks.find(
        "netHudStorePlayerIdentity(outState->playerTwoName, 1u);")
          != std::string::npos);

    const std::string editor = sourceSlice(
        renderer, "void DrawHudLayoutTab(const EfzDx9PresentParams &params)",
        "\nbool BeginTabBody(");
    CHECK(!editor.empty());
    CHECK(editor.find("Player 1 nickname") != std::string::npos);
    CHECK(editor.find("Player 2 nickname") != std::string::npos);
    CHECK(editor.find("g_hudPreviewPlayerOne") != std::string::npos);
    CHECK(editor.find("g_hudPreviewPlayerTwo") != std::string::npos);
    CHECK(editor.find("never saved or sent") != std::string::npos);
    CHECK(editor.find("HudIniSet(\"Player") == std::string::npos);
    CHECK(renderer.find("case WM_IME_CHAR:") != std::string::npos);
    CHECK(renderer.find("bool SubmitImeCharacter(HWND hwnd, WPARAM value)")
          != std::string::npos);
    CHECK(renderer.find("msg == WM_IME_CHAR") != std::string::npos);

    const std::string draw = sourceSlice(
        renderer, "void DrawNetplayHud(const EfzDx9PresentParams &params)",
        "\nvoid DrawGenericOverlay(");
    CHECK(!draw.empty());
    CHECK(draw.find("if (!haveLive)") != std::string::npos);
    CHECK(draw.find("g_presenter.menuVisible && g_hudPreview")
          != std::string::npos);
    CHECK(draw.find("g_hudPreviewPlayerOne") != std::string::npos);
    CHECK(draw.find("g_hudPreviewPlayerTwo") != std::string::npos);
}

TEST(ImGuiMenuAndInGameHudHaveIndependentPersistedFontScales) {
    const std::string renderer =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/ddraw.c");
    CHECK(!renderer.empty());
    CHECK(!wrapper.empty());

    CHECK(renderer.find("kOverlayFontPixels = 14.0f") != std::string::npos);
    CHECK(renderer.find("\"ImGuiFontScalePct\"") != std::string::npos);
    CHECK(renderer.find(
              "constexpr int kHudDefFontScalePct = 107;")
          != std::string::npos);
    CHECK(renderer.find(
              "constexpr int kMenuDefFontScalePct = 135;")
          != std::string::npos);
    CHECK(renderer.find("\"Menu scale\"") != std::string::npos);
    CHECK(renderer.find("\"Nickname scale\"")
          != std::string::npos);
    CHECK(renderer.find("ImGui::GetIO().FontGlobalScale")
          != std::string::npos);
    CHECK(wrapper.find("Menu size: 75-200.") != std::string::npos);
    CHECK(wrapper.find("Nickname and match text size: 50-300.")
          != std::string::npos);

    const std::string hud = sourceSlice(
        renderer, "void DrawNetplayHud(const EfzDx9PresentParams &params)",
        "\nvoid DrawGenericOverlay(");
    CHECK(!hud.empty());
    CHECK(hud.find("font->FontSize * scale * userScale")
          != std::string::npos);
    CHECK(hud.find(
              "const float fontSize = ImGui::GetFontSize() * scale * userScale")
          == std::string::npos);

    const std::string present = sourceSlice(
        renderer, "if (PresenterNeedsImGuiPath())",
        "\n        ImGui_ImplDX9_NewFrame();");
    CHECK(!present.empty());
    CHECK(present.find("HudLayoutEnsureLoaded();") != std::string::npos);
    CHECK(present.find("ImGui::GetIO().FontGlobalScale")
          != std::string::npos);
}

TEST(CasterNicknamesUseUtf8ForEditingStorageTransportAndRendering) {
    const std::string caster = readQohSource("src/client/Main.cpp");
    const std::string console = readQohSource("src/client/ConsoleUi.cpp");
    CHECK(!caster.empty());
    CHECK(!console.empty());

    CHECK(caster.find("config.localNameCodePage = CP_UTF8;")
          != std::string::npos);
    CHECK(caster.find("config.remoteNameCodePage = CP_UTF8;")
          != std::string::npos);
    CHECK(caster.find("myInit.nameCodePage = CP_UTF8;")
          != std::string::npos);
    CHECK(caster.find("nameCodePage = GetACP()") == std::string::npos);
    CHECK(caster.find("playerNameForUtf8Display(") != std::string::npos);
    CHECK(caster.find("nicknameUtf8Hex") != std::string::npos);
    CHECK(caster.find("CommandLineToArgvW(GetCommandLineW()")
          != std::string::npos);
    CHECK(caster.find("processArgumentUtf8(argc, nicknameIndex, nicknameUtf8)")
          != std::string::npos);
    CHECK(caster.find(
        "\"Your nickname\", tmp, s.nickname, CP_UTF8,")
          != std::string::npos);

    CHECK(console.find("MultiByteToWideChar(\n        codePage, flags")
          != std::string::npos);
    CHECK(console.find("WriteConsoleOutputCharacterW(")
          != std::string::npos);
    CHECK(console.find("WriteConsoleOutputCharacterA(")
          == std::string::npos);
    CHECK(console.find("FillConsoleOutputCharacterA(")
          == std::string::npos);
}

TEST(OfflineVsScoreboardShowsWinsOnTheNamePlatesForVsLaunchesOnly) {
    const std::string dllMain = readQohSource("src/DllMain.cpp");
    const std::string hooks = readQohSource("src/GameHooks.cpp");
    CHECK(!dllMain.empty());
    CHECK(!hooks.empty());

    const std::string boot = withoutWhitespace(sourceSlice(
        dllMain, "if (haveIpc && cfg.directBoot) {",
        "\n    if (haveIpc && cfg.probeMode != 0) {"));
    CHECK(!boot.empty());
    CHECK(boot.find("}else{qoh99::enableOfflineVsScoreboard();"
                    "qgame::startDirectBootToCharSelect(cfg.vsRow);}")
          != std::string::npos);
    std::size_t calls = 0;
    for (std::size_t at = dllMain.find("enableOfflineVsScoreboard();");
         at != std::string::npos;
         at = dllMain.find("enableOfflineVsScoreboard();", at + 1)) {
        ++calls;
    }
    CHECK_EQ(calls, static_cast<std::size_t>(1));

    const std::string tally = withoutWhitespace(sourceSlice(
        hooks, "void netTrackCompletedGameWins() {",
        "\ntemplate <size_t N>"));
    CHECK(tally.find("if(!g_netActive&&!g_offlineVsScoreboard)return;")
          != std::string::npos);

    const std::string tick = withoutWhitespace(sourceSlice(
        hooks, "void netOfflineFrameTick() {",
        "\n    {\n        static bool s_armLogged"));
    const std::size_t tallyAt = tick.find("netTrackCompletedGameWins();");
    const std::size_t rematchAt = tick.find("netOfflineRematchTick();");
    CHECK(tallyAt != std::string::npos);
    CHECK(rematchAt != std::string::npos);
    CHECK(tallyAt < rematchAt);

    const std::string provider = sourceSlice(
        hooks, "BOOL __stdcall netHudProvider(",
        "\nbool netPrepareOverlayHud(");
    CHECK(!provider.empty());
    const std::string offline = withoutWhitespace(sourceSlice(
        provider, "if (!g_netActive && g_offlineVsScoreboard) {",
        "if (!g_netActive || (!hudGameplay && !hudCharSelect)) {"));
    CHECK(!offline.empty());
    CHECK(offline.find("\"Wins:%u\",g_netGameWins[0]") != std::string::npos);
    CHECK(offline.find("\"Wins:%u\",g_netGameWins[1]") != std::string::npos);
    CHECK(offline.find("outState->sessionDetail[0]='\\0';")
          != std::string::npos);
}

TEST(ReplayTransportReadoutKeepsFramesAndSpeedInThePingSlot) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string caster = readQohSource("src/client/Main.cpp");
    CHECK(!wrapper.empty());
    CHECK(!caster.empty());

    const std::string draw = sourceSlice(
        wrapper, "void DrawReplayTransport(const EfzDx9PresentParams &params)",
        "\nvoid DrawTrainingFrameAdvantageOverlay(");
    CHECK(!draw.empty());

    CHECK(draw.find("status.nextRecord") != std::string::npos);
    CHECK(draw.find("status.endpointRecord") != std::string::npos);
    CHECK(draw.find("FormatReplaySpeed(") != std::string::npos);
    CHECK(draw.find("\"%s  %lu / %lu  %s\"") != std::string::npos);

    CHECK(wrapper.find("kHudDefSessionX = 341, kHudDefSessionY = 462")
          != std::string::npos);
    CHECK(draw.find("HudResolveX(params, g_hudLayout.sx") != std::string::npos);
    CHECK(draw.find("HudResolveY(params, g_hudLayout.sy") != std::string::npos);
    CHECK(draw.find("kHudDefSessionX") != std::string::npos);
    CHECK(draw.find("kHudDefSessionY") != std::string::npos);

    CHECK(draw.find("DrawCenteredHudLine(") != std::string::npos);
    CHECK(draw.find("GameScale(params)") != std::string::npos);
    CHECK(draw.find("fontScalePct") != std::string::npos);
    CHECK(draw.find("InputDisplayScale(") == std::string::npos);
    CHECK(draw.find("AddRectFilled(") == std::string::npos);
    CHECK(draw.find("drawList->AddText(") == std::string::npos);

    CHECK(draw.find("HudLayoutEnsureLoaded();") != std::string::npos);

    for (const unsigned char byte : draw)
        CHECK(byte < 0x80u);

    const std::string ddraw =
        readWorkspaceSource("ddraw_wrapper/ddraw.c");
    CHECK(ddraw.find("\"ReplayPause\"") != std::string::npos);
    CHECK(ddraw.find("\"ReplayStep\"") != std::string::npos);
    CHECK(ddraw.find("\"ReplaySlower\"") != std::string::npos);
    CHECK(ddraw.find("\"ReplayFaster\"") != std::string::npos);
    CHECK(ddraw.find("WRAPPER_HOTKEY_REPLAY_STEP") != std::string::npos);
    CHECK(ddraw.find("EFZDX9_Presenter_SubmitReplayHotkey(")
          != std::string::npos);
    CHECK(wrapper.find("case VK_SPACE:") == std::string::npos);
    CHECK(wrapper.find("case VK_RIGHT:") == std::string::npos);

    const std::string controls = sourceSlice(
        caster, "void controlsMenu(qui::Console& con) {",
        "con.menuTick(spec, selection, 30000)");
    CHECK(!controls.empty());
    CHECK(controls.find("Mode controls") != std::string::npos);
    CHECK(caster.find(
              "add(\"ReplayPause\", qinput::ModeControlAction::ReplayPause,")
          != std::string::npos);
    CHECK(caster.find(
              "add(\"ReplayStep\", qinput::ModeControlAction::ReplayStep,")
          != std::string::npos);
    CHECK(caster.find("add(\"ReplaySlower\"") != std::string::npos);
    CHECK(caster.find("add(\"ReplayFaster\"") != std::string::npos);

    CHECK_EQ(countOccurrences(controls, "spec.below.push_back"), 3u);
}

TEST(CasterDoesNotExposeAnObsoleteOnlineSafetyPolicyScreen) {
    const std::string caster = readQohSource("src/client/Main.cpp");
    CHECK(!caster.empty());
    CHECK(caster.find("Online extra F-keys") == std::string::npos);
    CHECK(caster.find("Online safety policy") == std::string::npos);
    CHECK(caster.find("hidden developer keys and native extra gamepad")
          == std::string::npos);
}

TEST(ReplayTransportBindsBothStepKeysAndNeverGatesThemOnPlaying) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string ddraw =
        readWorkspaceSource("ddraw_wrapper/ddraw.c");
    CHECK(!wrapper.empty());
    CHECK(!ddraw.empty());

    const std::string keys = sourceSlice(
        ddraw, "static BOOL wrapper_dispatch_hotkey(",
        "\n#define WM_QOH99_HOTKEY (");
    CHECK(!keys.empty());

    CHECK(keys.find("WRAPPER_HOTKEY_REPLAY_STEP") != std::string::npos);
    CHECK_EQ(countOccurrences(keys, "QOH99_REPLAY_TRANSPORT_STEP,"), 1u);
    CHECK(keys.find("QOH99_REPLAY_TRANSPORT_STEP, repeat")
          != std::string::npos);
    CHECK(keys.find("QOH99_REPLAY_TRANSPORT_TOGGLE_PAUSE, FALSE")
          != std::string::npos);

    CHECK(keys.find("status.paused") == std::string::npos);
    CHECK(keys.find("status.mayHold") == std::string::npos);
    CHECK(keys.find("status.pendingSteps") == std::string::npos);
    const std::string submit = sourceSlice(
        wrapper, "extern \"C\" BOOL EFZDX9_Presenter_SubmitReplayHotkey(",
        "\nextern \"C\" BOOL EFZDX9_Presenter_PrepareDevice(");
    CHECK(!submit.empty());
    CHECK(submit.find("ReplayTransportSubmit(status, command);")
          != std::string::npos);
    CHECK(submit.find("ReplayTransportPoll(status)") != std::string::npos);
}

TEST(ReplayRestartAndBackwardStepReachOnlyTheReplayTransport) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string ddraw =
        readWorkspaceSource("ddraw_wrapper/ddraw.c");
    const std::string keys = sourceSlice(
        ddraw, "static BOOL wrapper_dispatch_hotkey(",
        "\n#define WM_QOH99_HOTKEY (");
    const std::string submit = sourceSlice(
        wrapper, "extern \"C\" BOOL EFZDX9_Presenter_SubmitReplayHotkey(",
        "\nextern \"C\" BOOL EFZDX9_Presenter_PrepareDevice(");
    const std::string spectator = sourceSlice(
        submit, "if (g_presenter.spectatorHotkeySink != nullptr", "\n    return FALSE;");
    CHECK(keys.find("QOH99_REPLAY_TRANSPORT_STEP_BACK, repeat")
          != std::string::npos);
    CHECK(keys.find("QOH99_REPLAY_TRANSPORT_RESTART, FALSE")
          != std::string::npos);
    CHECK(submit.find("command != QOH99_REPLAY_TRANSPORT_STEP_BACK")
          != std::string::npos);
    CHECK(submit.find("command != QOH99_REPLAY_TRANSPORT_RESTART")
          != std::string::npos);
    CHECK(!spectator.empty());
    CHECK(spectator.find("QOH99_REPLAY_TRANSPORT_STEP_BACK") == std::string::npos);
    CHECK(spectator.find("QOH99_REPLAY_TRANSPORT_RESTART") == std::string::npos);
    const std::string defaults = sourceSlice(
        ddraw, "static void wrapper_hotkey_default(",
        "\nstatic void wrapper_hotkeys_load(");
    CHECK(defaults.find("{ 0xC7, 0, 0, 0 }") != std::string::npos);
    CHECK(defaults.find("{ 0xCB, 0x33, 0, 0 }") != std::string::npos);
    CHECK(ddraw.find("\"ReplayRestart\", \"ReplayStepBack\"") != std::string::npos);
    CHECK(wrapper.find("ImGui::Button(\"Previous frame\")") != std::string::npos);
    CHECK(wrapper.find("ImGui::Button(\"Restart replay\")") != std::string::npos);
}

TEST(ReplaySlowMotionSpendsARealPresentationPeriodPerSkippedSlot) {
    const std::string hooks = readQohSource("src/GameHooks.cpp");
    const std::string replay =
        readQohSource("src/game/replay/ReplayRuntime.cpp");
    const std::string replayAbi =
        readQohSource("src/game/replay/ReplayRuntime.h");
    CHECK(!hooks.empty());
    CHECK(!replay.empty());
    CHECK(!replayAbi.empty());

    CHECK(replayAbi.find("replaySpeedTicksForSlotV3(") != std::string::npos);
    CHECK(replay.find("replayTransportTicksForSlotV3(") != std::string::npos);
    CHECK(replay.find("qreplay::kSpeedNormal / s.speedPermilleV3")
          == std::string::npos);
    CHECK(replay.find("slowSlotV3") == std::string::npos);
    const char* creditOwners[] = {
        "void applyPlaybackSpeedV3(", "void resetPlaybackSessionV3(",
        "bool requestNavigationV3(",
    };
    for (const char* owner : creditOwners) {
        const std::string body = sourceSlice(replay, owner, "\n}");
        CHECK(!body.empty());
        CHECK(body.find("replaySpeedInitialCreditV3(") != std::string::npos);
    }
    for (const char* owner : {"bool startPlayback(", "bool replayBeginNextGameV3("}) {
        const std::string body = sourceSlice(replay, owner, "\n}");
        CHECK(body.find("resetPlaybackSessionV3(") != std::string::npos);
    }

    CHECK(replayAbi.find("bool slowMotionHold = false;") != std::string::npos);
    CHECK_EQ(countOccurrences(replay, "plan.slowMotionHold = !s.pausedV3;"),
             1u);

    const std::string hold = sourceSlice(
        hooks, "static int doReplayV3(FrameDispatchFn orig) {",
        "\n    const uint32_t totalTicks =");
    CHECK(!hold.empty());
    CHECK(hold.find("const bool cadenceHold = plan.slowMotionHold")
          != std::string::npos);
    CHECK(hold.find("if (!cadenceHold || !netPaceIdleSlot()) {")
          != std::string::npos);
    CHECK(hold.find("netPaceSemanticHold();") != std::string::npos);
    CHECK(hold.find("Sleep(1);") != std::string::npos);

    const std::string idle = sourceSlice(
        hooks, "static bool netPaceIdleSlot() {", "\ninline void "
        "netPaceSemanticHold(");
    CHECK(!idle.empty());
    CHECK(idle.find("g_paceDeadline.poll(now)") != std::string::npos);
    CHECK(idle.find("g_netPacedTickSerial") == std::string::npos);
    CHECK(idle.find("g_netPresentationSlotGrants") == std::string::npos);
    CHECK(idle.find("g_wrapperPacingOwned") != std::string::npos);
    CHECK(idle.find("qgame::replay::replayHeadlessV3()") != std::string::npos);
    CHECK(idle.find("g_netPacingClockFault") != std::string::npos);
    CHECK(idle.find("waitStartedMs > 500u") != std::string::npos);

    const std::string legacy = sourceSlice(
        hooks, "const qgame::replay::ReplayTickPlan plan =\n        qgame::replay::replayPlanOuterSlot();\n    replaySetCustomHitboxes(",
        "\n    const uint32_t totalTicks =");
    CHECK(!legacy.empty());
    CHECK(legacy.find("netPaceIdleSlot()") == std::string::npos);
}

TEST(InputDisplayTrainingProducerFoldsOncePerAdmittedRealStep) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());
    const std::string compactRuntime = withoutWhitespace(runtime);

    CHECK_EQ(countOccurrences(compactRuntime,
                              "qinput::inputDisplay().publishStep("),
             1u);

    const std::string compose = sourceSlice(
        runtime, "static bool trainingComposeInputForStep()",
        "\nstatic bool trainingRestoreScratchFailSafe()");
    CHECK(!compose.empty());
    const std::string compactCompose = withoutWhitespace(compose);
    CHECK(compactCompose.find(
              "display.mode=qinput::InputDisplayMode::Training;")
          != std::string::npos);
    CHECK(compactCompose.find("display.stepSerial=g_trainingStepSerial;")
          != std::string::npos);
    CHECK(compactCompose.find(
              "display.drivenLane=qtraining::trainingDrivenLane("
              "macroRecordOwnership,g_trainingPlayerSwap);")
          != std::string::npos);
    CHECK(compactCompose.find(
              "display.laneActiveMask=1u<<display.drivenLane;")
          != std::string::npos);
    CHECK(compactCompose.find("display.laneLogicalMask[0]=composed.p1Mask;")
          != std::string::npos);
    CHECK(compactCompose.find("display.laneLogicalMask[1]=composed.p2Mask;")
          != std::string::npos);
    CHECK(compactCompose.find(
              "(void)qinput::inputDisplay().publishStep(display);")
          != std::string::npos);

    const std::size_t latch =
        compose.find("g_trainingLastComposedInput[1] = composed.p2Mask;");
    const std::size_t publish = compose.find("publishStep(display)");
    CHECK(latch != std::string::npos);
    CHECK(publish != std::string::npos);
    CHECK(latch < publish);

    const std::string publishStatus = sourceSlice(
        runtime, "static void trainingPublishStatus()",
        "\nstatic BOOL __stdcall trainingStatusProvider(");
    CHECK(!publishStatus.empty());
    CHECK(publishStatus.find("inputDisplay(") == std::string::npos);

    const std::string admission = sourceSlice(
        runtime, "static int trainingAdmitAfterNativeGate(",
        "\nstatic int __cdecl hkTrainingStepGate()");
    CHECK(!admission.empty());
    const std::size_t pauseGate =
        admission.find("if (!trainingAdmitPausedFrame())");
    const std::size_t bumpSerial = admission.find("++g_trainingStepSerial;");
    const std::size_t composeCall =
        admission.find("trainingComposeInputForStep()");
    CHECK(pauseGate != std::string::npos);
    CHECK(bumpSerial != std::string::npos);
    CHECK(composeCall != std::string::npos);
    CHECK(pauseGate < bumpSerial);
    CHECK(bumpSerial < composeCall);

    CHECK_EQ(countOccurrences(compactRuntime,
                              "qinput::inputDisplay().reset(g_trainingGeneration);"),
             2u);
    CHECK_EQ(countOccurrences(compactRuntime,
                              "qinput::inputDisplay().publishIdle();"),
             2u);
    const std::string newGeneration = sourceSlice(
        runtime, "static void trainingBeginNewGeneration()",
        "\nstatic void trainingArmInternal(");
    CHECK(!newGeneration.empty());
    CHECK(newGeneration.find("qinput::inputDisplay().reset(")
          != std::string::npos);
    const std::string returnToSelect = sourceSlice(
        runtime,
        "static CommandAckResult "
        "trainingBeginReturnToCharacterSelect()",
        "\nstatic CommandAckResult trainingRefillEnvironmentReady(");
    CHECK(!returnToSelect.empty());
    CHECK(returnToSelect.find("qinput::inputDisplay().publishIdle();")
          != std::string::npos);
}

TEST(InputDisplayReplayProducerFoldsTheLatchAndCannotDisturbVerification) {
    const std::string replay =
        readQohSource("src/game/replay/ReplayRuntime.cpp");
    CHECK(!replay.empty());

    const std::string helper = sourceSlice(
        replay, "void publishInputDisplayLatch(RuntimeState& s) {",
        "\nconst char* haltReasonText(");
    CHECK(!helper.empty());
    const std::string compactHelper = withoutWhitespace(helper);
    CHECK(compactHelper.find(
              "display.mode=qinput::InputDisplayMode::ReplayPlayback;")
          != std::string::npos);
    CHECK(compactHelper.find("display.stepSerial=s.latchTick;")
          != std::string::npos);
    CHECK(compactHelper.find(
              "display.drivenLane=qinput::kInputDisplayNoLane;")
          != std::string::npos);
    CHECK(helper.find("qinput::logicalFromCompact(") != std::string::npos);
    CHECK(helper.find("qinput::kCanonicalFightMask") != std::string::npos);
    CHECK(helper.find("replayInjectedAction(") == std::string::npos);
    CHECK(helper.find("nowMicros(") == std::string::npos);
    CHECK(helper.find("deviceCalls") == std::string::npos);
    CHECK(helper.find("QLOG(") == std::string::npos);
    CHECK(helper.find("(void)qinput::inputDisplay().publishStep(display);")
          != std::string::npos);

    const std::string beginFrame = sourceSlice(
        replay, "bool replayBeginFrameV2(", "\nbool replayCommitFrameV2(");
    CHECK(!beginFrame.empty());
    CHECK(beginFrame.find("publishInputDisplayLatch(s);") != std::string::npos);
    CHECK(beginFrame.find("++s.latchTick;")
          < beginFrame.find("publishInputDisplayLatch(s);"));

    const std::string nextFrame = sourceSlice(
        replay, "bool replayNextFrame(qreplay::ReplayFrameV1& out) {",
        "\nuint16_t replayInjectedAction(");
    CHECK(!nextFrame.empty());
    CHECK(nextFrame.find("publishInputDisplayLatch(s);") != std::string::npos);
    CHECK(nextFrame.find("++s.latchTick;")
          < nextFrame.find("publishInputDisplayLatch(s);"));

    const std::string commitFrame = sourceSlice(
        replay, "bool replayCommitFrameV2(",
        "\nbool replayTakeoverNeedsSnapshot()");
    CHECK(!commitFrame.empty());
    CHECK(commitFrame.find("s.latched.fill(0u);") != std::string::npos);
    CHECK(commitFrame.find("publishInputDisplayLatch") == std::string::npos);
    CHECK(commitFrame.find("inputDisplay(") == std::string::npos);

    const std::string start = sourceSlice(
        replay, "bool startPlayback(const qipc::IpcConfig& cfg,",
        "\nbool ownExecutableSha256(");
    CHECK(!start.empty());
    const std::size_t modeGate = start.find("cfg.mode != qipc::MODE_REPLAY");
    const std::size_t bump = start.find("++s.inputDisplayGeneration;");
    CHECK(modeGate != std::string::npos);
    CHECK(bump != std::string::npos);
    CHECK(modeGate < bump);
    CHECK(start.find("qinput::inputDisplay().reset(s.inputDisplayGeneration);")
          != std::string::npos);

    const std::string stop = sourceSlice(
        replay, "void stopReplayRuntime() {", "\nbool replayRuntimeActive()");
    CHECK(!stop.empty());
    CHECK(stop.find("qinput::inputDisplay().publishIdle();")
          != std::string::npos);
}

TEST(TrainingReturnToSelectHotkeyDrivesTheOneExitTransaction) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());

    const std::string edges = sourceSlice(
        runtime, "static bool trainingApplyActionEdges(",
        "\nstatic bool trainingAdmitPausedFrame()");
    CHECK(!edges.empty());
    const std::size_t f1 = edges.find(
        "fired(qtraining::TrainingAction::ReturnToCharacterSelect)");
    CHECK(f1 != std::string::npos);
    CHECK(edges.find("trainingBeginReturnToCharacterSelect()")
          != std::string::npos);
    CHECK(edges.find("g_trainingCommands") == std::string::npos);
    CHECK(edges.find("TrainingCommandV1") == std::string::npos);
    CHECK(edges.find("tryEnqueue(") == std::string::npos);

    const std::size_t slots =
        edges.find("fired(qtraining::TrainingAction::NextSlot)");
    const std::size_t reset =
        edges.find("fired(qtraining::TrainingAction::ResetPosition)");
    const std::size_t save =
        edges.find("fired(qtraining::TrainingAction::SaveState)");
    const std::size_t pause =
        edges.find("fired(qtraining::TrainingAction::TogglePause)");
    CHECK(slots != std::string::npos);
    CHECK(reset != std::string::npos);
    CHECK(save != std::string::npos);
    CHECK(pause != std::string::npos);
    CHECK(reset < f1);
    CHECK(f1 < slots);
    CHECK(f1 < save);
    CHECK(f1 < pause);

    const std::string admission = sourceSlice(
        runtime, "static int trainingAdmitAfterNativeGate(",
        "\nstatic int __cdecl hkTrainingStepGate()");
    CHECK(!admission.empty());
    const std::string compactAdmission = withoutWhitespace(admission);
    const std::size_t actionTable =
        compactAdmission.find("trainingApplyActionEdges(actions,");
    CHECK(actionTable != std::string::npos);
    const std::size_t returning = actionTable == std::string::npos
        ? std::string::npos
        : compactAdmission.find("if(g_trainingReturningToSelect){",
                                actionTable);
    const std::size_t overlay =
        compactAdmission.rfind("trainingApplyConfigOverlay()");
    CHECK(returning != std::string::npos);
    CHECK(overlay != std::string::npos);
    CHECK(actionTable < returning);
    CHECK(returning < overlay);
    CHECK_EQ(countOccurrences(compactAdmission,
                              "if(g_trainingReturningToSelect){"),
             2u);
    CHECK(compactAdmission.find("g_trainingScratch.valid=false;")
          != std::string::npos);

    const std::string nativeEdges = sourceSlice(
        runtime, "static void trainingClearNativeDeveloperEdges()",
        "\nstatic bool trainingRepairHumanOwnership()");
    CHECK(!nativeEdges.empty());
    CHECK(nativeEdges.find("0x3B") == std::string::npos);

    CHECK((qtraining::trainingImageMutatingActionMask()
           & qtraining::trainingActionBit(
               qtraining::TrainingAction::ReturnToCharacterSelect)) == 0u);
    CHECK(!qtraining::commandOwnsNativeImageTransaction(
        qtraining::TrainingCommandKind::ReturnToCharacterSelect));
    CHECK(!qtraining::commandMayCoalesce(
        qtraining::TrainingCommandKind::ReturnToCharacterSelect));
    CHECK_EQ(static_cast<unsigned>(qtraining::kTrainingActionCount), 16u);
    CHECK_EQ(static_cast<unsigned>(qtraining::trainingActionBit(
                 qtraining::TrainingAction::ReturnToCharacterSelect)),
             1u << 9);
    const std::string modePolicy =
        readQohSource("src/input/ModeControlPolicy.cpp");
    CHECK(modePolicy.find(
              "ModeControlAction::TrainingReturnCharacterSelect, 0x3B")
          != std::string::npos);
    const std::string hooks = readQohSource("src/GameHooks.cpp");
    CHECK(hooks.find(
              "ModeControlAction::TrainingReturnCharacterSelect")
          != std::string::npos);
    CHECK(hooks.find("TrainingAction::ReturnToCharacterSelect")
          != std::string::npos);
}

TEST(TrainingRawFighterReadoutCarriesNoInventedLabels) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string group = sourceSlice(
        wrapper,
        "void DrawTrainingFighterReadout(const Qoh99TrainingStatusV1 &status)",
        "\nvoid DrawTrainingDamageReadout(");
    CHECK(!group.empty());

    CHECK(group.find("TrainingHasFighterStatus(status)") != std::string::npos);
    CHECK(group.find("status.fighterReadoutValid") != std::string::npos);

    const std::string labels = sourceSlice(
        group, "const RawRow rows[] = {", "};");
    CHECK(!labels.empty());
    CHECK(labels.find("{ \"action\",") != std::string::npos);
    CHECK(labels.find("{ \"frame\",") != std::string::npos);
    CHECK(labels.find("{ \"lock end\",") != std::string::npos);
    CHECK(labels.find("{ \"anim\",") != std::string::npos);
    CHECK(labels.find("{ \"next\",") != std::string::npos);
    CHECK(labels.find("{ \"transition\",") != std::string::npos);
    CHECK(labels.find("{ \"prev\",") != std::string::npos);
    CHECK(group.find("DrawTrainingReadoutRow(\"move gate\"")
          != std::string::npos);

    CHECK(group.find("the names are placeholders") != std::string::npos);
    CHECK(group.find("whether a character can act") != std::string::npos);

    CHECK(group.find("static_cast<INT32>(status.p1PositionX)")
          != std::string::npos);
    CHECK(group.find("static_cast<INT32>(status.p2PositionY)")
          != std::string::npos);
}

TEST(TrainingActionStateLogIsPresentationOnlyAndFailsClosed) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    const std::string sampler = sourceSlice(
        wrapper, "void SampleTrainingActionStateLog()",
        "\nconstexpr float kIdleTestP1Column = 210.0f;");
    CHECK(!sampler.empty());

    CHECK(sampler.find("deviceState() == kDeviceWorking")
          != std::string::npos);

    CHECK(sampler.find("TrainingHasFighterStatus(status)") != std::string::npos);
    CHECK(sampler.find("status.fighterReadoutValid != 0") != std::string::npos);
    CHECK(sampler.find("TrainingHasRecoveryStatus(status)")
          != std::string::npos);
    CHECK(sampler.find("status.resourcePolicyValid != 0") != std::string::npos);
    CHECK(sampler.find("recoveryReadoutValid") == std::string::npos);

    CHECK(sampler.find("status.realStepSerialHigh") != std::string::npos);
    CHECK(sampler.find("status.realStepSerialLow") != std::string::npos);
    CHECK(sampler.find("step == g_actionStateLog.lastStepSerial")
          != std::string::npos);
    CHECK(sampler.find("step < g_actionStateLog.lastStepSerial")
          != std::string::npos);
    CHECK(sampler.find("status.explicitTrainingPresent == 0")
          != std::string::npos);
    CHECK(sampler.find("status.inMatchReady == 0") != std::string::npos);

    CHECK(sampler.find("EnqueueTrainingCommand") == std::string::npos);

    CHECK_EQ(countOccurrences(wrapper, "SampleTrainingActionStateLog();"), 1u);
    const std::string present = sourceSlice(
        wrapper,
        "extern \"C\" BOOL EFZDX9_Presenter_Present(const EfzDx9PresentParams "
        "*params)",
        "\nextern \"C\" void EFZDX9_Presenter_SetActive(");
    CHECK(!present.empty());
    const std::size_t sampleCall =
        present.find("SampleTrainingActionStateLog();");
    const std::size_t deviceGate = present.find("if (!deviceReady())");
    CHECK(sampleCall != std::string::npos);
    CHECK(deviceGate != std::string::npos);
    CHECK(sampleCall < deviceGate);
}

TEST(TrainingIdleTestRawBlockNamesBothActionFieldsAndDeclaresWhatItLacks) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    const std::string block = sourceSlice(
        wrapper,
        "void DrawTrainingIdleTestRawBlock(const Qoh99TrainingStatusV1 &status)",
        "\nstruct TrainingActionStateRowRef");
    CHECK(!block.empty());

    CHECK(block.find("sub_47CAE0") != std::string::npos);
    CHECK(block.find("sub_4017F0") != std::string::npos);
    CHECK(block.find("They are not always") != std::string::npos);
    CHECK(block.find("action (+48)") != std::string::npos);
    CHECK(block.find("dispatch (+84)") != std::string::npos);

    CHECK(block.find("not published") != std::string::npos);
    CHECK(block.find("+1456") != std::string::npos);
    CHECK(block.find("+1376") != std::string::npos);
    CHECK(block.find("0x88098C") != std::string::npos);
    CHECK(block.find("0x6275A0") != std::string::npos);

    CHECK(block.find("sizeof(Qoh99TrainingStatusV1)") == std::string::npos);
    CHECK(block.find("TrainingHasFighterStatus(status)") != std::string::npos);
    CHECK(block.find("TrainingHasRecoveryStatus(status)") != std::string::npos);

    CHECK(block.find("status.p1LifePendingDamage") != std::string::npos);
    CHECK(wrapper.find("void FormatIdleTestSigned(") != std::string::npos);

    const std::string diagnostics = sourceSlice(
        wrapper, "void DrawDiagnosticsTab(", "\nvoid DrawTrainingHelpTab()");
    CHECK(!diagnostics.empty());
    CHECK(diagnostics.find("DrawTrainingIdleTestRawBlock(status);")
          != std::string::npos);
    CHECK(diagnostics.find("DrawTrainingActionStateLog(status);")
          != std::string::npos);
    CHECK(diagnostics.find("ImGui::TreeNode(\"Idle test\")")
          != std::string::npos);
    CHECK(diagnostics.find("ImGui::TreeNode(\"Action state log\")")
          != std::string::npos);
    CHECK(diagnostics.find("DrawTrainingRestoreSummary(status);")
          != std::string::npos);
    CHECK(diagnostics.find("DrawTrainingCharacterSummary(status);")
          != std::string::npos);

    const std::string log = sourceSlice(
        wrapper,
        "void DrawTrainingActionStateLog(const Qoh99TrainingStatusV1 &status)",
        "\nvoid DrawTrainingTab(");
    CHECK(!log.empty());
    CHECK(log.find("steps missed") != std::string::npos);
    CHECK(log.find("lower bounds") != std::string::npos);
    CHECK(log.find("ImGui::SetClipboardText") != std::string::npos);
    CHECK(log.find("EnqueueTrainingCommand") == std::string::npos);
}

TEST(TrainingWrapperOffersHeuristicGuardAndNeverTheQualifiedAuto) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string tab = sourceSlice(
        wrapper, "void DrawDummyTab(const Qoh99TrainingStatusV1 &status)",
        "\nvoid DrawMacroSlotSelector(");
    CHECK(!tab.empty());

    const std::string values = sourceSlice(
        tab, "static const DWORD kGuardModeValues[] = {", "};");
    CHECK(!values.empty());
    CHECK(values.find("QOH99_TRAINING_GUARD_AUTO_HEURISTIC")
          != std::string::npos);
    CHECK(values.find("QOH99_TRAINING_GUARD_AUTO\n") == std::string::npos);
    CHECK(values.find("QOH99_TRAINING_GUARD_AUTO,") == std::string::npos);
    CHECK(values.find("QOH99_TRAINING_GUARD_AUTO ") == std::string::npos);
    CHECK(values.find("QOH99_TRAINING_GUARD_ALWAYS_AIR")
          != std::string::npos);

    const std::string labels = sourceSlice(
        tab, "static const char *kGuardModes[] = {", "};");
    CHECK(!labels.empty());
    CHECK(labels.find("\"Adaptive\"") != std::string::npos);
    CHECK(labels.find("\"All\"") != std::string::npos);
    CHECK(labels.find("Auto") == std::string::npos);
    CHECK(labels.find("\"Air\"") != std::string::npos);

    CHECK(tab.find("status.guardMode == QOH99_TRAINING_GUARD_AUTO_HEURISTIC")
          != std::string::npos);
    CHECK(tab.find("is a rough rule, not a real blocker") != std::string::npos);
    CHECK(tab.find("if (ImGui::TreeNode(\"What it gets wrong\"))")
          != std::string::npos);
    CHECK(tab.find("Grounded overheads") != std::string::npos);
    CHECK(tab.find("Throws. Nothing blocks those.") != std::string::npos);
    CHECK(tab.find("Projectiles and helpers") != std::string::npos);
    CHECK(tab.find("switching ground/air on the contact frame")
          != std::string::npos);

    CHECK(tab.find("status.guardMode == QOH99_TRAINING_GUARD_ALWAYS_AIR")
          != std::string::npos);
    CHECK(tab.find("makes the dummy jump. That is intentional")
          != std::string::npos);
    CHECK(tab.find("if (ImGui::TreeNode(\"Why it jumps\"))")
          != std::string::npos);
    CHECK(tab.find("It retreats while it does this") != std::string::npos);

    CHECK(tab.find("QOH99_TRAINING_COMMAND_SET_TECH") == std::string::npos);
}

TEST(TrainingWrapperGatesNewGroupsOnTheirOwnTier) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    const std::string gate = sourceSlice(
        wrapper,
        "bool TrainingHasRecoveryStatus(const Qoh99TrainingStatusV1 &status)",
        "\nvoid DrawTrainingStaleDllReason(");
    CHECK(!gate.empty());
    CHECK(gate.find("QOH99_TRAINING_STATUS_V1_RECOVERY_TIER")
          != std::string::npos);
    CHECK(gate.find("sizeof(Qoh99TrainingStatusV1)") == std::string::npos);

    const std::string recovery = sourceSlice(
        wrapper, "void DrawDummyRecoverySection(", "\nbool RecoveryActionInBranchSet(");
    CHECK(!recovery.empty());
    CHECK(recovery.find("TrainingHasRecoveryStatus(status)")
          != std::string::npos);
    CHECK(recovery.find("DrawTrainingStaleDllReason(") != std::string::npos);
    CHECK(recovery.find("DrawTrainingUnavailableReason(") != std::string::npos);
    CHECK(recovery.find("ImGui::PushID(\"recovery_policies\")")
          != std::string::npos);
    CHECK(recovery.find("ImGui::PushID(static_cast<int>(slots[i].slot))")
          != std::string::npos);

    const std::string values = sourceSlice(
        wrapper, "void DrawTrainingResourceValues(",
        "\nvoid DrawTrainingResourcePolicies(");
    CHECK(!values.empty());
    CHECK(values.find("TrainingHasRecoveryStatus(status)")
          != std::string::npos);
    CHECK(values.find(
              "\"Health##set_life\", &g_trainingUi.lifeSetValue, 1, lifeMax")
          != std::string::npos);
    CHECK(values.find("status.resourcePolicyValid != 0") != std::string::npos);
    CHECK(values.find("status.lifeSetMax != 0") != std::string::npos);

    const std::string policies = sourceSlice(
        wrapper, "void DrawTrainingResourcePolicies(",
        "\nvoid DrawTrainingHoldControls(");
    CHECK(!policies.empty());
    CHECK(policies.find("TrainingHasRecoveryStatus(status)")
          != std::string::npos);

    const std::string idle = sourceSlice(
        wrapper,
        "void DrawTrainingIdleTestRawBlock(const Qoh99TrainingStatusV1 &status)",
        "\nstruct TrainingActionStateRowRef");
    CHECK(!idle.empty());

    CHECK(idle.find("A working definition, not a complete")
          != std::string::npos);
    CHECK(idle.find("It fails closed on purpose") != std::string::npos);
    CHECK(idle.find("\"Both idle, walking or crouching") != std::string::npos);
    CHECK(idle.find("Every state it accepts") != std::string::npos);
    CHECK(idle.find("you can act out of") != std::string::npos);
    CHECK(idle.find("The reverse does not hold") != std::string::npos);
    CHECK(idle.find("conservative floor") != std::string::npos);
    CHECK(idle.find("free movement -- idle, walk forward, walk back")
          != std::string::npos);
    CHECK(idle.find("being hit and being thrown all read as not neutral")
          != std::string::npos);
    CHECK(policies.find("holding a stance") == std::string::npos);
    CHECK(idle.find("holding a stance") == std::string::npos);
    CHECK(idle.find("needs BOTH characters") != std::string::npos);
    CHECK(idle.find("TrainingAppendNeutralReasons(") != std::string::npos);
    CHECK(idle.find("Restored %lu times, refused %lu") != std::string::npos);

    CHECK(policies.find("TrainingAppendNeutralReasons(") == std::string::npos);
    CHECK(policies.find("TrainingComboEpisodeName(") == std::string::npos);

    CHECK(policies.find("\"Restore full HP") != std::string::npos);
    CHECK(policies.find("\"Restore all") != std::string::npos);
    CHECK(policies.find("##policy_target") == std::string::npos);
    CHECK(policies.find("QOH99_TRAINING_TRIGGER_WHEN_NEUTRAL")
          != std::string::npos);
    CHECK(policies.find("QOH99_TRAINING_TRIGGER_OFF") != std::string::npos);
    CHECK(policies.find("QOH99_TRAINING_REFILL_BOTH") != std::string::npos);
    CHECK(policies.find("QOH99_TRAINING_COMMAND_SET_HEALTH_POLICY")
          != std::string::npos);
    CHECK(policies.find("QOH99_TRAINING_COMMAND_SET_EX_POLICY")
          != std::string::npos);
    CHECK(policies.find("QOH99_TRAINING_COMMAND_SET_GUARD_GAUGE_POLICY")
          != std::string::npos);
    CHECK(policies.find("status.healthTrigger") != std::string::npos);
}

TEST(TrainingCombatToolUiMirrorsTheAppendAndHidesTheLegacyThrowRow) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    const std::string gate = sourceSlice(
        wrapper,
        "bool TrainingHasCombatToolsStatus(const Qoh99TrainingStatusV1 &status)",
        "\nvoid SeedTrainingCombatToolDrafts(");
    CHECK(!gate.empty());
    CHECK(gate.find("QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER")
          != std::string::npos);
    CHECK(gate.find("QOH99_TRAINING_STATUS_V1_ACTION_TRIGGERS_TIER")
          != std::string::npos);
    CHECK(gate.find("trainingMoveCatalogId()") != std::string::npos);
    CHECK(gate.find("sizeof(Qoh99TrainingStatusV1)") == std::string::npos);

    const std::string seed = sourceSlice(
        wrapper, "void SeedTrainingCombatToolDrafts(",
        "\nstruct PresenterModeConfig");
    CHECK(!seed.empty());
    CHECK(seed.find("status.throwTechChancePercent") != std::string::npos);
    CHECK(seed.find("status.throwTechChancePercent != 0u")
          != std::string::npos);
    CHECK(seed.find("status.triggerMacroSlot[event]") != std::string::npos);
    CHECK(seed.find("status.triggerMoveIndex[event]") != std::string::npos);
    CHECK(seed.find("status.triggerDelaySteps[event]") != std::string::npos);
    CHECK(seed.find("status.triggerChancePercent[event]")
          != std::string::npos);
    CHECK(seed.find("event < QOH99_TRAINING_TRIGGER_EVENT_COUNT")
          != std::string::npos);
    CHECK(seed.find("QOH99_TRAINING_TRIGGER_MAX_DELAY_STEPS")
          != std::string::npos);

    const std::string enqueue = sourceSlice(
        wrapper, "void EnqueueTrainingTriggerConfig(",
        "\nvoid DrawDummyReactionsSection(");
    CHECK(!enqueue.empty());
    const std::string compactEnqueue = withoutWhitespace(enqueue);
    CHECK(compactEnqueue.find(
              "triggerDelaySteps[event])&0xFFFFu") != std::string::npos);
    CHECK(compactEnqueue.find(
              "triggerChancePercent[event])&0xFFFFu)<<16u")
          != std::string::npos);
    CHECK(enqueue.find("QOH99_TRAINING_COMMAND_SET_TRIGGER_CONFIG")
          != std::string::npos);
    CHECK(enqueue.find("QOH99_TRAINING_TRIGGER_MOVE_VALUE_FLAG")
          != std::string::npos);
    CHECK(enqueue.find("if (move.valid()) return move.input.data();")
          != std::string::npos);
    CHECK(enqueue.find("TrainingMoveContextLabel(") != std::string::npos);
    CHECK(wrapper.find("move.label.data()") == std::string::npos);

    const std::string reactions = sourceSlice(
        wrapper, "void DrawDummyReactionsSection(", "\nbool TrainingContactPolicyReady(");
    CHECK(!reactions.empty());
    const std::string eventNames = sourceSlice(
        reactions, "static const char *const kEventNames[] = {", "\n    };");
    CHECK(!eventNames.empty());
    for (const char* label : {"On hit", "After block", "After hitstun",
                              "On wakeup", "After air tech",
                              "After ground tech", "After throw tech",
                              "Guard cancel"}) {
        CHECK(eventNames.find(label) != std::string::npos);
    }
    CHECK_EQ(countOccurrences(eventNames, "\""), 16u);
    const std::string eventTips = sourceSlice(
        reactions, "static const char *const kEventTips[] = {", "\n    };");
    CHECK(!eventTips.empty());
    CHECK(eventTips.find("The moment your attack connects.")
          != std::string::npos);
    CHECK(eventTips.find("after being hit.") != std::string::npos);
    CHECK(eventTips.find("Getting up with no tech.") != std::string::npos);
    CHECK(eventTips.find("Auto recovery uses a tech row instead.")
          != std::string::npos);
    CHECK(reactions.find("ImGui::SeparatorText(qi18n::tr(\"Triggers\"));")
          != std::string::npos);
    CHECK(reactions.find("Pick what the dummy does after something happens")
          != std::string::npos);
    CHECK(reactions.find("if (ImGui::TreeNode(\"Details\"))")
          != std::string::npos);
    CHECK(reactions.find("Guard cancel first, then wakeup and ")
          != std::string::npos);
    CHECK(reactions.find("priority is: Guard cancel, exact wake-up or ")
          == std::string::npos);
    CHECK(reactions.find("Automatic recovery replaces a natural wake-up")
          == std::string::npos);
    CHECK(reactions.find("ImGui::Selectable(\"Off\"")
          != std::string::npos);
    CHECK(reactions.find("\"Recording %d%s\"") != std::string::npos);
    CHECK(reactions.find("status.validMacroSlots & (1u << (slot - 1))")
          != std::string::npos);
    CHECK(reactions.find("slot <= static_cast<int>(QOH99_TRAINING_SLOT_COUNT)")
          != std::string::npos);
    CHECK(reactions.find("trainingMoveCount(") != std::string::npos);
    CHECK(wrapper.find("trainingMoveTriggerEligible(")
          != std::string::npos);
    CHECK(reactions.find("TrainingMoveWasAlreadyListed(")
          != std::string::npos);
    CHECK(reactions.find("ImGui::Selectable(move.input.data(), selected)")
          != std::string::npos);
    CHECK(reactions.find("TrainingMoveContextLabel(move.context)")
          != std::string::npos);
    CHECK(reactions.find("Character moves") != std::string::npos);
    CHECK(reactions.find("QOH99_TRAINING_CAP_TRIGGERS")
          != std::string::npos);
    CHECK(reactions.find("QOH99_TRAINING_TRIGGER_MAX_DELAY_STEPS")
          != std::string::npos);
    CHECK(reactions.find("QOH99_TRAINING_TRIGGER_MAX_CHANCE_PERCENT")
          != std::string::npos);
    CHECK(reactions.find("QOH99_TRAINING_STATUS_V1_COMBAT_TOOLS_TIER")
          != std::string::npos);

    const std::string dummy = sourceSlice(
        wrapper, "void DrawDummyTab(", "\nvoid DrawMacroSlotSelector(");
    CHECK(!dummy.empty());
    CHECK(dummy.find("ImGui::BeginTabItem(qi18n::tr(\"Guard & stance\"))")
          != std::string::npos);
    CHECK(dummy.find("ImGui::BeginTabItem(qi18n::tr(\"Recovery\"))") != std::string::npos);
    CHECK(dummy.find("ImGui::BeginTabItem(qi18n::tr(\"Triggers\"))") == std::string::npos);
    CHECK(dummy.find("ImGui::BeginTabItem(qi18n::tr(\"Reactions\"))")
          == std::string::npos);
    CHECK(dummy.find("DrawDummyReactionsSection(status);")
          == std::string::npos);

    const std::string recovery = sourceSlice(
        wrapper, "void DrawDummyRecoverySection(",
        "\nbool RecoveryActionInBranchSet(");
    CHECK(!recovery.empty());
    const std::string throwTech = sourceSlice(
        recovery, "ImGui::SeparatorText(qi18n::tr(\"Throw tech\"))",
        "\n    ImGui::EndDisabled();");
    CHECK(!throwTech.empty());
    CHECK(throwTech.find("Automatic throw tech##throw_tech")
          != std::string::npos);
    CHECK(throwTech.find("ImGui::Checkbox(") != std::string::npos);
    CHECK(throwTech.find("ImGui::SliderInt(") == std::string::npos);
    CHECK(throwTech.find("QOH99_TRAINING_TRIGGER_MAX_CHANCE_PERCENT : 0u")
          != std::string::npos);
    CHECK(recovery.find("QOH99_TRAINING_CAP_THROW_TECH")
          != std::string::npos);
    CHECK(recovery.find("QOH99_TRAINING_COMMAND_SET_THROW_TECH_CHANCE")
          != std::string::npos);
    const std::string selectorRows = sourceSlice(
        recovery, "const SlotRow slots[] = {", "\n    };\n");
    CHECK(!selectorRows.empty());
    CHECK(selectorRows.find("QOH99_TRAINING_RECOVERY_SLOT_THROW_ESCAPE")
          == std::string::npos);

    const std::string view = sourceSlice(
        wrapper, "void DrawViewTab(", "\nvoid DrawTrainingHotkeysTab(");
    CHECK(!view.empty());
    CHECK(view.find("Show frame advantage") != std::string::npos);
    CHECK(view.find("QOH99_TRAINING_CAP_FRAME_ADVANTAGE")
          != std::string::npos);
    CHECK(view.find("QOH99_TRAINING_COMMAND_SET_FRAME_ADVANTAGE_ENABLED")
          != std::string::npos);

    const std::string overlay = sourceSlice(
        wrapper, "void DrawTrainingFrameAdvantageOverlay(",
        "\nvoid DrawTexturedQuad(");
    CHECK(!overlay.empty());
    CHECK(overlay.find("status.explicitTrainingPresent == 0")
          != std::string::npos);
    CHECK(overlay.find("g_presenter.menuVisible && g_hudPreview")
          != std::string::npos);
    CHECK(overlay.find("status.inMatchReady == 0")
          != std::string::npos);
    CHECK(overlay.find(
              "status.lifecycleState != QOH99_TRAINING_LIFECYCLE_ACTIVE")
          != std::string::npos);
    CHECK(overlay.find("TrainingHasCombatToolsStatus(status)")
          != std::string::npos);
    CHECK(overlay.find("QOH99_TRAINING_CAP_FRAME_ADVANTAGE")
          != std::string::npos);
    CHECK(overlay.find("status.frameAdvantageEnabled == 0")
          != std::string::npos);
    CHECK(overlay.find("status.combatToolsReadoutValid == 0")
          != std::string::npos);
    CHECK(overlay.find("static_cast<INT32>(status.frameAdvantageValue)")
          != std::string::npos);
    CHECK(overlay.find("P%u  %+d  %s") != std::string::npos);
    CHECK(overlay.find("QOH99_TRAINING_CONTACT_BLOCK")
          != std::string::npos);
    CHECK(overlay.find("QOH99_TRAINING_CONTACT_THROW")
          != std::string::npos);
    CHECK(overlay.find("contact = \"throw\"") != std::string::npos);
    CHECK(overlay.find("\"Trade\"") != std::string::npos);
    CHECK(overlay.find("QOH99_TRAINING_FRAME_ADVANTAGE_MEASURING")
          == std::string::npos);
    CHECK(overlay.find("QOH99_TRAINING_FRAME_ADVANTAGE_TIMED_OUT")
          == std::string::npos);
    CHECK(overlay.find("QOH99_TRAINING_FRAME_ADVANTAGE_UNMEASURABLE")
          == std::string::npos);
    CHECK(overlay.find("Frame advantage ...") == std::string::npos);
    CHECK(overlay.find("Frame advantage: no result") == std::string::npos);
    CHECK(overlay.find("HudLayoutEnsureLoaded();") != std::string::npos);
    CHECK(overlay.find("g_hudLayout.fontScalePct") != std::string::npos);
    CHECK(overlay.find("HudResolveX(params, g_hudLayout.sx")
          != std::string::npos);
    CHECK(overlay.find("HudResolveY(params, g_hudLayout.sy")
          != std::string::npos);
    CHECK(overlay.find("kHudDefSessionX") != std::string::npos);
    CHECK(overlay.find("kHudDefSessionY") != std::string::npos);
    CHECK(overlay.find("GameToScreenX(params, 320.0f)")
          == std::string::npos);
    CHECK(overlay.find("GameToScreenY(params, 118.0f)")
          == std::string::npos);
    CHECK(overlay.find("EnqueueTrainingCommand") == std::string::npos);
    CHECK(wrapper.find("DrawTrainingFrameAdvantageOverlay(params);")
          != std::string::npos);

    const std::string debugOverlay = sourceSlice(
        wrapper, "void DrawTrainingDebugOverlay(",
        "\nvoid DrawTexturedQuad(");
    CHECK(!debugOverlay.empty());
    CHECK(debugOverlay.find("TrainingHasDebugOverlayStatus(status)")
          != std::string::npos);
    CHECK(debugOverlay.find("status.debugOverlayEnabled == 0u")
          != std::string::npos);
    CHECK(debugOverlay.find("status.resourcePolicyValid != 0u")
          != std::string::npos);
    CHECK(debugOverlay.find("status.p1ActionFrame") != std::string::npos);
    CHECK(debugOverlay.find("status.p2ActionFrame") != std::string::npos);
    CHECK(debugOverlay.find("status.throwTechOpportunities")
          != std::string::npos);
    CHECK(debugOverlay.find("status.throwTechAutomaticAccepted")
          != std::string::npos);
    CHECK(debugOverlay.find("status.throwTechResultObserved")
          != std::string::npos);
    CHECK(debugOverlay.find("ImGui::GetFontSize()") != std::string::npos);
    CHECK(debugOverlay.find("g_presenter.menuVisible") == std::string::npos);
    CHECK(wrapper.find("DrawTrainingDebugOverlay(params);")
          != std::string::npos);

    const std::string invalidate = sourceSlice(
        wrapper, "void InvalidateTrainingUiDrafts()",
        "\nvoid RefreshTrainingUiForStatus(");
    CHECK(!invalidate.empty());
    CHECK(invalidate.find("g_trainingUi.combatToolsDraftsSeeded = false")
          != std::string::npos);
}

TEST(TrainingWrapperRecoveryTermsUseTheSameMasksAsTheDll) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string debug = sourceSlice(
        wrapper, "void DrawTrainingRecoveryDebug(", "\nvoid DrawDummyTab(");
    CHECK(!debug.empty());

    CHECK(debug.find("status.p2MoveGateByte & 0x0Fu") != std::string::npos);
    CHECK(debug.find("status.p2MoveGateByte & 0x07u") != std::string::npos);
    CHECK(withoutWhitespace(debug).find(
              "manualPosture==1||manualPosture==2||manualPosture==4")
          != std::string::npos);
    CHECK(debug.find("terminalPosture == 1 || terminalPosture == 2")
          != std::string::npos);

    CHECK(debug.find("\"touching a wall\"") != std::string::npos);
    CHECK(debug.find("TrainingHasWallStatus(status)") != std::string::npos);
    CHECK(debug.find("\"world LEFT\"") != std::string::npos);
    CHECK(debug.find("\"wall contact (+1390)\"") != std::string::npos);
    CHECK(debug.find("facing byte %lu") != std::string::npos);
    CHECK(debug.find("facing left") == std::string::npos);

    const std::string gate = sourceSlice(
        wrapper,
        "bool TrainingHasWallStatus(const Qoh99TrainingStatusV1 &status)",
        "\nbool TrainingHasRecoveryDiagnosticStatus(");
    CHECK(!gate.empty());
    CHECK(gate.find("QOH99_TRAINING_STATUS_V1_WALL_TIER") != std::string::npos);
    CHECK(gate.find("sizeof(Qoh99TrainingStatusV1)") == std::string::npos);
    const std::string diagGate = sourceSlice(
        wrapper,
        "bool TrainingHasRecoveryDiagnosticStatus(",
        "\nvoid DrawTrainingStaleDllReason(");
    CHECK(!diagGate.empty());
    CHECK(diagGate.find("QOH99_TRAINING_STATUS_V1_RECOVERY_DIAG_TIER")
          != std::string::npos);
    CHECK(diagGate.find("sizeof(Qoh99TrainingStatusV1)") == std::string::npos);
}

TEST(TrainingWrapperNamesEveryReasonRecoveryDidNotFire) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string debug = sourceSlice(
        wrapper, "void DrawTrainingRecoveryDebug(", "\nvoid DrawDummyTab(");
    CHECK(!debug.empty());

    CHECK(debug.find("TrainingHasRecoveryDiagnosticStatus(status)")
          != std::string::npos);
    CHECK(debug.find("TrainingRecoveryEvaluatorReasonText(")
          != std::string::npos);
    CHECK(debug.find("TrainingRecoveryEvaluatorAllowsFiring(")
          != std::string::npos);
    CHECK(debug.find("QOH99_TRAINING_STATUS_V1_RECOVERY_DIAG_TIER")
          != std::string::npos);
    const std::string reasons = sourceSlice(
        wrapper, "const char *TrainingRecoveryEvaluatorReasonText(",
        "\nbool TrainingRecoveryEvaluatorAllowsFiring(");
    CHECK(!reasons.empty());
    CHECK(reasons.find("QOH99_TRAINING_RECOVERY_EVALUATOR_NOT_ATTEMPTED")
          != std::string::npos);
    CHECK(reasons.find("build-verification check") != std::string::npos);
    CHECK(reasons.find("QOH99_TRAINING_RECOVERY_EVALUATOR_ADDRESS_UNRESOLVED")
          != std::string::npos);
    CHECK(reasons.find("could not locate the game's own") != std::string::npos);
    CHECK(reasons.find("QOH99_TRAINING_RECOVERY_EVALUATOR_PROLOGUE_MISMATCH")
          != std::string::npos);
    CHECK(reasons.find("bytes there are not what it expects")
          != std::string::npos);
    CHECK(reasons.find("QOH99_TRAINING_RECOVERY_EVALUATOR_QUALIFIED")
          != std::string::npos);
    CHECK(reasons.find("recovery will work") == std::string::npos);

    CHECK(debug.find("QOH99_TRAINING_TERMINAL_PRESERVE") != std::string::npos);
    CHECK(debug.find("Leave it alone") != std::string::npos);
    CHECK(debug.find("setting: off -- not attempted") != std::string::npos);
    CHECK(debug.find("status.recoveryModeAir") != std::string::npos);
    CHECK(debug.find(
              "branch == QOH99_TRAINING_RECOVERY_BRANCH_THROW_ESCAPE")
          != std::string::npos);

    const std::string recoveryControls = sourceSlice(
        wrapper, "void DrawDummyRecoverySection(",
        "\nbool RecoveryActionInBranchSet(");
    const std::string recoverySummary = sourceSlice(
        wrapper, "void DrawTrainingRecoverySummary(",
        "\nvoid DrawOverlayStatusSummary(");
    CHECK(!recoveryControls.empty());
    CHECK(!recoverySummary.empty());
    const std::string selectorRows = sourceSlice(
        recoveryControls, "const SlotRow slots[] = {", "\n    };\n");
    CHECK(!selectorRows.empty());
    CHECK(selectorRows.find("QOH99_TRAINING_RECOVERY_SLOT_THROW_ESCAPE")
          == std::string::npos);
    CHECK(selectorRows.find("Throw escape") == std::string::npos);
    CHECK(recoverySummary.find("recoveryModeThrowEscape")
          == std::string::npos);
    CHECK(recoverySummary.find("Throw escape") == std::string::npos);

    CHECK(debug.find("(+48 is %lu)") != std::string::npos);
    CHECK(debug.find("\"action (+48)\"") != std::string::npos);

    CHECK(debug.find("\"+158 & 0x08\"") != std::string::npos);
    CHECK(debug.find("\"+158 & 0x04, 0x08 clear\"") != std::string::npos);
    CHECK(debug.find("\"this frame allows it\"") == std::string::npos);

    CHECK(debug.find("(lock is %lu)") != std::string::npos);

    CHECK(debug.find("status.recoveryTapsEmitted") != std::string::npos);
    CHECK(debug.find("after %lu tap%s") != std::string::npos);

    CHECK(debug.find("status.recoveryArmableMask") != std::string::npos);
    CHECK(debug.find("\"armable\"") != std::string::npos);

    const std::string standDown = sourceSlice(
        wrapper, "void DrawTrainingRecoveryStandDown(",
        "\nvoid DrawTrainingRecoveryDebug(");
    CHECK(!standDown.empty());
    CHECK(standDown.find("sizeof(Qoh99TrainingStatusV1)") == std::string::npos);
    CHECK(standDown.find("QOH99_TRAINING_RECOVERY_STAND_DOWN_NOT_READABLE")
          != std::string::npos);
    CHECK(standDown.find("QOH99_TRAINING_RECOVERY_STAND_DOWN_KO_LATCH")
          != std::string::npos);
    CHECK(standDown.find(
              "QOH99_TRAINING_RECOVERY_STAND_DOWN_DIRECTION_SUPPRESSED")
          != std::string::npos);
    CHECK(standDown.find("can NEVER produce a recovery") != std::string::npos);
    CHECK(standDown.find("Close the menu") != std::string::npos);
    CHECK(standDown.find("QOH99_TRAINING_HOLD_MENU") != std::string::npos);
    CHECK(standDown.find("last frame that actually ran") != std::string::npos);
    CHECK(debug.find("DrawTrainingRecoveryStandDown(status, haveDiagnostics)")
          != std::string::npos);

    const std::string dummy = sourceSlice(
        wrapper, "void DrawDummyRecoverySection(",
        "\nbool RecoveryActionInBranchSet(");
    CHECK(!dummy.empty());
    CHECK(dummy.find("TrainingHasRecoveryDiagnosticStatus(status)")
          != std::string::npos);
    CHECK(dummy.find("TrainingRecoveryEvaluatorReasonText(")
          != std::string::npos);
    CHECK(dummy.find("!haveEvaluatorReason") != std::string::npos);
}

TEST(TrainingRecoveryDiagnosticsReportTheGateAndNeverOpenIt) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    const std::string compact = withoutWhitespace(inl);

    CHECK(compact.find("qtraining::recoveryEvaluatorReasonFor(")
          != std::string::npos);
    CHECK(compact.find("status.recoveryEvaluatorReason=static_cast<uint32_t>("
                       "g_trainingRecoveryEvaluatorReason);")
          != std::string::npos);
    CHECK(compact.find("recoveryEvaluatorReasonFor(true,"
                       "report.addressesResolved,report.prologuesMatched)")
          != std::string::npos);
    CHECK(compact.find("RecoveryEvaluatorReason::Qualified") == std::string::npos);
    CHECK(compact.find("g_trainingRecoveryEvaluatorQualified="
                       "report.addressesResolved&&report.prologuesMatched;")
          != std::string::npos);
    CHECK(compact.find("evaluatorQualified=true;") == std::string::npos);

    CHECK(compact.find("nativeRecoveryReleaseQualified(mode,"
                       "g_trainingRecoverySettings.evaluatorQualified))")
          != std::string::npos);
    CHECK(compact.find("returnCommandAckResult::RejectedUnavailable;")
          != std::string::npos);

    const std::string header =
        readQohSource("src/training/TrainingRecovery.h");
    CHECK(!header.empty());
    CHECK(header.find("bool attempted") != std::string::npos);
    CHECK(header.find("NotAttempted") != std::string::npos);
    CHECK(withoutWhitespace(header).find("boolevaluatorQualified=false;")
          != std::string::npos);

    const std::string recoveryCpp =
        readQohSource("src/training/TrainingRecovery.cpp");
    CHECK(!recoveryCpp.empty());
    for (const char* accessor : {"rd8(", "rd32(", "wr8(", "wr32(",
                                 "rangeReadable(", "trainingFighter(",
                                 "uintptr_t", "windows.h"}) {
        CHECK(recoveryCpp.find(accessor) == std::string::npos);
        CHECK(header.find(accessor) == std::string::npos);
    }
}

TEST(TrainingRecoveryEvaluatorFunctionsAreVerifiedAndNeverHooked) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    const std::string hooks = readQohSource("src/GameHooks.cpp");
    CHECK(!inl.empty());
    CHECK(!hooks.empty());
    const std::string inlCode = withoutWhitespace(inl);
    const std::string hooksCode = withoutWhitespace(hooks);

    for (const char* target : {"0x47CAE0", "0x47B8F0", "0x47BF70",
                               "FunctionId::ManualRecoveryEvaluator",
                               "FunctionId::CommandMatch",
                               "FunctionId::FighterRecordInput"}) {
        CHECK(inlCode.find(target) == std::string::npos);
        CHECK(hooksCode.find(target) == std::string::npos);
    }

    CHECK(inlCode.find("trainingQualifyRecoveryEvaluator();")
          != std::string::npos);
    CHECK(hooksCode.find("installTrainingHooksOnce()") != std::string::npos);
    const std::size_t installTraining =
        hooksCode.find("installTrainingHooksOnce()");
    const std::size_t applyQueued = hooksCode.rfind("MH_ApplyQueued()");
    CHECK(installTraining != std::string::npos);
    CHECK(applyQueued != std::string::npos);
    CHECK(installTraining < applyQueued);

    CHECK(inlCode.find("trainingReadImageBytes(void*") != std::string::npos);
    CHECK(inlCode.find("se::verifyCodeBytes(kTrainingRecovery")
          == std::string::npos);
    CHECK(inlCode.find("if(!rangeReadable(address,count))returnfalse;")
          != std::string::npos);

    const std::string qualification =
        readQohSource("src/training/NativeRecoveryQualification.cpp");
    const std::string qualificationHeader =
        readQohSource("src/training/NativeRecoveryQualification.h");
    CHECK(!qualification.empty());
    CHECK(!qualificationHeader.empty());
    CHECK(qualificationHeader.find("kRecoveryRequirementCount = 3")
          != std::string::npos);
    CHECK(qualification.find("FunctionId::ThrowTechEvaluator")
          == std::string::npos);
    CHECK(qualification.find("FunctionId::ThrowEscapeEvaluator")
          == std::string::npos);
    CHECK(qualification.find("FunctionId::ThrowContactTest")
          == std::string::npos);
    for (const char* forbidden : {"wr8(", "wr32(", "MH_CreateHook(",
                                  "MH_EnableHook(", "MH_QueueEnableHook(",
                                  "VirtualProtect(", "std::memcpy("}) {
        CHECK(qualification.find(forbidden) == std::string::npos);
        CHECK(qualificationHeader.find(forbidden) == std::string::npos);
    }
    CHECK(qualification.find("resolveSelectedFunction") != std::string::npos);
    CHECK(qualification.find("resolveNativeSymbolVa(") != std::string::npos);
    CHECK(qualification.find("profile.id") != std::string::npos);
    CHECK(qualification.find("symbol.decemberPrologue") != std::string::npos);
    CHECK(qualification.find("symbol.novemberPrologue") != std::string::npos);
    CHECK(qualification.find("SignatureNotCurated") != std::string::npos);
    CHECK(qualification.find("PrologueSignatureStatus::Curated")
          != std::string::npos);
}

TEST(TrainingCombatHooksAreProfileQualifiedAtomicAndOfflineOnly) {
    using qoh99::generated_model::BuildId;
    using qoh99::generated_model::FunctionId;
    using qoh99::generated_model::NativeRuntimeUse;
    using qoh99::generated_model::NativeShiftClass;
    using qoh99::generated_model::NativeSymbolId;
    using qoh99::generated_model::PrologueSignatureStatus;

    struct HookSeam {
        FunctionId functionId;
        NativeSymbolId symbolId;
        uint32_t novemberVa;
        uint32_t decemberVa;
    };
    const HookSeam seams[] = {
        { FunctionId::ThrowTechEvaluator, NativeSymbolId::ThrowTechEvaluator,
          0x47D9B0u, 0x47D9D0u },
        { FunctionId::GuardContactResolver,
          NativeSymbolId::GuardContactResolver, 0x41D950u, 0x41D950u },
        { FunctionId::HitContactResolver,
          NativeSymbolId::HitContactResolver, 0x466770u, 0x466770u },
        { FunctionId::NaturalActionTerminal,
          NativeSymbolId::NaturalActionTerminal, 0x412290u, 0x412290u },
    };
    for (const HookSeam& seam : seams) {
        CHECK_EQ(qoh99::generated_model::resolveFunctionVa(
                     BuildId::Se2006, seam.functionId), seam.novemberVa);
        CHECK_EQ(qoh99::generated_model::resolveFunctionVa(
                     BuildId::Game2000, seam.functionId), seam.decemberVa);
        CHECK_EQ(qoh99::generated_model::resolveNativeSymbolVa(
                     BuildId::Se2006, seam.symbolId), seam.novemberVa);
        CHECK_EQ(qoh99::generated_model::resolveNativeSymbolVa(
                     BuildId::Game2000, seam.symbolId), seam.decemberVa);
        const qoh99::generated_model::NativeSymbol* symbol =
            qoh99::generated_model::findNativeSymbol(seam.symbolId);
        CHECK(symbol != nullptr);
        if (symbol == nullptr) continue;
        CHECK(symbol->functionId == seam.functionId);
        CHECK(symbol->novemberPrologue.status
              == PrologueSignatureStatus::Curated);
        CHECK(symbol->decemberPrologue.status
              == PrologueSignatureStatus::Curated);
        CHECK_EQ(symbol->novemberPrologue.size, 16u);
        CHECK_EQ(symbol->decemberPrologue.size, 16u);
        CHECK(symbol->novemberPrologue.bytes != nullptr);
        CHECK(symbol->decemberPrologue.bytes != nullptr);
        CHECK(std::memcmp(symbol->novemberPrologue.bytes,
                          symbol->decemberPrologue.bytes, 16u) == 0);
    }
    const uint8_t throwTechPrologue[16] = {
        0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0xF0, 0x00,
        0x00, 0x00, 0xA9, 0x00, 0x00, 0xFF, 0xFF, 0x56,
    };
    const qoh99::generated_model::NativeSymbol* throwTechSymbol =
        qoh99::generated_model::findNativeSymbol(
            NativeSymbolId::ThrowTechEvaluator);
    CHECK(throwTechSymbol != nullptr);
    if (throwTechSymbol != nullptr) {
        CHECK(std::memcmp(throwTechSymbol->novemberPrologue.bytes,
                          throwTechPrologue,
                          sizeof(throwTechPrologue)) == 0);
        CHECK(std::memcmp(throwTechSymbol->decemberPrologue.bytes,
                          throwTechPrologue,
                          sizeof(throwTechPrologue)) == 0);
    }
    const uint8_t throwInstallPrologue[16] = {
        0x56, 0x57, 0x8B, 0xF9, 0xB8, 0x46, 0x00, 0x00,
        0x00, 0x8B, 0xF2, 0x39, 0x47, 0x48, 0x0F, 0x85,
    };
    const qoh99::generated_model::NativeSymbol* throwInstallSymbol =
        qoh99::generated_model::findNativeSymbol(
            NativeSymbolId::EditedActorTransition);
    CHECK(throwInstallSymbol != nullptr);
    if (throwInstallSymbol != nullptr) {
        CHECK_EQ(qoh99::generated_model::resolveNativeSymbolVa(
                     BuildId::Se2006,
                     NativeSymbolId::EditedActorTransition), 0x42A440u);
        CHECK_EQ(qoh99::generated_model::resolveNativeSymbolVa(
                     BuildId::Game2000,
                     NativeSymbolId::EditedActorTransition), 0x42A440u);
        CHECK(throwInstallSymbol->functionId == FunctionId::Unknown);
        CHECK(throwInstallSymbol->shiftClass
              == NativeShiftClass::SemanticallyEdited);
        CHECK(throwInstallSymbol->runtimeUse
              == NativeRuntimeUse::SeamResolved);
        CHECK(throwInstallSymbol->novemberPrologue.status
              == PrologueSignatureStatus::Curated);
        CHECK(throwInstallSymbol->decemberPrologue.status
              == PrologueSignatureStatus::Curated);
        CHECK_EQ(throwInstallSymbol->novemberPrologue.size, 16u);
        CHECK_EQ(throwInstallSymbol->decemberPrologue.size, 16u);
        CHECK(std::memcmp(throwInstallSymbol->novemberPrologue.bytes,
                          throwInstallPrologue,
                          sizeof(throwInstallPrologue)) == 0);
        CHECK(std::memcmp(throwInstallSymbol->decemberPrologue.bytes,
                          throwInstallPrologue,
                          sizeof(throwInstallPrologue)) == 0);
    }

    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());
    const std::string compactRuntime = withoutWhitespace(runtime);
    CHECK(compactRuntime.find(
              "kTrainingFighterAuthoredFlagsOffset=158u;")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "kTrainingFighterThrowTechLatchOffset=1472u;")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "kTrainingFighterPairedActorOffset=1436u;")
          != std::string::npos);
    CHECK(compactRuntime.find("kTrainingFighterFreezeOffset=1500u;")
          != std::string::npos);

    const std::string throwReceptionProof = sourceSlice(
        runtime, "static bool trainingCombatExactThrowReception(",
        "\nstatic int __fastcall hkTrainingGuardContact(");
    CHECK(!throwReceptionProof.empty());
    const std::string compactThrowReceptionProof =
        withoutWhitespace(throwReceptionProof);
    CHECK(compactThrowReceptionProof.find(
              "combatActionIsThrowReception(action)"
              "&&(dispatch==qtraining::TrainingDispatchFamily::ThrowReception"
              "||dispatch==qtraining::TrainingDispatchFamily::"
              "ThrowReception208)")
          != std::string::npos);
    CHECK(compactThrowReceptionProof.find(
              "rd32(attacker+kTrainingFighterPairedActorOffset)!=victim")
          != std::string::npos);
    CHECK(compactThrowReceptionProof.find(
              "constboolexactThrower=attackerAction==70u;")
          != std::string::npos);
    CHECK(compactThrowReceptionProof.find(
              "constboolexactTechCounterpart=allowThrowTechResult"
              "&&attackerAction==72u"
              "&&qtraining::combatActionIsThrowTechResult(victimAction);")
          != std::string::npos);
    CHECK(compactThrowReceptionProof.find(
              "trainingRecordContact("
              "qtraining::CombatContactKind::Throw,attacker,victim,")
          != std::string::npos);
    CHECK(compactThrowReceptionProof.find(
              "qtraining::combatActionIsThrowTechResult(victimAction)")
          != std::string::npos);
    CHECK(compactThrowReceptionProof.find(
              "trainingExactThrowTechCounterpartTransition(")
          != std::string::npos);
    CHECK(compactThrowReceptionProof.find(
              "qtraining::combatThrowTechCounterpartContinues(")
          != std::string::npos);

    const std::string resolver = sourceSlice(
        runtime, "static bool trainingResolveQualifiedHook(",
        "\nstatic void trainingQualifyRecoveryEvaluator(");
    CHECK(!resolver.empty());
    const std::string compactResolver = withoutWhitespace(resolver);
    CHECK(resolver.find("runtime_profile::selectedProfile()")
          != std::string::npos);
    CHECK(resolver.find("runtime_profile::resolveSelectedFunction(")
          != std::string::npos);
    CHECK(compactResolver.find("symbol->functionId!=functionId")
          != std::string::npos);
    CHECK(resolver.find("resolveNativeSymbolVa(profile->id, symbolId)")
          != std::string::npos);
    CHECK(compactResolver.find(
              "casegenerated_model::BuildId::Game2000:"
              "signature=&symbol->decemberPrologue;")
          != std::string::npos);
    CHECK(compactResolver.find(
              "casegenerated_model::BuildId::Se2006:"
              "signature=&symbol->novemberPrologue;")
          != std::string::npos);
    CHECK(resolver.find("PrologueSignatureStatus::Curated")
          != std::string::npos);
    CHECK(resolver.find("rangeReadable(outVa, signature->size)")
          != std::string::npos);
    CHECK(resolver.find("std::memcmp(reinterpret_cast<const void*>(outVa)")
          != std::string::npos);
    CHECK(resolver.find("trainingResolveQualifiedNativeHook(")
          != std::string::npos);
    CHECK(resolver.find("NativeShiftClass::SemanticallyEdited")
          != std::string::npos);
    CHECK(resolver.find("NativeRuntimeUse::SeamResolved")
          != std::string::npos);

    const std::string install = sourceSlice(
        runtime, "static bool installTrainingHooksOnce()",
        "\nstatic void trainingBeginNewGeneration()");
    CHECK(!install.empty());
    const std::size_t firstCreate = install.find("MH_CreateHook(");
    const std::size_t lastQualification =
        install.rfind("trainingResolveQualifiedHook(");
    const std::size_t nativeQualification =
        install.find("trainingResolveQualifiedNativeHook(");
    CHECK(firstCreate != std::string::npos);
    CHECK(lastQualification != std::string::npos);
    CHECK(nativeQualification != std::string::npos);
    CHECK(lastQualification < firstCreate);
    CHECK(nativeQualification < firstCreate);
    for (const char* id : {
             "FunctionId::ThrowTechEvaluator",
             "FunctionId::GuardContactResolver",
             "FunctionId::HitContactResolver",
             "FunctionId::NaturalActionTerminal"}) {
        CHECK(install.find(id) != std::string::npos);
    }
    CHECK_EQ(countOccurrences(install, "trainingResolveQualifiedHook("), 4u);
    CHECK_EQ(countOccurrences(
                 install, "trainingResolveQualifiedNativeHook("), 1u);
    CHECK(install.find("NativeSymbolId::EditedActorTransition")
          != std::string::npos);
    CHECK_EQ(countOccurrences(install, "MH_CreateHook("), 9u);
    CHECK_EQ(countOccurrences(install, "MH_QueueEnableHook("), 9u);
    CHECK(install.find("MH_EnableHook(") == std::string::npos);
    CHECK(install.find("MH_ApplyQueued(") == std::string::npos);
    CHECK(install.find("g_trainingThrowInstallFn = 0u")
          != std::string::npos);
    CHECK(install.find("g_trainingThrowTechFn = 0u") != std::string::npos);
    CHECK(install.find("g_trainingGuardContactFn = 0u")
          != std::string::npos);
    CHECK(install.find("g_trainingHitContactFn = 0u")
          != std::string::npos);
    CHECK(install.find("g_trainingNaturalActionTerminalFn = 0u")
          != std::string::npos);
    const std::size_t rollbackNatural = install.find(
        "trainingRemoveCreatedHook(g_trainingNaturalActionTerminalFn)");
    const std::size_t rollbackHit = install.find(
        "trainingRemoveCreatedHook(g_trainingHitContactFn)");
    const std::size_t rollbackGuard = install.find(
        "trainingRemoveCreatedHook(g_trainingGuardContactFn)");
    const std::size_t rollbackThrow = install.find(
        "trainingRemoveCreatedHook(g_trainingThrowTechFn)");
    const std::size_t rollbackThrowInstall = install.find(
        "trainingRemoveCreatedHook(g_trainingThrowInstallFn)");
    CHECK(rollbackNatural < rollbackHit);
    CHECK(rollbackHit < rollbackGuard);
    CHECK(rollbackGuard < rollbackThrow);
    CHECK(rollbackThrow < rollbackThrowInstall);
    const std::size_t failedTransaction = install.find("if (!enabled)");
    CHECK(failedTransaction != std::string::npos);
    CHECK(install.find("g_trainingThrowTechHookReady = true")
          > failedTransaction);
    CHECK(install.find("g_trainingContactHooksReady = true")
          > failedTransaction);

    const std::string gate = sourceSlice(
        runtime, "static bool trainingQualifiedGameplayGate()",
        "\nstatic void trainingRecordContact(");
    CHECK(!gate.empty());
    const std::string compactGate = withoutWhitespace(gate);
    for (const char* clause : {
             "g_trainingLaunchRequested", "g_trainingHooksInstalled",
             "!g_trainingMutationFailed", "!g_netActive",
             "g_mode==RunMode::Observe", "rd32(kTrainingModeAddr)==0u",
             "rd32(kSceneStateAddr)==2u", "rd32(kMatchSubAddr)==0u",
             "rd32(kTrainingVsLaunchAddr)!=1u",
             "rd8(kTrainingPauseAddr)==0u",
             "rd8(kTrainingFreezeAddr)==0u"}) {
        CHECK(compactGate.find(clause) != std::string::npos);
    }
    CHECK(compactGate.find(
              "returng_trainingStepPending&&trainingQualifiedGameplayGate();")
          != std::string::npos);

    const std::string guardHook = sourceSlice(
        runtime, "static int __fastcall hkTrainingGuardContact(",
        "\nstatic int __fastcall hkTrainingHitContact(");
    const std::string hitHook = sourceSlice(
        runtime, "static int __fastcall hkTrainingHitContact(",
        "\nstatic int __fastcall hkTrainingNaturalActionTerminal(");
    CHECK(!guardHook.empty());
    CHECK(!hitHook.empty());
    CHECK(guardHook.find("g_trainingOrigGuardContact")
          < guardHook.find("if (result == 1)"));
    CHECK(guardHook.find("CombatContactKind::Block") != std::string::npos);
    CHECK(hitHook.find("g_trainingOrigHitContact")
          < hitHook.find("if (result > 0)"));
    CHECK(hitHook.find("CombatContactKind::Hit") != std::string::npos);
    CHECK(guardHook.find("result != 0") == std::string::npos);
    CHECK(hitHook.find("result != 0") == std::string::npos);

    const std::string naturalHook = sourceSlice(
        runtime, "static int __fastcall hkTrainingNaturalActionTerminal(",
        "\nstatic uint64_t trainingCombatMix64(");
    CHECK(!naturalHook.empty());
    CHECK(naturalHook.find("g_trainingOrigNaturalActionTerminal")
          < naturalHook.find("if (!capture || before == 1u"));
    CHECK(naturalHook.find("before == 1u") != std::string::npos);
    CHECK(naturalHook.find(
              "rd32(actor + kTrainingFighterInitStatusOffset) != 1u")
          != std::string::npos);
    CHECK(naturalHook.find("trainingFighter(owner, false) == actor")
          != std::string::npos);
    CHECK(naturalHook.find("pre.actionEpoch = identity.actionEpoch")
          != std::string::npos);
    CHECK(naturalHook.find("pulse.actorToken = identity.actorToken")
          != std::string::npos);
    CHECK(naturalHook.find("pulse.actionEpoch = identity.actionEpoch")
          != std::string::npos);
    CHECK(naturalHook.find("pulse.stepSerial = g_trainingStepSerial")
          != std::string::npos);

    const std::string throwInstallHook = sourceSlice(
        runtime, "static int __fastcall hkTrainingThrowInstall(",
        "\nstatic int __fastcall hkTrainingThrowTech(");
    CHECK(!throwInstallHook.empty());
    CHECK_EQ(countOccurrences(
                 throwInstallHook,
                 "g_trainingOrigThrowInstall(attackerValue, victimValue)"),
             1u);
    CHECK(throwInstallHook.find(
              "g_trainingOrigThrowInstall(attackerValue, victimValue)")
          < throwInstallHook.find("const bool exactInstalledPair"));
    CHECK(throwInstallHook.find(
              "kTrainingFighterNextActionOffset) == 70u")
          != std::string::npos);
    CHECK(throwInstallHook.find(
              "kTrainingFighterPrevActionOffset) != 70u")
          != std::string::npos);
    CHECK(throwInstallHook.find(
              "kTrainingFighterNextActionOffset) == 200u")
          != std::string::npos);
    CHECK(throwInstallHook.find(
              "trainingLatchPendingInstalledThrowContact(attacker, victim)")
          != std::string::npos);
    CHECK(throwInstallHook.find("trainingAutomaticThrowTechAllowed(")
          != std::string::npos);
    CHECK(throwInstallHook.find("kTrainingFighterThrowTechLatchOffset")
          != std::string::npos);
    CHECK(throwInstallHook.find("| 1u") != std::string::npos);
    CHECK_EQ(countOccurrences(throwInstallHook, "wr32("), 1u);

    const std::string throwPolicy = sourceSlice(
        runtime, "static bool trainingAutomaticThrowTechAllowed(",
        "\nstatic int __fastcall hkTrainingThrowInstall(");
    CHECK(!throwPolicy.empty());
    const std::string compactThrowPolicy = withoutWhitespace(throwPolicy);
    for (const char* clause : {
             "g_trainingThrowTechHookReady",
             "trainingCombatDetourActive()",
             "g_trainingLifecycle!=TrainingLifecycleState::Active",
             "g_trainingOperationNeutralForStep",
             "g_trainingRoundStartNeutralPending",
             "rd32(kRoundResultStateAddr)!=0u",
             "rd32(attacker+kFighterRoundLostOffset)!=0u",
             "rd32(victim+kFighterRoundLostOffset)!=0u",
             "victimOwner==trainingPolicySlot()"}) {
        CHECK(compactThrowPolicy.find(clause) != std::string::npos);
    }
    CHECK(compactThrowPolicy.find("victimOwner==1u") == std::string::npos);
    CHECK(compactRuntime.find(
              "g_trainingOperationNeutralForStep=operationNeutral;")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "g_trainingStepPending=false;"
              "g_trainingOperationNeutralForStep=false;")
          != std::string::npos);

    const std::string throwHook = sourceSlice(
        runtime, "static int __fastcall hkTrainingThrowTech(",
        "\nstatic void trainingAdvanceCombatToolsPostStep()");
    CHECK(!throwHook.empty());
    CHECK(throwHook.find("g_trainingOrigThrowTech(fighter, evalFlags)")
          < throwHook.find("!trainingCombatDetourActive()"));
    CHECK(throwHook.find("owner != trainingPolicySlot()")
          != std::string::npos);
    CHECK(throwHook.find("owner != 1u") == std::string::npos);
    CHECK(throwHook.find("nativeAcceptedDecision") != std::string::npos);
    CHECK(throwHook.find("return nativeResult") != std::string::npos);
    CHECK(throwHook.find("kTrainingFighterThrowTechLatchOffset")
          == std::string::npos);
    CHECK_EQ(countOccurrences(throwHook, "wr32("), 0u);
    CHECK(throwHook.find("wr8(") == std::string::npos);
    for (const char* forbidden : {
             "kTrainingFighterActionStateOffset",
             "kTrainingFighterDispatchActionOffset",
             "kTrainingFighterTransitionRequestOffset",
             "g_trainingFinalInput"}) {
        CHECK(throwHook.find(forbidden) == std::string::npos);
    }

    const std::string response = sourceSlice(
        runtime, "static bool trainingNativeResponseReady(",
        "\nstatic uint64_t trainingCharacterFingerprint(");
    CHECK(!response.empty());
    const std::string compactResponse = withoutWhitespace(response);
    CHECK(compactResponse.find(
              "rd32(fighter+kFighterRoundLostOffset)==0u")
          != std::string::npos);
    CHECK(compactResponse.find(
              "rd32(fighter+kTrainingFighterFreezeOffset)==0u")
          != std::string::npos);
    CHECK(compactResponse.find(
              "trainingCombinedAuthoredPermission(fighter,owner);")
          != std::string::npos);
    CHECK(compactResponse.find("if((permission&0x01u)==0u)returnfalse;")
          != std::string::npos);
    CHECK(compactResponse.find(
              "if(qtraining::combatActionCannotCommitCommand(action))"
              "returnfalse;")
          != std::string::npos);
    CHECK(compactResponse.find(
              "!qtraining::combatActionIsNativeReactionFamily(action)"
              "||(permission&0x0Cu)==0u;")
          != std::string::npos);
    const std::string actors = sourceSlice(
        runtime, "static bool trainingActorSlotLive(",
        "\nstatic void trainingObserveActorPools(");
    CHECK(!actors.empty());
    CHECK(withoutWhitespace(actors).find("if(character==0xFFu)returnfalse;")
          != std::string::npos);
    const std::string pools = sourceSlice(
        runtime, "static void trainingObserveActorPools(",
        "\nstatic bool trainingNativeTerminalCommandWindow(");
    CHECK(!pools.empty());
    CHECK(withoutWhitespace(pools).find(
              "if(!g_trainingDebugOverlayEnabled)return;")
          != std::string::npos);
    CHECK(pools.find("kShadowPool") == std::string::npos);
    for (const char* pool : {"addr::kEffectPool", "addr::kActorClonePool",
                             "addr::kHelperCloneP1"}) {
        CHECK(pools.find(pool) != std::string::npos);
    }
    const std::string terminal = sourceSlice(
        runtime, "static bool trainingNativeTerminalCommandWindow(",
        "\nstatic uint16_t trainingReadFrameU16(");
    CHECK(!terminal.empty());
    CHECK(withoutWhitespace(terminal).find(
              "(rd32(fighter+kTrainingFighterTerminalReadyOffset)&0x01u)!=0u")
          != std::string::npos);
    CHECK(runtime.find("trainingNativeTerminalCommandWindow(fighter)"
                       " ? 1u : 0u") == std::string::npos);
    CHECK(compactResponse.find(
              "staticbooltrainingNativeGuardCancelReady("
              "uintptr_tfighter,uint8_towner)")
          != std::string::npos);
    CHECK(compactResponse.find(
              "qtraining::combatActionIsGuard(action)"
              "&&rd32(fighter+kFighterRoundLostOffset)==0u"
              "&&rd32(fighter+kTrainingFighterFreezeOffset)==0u"
              "&&(trainingCombinedAuthoredPermission(fighter,owner)&0x04u)"
              "!=0u;")
          != std::string::npos);

    const std::string postStep = sourceSlice(
        runtime, "static void trainingAdvanceCombatToolsPostStep()",
        "\nstatic int trainingHoldFailureFrame(");
    CHECK(!postStep.empty());
    const std::string compactPostStep = withoutWhitespace(postStep);
    CHECK(compactPostStep.find(
              "sample.owners[owner].guardCancelReady="
              "trainingNativeGuardCancelReady(fighter,owner);")
          != std::string::npos);
}

TEST(TrainingCombatPoliciesStayPureAndResetAtTimelineBoundaries) {
    const std::string header =
        readQohSource("src/training/TrainingCombatTools.h");
    const std::string policy =
        readQohSource("src/training/TrainingCombatTools.cpp");
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    const std::string macros =
        readQohSource("src/training/TrainingMacros.cpp");
    CHECK(!header.empty());
    CHECK(!policy.empty());
    CHECK(!runtime.empty());
    CHECK(!macros.empty());

    for (const char* primitive : {"rd8(", "rd32(", "wr8(", "wr32(",
                                  "rangeReadable(", "MH_CreateHook(",
                                  "VirtualProtect(", "windows.h"}) {
        CHECK(header.find(primitive) == std::string::npos);
        CHECK(policy.find(primitive) == std::string::npos);
    }
    const std::string compactHeader = withoutWhitespace(header);
    CHECK(compactHeader.find(
              "returncontact.serial!=0"
              "&&(contact.kind==CombatContactKind::Hit"
              "||contact.kind==CombatContactKind::Block"
              "||contact.kind==CombatContactKind::Throw)"
              "&&contact.attackerOwner<kCombatOwnerCount"
              "&&contact.victimOwner<kCombatOwnerCount"
              "&&contact.attackerOwner!=contact.victimOwner;")
          != std::string::npos);

    const std::string compactPolicy = withoutWhitespace(policy);
    CHECK(compactPolicy.find(
              "if(guardCancelLatched_"
              "&&sample.stepSerial>=guardCancelLatchStep_"
              "&&combatActionIsGuard(dummyAction)"
              "&&sample.owners[kCombatDummyOwner].guardCancelReady)")
          != std::string::npos);
    CHECK(compactPolicy.find(
              "schedule(TrainingTriggerEvent::GuardCancel,sample.stepSerial,"
              "guardCancelContactSerial_);")
          != std::string::npos);
    CHECK(compactPolicy.find(
              "discardPendingForNewCombatEpisode(contact.stepSerial);")
          != std::string::npos);
    CHECK(compactPolicy.find(
              "returnrequest.macroSlot!=0u"
              "||request.event==TrainingTriggerEvent::GuardCancel"
              "||moveResponseReady;")
          != std::string::npos);
    CHECK(compactPolicy.find(
              "PrimaryRecoveryDispositiondisposition="
              "attacker.primaryRecovery;")
          != std::string::npos);
    CHECK(compactPolicy.find(
              "casePrimaryRecoveryDisposition::Ready:"
              "status_.attackerReady=true;")
          != std::string::npos);
    const std::size_t continuedDisposition = compactPolicy.find(
        "casePrimaryRecoveryDisposition::Continued:");
    CHECK(continuedDisposition != std::string::npos);
    CHECK(compactPolicy.find(
              "status_.attackerActionEpoch=attacker.actionEpoch;",
              continuedDisposition)
          != std::string::npos);
    CHECK(compactPolicy.find(
              "elseif(attacker.responseReady)")
          != std::string::npos);
    CHECK(compactPolicy.find(
              "if(defender.responseReady){"
              "status_.defenderReady=true;")
          != std::string::npos);
    CHECK(compactHeader.find(
              "kFrameAdvantageReadoutLifetimeSteps=60")
          != std::string::npos);
    CHECK(compactPolicy.find(
              "++retainedReadoutAgeSteps_;"
              "if(retainedReadoutAgeSteps_"
              ">=kFrameAdvantageReadoutLifetimeSteps){"
              "clearReadout();")
          != std::string::npos);
    CHECK(macros.find("requestTriggeredSequence") != std::string::npos);
    CHECK(macros.find("leadingNeutralRedundant") != std::string::npos);
    CHECK(macros.find("generatedPlaybackActive_") != std::string::npos);
    CHECK(withoutWhitespace(runtime).find(
              "g_trainingGuardDuringPlayback&&!macro.generatedPlayback")
          != std::string::npos);

    const std::string resetMachines = sourceSlice(
        runtime, "static void trainingResetTimelineMachines(",
        "\nstatic bool trainingFinishRestoredImage(");
    CHECK(!resetMachines.empty());
    CHECK(withoutWhitespace(resetMachines).find(
              "trainingResetCombatToolTimeline(preserveObservationCounters);")
          != std::string::npos);

    const std::string combatAdvance = sourceSlice(
        runtime, "static void trainingAdvanceCombatToolsPostStep()",
        "\nstatic int trainingHoldFailureFrame(");
    CHECK(!combatAdvance.empty());
    const std::string compactCombatAdvance = withoutWhitespace(combatAdvance);
    const std::size_t overflowGate = compactCombatAdvance.find(
        "if(g_trainingContactDrops!=0u)");
    const std::size_t overflowReset = compactCombatAdvance.find(
        "trainingResetCombatToolTimeline(true);", overflowGate);
    const std::size_t triggerAdvance = compactCombatAdvance.find(
        "g_trainingTriggers.advance(");
    CHECK(overflowGate != std::string::npos);
    CHECK(overflowReset != std::string::npos);
    CHECK(triggerAdvance != std::string::npos);
    CHECK(overflowReset < triggerAdvance);
    CHECK(compactCombatAdvance.find(
              "g_trainingFrameAdvantage.advance(")
          != std::string::npos);
    CHECK(compactCombatAdvance.find(
              "trainingFlushPendingThrowContacts();")
          != std::string::npos);
    CHECK(compactCombatAdvance.find(
              "trainingRecordThrowReceptionEntries();")
          != std::string::npos);
    CHECK_EQ(countOccurrences(
                 runtime, "g_trainingFrameAdvantage.advance("), 1u);

    const std::string actionEdges = sourceSlice(
        runtime, "static bool trainingApplyActionEdges(",
        "\ntypedef BOOL(__stdcall* TrainingPresentHeldFrameFn)(void);");
    CHECK(!actionEdges.empty());
    CHECK_EQ(countOccurrences(
                 actionEdges, "trainingResetTimelineMachines(true);"), 2u);
    CHECK_EQ(countOccurrences(
                 actionEdges, "g_trainingMacros.requestStop()"), 2u);
    CHECK(actionEdges.find("g_trainingMacros.reset()") == std::string::npos);

    const std::string outerTick = sourceSlice(
        runtime, "static void trainingOuterTick()",
        "\nstatic bool trainingInCharacterSelectFrontend()");
    CHECK(!outerTick.empty());
    const std::string compactOuterTick = withoutWhitespace(outerTick);
    CHECK(compactOuterTick.find(
              "g_trainingHadActiveMatch"
              "&&g_trainingLastScene==2u&&g_trainingLastMatchSub==0u"
              "&&(scene!=2u||matchSub!=0u)")
          != std::string::npos);
    CHECK(compactOuterTick.find(
              "trainingResetTimelineMachines(true);"
              "(void)g_trainingMacros.requestStop();")
          != std::string::npos);
    CHECK(outerTick.find("g_trainingMacros.reset()") == std::string::npos);
}

TEST(TrainingFrameAdvantageUsesTheExhaustiveQohActionCatalog) {
    const std::string catalogHeader =
        readQohSource("src/training/TrainingActionCatalog.h");
    const std::string catalog =
        readQohSource("src/training/TrainingActionCatalog.cpp");
    const std::string generator = readQohSource(
        "src/training/generated/generate_training_action_catalog.py");
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!catalogHeader.empty());
    CHECK(!catalog.empty());
    CHECK(!generator.empty());
    CHECK(!runtime.empty());
    CHECK(!wrapper.empty());

    const std::string compactHeader = withoutWhitespace(catalogHeader);
    CHECK(compactHeader.find("kTrainingActionResourceCount=35")
          != std::string::npos);
    CHECK(compactHeader.find("kTrainingActionStateCount=256")
          != std::string::npos);
    CHECK(compactHeader.find("kTrainingActionAuthoredStateCount=3826")
          != std::string::npos);
    CHECK(compactHeader.find("kTrainingDispatchCaseCount=101")
          != std::string::npos);
    CHECK(compactHeader.find("kTrainingNativeMoveBehaviorRuleCount=113")
          != std::string::npos);
    CHECK(compactHeader.find(
              "kTrainingNativeMoveBehaviorSourceStateCount=69")
          != std::string::npos);
    CHECK(compactHeader.find("kTrainingMoveSelectableCharacterCount=34")
          != std::string::npos);
    CHECK(compactHeader.find("kTrainingMoveMaximumPerCharacter=54")
          != std::string::npos);
    CHECK(compactHeader.find("kTrainingMoveMaximumSequenceSteps=9")
          != std::string::npos);
    CHECK(catalogHeader.find("ThrowTechCounterpart") != std::string::npos);
    CHECK(catalogHeader.find("ProvenExcludedContinuation")
          != std::string::npos);
    CHECK(catalog.find("action48 == 71u") != std::string::npos);
    CHECK(catalog.find("action48 == 72u") != std::string::npos);
    CHECK(catalog.find("action48 == 73u") != std::string::npos);
    CHECK(catalog.find("action48 >= 200u") != std::string::npos);
    CHECK(catalog.find("matchCount > 1u") != std::string::npos);
    CHECK(catalog.find("trainingMoveDirectTriggerEligible")
          != std::string::npos);
    CHECK(catalog.find("trainingMoveGuardCancelTriggerEligible")
          != std::string::npos);
    CHECK(catalog.find("trainingMoveTriggerEligible")
          != std::string::npos);

    CHECK(generator.find("qoh99-se1126") != std::string::npos);
    CHECK(generator.find("qoh99-withse") != std::string::npos);
    CHECK(generator.find("nativeMoveBehavior") != std::string::npos);
    CHECK(generator.find("nov_table != dec_table") != std::string::npos);
    CHECK(generator.find("nov_rules != dec_rules") != std::string::npos);
    CHECK(generator.find("nov_moves != dec_moves") != std::string::npos);
    CHECK(generator.find("steps.append(0)") != std::string::npos);

    const std::string compactRuntime = withoutWhitespace(runtime);
    CHECK(runtime.find("#include \"training/TrainingActionCatalog.h\"")
          != std::string::npos);
    CHECK(runtime.find("trainingLoopingAirAction") == std::string::npos);
    CHECK(runtime.find("trainingPostLandingLocomotion")
          == std::string::npos);
    CHECK(compactRuntime.find(
              "qtraining::trainingNativeMoveTransition(")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "trainingExactThrowTechCounterpartTransition(")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "qtraining::combatThrowAttackerTupleIsHold("
              "expectation.resourceAction,expectation.dispatchAction)")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "qtraining::PrimaryRecoveryDisposition::Continued;")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "pulse.actionEpoch==expectation.actionEpoch")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "recoveryDisposition="
              "qtraining::PrimaryRecoveryDisposition::Ready;")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "rd32(victim+kTrainingFighterMoveGateOffset)&0x20u")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "sample.roundValid=roundValid;")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "trainingResetCombatToolTimeline(true);")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "qtraining::isEncodedTrainingTriggerMoveValue(command.value)")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "qtraining::trainingMoveTriggerEligible(move,event)")
          != std::string::npos);
    CHECK(compactRuntime.find("validMoveMask|=") != std::string::npos);
    CHECK(compactRuntime.find(
              "g_trainingMacros.requestTriggeredSequence(")
          != std::string::npos);
    const std::string wakeupProof = withoutWhitespace(sourceSlice(
        runtime, "static bool trainingWakeupLockedStepsRemaining(",
        "\nstatic bool trainingWakeupActivationStillReady("));
    CHECK(!wakeupProof.empty());
    CHECK(wakeupProof.find("kTrainingFighterFloorModeOffset")
          != std::string::npos);
    CHECK(wakeupProof.find("kTrainingFighterVerticalVelocityOffset")
          != std::string::npos);
    CHECK(wakeupProof.find("kTrainingFighterVerticalTweenDeltaOffset")
          != std::string::npos);
    CHECK(wakeupProof.find("kTrainingFrameVerticalVelocityOffset")
          != std::string::npos);
    CHECK(wakeupProof.find("kTrainingFrameVerticalAccelerationOffset")
          != std::string::npos);
    CHECK(wakeupProof.find("motionFlags&0x04u") != std::string::npos);
    CHECK(wakeupProof.find("initializesVerticalMotion")
          != std::string::npos);
    CHECK(wakeupProof.find(
              "trainingCombinedAuthoredPermission(fighter,1u)"
              "&permissionRefuseMask")
          != std::string::npos);
    CHECK(wakeupProof.find("permissionRefuseMask=guardReaction?0x09u:0x0Du")
          != std::string::npos);
    CHECK(wakeupProof.find("combatActionIsGroundHit(action)")
          != std::string::npos);
    CHECK(wakeupProof.find("UINTPTR_MAX") != std::string::npos);
    CHECK(wakeupProof.find("kTrainingFighterAuthoredFlagsOffset")
          != std::string::npos);
    CHECK(wakeupProof.find(
              "combatActionIsNaturalWakeupPath(action)")
          != std::string::npos);
    const std::string activationGate = withoutWhitespace(sourceSlice(
        runtime, "static bool trainingWakeupActivationStillReady(",
        "\nstatic void trainingCancelWakeupReversal("));
    CHECK(!activationGate.empty());
    CHECK(activationGate.find("kTrainingFighterTransitionFlagOffset")
          == std::string::npos);
    CHECK(activationGate.find("kTrainingFighterForcedActionOffset")
          != std::string::npos);
    CHECK(activationGate.find("trainingNativeResponseReady(fighter,1u)")
          != std::string::npos);
    CHECK(activationGate.find("kTrainingCommandDirectionKillOffset")
          != std::string::npos);
    const std::string triggerAdvance = withoutWhitespace(sourceSlice(
        runtime, "static void trainingAdvanceCombatToolsPostStep()",
        "\nstatic int trainingHoldFailureFrame("));
    CHECK(!triggerAdvance.empty());
    CHECK(triggerAdvance.find(
              "combatActionIsNaturalWakeupPath("
              "sample.owners[qtraining::kCombatDummyOwner].action)")
          != std::string::npos);
    CHECK(triggerAdvance.find(
              "constuint32_tlastComposed=g_trainingLastComposedInput[1];")
          != std::string::npos);
    CHECK(triggerAdvance.find(
              "constboolinputAlreadyNeutral=lastComposed==0u;")
          != std::string::npos);
    CHECK(triggerAdvance.find("leadingNeutralRedundant)")
          != std::string::npos);
    CHECK(triggerAdvance.find("g_trainingFinalInput[1]==0u")
          == std::string::npos);
    CHECK(triggerAdvance.find(
              "g_trainingGuardCancelResponsePlayback="
              "trigger.event=="
              "qtraining::TrainingTriggerEvent::GuardCancel;")
          != std::string::npos);
    CHECK(triggerAdvance.find("!g_trainingRecoveryEmission.active")
          != std::string::npos);
    CHECK(triggerAdvance.find(
              "g_trainingRecoveryEmission.state=="
              "qtraining::RecoveryState::Idle")
          == std::string::npos);
    const std::string inputCompose = withoutWhitespace(sourceSlice(
        runtime, "static bool trainingComposeInputForStep()",
        "\nstatic bool trainingRestoreScratchFailSafe()"));
    CHECK(!inputCompose.empty());
    CHECK(inputCompose.find(
              "constboolactivationPending="
              "g_trainingMacros.cursor()<="
              "g_trainingWakeupReversal.commitIndex;")
          != std::string::npos);
    CHECK(inputCompose.find(
              "activationPending&&"
              "!g_trainingWakeupReversal.releasedEarly&&"
              "!trainingWakeupActivationStillReady(p2)")
          != std::string::npos);
    CHECK(inputCompose.find(
              "g_trainingCombatIdentity[1].actorToken=="
              "g_trainingWakeupReversal.actorToken")
          != std::string::npos);
    CHECK(inputCompose.find("!g_trainingRecoveryEmission.active")
          != std::string::npos);
    CHECK(inputCompose.find(
              "g_trainingRecoveryEmission.state=="
              "qtraining::RecoveryState::Idle")
          == std::string::npos);
    const std::size_t guardWindowCheck = inputCompose.find(
        "!trainingNativeGuardCancelReady(p2,1u)");
    const std::size_t guardedStop = inputCompose.find(
        "g_trainingMacros.requestStop();", guardWindowCheck);
    const std::size_t macroAdvance = inputCompose.find(
        "g_trainingMacros.advance(g_trainingStepSerial,macroInput);");
    CHECK(guardWindowCheck != std::string::npos);
    CHECK(guardedStop != std::string::npos);
    CHECK(macroAdvance != std::string::npos);
    CHECK(guardWindowCheck < guardedStop);
    CHECK(guardedStop < macroAdvance);
    CHECK(compactRuntime.find(
              "status.triggerMoveCatalogId="
              "qtraining::trainingMoveCatalogId();")
          != std::string::npos);
    CHECK(wrapper.find(
              "case QOH99_TRAINING_FRAME_ADVANTAGE_COMPLETE:")
          != std::string::npos);
    CHECK(wrapper.find(
              "case QOH99_TRAINING_FRAME_ADVANTAGE_TRADE:")
          != std::string::npos);
    CHECK(wrapper.find(
              "case QOH99_TRAINING_FRAME_ADVANTAGE_UNMEASURABLE:")
          == std::string::npos);
    CHECK(wrapper.find("TrainingActionTriggerCatalogMatches")
          != std::string::npos);
    CHECK(wrapper.find("QOH99_TRAINING_TRIGGER_MOVE_VALUE_FLAG")
          != std::string::npos);
}

TEST(TrainingWrapperRendersHoldReasonsRatherThanOnePausedFlag) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string hold = sourceSlice(
        wrapper, "void DrawTrainingHoldControls(",
        "\nvoid DrawTrainingFighterReadout(");
    const std::string dummyTab = sourceSlice(
        wrapper, "void DrawDummyTab(", "\nvoid DrawMacroSlotSelector(");
    CHECK(!dummyTab.empty());
    CHECK(!hold.empty());
    CHECK(hold.find("QOH99_TRAINING_HOLD_USER") != std::string::npos);
    CHECK(hold.find("QOH99_TRAINING_HOLD_MENU") != std::string::npos);
    CHECK(hold.find("this menu is holding it too") != std::string::npos);
    CHECK(hold.find("still paused when you ") != std::string::npos);
    CHECK(hold.find("Held while this menu is open") != std::string::npos);
    CHECK(hold.find("The dummy keeps guarding while the menu is open")
          == std::string::npos);
    CHECK(dummyTab.find("The dummy keeps guarding while the menu is open")
          != std::string::npos);
    CHECK(hold.find("QOH99_TRAINING_COMMAND_FRAME_STEP") == std::string::npos);
    CHECK(hold.find("QOH99_TRAINING_COMMAND_TOGGLE_PAUSE")
          != std::string::npos);
}

TEST(TrainingMenuIsCompactAndRefreshesDraftsAtEveryReentryBoundary) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    const std::string refresh = sourceSlice(
        wrapper, "void RefreshTrainingUiForStatus(",
        "\nbool TrainingHasExtendedStatus(");
    CHECK(!refresh.empty());
    CHECK(refresh.find("status.trainingGeneration") != std::string::npos);
    CHECK(refresh.find("status.lifecycleState") != std::string::npos);
    CHECK(refresh.find("status.scene") != std::string::npos);
    CHECK(refresh.find("status.matchSubstate") != std::string::npos);
    CHECK(refresh.find("refreshDraftsOnNextDraw") != std::string::npos);
    CHECK(refresh.find("InvalidateTrainingUiDrafts();") != std::string::npos);

    const std::string toggle = sourceSlice(
        wrapper, "extern \"C\" void EFZDX9_Presenter_ToggleMenu(void)",
        "\nextern \"C\" void EFZDX9_Presenter_ToggleInputDisplay(");
    CHECK(!toggle.empty());
    CHECK(toggle.find("g_trainingUi.refreshDraftsOnNextDraw = true")
          != std::string::npos);

    const std::string menu = sourceSlice(
        wrapper, "void BuildDebugMenu(const EfzDx9PresentParams &params)",
        "\nbool IsInputMessage(");
    CHECK(!menu.empty());
    CHECK(menu.find("RefreshTrainingUiForStatus(training);")
          != std::string::npos);
    CHECK(menu.find("ImGui::BeginTabBar(") == std::string::npos);
    CHECK(menu.find("qoh99_training_tools_tabs") == std::string::npos);
    CHECK(menu.find("DrawTrainingMenuSidebar(trainingPresent, advanced)")
          != std::string::npos);
    CHECK(menu.find("DrawRecordingTab(training);") != std::string::npos);
    CHECK(menu.find("DrawStateTab(training);") != std::string::npos);
    CHECK(menu.find("DrawDummyReactionsSection(training);")
          != std::string::npos);
    for (const char* page : {"kTrainingPageQuick", "kTrainingPageDummy",
                             "kTrainingPageReactions",
                             "kTrainingPageRecording", "kTrainingPageView",
                             "kTrainingPageKeys", "kTrainingPageHelp",
                             "kTrainingPageDiagnostics"}) {
        CHECK(menu.find(page) != std::string::npos);
    }

    const std::string practice = sourceSlice(
        wrapper, "void DrawTrainingTab(const Qoh99TrainingStatusV1 &status)",
        "\nvoid DrawDummyRecoverySection(");
    CHECK(!practice.empty());
    CHECK(practice.find("ImGui::BeginTabItem(qi18n::tr(\"Position\"))")
          != std::string::npos);
    CHECK(practice.find("ImGui::BeginTabItem(qi18n::tr(\"Resources\"))")
          != std::string::npos);
    CHECK(practice.find("ImGui::BeginTabItem(qi18n::tr(\"Session\"))")
          != std::string::npos);
    CHECK(practice.find("ImGui::SeparatorText(qi18n::tr(\"Position\"))")
          != std::string::npos);
    CHECK(practice.find(
              "ImGui::SeparatorText(qi18n::tr(\"Health, meter and guard gauge\"))")
          != std::string::npos);
    CHECK(practice.find("Return to Character Select")
          != std::string::npos);

    const std::string diagnostics = sourceSlice(
        wrapper, "void DrawDiagnosticsTab(", "\nvoid DrawTrainingHelpTab()");
    CHECK(!diagnostics.empty());
    CHECK(diagnostics.find("DrawTrainingStatusSummary(status);")
          != std::string::npos);
    CHECK(diagnostics.find("DrawOverlayStatusSummary(params);")
          != std::string::npos);
    CHECK(wrapper.find("void DrawNativeHudControls(") != std::string::npos);
    CHECK_EQ(countOccurrences(wrapper, "DrawNativeHudControls("), 3u);
    CHECK(diagnostics.find("status.nativeHudOwner != 0u") != std::string::npos);
    CHECK(diagnostics.find("DrawNativeHudControls(status);")
          != std::string::npos);

    CHECK(diagnostics.find("ImGui::BeginTabItem(qi18n::tr(\"Bridge\"))")
          != std::string::npos);
    CHECK(diagnostics.find("ImGui::BeginTabItem(qi18n::tr(\"Status\"))")
          != std::string::npos);
    CHECK(diagnostics.find("ImGui::BeginTabItem(qi18n::tr(\"Combat\"))")
          != std::string::npos);
    CHECK(diagnostics.find("ImGui::BeginTabItem(qi18n::tr(\"Tools\"))")
          != std::string::npos);
    for (const char* call : {"DrawTrainingDetails(status);",
                             "DrawDummyDetails(status);",
                             "DrawTrainingFighterReadout(status);",
                             "DrawTrainingDamageReadout(status);",
                             "DrawTrainingIdleTestRawBlock(status);",
                             "DrawTrainingActionStateLog(status);",
                             "DrawInputDetails(status);",
                             "DrawSaveStateDetails(status);",
                             "DrawWindowDetails(params);"}) {
        CHECK(diagnostics.find(call) != std::string::npos);
    }
    CHECK(diagnostics.find("DrawTrainingRecoveryDebug(status);")
          != std::string::npos);
    CHECK(diagnostics.find("Why recovery did or did not fire")
          != std::string::npos);

    const std::string playerSummaries = sourceSlice(
        wrapper, "void DrawTrainingStatusSummary(",
        "\nvoid DrawNativeHudControls(");
    CHECK(!playerSummaries.empty());
    CHECK(playerSummaries.find("sub_") == std::string::npos);
    CHECK(playerSummaries.find("\"Offset ") == std::string::npos);
    CHECK(playerSummaries.find("\"0x%") == std::string::npos);
    CHECK(playerSummaries.find("\"Bridge") == std::string::npos);
    CHECK(playerSummaries.find("\"Generation") == std::string::npos);
    CHECK(playerSummaries.find("\"Queue") == std::string::npos);
    CHECK(playerSummaries.find("\"Current state\"") != std::string::npos);
    CHECK(playerSummaries.find("\"State frame\"") != std::string::npos);

    const std::size_t persistentStep = menu.find(
        "DrawPersistentTrainingFrameStep(training);");
    const std::string nav = sourceSlice(
        wrapper, "TrainingMenuPage DrawTrainingMenuSidebar(",
        "\nvoid BuildDebugMenu(");
    CHECK(!nav.empty());
    CHECK(nav.find("static bool advancedEnabled") != std::string::npos);
    CHECK(nav.find("ImGui::Checkbox(\"Advanced\", &advancedEnabled)")
          != std::string::npos);
    CHECK(nav.find("ImGui::Checkbox(\"Advanced\", &advanced)")
          == std::string::npos);
    CHECK(nav.find("advanced = advancedEnabled;") != std::string::npos);
    CHECK(nav.find("OverlayIniSet(\"MenuAdvanced\"") != std::string::npos);
    CHECK(nav.find("OverlayIniGet(\"MenuAdvanced\"") != std::string::npos);
    const std::size_t sidebar = menu.find("DrawTrainingMenuSidebar(");
    CHECK(persistentStep != std::string::npos);
    CHECK(sidebar != std::string::npos);
    CHECK(persistentStep < sidebar);
    const std::string stepButton = sourceSlice(
        wrapper, "void DrawPersistentTrainingFrameStep(",
        "\nvoid BuildDebugMenu(");
    CHECK(!stepButton.empty());
    CHECK(stepButton.find("\"Step 1 frame\"") != std::string::npos);
    CHECK(stepButton.find("QOH99_TRAINING_COMMAND_FRAME_STEP")
          != std::string::npos);

    const std::string help = sourceSlice(
        wrapper, "void DrawTrainingHelpTab()", "\nvoid DrawNetplayKeysTab(");
    CHECK(!help.empty());
    CHECK(help.find("qoh99_help_tabs") == std::string::npos);
    CHECK(help.find("Training controls are grouped by task")
          != std::string::npos);
    CHECK(help.find("\xE2\x80\x94") == std::string::npos);

    CHECK(wrapper.find("void TrainingTextDisabledWrapped(")
          != std::string::npos);
    CHECK(wrapper.find("void TrainingTextColoredWrapped(")
          != std::string::npos);
    CHECK(wrapper.find("void TrainingBulletTextWrapped(")
          != std::string::npos);
    const std::string recovery = sourceSlice(
        wrapper, "void DrawDummyRecoverySection(",
        "\nbool RecoveryActionInBranchSet(");
    CHECK(!recovery.empty());
    CHECK(recovery.find("TrainingTextColoredWrapped(")
          != std::string::npos);
    CHECK(recovery.find("TrainingTextDisabledWrapped(")
          != std::string::npos);
}

TEST(TrainingWrapperHotkeyListMatchesTheBoundKeys) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string tab = sourceSlice(
        wrapper, "void DrawTrainingHotkeysTab(", "\nvoid BuildDebugMenu(");
    CHECK(!tab.empty());

    CHECK(tab.find("Return to character select") != std::string::npos);
    CHECK(tab.find("Cycle dummy stance") != std::string::npos);
    CHECK(tab.find("Cycle guard mode") != std::string::npos);
    CHECK(tab.find("Restore all - toggles the automatic restore")
          != std::string::npos);
    CHECK(tab.find("Refill all - health, EX meter and guard gauge")
          == std::string::npos);
    CHECK(tab.find(
              "Charge EX fraction - hold to fill continuously and add stocks")
          != std::string::npos);
    CHECK(tab.find(
              "Toggle debug overlay - show or hide live action, wake-up, and throw details")
          != std::string::npos);
    CHECK(tab.find("Every binding above comes from the launcher's Controls")
          != std::string::npos);
    CHECK(tab.find("Custom key and pad bindings") == std::string::npos);
}

TEST(TrainingWrapperCommandKindsAndCapabilitiesMirrorExactly) {
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_CONTACT_NONE), 0u);
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_CONTACT_HIT), 1u);
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_CONTACT_BLOCK), 2u);
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_CONTACT_THROW), 3u);
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_HP_REGEN),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetHpRegen));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_STANCE),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetStance));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_TECH),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetTech));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_REFILL_LIFE),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::RefillLife));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_REFILL_GUARD_GAUGE),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::RefillGuardGauge));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_REFILL_EX_METER),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::RefillExMeter));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_CAP_REFILL),
             static_cast<unsigned>(qtraining::TrainingCapabilityRefill));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_THROW_TECH_CHANCE), 36u);
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_TRIGGER_CONFIG),
             37u);
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_FRAME_ADVANTAGE_ENABLED), 38u);
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_DEBUG_OVERLAY_ENABLED), 39u);
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_STATE_CHANGE_LOG_ENABLED), 40u);
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_FRAME_TRACE_ENABLED), 42u);
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_FRAME_TRACE_ENABLED),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetFrameTraceEnabled));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_COUNT), 49u);
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_PLAYER_SWAP),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetPlayerSwap));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_MACRO_AFTER_RESET),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetMacroAfterReset));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_DEBUG_OVERLAY_ENABLED),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetDebugOverlayEnabled));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_STATE_CHANGE_LOG_ENABLED),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetStateChangeLogEnabled));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_THROW_TECH_CHANCE),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetThrowTechChance));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_COMMAND_SET_TRIGGER_CONFIG),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetTriggerConfig));
    CHECK_EQ(static_cast<unsigned>(
                 QOH99_TRAINING_COMMAND_SET_FRAME_ADVANTAGE_ENABLED),
             static_cast<unsigned>(
                 qtraining::TrainingCommandKind::SetFrameAdvantageEnabled));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_CAP_TRIGGERS), 1u << 10);
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_CAP_FRAME_ADVANTAGE),
             1u << 11);
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_CAP_THROW_TECH), 1u << 12);
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_CAP_TRIGGERS),
             static_cast<unsigned>(qtraining::TrainingCapabilityTriggers));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_CAP_FRAME_ADVANTAGE),
             static_cast<unsigned>(
                 qtraining::TrainingCapabilityFrameAdvantage));
    CHECK_EQ(static_cast<unsigned>(QOH99_TRAINING_CAP_THROW_TECH),
             static_cast<unsigned>(qtraining::TrainingCapabilityThrowTech));

    qtraining::TrainingCommandV1 retired{};
    retired.commandKind = QOH99_TRAINING_COMMAND_SET_HP_REGEN;
    CHECK(!qtraining::validCommandPayload(retired));
    retired.value = 1;
    CHECK(!qtraining::validCommandPayload(retired));

    qtraining::TrainingCommandV1 refill{};
    refill.commandKind = QOH99_TRAINING_COMMAND_REFILL_EX_METER;
    refill.value = 2u;
    CHECK(qtraining::validCommandPayload(refill));
    refill.value = 3u;
    CHECK(!qtraining::validCommandPayload(refill));
    CHECK(!qtraining::commandMayCoalesce(
        qtraining::TrainingCommandKind::RefillLife));
    CHECK(!qtraining::commandMayCoalesce(
        qtraining::TrainingCommandKind::RefillGuardGauge));
    CHECK(!qtraining::commandMayCoalesce(
        qtraining::TrainingCommandKind::RefillExMeter));
    CHECK(!qtraining::commandOwnsNativeImageTransaction(
        qtraining::TrainingCommandKind::RefillLife));
    CHECK(!qtraining::commandOwnsNativeImageTransaction(
        qtraining::TrainingCommandKind::RefillGuardGauge));
    CHECK(!qtraining::commandOwnsNativeImageTransaction(
        qtraining::TrainingCommandKind::RefillExMeter));
    CHECK(!qtraining::commandOwnsNativeImageTransaction(
        qtraining::TrainingCommandKind::SetStance));
    CHECK(!qtraining::commandOwnsNativeImageTransaction(
        qtraining::TrainingCommandKind::SetTech));

    qtraining::TrainingCommandV1 combat{};
    combat.commandKind = static_cast<uint32_t>(
        qtraining::TrainingCommandKind::SetThrowTechChance);
    combat.value = qtraining::kTrainingTriggerMaximumChancePercent;
    CHECK(qtraining::validCommandPayload(combat));
    combat.value = qtraining::kTrainingTriggerMaximumChancePercent + 1u;
    CHECK(!qtraining::validCommandPayload(combat));
    combat.value = 100u;
    combat.flags = 1u;
    CHECK(!qtraining::validCommandPayload(combat));

    combat = qtraining::TrainingCommandV1{};
    combat.commandKind = static_cast<uint32_t>(
        qtraining::TrainingCommandKind::SetTriggerConfig);
    combat.slotOrSetting = static_cast<uint32_t>(
        qtraining::TrainingTriggerEvent::GuardCancel);
    combat.value = qtraining::kTrainingMacroSlotCount;
    combat.flags =
        (qtraining::kTrainingTriggerMaximumChancePercent << 16u)
        | qtraining::kTrainingTriggerMaximumDelaySteps;
    CHECK(qtraining::validCommandPayload(combat));
    combat.slotOrSetting = qtraining::kTrainingTriggerEventCount;
    CHECK(!qtraining::validCommandPayload(combat));
    combat.slotOrSetting = static_cast<uint32_t>(
        qtraining::TrainingTriggerEvent::GuardCancel);
    combat.value = qtraining::kTrainingTriggerRandomMacroSlot;
    CHECK(qtraining::validCommandPayload(combat));
    CHECK_EQ(combat.value,
             static_cast<uint32_t>(QOH99_TRAINING_TRIGGER_RANDOM_RECORDING));
    combat.value = qtraining::kTrainingTriggerRandomMacroSlot + 1u;
    CHECK(!qtraining::validCommandPayload(combat));
    combat.value = qtraining::encodeTrainingTriggerMoveValue(1u);
    CHECK(qtraining::validCommandPayload(combat));
    CHECK_EQ(combat.value,
             static_cast<uint32_t>(QOH99_TRAINING_TRIGGER_MOVE_VALUE_FLAG)
                 | 1u);
    combat.value = qtraining::encodeTrainingTriggerMoveValue(
        qtraining::kTrainingTriggerMaximumMoveIndex);
    CHECK(qtraining::validCommandPayload(combat));
    combat.value = qtraining::kTrainingTriggerMoveValueFlag;
    CHECK(!qtraining::validCommandPayload(combat));
    combat.value = qtraining::kTrainingTriggerMoveValueFlag
        | (qtraining::kTrainingTriggerMaximumMoveIndex + 1u);
    CHECK(!qtraining::validCommandPayload(combat));
    combat.value = qtraining::kTrainingMacroSlotCount;
    combat.flags =
        (qtraining::kTrainingTriggerMaximumChancePercent << 16u)
        | (qtraining::kTrainingTriggerMaximumDelaySteps + 1u);
    CHECK(!qtraining::validCommandPayload(combat));
    combat.flags =
        ((qtraining::kTrainingTriggerMaximumChancePercent + 1u) << 16u)
        | qtraining::kTrainingTriggerMaximumDelaySteps;
    CHECK(!qtraining::validCommandPayload(combat));

    combat = qtraining::TrainingCommandV1{};
    combat.commandKind = static_cast<uint32_t>(
        qtraining::TrainingCommandKind::SetFrameAdvantageEnabled);
    combat.value = 1u;
    CHECK(qtraining::validCommandPayload(combat));
    combat.value = 2u;
    CHECK(!qtraining::validCommandPayload(combat));

    for (qtraining::TrainingCommandKind kind : {
             qtraining::TrainingCommandKind::SetThrowTechChance,
             qtraining::TrainingCommandKind::SetTriggerConfig,
             qtraining::TrainingCommandKind::SetFrameAdvantageEnabled}) {
        CHECK(qtraining::commandMayCoalesce(kind));
        CHECK(!qtraining::commandOwnsNativeImageTransaction(kind));
    }
}

TEST(TrainingRefillsAreOwnedTransactionsNotTheBroadNativeFlag) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());
    const std::string compactRuntime = withoutWhitespace(runtime);

    CHECK(runtime.find("g_trainingHpRegen") == std::string::npos);

    const std::string invariants = sourceSlice(
        runtime, "static bool trainingInvariantsHold()",
        "\nstatic bool trainingPrepareRollbackLayout()");
    CHECK(!invariants.empty());
    CHECK(withoutWhitespace(invariants).find(
              "rd32(kNativeFatalHitDisableAddr)==0u")
          != std::string::npos);
    CHECK(invariants.find("g_trainingHpRegen") == std::string::npos);

    const std::string edges = sourceSlice(
        runtime, "static void trainingClearNativeDeveloperEdges()",
        "\nstatic bool trainingRepairHumanOwnership()");
    CHECK(!edges.empty());
    const std::string compactEdges = withoutWhitespace(edges);
    CHECK(compactEdges.find("g_trainingRefillRequest|=") == std::string::npos);
    CHECK(compactEdges.find("g_trainingHitboxMode=") == std::string::npos);
    CHECK(compactEdges.find("rd8(0x8A5D") == std::string::npos);
    CHECK(compactEdges.find("wr32(kNativeFatalHitDisableAddr,0u);")
          != std::string::npos);
    CHECK(edges.find("trainingApplyLifeRefill") == std::string::npos);
    CHECK(edges.find("trainingApplyGuardGaugeRefill")
          == std::string::npos);
    CHECK(edges.find("trainingApplyExMeterRefill")
          == std::string::npos);
    const std::string hooks = readQohSource("src/GameHooks.cpp");
    const std::string commonSuppressor = sourceSlice(
        hooks, "inline void clearOwnedNativeDeveloperEdges() {",
        "\n}\n\ninline void applyNativeDeveloperEdgePolicy() {");
    CHECK(!commonSuppressor.empty());
    const std::string compactCommon = withoutWhitespace(commonSuppressor);
    CHECK(compactEdges.find("clearOwnedNativeDeveloperEdges();")
          != std::string::npos);
    CHECK(compactCommon.find("address=0x8A5D40u;address<=0x8A5D4Fu")
          != std::string::npos);
    CHECK(compactCommon.find("wr8(0x8A5D52u,0u);") != std::string::npos);
    CHECK(compactCommon.find("wr32(0x66ECC4u,rd32(0x66ECC4u)&~3u);")
          != std::string::npos);

    CHECK(countOccurrences(compactRuntime,
                           "wr32(kNativeFatalHitDisableAddr,")
          == 1u);
    CHECK(compactRuntime.find("wr32(kNativeFatalHitDisableAddr,1u)")
          == std::string::npos);

    const std::string life = sourceSlice(
        runtime, "static CommandAckResult trainingApplyLifeRefill(",
        "\nstatic bool trainingGuardGaugeAddress(");
    CHECK(!life.empty());
    CHECK(life.find("qtraining::planLifeRefill(") != std::string::npos);
    CHECK(life.find("trainingWriteLifeComponents(") != std::string::npos);
    CHECK(life.find("qtraining::planLifeRefill(")
          < life.find("trainingWriteLifeComponents("));
    CHECK(life.find("netSetBattleLifeEffective") == std::string::npos);
    CHECK(life.find("kNativeFatalHitDisableAddr") == std::string::npos);
    CHECK(life.find("kTrainingCopiedConfigAddr") != std::string::npos);
    CHECK(life.find("config+44 diverged") != std::string::npos);
    CHECK(life.find("trainingFailMutation(") != std::string::npos);
    CHECK(life.find("trainingRestoreScratch") == std::string::npos);

    const std::string lifeAddress = sourceSlice(
        runtime, "static bool trainingLifeRecordAddress(",
        "\nstatic TrainingLifeComponents trainingReadLifeComponents(");
    CHECK(!lifeAddress.empty());
    CHECK(withoutWhitespace(lifeAddress).find(
              "player>=qtraining::kTrainingRefillPlayerCount)"
              "returnfalse;")
          != std::string::npos);
    CHECK(withoutWhitespace(lifeAddress).find("rangeReadable(life,16u)")
          != std::string::npos);

    const std::string gauge = sourceSlice(
        runtime,
        "static CommandAckResult trainingApplyGuardGaugeRefill(",
        "\nstatic bool trainingExRecordAddress(");
    CHECK(!gauge.empty());
    CHECK(gauge.find("qtraining::planGuardGaugeRefill(")
          != std::string::npos);
    CHECK(gauge.find("qtraining::planGuardGaugeRefill(")
          < gauge.find("wr32("));
    CHECK(countOccurrences(gauge, "wr32(") == 1u);
    CHECK(gauge.find("kHealthBase") == std::string::npos);
    CHECK(gauge.find("kHealthArray") == std::string::npos);
    CHECK(gauge.find("kHealthStride") == std::string::npos);
    CHECK(gauge.find("kBattleLife") == std::string::npos);
    CHECK(gauge.find("kNativeFatalHitDisableAddr") == std::string::npos);
    CHECK(gauge.find("guard gauge rejected") != std::string::npos);
    CHECK(gauge.find("trainingRestoreScratch") == std::string::npos);

    const std::string gaugeAddress = sourceSlice(
        runtime, "static bool trainingGuardGaugeAddress(",
        "\nstatic CommandAckResult trainingApplyGuardGaugeRefill(");
    CHECK(!gaugeAddress.empty());
    CHECK(gaugeAddress.find("kGuardGaugeBase") != std::string::npos);
    CHECK(gaugeAddress.find("addr::kGuardGaugeStride")
          != std::string::npos);
    CHECK(gaugeAddress.find("kHealthBase") == std::string::npos);
    CHECK(gaugeAddress.find("trainingFighter(player, true)")
          == std::string::npos);

    const std::string ex = sourceSlice(
        runtime, "static CommandAckResult trainingApplyExMeterRefill(",
        "\nstatic bool trainingChargeExFraction(");
    CHECK(!ex.empty());
    CHECK(ex.find("qtraining::planExRefill(") != std::string::npos);
    CHECK(ex.find("qtraining::planExRefill(") < ex.find("wr32("));
    CHECK(countOccurrences(ex, "wr32(") == 2u);
    const std::string compactEx = withoutWhitespace(ex);
    CHECK(compactEx.find(
              "wr32(record[player]+kTrainingExStocksOffset,")
          != std::string::npos);
    CHECK(compactEx.find(
              "wr32(record[player]+kTrainingExFractionOffset,")
          != std::string::npos);
    CHECK(compactEx.find("wr32(record[player]+kTrainingExPendingOffset")
          == std::string::npos);
    CHECK(compactEx.find("wr32(record[player]+kTrainingExStateOffset")
          == std::string::npos);
    CHECK(compactEx.find("wr32(record[player]+kTrainingExTimerOffset")
          == std::string::npos);
    CHECK(compactEx.find("wr32(record[player]+kTrainingExStocksOffset,")
          < compactEx.find(
                "wr32(record[player]+kTrainingExFractionOffset,"));
    CHECK(ex.find("CommandAckResult::RejectedUnavailable")
          != std::string::npos);
    CHECK(ex.find("EX busy") != std::string::npos);
    CHECK(ex.find("trainingRestoreScratch") == std::string::npos);

    const std::string admission = sourceSlice(
        runtime, "static int trainingAdmitAfterNativeGate(",
        "\nstatic int __cdecl hkTrainingStepGate()");
    CHECK(!admission.empty());
    const std::size_t actionTable =
        admission.find("trainingApplyActionEdges(actions,");
    const std::size_t consume =
        admission.find("trainingConsumeRefillRequests()");
    const std::size_t postcondition =
        admission.rfind("trainingApplyConfigOverlay()");
    const std::size_t postSnapshot =
        admission.rfind("trainingCaptureSnapshot(g_trainingScratch)");
    const std::size_t pauseHold =
        admission.find("trainingAdmitPausedFrame()");
    CHECK(actionTable != std::string::npos);
    CHECK(consume != std::string::npos);
    CHECK(postcondition != std::string::npos);
    CHECK(postSnapshot != std::string::npos);
    CHECK(pauseHold != std::string::npos);
    CHECK(actionTable < consume);
    CHECK(consume < postcondition);
    CHECK(postcondition < postSnapshot);
    CHECK(postSnapshot < pauseHold);
}

TEST(TrainingStanceAndTechAreHeldInputOnly) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    const std::string composer =
        readQohSource("src/training/TrainingInput.cpp");
    const std::string dummy =
        readQohSource("src/training/TrainingDummy.cpp");
    CHECK(!runtime.empty());
    CHECK(!composer.empty());
    CHECK(!dummy.empty());

    const std::string arms = sourceSlice(
        runtime, "case TrainingCommandKind::SetStance:",
        "\n        case TrainingCommandKind::RefillLife:");
    CHECK(!arms.empty());
    CHECK(arms.find("wr8(") == std::string::npos);
    CHECK(arms.find("wr32(") == std::string::npos);
    CHECK(arms.find("rd8(") == std::string::npos);
    CHECK(arms.find("rd32(") == std::string::npos);
    CHECK(arms.find("rangeReadable") == std::string::npos);
    CHECK(arms.find("trainingFighter(") == std::string::npos);
    CHECK(arms.find(
              "qtraining::dummyTechReleaseQualified(requested, false)")
          != std::string::npos);
    CHECK(arms.find("qtraining::dummyStanceValid(requested)")
          != std::string::npos);
    CHECK(arms.find("g_trainingGuardMode") == std::string::npos);
    CHECK(withoutWhitespace(runtime).find(
              "qtraining::guardModeReleaseQualified(requested,false)")
          != std::string::npos);

    CHECK(withoutWhitespace(runtime).find(
              "staticqtraining::GuardDecisiontrainingGuardDecision("
              "uint8_tdefenderFacing)")
          != std::string::npos);
    CHECK(runtime.find("chooseDummyPosture") != std::string::npos);

    const std::string compose = sourceSlice(
        runtime, "static bool trainingComposeInputForStep()",
        "\nstatic bool trainingRestoreScratchFailSafe()");
    CHECK(!compose.empty());
    const std::string compactCompose = withoutWhitespace(compose);
    CHECK(compactCompose.find(
              "trainingDummyPosture(guard,policyFacing,policyRecovery)")
          != std::string::npos);
    CHECK(compactCompose.find(
              "trainingGuardDecision(policyFacing);") != std::string::npos);
    CHECK(compactCompose.find(
              "trainingGuardAvoidTerminalRoll(guard);"
              "g_trainingLastGuardDecision=guard;")
          != std::string::npos);
    CHECK(compactCompose.find(
              "constqtraining::RecoveryEmission&policyRecovery="
              "policyFollowsHuman?g_trainingRecoveryEmissionP1:recovery;")
          != std::string::npos);
    CHECK(compactCompose.find(
              "constbooldummyOwnsDirection=macroRecordOwnership"
              "?(policyFollowsHuman&&!policyStandsDownForRecovery)"
              ":!macroNeutralOwnership;")
          != std::string::npos);
    CHECK(compactCompose.find(
              "constboolpolicyStandsDownForRecovery="
              "policyArmableMask!=0u&&!policyRecovery.active;")
          != std::string::npos);
    CHECK(compactCompose.find(
              "policyArmableMask=qtraining::recoveryArmableMask("
              "selfObservation);")
          != std::string::npos);
    CHECK(compactCompose.find(
              "swap?composed.p1Mask:composed.p2Mask,p2Facing);")
          != std::string::npos);
    CHECK(compactCompose.find(
              "input.dummyDirections=dummyOwnsDirection?"
              "(releaseGuardForResponse?0u:posture.directions):0u;")
          != std::string::npos);
    CHECK(compactCompose.find(
              "input.dummyOwnedAxes=dummyOwnsDirection?"
              "(releaseGuardForResponse?0u:posture.ownedAxes):0u;")
          != std::string::npos);
    CHECK(compactCompose.find("releaseGuardForResponse?0u:posture.directions")
          != std::string::npos);
    CHECK(compactCompose.find("releaseGuardForResponse?0u:posture.ownedAxes")
          != std::string::npos);
    CHECK(compactCompose.find("lockedStepsRemaining<=1u") != std::string::npos);

    CHECK(composer.find("p2 |= step.dummy") == std::string::npos);
    CHECK(composer.find("dummyGuardDirections") == std::string::npos);
    CHECK(composer.find(
              "lane = (lane & ~owned) | (step.dummyDirections & owned);")
          != std::string::npos);
    CHECK(composer.find(
              "uint32_t& lane =\n"
              "            trainingPolicyLane(step) == kTrainingDrivenLaneP1 "
              "? p1 : p2;")
          != std::string::npos);
    const std::string compactComposer = withoutWhitespace(composer);
    const std::size_t playbackYield =
        compactComposer.find("if(step.macroPlaybackActive){");
    const std::size_t yieldHorizontal = compactComposer.find(
        "if((lane&kDummyHorizontalAxis)!=0)owned&=~kDummyHorizontalAxis;");
    const std::size_t yieldVertical = compactComposer.find(
        "if((lane&kDummyVerticalAxis)!=0)owned&=~kDummyVerticalAxis;");
    CHECK(compactComposer.find("uint32_towned=step.dummyOwnedAxes;")
          != std::string::npos);
    CHECK(playbackYield != std::string::npos);
    CHECK(yieldHorizontal != std::string::npos);
    CHECK(yieldVertical != std::string::npos);
    CHECK(playbackYield < yieldHorizontal);
    CHECK(playbackYield < yieldVertical);
    CHECK(composer.find(
              "isCanonicalDummyOwnedAxes(step.dummyOwnedAxes)")
          != std::string::npos);
    CHECK(composer.find(
              "(step.dummyDirections & ~step.dummyOwnedAxes) == 0")
          != std::string::npos);

    CHECK(dummy.find("windows.h") == std::string::npos);
    CHECK(dummy.find("SDL_") == std::string::npos);
    CHECK(dummy.find("GetAsyncKeyState") == std::string::npos);
    CHECK(dummy.find("rd32(") == std::string::npos);
    CHECK(dummy.find("wr32(") == std::string::npos);
    CHECK(dummy.find("chooseDummyPosture") != std::string::npos);
}

TEST(TrainingDamageReadoutRespectsNativeAdmissionAndNeverDecodesCause) {
    const std::string runtime =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());

    const std::string damage = sourceSlice(
        runtime, "static int __fastcall hkTrainingDamage(",
        "\nstatic TrainingDisableReason trainingCurrentDisableReason()");
    CHECK(!damage.empty());
    const std::string compactDamage = withoutWhitespace(damage);

    CHECK(compactDamage.find("constboolbodyAdmitted=result==0;")
          != std::string::npos);
    CHECK(compactDamage.find(
              "constbooldeltaMatches="
              "observedDelta==static_cast<int64_t>(damage);")
          != std::string::npos);
    CHECK(compactDamage.find("if(bodyAdmitted&&deltaMatches)")
          != std::string::npos);
    CHECK(compactDamage.find("++g_trainingDamage.declinedSerial;")
          != std::string::npos);
    CHECK(compactDamage.find("++g_trainingDamage.admittedSerial;")
          != std::string::npos);

    const std::size_t nativeRead = compactDamage.find(
        "constint64_tobservedDelta=nativePending-pending;");
    const std::size_t capture =
        compactDamage.find("g_trainingDamage.damage=damage;");
    const std::size_t floor = compactDamage.find("if(effective<0)");
    CHECK(nativeRead != std::string::npos);
    CHECK(capture != std::string::npos);
    CHECK(floor != std::string::npos);
    CHECK(nativeRead < capture);
    CHECK(capture < floor);
    CHECK(compactDamage.find("g_trainingDamage.damage=damage;")
          != std::string::npos);
    const std::size_t retained = compactDamage.find(
        "g_trainingDamage.pendingDelta="
        "static_cast<int32_t>(preEffective);");
    CHECK(retained != std::string::npos);
    CHECK(floor < retained);
    const std::string clear = sourceSlice(
        runtime, "static void trainingClearLastDamageEvent()",
        "\nstruct alignas(16) TrainingFpuImage");
    CHECK(!clear.empty());
    CHECK(clear.find(
              "g_trainingDamage.victimSlot = "
              "kTrainingNoDamageDescription;")
          != std::string::npos);
    CHECK(runtime.find(
              "kTrainingNoDamageDescription = 0xFFFFFFFFu")
          != std::string::npos);

    CHECK(damage.find("pending delta mismatch") != std::string::npos);
    CHECK(compactDamage.find("++g_trainingStickyErrors;")
          != std::string::npos);

    CHECK(runtime.find("causeName") == std::string::npos);
    CHECK(runtime.find("hit type") == std::string::npos);
    CHECK(runtime.find("hitType") == std::string::npos);
    CHECK(compactDamage.find(
              "g_trainingDamage.cause=static_cast<uint32_t>(cause);")
          != std::string::npos);

    const std::string readout = sourceSlice(
        runtime, "static bool trainingReadCombatReadout(",
        "\nstatic void trainingPublishStatus()");
    CHECK(!readout.empty());
    CHECK(readout.find("kTrainingComboHitsOffset") != std::string::npos);
    CHECK(readout.find("kTrainingTeamComboDamageOffset")
          != std::string::npos);
    CHECK(readout.find("kTrainingMatchMaxComboHitsOffset")
          != std::string::npos);
    CHECK(readout.find("kFighterPlayerSlotOffset") != std::string::npos);
    CHECK(readout.find("kTrainingFighterExOwnerOffset")
          != std::string::npos);
    CHECK(countOccurrences(readout, "rangeReadable(") >= 4u);
    CHECK(readout.find("wr32(") == std::string::npos);
    CHECK(readout.find("wr8(") == std::string::npos);

    const std::string publish = sourceSlice(
        runtime, "static void trainingPublishStatus()",
        "\nstatic BOOL __stdcall trainingStatusProvider(");
    CHECK(!publish.empty());
    CHECK(publish.find("status.combatReadoutValid = 1u")
          != std::string::npos);
    CHECK(publish.find("trainingReadCombatReadout(0u, combat[0])")
          != std::string::npos);
    CHECK(publish.find("trainingReadCombatReadout(1u, combat[1])")
          != std::string::npos);
    CHECK(withoutWhitespace(publish).find(
              "if(status.inMatchReady"
              "&&trainingReadCombatReadout(0u,combat[0])")
          != std::string::npos);

    const std::string provider = sourceSlice(
        runtime, "static BOOL __stdcall trainingStatusProvider(",
        "\nstatic BOOL __stdcall trainingCommandEnqueue(");
    CHECK(!provider.empty());
    const std::string compactProvider = withoutWhitespace(provider);
    CHECK(compactProvider.find(
              "outSize<qtraining::kTrainingStatusV1BaseSize")
          != std::string::npos);
    CHECK(compactProvider.find(
              "qtraining::trainingStatusTierFor("
              "static_cast<uint32_t>(outSize));")
          != std::string::npos);
    CHECK(compactProvider.find("copied==0u||copied>sizeof(status)")
          != std::string::npos);
    CHECK(compactProvider.find("std::memcpy(outStatus,&status,copied);")
          != std::string::npos);
    CHECK(compactProvider.find("outStatus->structSize=copied;")
          != std::string::npos);
    CHECK(compactProvider.find(
              "static_assert(sizeof(status)==sizeof(*outStatus)")
          != std::string::npos);
}

TEST(TrainingRecoveryInjectsInputAndNeverWritesAnActionField) {
    const std::string recoveryCpp =
        readQohSource("src/training/TrainingRecovery.cpp");
    const std::string recoveryHeader =
        readQohSource("src/training/TrainingRecovery.h");
    CHECK(!recoveryCpp.empty());
    CHECK(!recoveryHeader.empty());

    for (const char* accessor : {"rd8(", "rd32(", "wr8(", "wr32(",
                                 "rangeReadable(", "trainingFighter(",
                                 "InterlockedCompareExchange", "windows.h"}) {
        CHECK(recoveryCpp.find(accessor) == std::string::npos);
        CHECK(recoveryHeader.find(accessor) == std::string::npos);
    }
    CHECK(recoveryHeader.find("0x6265DC") != std::string::npos);
    CHECK(recoveryCpp.find("uintptr_t") == std::string::npos);
    for (const char* relative : {"src/training/TrainingResources.cpp",
                                 "src/training/TrainingResources.h",
                                 "src/training/TrainingHold.h"}) {
        const std::string pure = readQohSource(relative);
        CHECK(!pure.empty());
        for (const char* accessor : {"rd8(", "rd32(", "wr8(", "wr32(",
                                     "rangeReadable("}) {
            CHECK(pure.find(accessor) == std::string::npos);
        }
    }

    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    const std::string code = withoutWhitespace(inl);

    CHECK(code.find("wr32(fighter+48") == std::string::npos);
    CHECK(code.find("wr32(defender+48") == std::string::npos);
    CHECK(code.find("kTrainingFighterActionOffset,") == std::string::npos);
    CHECK(code.find("wr32(fighter+kTrainingFighterDispatchActionOffset")
          == std::string::npos);
    CHECK(code.find("wr32(defender+kTrainingFighterDispatchActionOffset")
          == std::string::npos);

    CHECK_EQ(countOccurrences(code, "wr32(fighter+kTrainingFighterNextActionOffset"),
             std::size_t{0});
    CHECK(code.find("trainingRequestFighterAction") != std::string::npos);
    CHECK(code.find("still-in-stun") != std::string::npos);
    CHECK_EQ(countOccurrences(code, "wr32(fighter+kTrainingFighterTransitionFlagOffset"),
             std::size_t{0});
    CHECK(code.find(
              "wr32(fighters[player]+kTrainingFighterNextActionOffset")
          == std::string::npos);
    CHECK(code.find(
              "wr32(fighters[player]+kTrainingFighterTransitionFlagOffset")
          == std::string::npos);

    const std::string pairReset = withoutWhitespace(sourceSlice(
        inl, "static bool trainingResetPairToPreset(PositionPreset preset) {",
        "\n}"));
    CHECK(!pairReset.empty());
    CHECK(pairReset.find("trainingRestoreRoundCharacterActors()")
          != std::string::npos);
    CHECK(pairReset.find("trainingRestoreRoundCharacterRegions()")
          != std::string::npos);
    CHECK(pairReset.find("kTrainingFighterNextActionOffset")
          == std::string::npos);
    CHECK(pairReset.find("kTrainingFighterTransitionFlagOffset")
          == std::string::npos);

    const std::string roundStart = withoutWhitespace(sourceSlice(
        inl, "static CommandAckResult trainingApplyRoundStartNeutral(",
        "\n}"));
    CHECK(!roundStart.empty());
    CHECK(roundStart.find(
              "trainingResetPairToPreset(PositionPreset::RoundStart)")
          != std::string::npos);
    CHECK(roundStart.find("wr32(fighter+kTrainingFighterNextActionOffset")
          == std::string::npos);
    CHECK(roundStart.find(
              "wr32(fighter+kTrainingFighterTransitionFlagOffset")
          == std::string::npos);

    const std::string sampler = withoutWhitespace(sourceSlice(
        inl, "static bool trainingSampleRecoveryObservation(",
        "\nstatic qtraining::DummyPostureResult trainingDummyPosture("));
    CHECK(!sampler.empty());
    CHECK(sampler.find("wr32(") == std::string::npos);
    CHECK(sampler.find("wr8(") == std::string::npos);
    CHECK(sampler.find("rangeReadable(") != std::string::npos);
    CHECK(sampler.find("readable") != std::string::npos);

    CHECK(code.find("wr32(record+kTrainingRecoveryLockOffset")
          == std::string::npos);
    CHECK(code.find("wr32(record+kTrainingRecoveryPhaseLatchOffset")
          == std::string::npos);
    CHECK(countOccurrences(code, "rd32(record+kTrainingRecoveryLockOffset")
          > std::size_t{0});

    for (const char* forbidden : {",31u)", ",33u)", ",34u)", ",70u)"}) {
        std::string needle = "wr32(fighter+kTrainingFighterNextActionOffset";
        needle += forbidden;
        CHECK(code.find(needle) == std::string::npos);
    }

    CHECK_EQ(countOccurrences(code, "kTrainingResetFn"), std::size_t{2});
    CHECK(code.find("verifyCodeBytes(kTrainingResetFn") != std::string::npos);
    CHECK(code.find("kTrainingResetFn)(") == std::string::npos);
    CHECK(roundStart.find("kTrainingResetFn") == std::string::npos);
    CHECK(roundStart.find("trainingResetPairToPreset(PositionPreset::RoundStart)")
          != std::string::npos);

    const std::string composer = sourceSlice(
        inl, "static bool trainingComposeInputForStep()",
        "\nstatic bool trainingRestoreScratchFailSafe()");
    CHECK(!composer.empty());
    CHECK(composer.find("g_trainingRecovery.advance(") != std::string::npos);
    CHECK(composer.find("trainingSampleRecoveryObservation(")
          != std::string::npos);
    CHECK(withoutWhitespace(composer).find("wr32(") == std::string::npos);
}

TEST(TrainingRecoveryLockAppliesOnlyToTheManualFamily) {
    const std::string header =
        readQohSource("src/training/TrainingRecovery.h");
    const std::string cpp =
        readQohSource("src/training/TrainingRecovery.cpp");
    CHECK(!header.empty());
    CHECK(!cpp.empty());

    CHECK(header.find("constexpr bool recoveryBranchConsultsManualLock(")
          != std::string::npos);
    const std::string predicate = withoutWhitespace(sourceSlice(
        header, "constexpr bool recoveryBranchConsultsManualLock(", "}"));
    CHECK(!predicate.empty());
    CHECK(predicate.find("RecoveryBranch::Air22") != std::string::npos);
    CHECK(predicate.find("RecoveryBranch::Ground88") != std::string::npos);
    CHECK(predicate.find("RecoveryBranch::Wall66") != std::string::npos);
    CHECK(predicate.find("TerminalForward66") == std::string::npos);
    CHECK(predicate.find("TerminalBack44") == std::string::npos);
    CHECK(predicate.find("ThrowEscape") == std::string::npos);

    const std::string flat = withoutWhitespace(cpp);
    CHECK_EQ(countOccurrences(flat, "observation.recoveryLock==0"),
             std::size_t{1});
    CHECK_EQ(countOccurrences(flat, "observation.recoveryLock!=0"),
             std::size_t{1});
    CHECK(flat.find("recoveryBranchConsultsManualLock(attempt_.branch)")
          != std::string::npos);
    const std::string suspend = withoutWhitespace(sourceSlice(
        cpp, "if (attempt_.active\n        && recoveryBranchConsultsManualLock(attempt_.branch)",
        "if (attempt_.active) {"));
    CHECK(!suspend.empty());
    CHECK(suspend.find("recoveryBranchConsultsManualLock(attempt_.branch)")
          != std::string::npos);
    CHECK(suspend.find("observation.recoveryLock!=0") != std::string::npos);
}

TEST(TrainingRecoveryClaimsAResultItsOwnTapsProduced) {
    const std::string cpp =
        readQohSource("src/training/TrainingRecovery.cpp");
    CHECK(!cpp.empty());
    const std::string flat = withoutWhitespace(cpp);
    CHECK(flat.find("constboolattemptMayOwnResult=attempt_.active&&attempt_"
                    ".tapsEmitted>0")
          != std::string::npos);
    CHECK(flat.find("if(attemptMayOwnResult){") != std::string::npos);
    CHECK(flat.find("constboolbuffered=attempt_.state==RecoveryState::"
                    "ObservingBuffer")
          != std::string::npos);
    CHECK(cpp.find("RecoveryVerdict::AutomaticObserved") != std::string::npos);
}

TEST(TrainingRecoveryCountersSurviveASavestateLoad) {
    const std::string cpp =
        readQohSource("src/training/TrainingRecovery.cpp");
    const std::string header =
        readQohSource("src/training/TrainingRecovery.h");
    CHECK(!cpp.empty());
    CHECK(!header.empty());
    CHECK(header.find("void resetTimeline();") != std::string::npos);

    CHECK_EQ(countOccurrences(withoutWhitespace(cpp),
                              "counters_=RecoveryCounters{}"),
             std::size_t{1});
    const std::string timeline = withoutWhitespace(sourceSlice(
        cpp, "void TrainingRecoveryController::resetTimeline() {",
        "void TrainingRecoveryController::reset("));
    CHECK(!timeline.empty());
    CHECK(timeline.find("counters_") == std::string::npos);
    CHECK(timeline.find("rng_") == std::string::npos);
    CHECK(timeline.find("attempt_=Attempt{}") != std::string::npos);

    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    const std::string flat = withoutWhitespace(inl);
    CHECK(flat.find("trainingResetTimelineMachines(true)") != std::string::npos);
    CHECK_EQ(countOccurrences(flat, "trainingResetTimelineMachines(true)"),
             std::size_t{6});
    const std::string restore = withoutWhitespace(sourceSlice(
        inl, "static bool trainingFinishRestoredImage(",
        "\nstatic bool trainingRestoreSnapshot("));
    CHECK(!restore.empty());
    CHECK(restore.find("trainingResetTimelineMachines(true)")
          != std::string::npos);
}

TEST(TrainingAutomaticRecoveryIsClassifiedAndNeverSuppressed) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    const std::string composer = sourceSlice(
        inl, "static bool trainingComposeInputForStep()",
        "\nstatic bool trainingRestoreScratchFailSafe()");
    CHECK(!composer.empty());

    const std::string recovery =
        readQohSource("src/training/TrainingRecovery.cpp");
    CHECK(!recovery.empty());
    CHECK(recovery.find("RecoveryVerdict::AutomaticObserved")
          != std::string::npos);
    CHECK(recovery.find("RecoveryVerdict::BufferedObserved")
          != std::string::npos);
    CHECK(recovery.find("suppressAutomatic") == std::string::npos);
    CHECK(recovery.find("disableAutoRecovery") == std::string::npos);
    const std::string types = readQohSource("src/training/TrainingTypes.h");
    CHECK(!types.empty());
    CHECK(types.find("DisableAutoRecovery") == std::string::npos);
    CHECK(types.find("SuppressAutoRecovery") == std::string::npos);
}

TEST(TrainingMenuCaptureEngagesTheHoldAndNeverTheNativePauseByte) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    const std::string code = withoutWhitespace(inl);

    CHECK(code.find("qtraining::TrainingHoldg_trainingHold") != std::string::npos);
    CHECK(code.find("g_trainingHold.setMenuCaptured(") != std::string::npos);
    CHECK(code.find("g_trainingHold.toggleUserHold()") != std::string::npos);
    CHECK(code.find("g_trainingHold.requestFrameStep()") != std::string::npos);
    CHECK(code.find("boolg_trainingPaused") == std::string::npos);
    CHECK(code.find("g_trainingPaused=") == std::string::npos);

    CHECK_EQ(countOccurrences(code, "wr8(kTrainingPauseAddr,"),
             std::size_t{1});
    CHECK(code.find("wr8(kTrainingPauseAddr,0u)") != std::string::npos);
    CHECK(code.find("wr8(kTrainingPauseAddr,1u)") == std::string::npos);

    const std::string overrideMask = sourceSlice(
        inl, "int trainingOverrideMask(int player) {",
        "\nvoid trainingObserveOfflinePhysicalSnapshot(");
    CHECK(!overrideMask.empty());
    CHECK(withoutWhitespace(overrideMask).find("menuCaptured")
          == std::string::npos);
    const std::string overrideActive = sourceSlice(
        inl, "bool trainingOverrideActive(int player) {",
        "\nint trainingOverrideMask(int player) {");
    CHECK(!overrideActive.empty());
    CHECK(overrideActive.find("MenuCaptured") != std::string::npos);

    const std::string holdHeader = readQohSource("src/training/TrainingHold.h");
    CHECK(!holdHeader.empty());
}

TEST(TrainingHeldFrameRepaintsThroughTheWrapperAndSimulatesNothing) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    const std::string holdHeader =
        readQohSource("src/training/TrainingHold.h");
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/ddraw.c");
    const std::string exports =
        readWorkspaceSource("ddraw_wrapper/ddraw.def");
    CHECK(!inl.empty());
    CHECK(!holdHeader.empty());
    CHECK(!wrapper.empty());
    CHECK(!exports.empty());

    CHECK(holdHeader.find("bool presentHeldFrame = false;")
          != std::string::npos);
    CHECK(holdHeader.find("result.presentHeldFrame = true;")
          != std::string::npos);
    CHECK_EQ(countOccurrences(withoutWhitespace(holdHeader),
                              "result.presentHeldFrame=true;"),
             std::size_t{1});

    const std::string pauseGate = sourceSlice(
        inl, "static bool trainingAdmitPausedFrame()",
        "\nstatic bool trainingComposeInputForStep()");
    CHECK(!pauseGate.empty());
    const std::string compactPauseGate = withoutWhitespace(pauseGate);
    CHECK(compactPauseGate.find("if(admission.admit)returntrue;")
          != std::string::npos);
    CHECK(compactPauseGate.find(
              "if(admission.presentHeldFrame)trainingRepaintHeldFrame();")
          != std::string::npos);
    CHECK(pauseGate.find("wr8(") == std::string::npos);
    CHECK(pauseGate.find("wr32(") == std::string::npos);
    CHECK(pauseGate.find("g_trainingStepSerial") == std::string::npos);

    const std::string repaint = sourceSlice(
        inl, "typedef BOOL(__stdcall* TrainingPresentHeldFrameFn)(void);",
        "\nstruct TrainingReversalParseProbe {");
    CHECK(!repaint.empty());
    CHECK(repaint.find("EFZDDRAW_PresentHeldFrame") != std::string::npos);
    CHECK(repaint.find("wr8(") == std::string::npos);
    CHECK(repaint.find("wr32(") == std::string::npos);
    CHECK(repaint.find("rd8(") == std::string::npos);
    CHECK(repaint.find("rd32(") == std::string::npos);
    CHECK(repaint.find("g_trainingStepSerial") == std::string::npos);
    CHECK(repaint.find("kTrainingPauseAddr") == std::string::npos);
    CHECK(repaint.find("kTrainingFreezeAddr") == std::string::npos);
    CHECK(repaint.find("0x8A5D4") == std::string::npos);
    CHECK(repaint.find("trainingFailMutation(") == std::string::npos);
    CHECK(repaint.find("if (g_trainingHeldFramePresenter == nullptr) return;")
          != std::string::npos);

    const std::string compactInl = withoutWhitespace(inl);
    CHECK(compactInl.find("wr32(active+84u,0u);") != std::string::npos);
    CHECK(compactInl.find("wr32(active+88u,0u);") != std::string::npos);
    CHECK(compactInl.find("wr32(active+64u,0u);") != std::string::npos);
    CHECK(compactInl.find("wr32(active+80u,0u);") != std::string::npos);
    CHECK(compactInl.find("wr32(active+92u,0u);") != std::string::npos);
    CHECK(compactInl.find("wr32(active+96u,0u);") != std::string::npos);
    CHECK(compactInl.find("wr8(kTrainingFreezeAddr,1u)") == std::string::npos);
    CHECK(compactInl.find("wr8(kTrainingPauseAddr,1u)") == std::string::npos);
    CHECK_EQ(countOccurrences(compactInl, "wr8(kTrainingFreezeAddr,"),
             std::size_t{1});

    const std::string admission = sourceSlice(
        inl, "static int trainingAdmitAfterNativeGate(",
        "\nstatic int __cdecl hkTrainingStepGate()");
    CHECK(!admission.empty());
    const std::size_t postSnapshot =
        admission.rfind("trainingCaptureSnapshot(g_trainingScratch)");
    const std::size_t pauseHold =
        admission.find("if (!trainingAdmitPausedFrame())");
    const std::size_t bumpSerial =
        admission.find("++g_trainingStepSerial;");
    CHECK(postSnapshot != std::string::npos);
    CHECK(pauseHold != std::string::npos);
    CHECK(bumpSerial != std::string::npos);
    CHECK(postSnapshot < pauseHold);
    CHECK(pauseHold < bumpSerial);
    CHECK(admission.find("trainingInvariantsHold()") != std::string::npos);

    CHECK(exports.find("EFZDDRAW_PresentHeldFrame @18") != std::string::npos);
    CHECK(exports.find("EFZDX9_RegisterReplayTransportProvider @17")
          != std::string::npos);
    const std::string present = sourceSlice(
        wrapper, "BOOL __stdcall EFZDDRAW_PresentHeldFrame(void)",
        "\n__declspec(dllexport)");
    CHECK(!present.empty());
    CHECK(present.find("present_frame(g_dd, FALSE)") != std::string::npos);
    CHECK(withoutWhitespace(present).find(
        "published=qoh99_netplay_owns_pacing()?present_snapshot(g_dd)"
        ":present_frame(g_dd,FALSE);") != std::string::npos);
    CHECK(present.find("qoh99_frame_commit") == std::string::npos);
    CHECK(present.find("qoh99_frame_published") == std::string::npos);
    CHECK(present.find("qoh99_snapshot_store") == std::string::npos);
    CHECK(present.find("g_runtimeFpsLimit") != std::string::npos);
    CHECK(present.find("minTicks") != std::string::npos);
    CHECK(withoutWhitespace(present).find(
              "minTicks=g_qpcFreq.QuadPart/((LONGLONG)limit*2);")
          != std::string::npos);
}

TEST(TrainingNativeDeveloperEdgeSuppressorOnlyClearsAndNeverActs) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    const std::string suppressor = sourceSlice(
        inl, "static void trainingClearNativeDeveloperEdges() {",
        "\nstatic bool trainingRepairHumanOwnership() {");
    CHECK(!suppressor.empty());
    const std::string body = withoutWhitespace(suppressor);

    CHECK(body.find("rd8(0x8A5D") == std::string::npos);
    CHECK(body.find("rd8(") == std::string::npos);
    CHECK(body.find("g_trainingHitboxMode=") == std::string::npos);
    CHECK(body.find("g_trainingRefillRequest|=") == std::string::npos);
    CHECK(body.find("clearOwnedNativeDeveloperEdges();")
          != std::string::npos);
    const std::string hooks = readQohSource("src/GameHooks.cpp");
    const std::string shared = sourceSlice(
        hooks, "inline void clearOwnedNativeDeveloperEdges() {",
        "\n}\n\ninline void applyNativeDeveloperEdgePolicy() {");
    CHECK(!shared.empty());
    const std::string sharedBody = withoutWhitespace(shared);
    CHECK(sharedBody.find("wr8(address,0u)") != std::string::npos);
    CHECK(sharedBody.find("0x8A5D40") != std::string::npos);
    CHECK(body.find("wr32(kNativeFatalHitDisableAddr,0u)") != std::string::npos);

    const std::string compactInl = withoutWhitespace(inl);
    CHECK(compactInl.find("trainingForceFighterIdle(")
          == std::string::npos);
    CHECK(compactInl.find("trainingRequestBothFightersIdle(")
          == std::string::npos);
    const std::string pairReset = withoutWhitespace(sourceSlice(
        inl, "static bool trainingResetPairToPreset(PositionPreset preset) {",
        "\n}"));
    CHECK(!pairReset.empty());
    CHECK(pairReset.find("trainingRestoreRoundCharacterActors()")
          != std::string::npos);
    CHECK(pairReset.find("trainingRestoreRoundCharacterRegions()")
          != std::string::npos);

    const std::string edges = sourceSlice(
        inl, "static bool trainingApplyActionEdges(",
        "\nstatic bool trainingAdmitPausedFrame()");
    CHECK(!edges.empty());
    for (const char* action : {"TrainingAction::RefillAll",
                               "TrainingAction::ToggleHitboxes",
                               "TrainingAction::ResetPositionRoundStart",
                               "TrainingAction::CycleDummyStance",
                               "TrainingAction::CycleGuardMode",
                               "TrainingAction::TogglePause",
                               "TrainingAction::FrameStep",
                               "TrainingAction::ReturnToCharacterSelect"}) {
        CHECK(edges.find(action) != std::string::npos);
    }
    CHECK(edges.find("nextReleasedGuardMode(") != std::string::npos);
    CHECK(edges.find("nextDummyStance(") != std::string::npos);
    CHECK(edges.find("GuardMode::AutoHeuristic") == std::string::npos);
    CHECK(edges.find("kTrainingResetFn") == std::string::npos);
    const std::string compactEdges = withoutWhitespace(edges);
    CHECK(compactEdges.find("trainingRestoreAllPresetActive()")
          != std::string::npos);
    CHECK(compactEdges.find("ResourceTrigger::WhenNeutral")
          != std::string::npos);
    CHECK(compactEdges.find("ResourceTrigger::Off") != std::string::npos);
    CHECK(compactEdges.find("TrainingRefillTarget::Both") != std::string::npos);
    CHECK(compactEdges.find("g_trainingRefillRequest|=") == std::string::npos);
    CHECK(compactEdges.find("kTrainingRefillNativeF4") == std::string::npos);

    const std::string preset = sourceSlice(
        inl, "static bool trainingRestoreAllPresetActive()",
        "\nstatic qtraining::NeutralTrigger");
    CHECK(!preset.empty());
    const std::string compactPreset = withoutWhitespace(preset);
    CHECK(compactPreset.find(
              "policy->trigger!=qtraining::ResourceTrigger::WhenNeutral")
          != std::string::npos);
    CHECK(compactPreset.find(
              "policy->target!=qtraining::TrainingRefillTarget::Both")
          != std::string::npos);

    CHECK(edges.find("TrainingAction::ChargeExFraction")
          == std::string::npos);
    CHECK(inl.find("planMeterChargeCadence(") != std::string::npos);
    CHECK(inl.find("trainingChargeExFraction(") != std::string::npos);
    const std::string charge = sourceSlice(
        inl, "static bool trainingChargeExFraction(",
        "\nstatic bool trainingConsumeRefillRequests(");
    CHECK(!charge.empty());
    const std::string busy = sourceSlice(
        charge, "if (plan[player].verdict == RefillVerdict::Busy)",
        "\n        if (plan[player].verdict != RefillVerdict::Apply)");
    CHECK(!busy.empty());
    CHECK(busy.find("continue;") != std::string::npos);
    CHECK(busy.find("return true;") == std::string::npos);
    const std::size_t stockWrite = charge.find(
        "wr32(record[player] + kTrainingExStocksOffset");
    const std::size_t fractionWrite = charge.find(
        "wr32(record[player] + kTrainingExFractionOffset");
    CHECK(stockWrite != std::string::npos);
    CHECK(fractionWrite != std::string::npos);
    CHECK(stockWrite < fractionWrite);
    const std::string types = readQohSource("src/training/TrainingTypes.h");
    CHECK(types.find("plan.stocks = static_cast<int32_t>(stocks + 1)")
          != std::string::npos);
    CHECK(types.find("plan.fraction = kExFractionFull")
          != std::string::npos);
    CHECK(types.find("if (stocks == kExStocksFull)") != std::string::npos);
    CHECK(types.find("plan.stocks = 0") != std::string::npos);
    const std::string modePolicy =
        readQohSource("src/input/ModeControlPolicy.cpp");
    CHECK(modePolicy.find(
              "ModeControlAction::TrainingChargeExFraction, 0x3F")
          != std::string::npos);
}

TEST(TrainingResourcePoliciesAreStepGatedAndFailClosed) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    const std::string policies = sourceSlice(
        inl, "static bool trainingConsumeResourcePolicies() {",
        "\nstatic bool trainingDrainFailedRecoveryCommands(");
    CHECK(!policies.empty());
    CHECK(policies.find("g_trainingResourcePolicyStepSerial")
          != std::string::npos);
    CHECK(policies.find("g_trainingStepSerial") != std::string::npos);
    CHECK(policies.find("resolvePolicyFire(") != std::string::npos);
    CHECK(policies.find("resolveHealthPolicyFire(") != std::string::npos);
    CHECK(policies.find("g_trainingNeutralTrigger") != std::string::npos);
    CHECK(policies.find("g_trainingExactIdleTrigger") != std::string::npos);
    CHECK(policies.find("signals.neutralReady") != std::string::npos);
    CHECK(policies.find("signals.exactIdleReady") != std::string::npos);
    CHECK(inl.find("g_trainingResetAcceptedThisStep") == std::string::npos);
    CHECK(policies.find("signals.resetAccepted") == std::string::npos);
    CHECK(policies.find("if (changed) ++g_trainingResourcePolicyFires")
          != std::string::npos);
    const std::string resources =
        readQohSource("src/training/TrainingResources.h");
    CHECK(resources.find("kTrainingExactIdleHoldSteps = 11")
          != std::string::npos);
    CHECK(resources.find("trainingExactIdleAction") != std::string::npos);
    const std::string resourceLogic =
        readQohSource("src/training/TrainingResources.cpp");
    CHECK(withoutWhitespace(resourceLogic).find(
              "signals.resetAccepted||signals.neutralReady||"
              "signals.exactIdleReady")
          != std::string::npos);
    CHECK(withoutWhitespace(resourceLogic).find(
              "signals.resetAccepted||signals.exactIdleReady")
          != std::string::npos);

    const std::string resetCapture = sourceSlice(
        inl, "static bool trainingCaptureResetResourceState(",
        "\nstatic bool trainingApplyResetResourceState(");
    CHECK(!resetCapture.empty());
    CHECK(resetCapture.find("signals.resetAccepted = true;")
          != std::string::npos);
    CHECK(resetCapture.find("resolveHealthPolicyFire(")
          != std::string::npos);
    CHECK(resetCapture.find("resolvePolicyFire(") != std::string::npos);

    const std::string code = withoutWhitespace(inl);
    CHECK(code.find("wr32(kNativeFatalHitDisableAddr,1u)") == std::string::npos);
    CHECK_EQ(countOccurrences(code, "wr32(kNativeFatalHitDisableAddr,0u)"),
             std::size_t{1});
    qtraining::TrainingCommandV1 retired{};
    retired.commandKind = static_cast<uint32_t>(
        qtraining::TrainingCommandKind::SetHpRegen);
    CHECK(!qtraining::validCommandPayload(retired));
}

TEST(TrainingPositionResetCommitsResourcesSynchronously) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());

    const std::string capture = sourceSlice(
        inl, "static bool trainingCaptureResetResourceState(",
        "\nstatic bool trainingApplyResetResourceState(");
    const std::string apply = sourceSlice(
        inl, "static bool trainingApplyResetResourceState(",
        "\nconstexpr uintptr_t kTrainingForcedActionApplyAddr");
    CHECK(!capture.empty());
    CHECK(!apply.empty());

    CHECK(capture.find("signals.resetAccepted = true;")
          != std::string::npos);
    CHECK(capture.find("planLifeReset(") != std::string::npos);
    CHECK(capture.find("planExReset(") != std::string::npos);
    CHECK(capture.find("qtraining::kGuardGaugeFull")
          != std::string::npos);

    CHECK(capture.find("trainingApplyLifeRefill(") == std::string::npos);
    CHECK(capture.find("trainingApplyExMeterRefill(") == std::string::npos);
    CHECK(apply.find("plan.steps[step].offset") != std::string::npos);
    const std::size_t exState =
        apply.find("record + kTrainingExStateOffset, 0u");
    const std::size_t exTimer =
        apply.find("record + kTrainingExTimerOffset, 0u");
    const std::size_t exPending =
        apply.find("record + kTrainingExPendingOffset + 4u * lane, 0u");
    const std::size_t exStocks =
        apply.find("record + kTrainingExStocksOffset");
    const std::size_t exFraction =
        apply.find("record + kTrainingExFractionOffset");
    CHECK(exState != std::string::npos);
    CHECK(exTimer != std::string::npos);
    CHECK(exPending != std::string::npos);
    CHECK(exStocks != std::string::npos);
    CHECK(exFraction != std::string::npos);
    CHECK(exState < exTimer);
    CHECK(exTimer < exPending);
    CHECK(exPending < exStocks);
    CHECK(exStocks < exFraction);
}

TEST(TrainingCloseResetGeometryLeavesRoundStartOnItsNativeAnchors) {
    const std::string positions =
        readQohSource("src/training/TrainingPositions.cpp");
    CHECK(!positions.empty());
    CHECK(positions.find("closeAnchorSeparation(") != std::string::npos);
    CHECK(positions.find("closeContactGap") != std::string::npos);
    const std::string planner = withoutWhitespace(sourceSlice(
        positions, "PositionPlan planPositionPreset(", "\n}\n\n}"));
    CHECK(!planner.empty());
    CHECK(planner.find("closeAnchorSeparation(config.p2Bounds,config.p1Bounds,")
          != std::string::npos);
    CHECK(planner.find("closeAnchorSeparation(config.p1Bounds,config.p2Bounds,")
          != std::string::npos);
    CHECK(planner.find("p1=config.nativeP1X;") != std::string::npos);
    CHECK(planner.find("p2=config.nativeP2X;") != std::string::npos);
    CHECK(planner.find("plan.cameraX=config.nativeCameraX;")
          != std::string::npos);
    CHECK(planner.find("if(preset!=PositionPreset::RoundStart){")
          != std::string::npos);

    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    const std::string code = withoutWhitespace(inl);
    CHECK(code.find("plan.anchorSeparation") != std::string::npos);
    CHECK(code.find("closeContactGap=0") != std::string::npos);
}

TEST(TrainingRecoveryEligibilityReadsTheDispatchStateAtPlus48) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    const std::string code = withoutWhitespace(inl);

    CHECK(code.find("kTrainingFighterActionStateOffset=48u") != std::string::npos);
    CHECK(code.find("kTrainingFighterDispatchActionOffset=84u")
          != std::string::npos);

    const std::string sampler = withoutWhitespace(sourceSlice(
        inl, "static bool trainingSampleRecoveryObservation(",
        "\nstatic qtraining::DummyPostureResult trainingDummyPosture("));
    CHECK(!sampler.empty());

    CHECK_EQ(countOccurrences(
                 sampler,
                 "out.action=rd32(defender+kTrainingFighterActionStateOffset)"),
             std::size_t{1});
    CHECK_EQ(countOccurrences(sampler, "kTrainingFighterDispatchActionOffset"),
             std::size_t{1});
    CHECK(sampler.find(
              "out.committedAction=rd32(defender+"
              "kTrainingFighterDispatchActionOffset)")
          != std::string::npos);
    for (const char* actor : {"fighter", "defender", "attacker"}) {
        for (const char* offset : {"kTrainingFighterActionStateOffset",
                                   "kTrainingFighterDispatchActionOffset"}) {
            std::string needle = "wr32(";
            needle += actor;
            needle += "+";
            needle += offset;
            CHECK(code.find(needle) == std::string::npos);
        }
    }

    const std::string recoveryCpp =
        readQohSource("src/training/TrainingRecovery.cpp");
    const std::string recoveryHeader =
        readQohSource("src/training/TrainingRecovery.h");
    CHECK(!recoveryCpp.empty());
    CHECK(!recoveryHeader.empty());
    CHECK(recoveryCpp.find("committedAction") == std::string::npos);
    CHECK_EQ(countOccurrences(recoveryHeader, "committedAction"),
             std::size_t{1});
    CHECK(withoutWhitespace(recoveryHeader).find("uint32_tcommittedAction=0;")
          != std::string::npos);

    const std::string compactRecovery = withoutWhitespace(recoveryHeader);
    CHECK_EQ(countOccurrences(compactRecovery,
                              "boolrecoveryAirEligibleAction(uint32_taction)"),
             std::size_t{1});
    CHECK_EQ(countOccurrences(compactRecovery,
                              "boolrecoveryGroundEligibleAction(uint32_taction)"),
             std::size_t{1});
    CHECK_EQ(countOccurrences(compactRecovery,
                              "boolrecoveryWallEligibleAction(uint32_taction)"),
             std::size_t{1});
    CHECK(compactRecovery.find("action<200&&") != std::string::npos);
    CHECK(compactRecovery.find("||action>=200;") != std::string::npos);
}

TEST(NativeHudDescriptorTransactionUnlinksThroughTheMirrorPointer) {
    const std::string hooks = readQohSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    const std::string compact = withoutWhitespace(hooks);

    CHECK(compact.find("kNativeHudListHeadPtr=qoh99::addr::kArenaMirrorPtr")
          != std::string::npos);
    CHECK(compact.find("constuintptr_tlistBase=rd32(kNativeHudListHeadPtr);")
          != std::string::npos);
    CHECK(compact.find("tx.headAddr=listBase+4u;") != std::string::npos);
    CHECK(compact.find("0x6177CCu+4u") == std::string::npos);

    CHECK(compact.find("if(listBase==0u)returntx;") != std::string::npos);

    CHECK(compact.find("if(rd32(tx.headAddr)!=tx.head)wr32(tx.headAddr,tx.head);")
          != std::string::npos);

    CHECK(compact.find("kNativeHudGlyphCountOffset=100404u") != std::string::npos);
    CHECK(compact.find("kNativeHudSpriteCountOffset=85524u") != std::string::npos);
    CHECK_EQ(countOccurrences(compact, "kNativeHudGlyphCountOffset"), 4u);
    CHECK_EQ(countOccurrences(compact, "kNativeHudSpriteCountOffset"), 4u);
    CHECK(compact.find(
              "if(rd32(tx.arena+kNativeHudGlyphCountOffset)>tx.glyphCount)")
          != std::string::npos);
    CHECK(compact.find(
              "if(rd32(tx.arena+kNativeHudSpriteCountOffset)>tx.spriteCount)")
          != std::string::npos);

    CHECK_EQ(countOccurrences(compact, "nativeHudBeginDescriptors();"), 3u);
    CHECK_EQ(countOccurrences(compact, "nativeHudDiscardDescriptors(tx);"), 3u);
}

TEST(TrainingFrameTraceIsPureObservationSampledAfterTheActorUpdate) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    const std::string compact = withoutWhitespace(inl);

    CHECK(compact.find("structTrainingFrameTraceFile{") != std::string::npos);
    CHECK(compact.find("std::FILE*file=nullptr;boolrequested=false;")
          != std::string::npos);

    const std::string observer = sourceSlice(
        inl, "static void trainingFrameTraceObserve() {",
        "\nstatic int trainingAdmitAfterNativeGate(");
    CHECK(!observer.empty());
    CHECK(observer.find("wr32(") == std::string::npos);
    CHECK(observer.find("wr16(") == std::string::npos);
    CHECK(observer.find("wr8(") == std::string::npos);

    const std::size_t stateLogCall =
        compact.find("trainingStateLogObserve(sample,g_trainingTriggers");
    const std::size_t traceCall =
        compact.find("trainingFrameTraceObserve();g_trainingContactCount=0u;");
    CHECK(stateLogCall != std::string::npos);
    CHECK(traceCall != std::string::npos);
    CHECK(stateLogCall < traceCall);

    CHECK(observer.find("if (framePtr != 0u") != std::string::npos);
    CHECK(observer.find("rangeReadable(static_cast<uintptr_t>(framePtr),")
          != std::string::npos);

    CHECK(compact.find("kTrainingFrameTraceRowCap=100000u")
          != std::string::npos);
    CHECK(compact.find("g_trainingFrameTrace.rows>=kTrainingFrameTraceRowCap")
          != std::string::npos);
    CHECK(compact.find("g_trainingFrameTrace.capped=true;")
          != std::string::npos);

    CHECK(observer.find("std::fflush(g_trainingFrameTrace.file);")
          != std::string::npos);

    CHECK(compact.find(
              "\"tick,player,action,cursor,hold,localFrame,duration,\"")
          != std::string::npos);
    CHECK(compact.find("\"activity,sprite") != std::string::npos);

    CHECK(compact.find("kTrainingFighterFrameRecordPtrOffset=1432u")
          != std::string::npos);
    CHECK(compact.find("kTrainingChrFrameRecordSize=64u")
          != std::string::npos);
    CHECK(compact.find(
              "static_assert(kTrainingFighterFrameRecordPtrOffset+4u<=addr::"
              "kFighterSize,")
          != std::string::npos);
}

TEST(TrainingAttackChainReArmsRecoveryInsteadOfEndingTheEpisode) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    const std::string tools =
        readQohSource("src/training/TrainingCombatTools.h");
    CHECK(!inl.empty());
    CHECK(!tools.empty());
    const std::string compact = withoutWhitespace(inl);
    const std::string compactTools = withoutWhitespace(tools);

    CHECK(compactTools.find("Continued=5,Rechained=6,") != std::string::npos);

    CHECK(compact.find(
              "actionMeta.semantic==qtraining::TrainingActionSemantic::Attack")
          != std::string::npos);
    CHECK(compact.find(
              "&&!trainingCombatExcludedFromConventionalAdvantage(actionMeta))")
          != std::string::npos);
    CHECK(compact.find(
              "qtraining::PrimaryRecoveryDisposition::Rechained;")
          != std::string::npos);

    const std::string changeBlock = sourceSlice(
        inl, "} else if (\n                        transition.proof",
        "qtraining::PrimaryRecoveryDisposition::Rechained;");
    CHECK(!changeBlock.empty());
    CHECK(changeBlock.find("expectation.resourceAction = action;")
          != std::string::npos);
    CHECK(changeBlock.find("expectation.actionEpoch = identity.actionEpoch;")
          != std::string::npos);
    CHECK(changeBlock.find("expectation = TrainingNaturalRecoveryExpectation{}")
          == std::string::npos);

    CHECK(compact.find(
              "naturalReady=recoveryDisposition==qtraining::"
              "PrimaryRecoveryDisposition::Ready;")
          != std::string::npos);

    const std::string cpp =
        readQohSource("src/training/TrainingCombatTools.cpp");
    CHECK(!cpp.empty());
    const std::string rechainCase = sourceSlice(
        cpp, "case PrimaryRecoveryDisposition::Rechained:", "break;");
    CHECK(!rechainCase.empty());
    CHECK(rechainCase.find(
              "status_.attackerActionEpoch = attacker.actionEpoch;")
          != std::string::npos);
    CHECK(rechainCase.find("++status_.chainedActions;") != std::string::npos);
    CHECK(rechainCase.find("attackerReady") == std::string::npos);
    CHECK(rechainCase.find("markUnmeasurable") == std::string::npos);
}

TEST(TrainingBlockstringGapCannotAttachToAnAlreadyPublishedNumber) {
    const std::string cpp =
        readQohSource("src/training/TrainingCombatTools.cpp");
    const std::string tools =
        readQohSource("src/training/TrainingCombatTools.h");
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!cpp.empty());
    CHECK(!tools.empty());
    CHECK(!wrapper.empty());
    const std::string compactCpp = withoutWhitespace(cpp);

    const std::string begin = sourceSlice(
        cpp, "void FrameAdvantageTracker::beginEpisode(",
        "\nvoid FrameAdvantageTracker::markTrade(");
    CHECK(!begin.empty());
    CHECK(begin.find("applyGapToEpisode(contact, attacker, defender,")
          != std::string::npos);
    CHECK(begin.find("const bool hadResolvedReadout =") != std::string::npos);
    CHECK(begin.find("hadResolvedReadout);") != std::string::npos);
    CHECK_EQ(countOccurrences(compactCpp, "applyGapToEpisode("), 2u);
    const std::string completion = sourceSlice(
        cpp, "status_.state = FrameAdvantageState::Complete;",
        "++status_.completedCount;");
    CHECK(!completion.empty());
    CHECK(completion.find("gapValid") == std::string::npos);
    CHECK(completion.find("gapFrames") == std::string::npos);

    CHECK_EQ(countOccurrences(compactCpp, "openGapCandidate("), 2u);
    const std::size_t readyLatch =
        compactCpp.find("status_.defenderReadyDelay=elapsed;");
    const std::size_t armSite =
        compactCpp.find("openGapCandidate(sample.stepSerial);");
    CHECK(readyLatch != std::string::npos);
    CHECK(armSite != std::string::npos);
    CHECK(readyLatch < armSite);

    const std::string unmeasurable = sourceSlice(
        cpp, "void FrameAdvantageTracker::markUnmeasurable(",
        "\nvoid FrameAdvantageTracker::clearReadout(");
    CHECK(!unmeasurable.empty());
    CHECK(unmeasurable.find("closeGapCandidate();") != std::string::npos);
    const std::string trade = sourceSlice(
        cpp, "void FrameAdvantageTracker::markTrade(",
        "\nvoid FrameAdvantageTracker::markUnmeasurable(");
    CHECK(!trade.empty());
    CHECK(trade.find("closeGapCandidate();") != std::string::npos);
    CHECK(trade.find("status_.gapValid = false;") != std::string::npos);

    const std::string apply = sourceSlice(
        cpp, "void FrameAdvantageTracker::applyGapToEpisode(",
        "\nvoid FrameAdvantageTracker::markUnmeasurable(");
    CHECK(!apply.empty());
    CHECK(apply.find("if (hadResolvedReadout) {") != std::string::npos);
    CHECK(apply.find("closeGapCandidate();") != std::string::npos);
    CHECK(apply.find("span != 0u") != std::string::npos);
    CHECK(withoutWhitespace(tools).find("kFrameAdvantageGapDisplaySteps=20;")
          != std::string::npos);
    CHECK(compactCpp.find("gapDisplaySteps_=kFrameAdvantageGapDisplaySteps;")
          != std::string::npos);
    CHECK(compactCpp.find("ageGapReadout();") != std::string::npos);

    const std::string compactTools = withoutWhitespace(tools);
    CHECK(compactTools.find("kFrameAdvantageMaxGapSteps=60;")
          != std::string::npos);
    CHECK(compactCpp.find("span<=kFrameAdvantageMaxGapSteps")
          != std::string::npos);

    const std::string compactWrapper = withoutWhitespace(wrapper);
    CHECK(compactWrapper.find("TrainingHasFrameAdvantageGapStatus(status)"
                              "&&status.frameAdvantageGapValid!=0u;")
          != std::string::npos);
    CHECK(compactWrapper.find("\"Gap%lu\"") != std::string::npos);
    CHECK(compactWrapper.find("\"P%u%+d%sgap%lu\"") == std::string::npos);
    CHECK(compactWrapper.find("constboolliveGap=") != std::string::npos);
}

TEST(TrainingAutoRecordReaderMirrorsTheNativeRecordAddressing) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    const std::string dummy = readQohSource("src/training/TrainingDummy.cpp");
    CHECK(!inl.empty());
    CHECK(!dummy.empty());
    const std::string compact = withoutWhitespace(inl);

    const std::string reader = sourceSlice(
        inl, "static void trainingAccumulateActiveGuardRecords(",
        "\nstatic qtraining::GuardDecision");
    CHECK(!reader.empty());

    CHECK(compact.find("kTrainingFrameNoCollisionSentinel=254u")
          != std::string::npos);
    CHECK(compact.find("kTrainingFrameProbeSentinel=255u")
          != std::string::npos);
    CHECK(reader.find("recordCount == 0u") != std::string::npos);
    CHECK(reader.find("recordCount == kTrainingFrameNoCollisionSentinel")
          != std::string::npos);
    CHECK(reader.find("recordCount == kTrainingFrameProbeSentinel")
          != std::string::npos);
    CHECK(reader.find("recordCount > kTrainingMaxScannedAttackRecords")
          != std::string::npos);

    CHECK(compact.find("kTrainingAttackRecordOrdinarySelectorMax=12u")
          != std::string::npos);
    CHECK(reader.find("selector > kTrainingAttackRecordOrdinarySelectorMax")
          != std::string::npos);

    CHECK(reader.find("out.legalPostureMask & legal") != std::string::npos);
    CHECK(reader.find("out.legalPostureMask | legal") == std::string::npos);

    CHECK_EQ(countOccurrences(reader, "rangeReadable("), 3u);
    CHECK(reader.find("out.valid = false;") != std::string::npos);
    CHECK(reader.find("kTrainingFighterFrameRecordPtrOffset")
          != std::string::npos);
    CHECK(reader.find("parked != 0u") != std::string::npos);

    CHECK(reader.find("wr32(") == std::string::npos);
    CHECK(reader.find("wr16(") == std::string::npos);
    CHECK(reader.find("wr8(") == std::string::npos);

    CHECK(compact.find("if(effectiveGuardMode==GuardMode::AutoRecord){")
          != std::string::npos);

    const std::string arm = sourceSlice(
        dummy, "case GuardMode::AutoRecord: {", "case GuardMode::Auto:");
    CHECK(!arm.empty());
    CHECK(arm.find("decision.guarding = true;") != std::string::npos);
    CHECK(countOccurrences(arm, "decision.logicalDirections = away")
          >= 2u);
    CHECK(arm.find("kLogicalUp") == std::string::npos);
}

TEST(TrainingResetEndsTheNativeComboInsteadOfLeavingItOnScreen) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    const std::string compact = withoutWhitespace(inl);

    CHECK(compact.find("kTrainingComboTeardownAddr=0x42A0A0u;")
          != std::string::npos);
    const std::string clear = sourceSlice(
        inl, "static void trainingClearNativeComboState(",
        "\nstatic bool trainingClearComboReadout()");
    CHECK(!clear.empty());
    CHECK(clear.find("kTrainingComboTeardownAddr)(fighter, 1);")
          != std::string::npos);

    CHECK(clear.find("kTrainingRecoveryLockOffset") == std::string::npos);
    CHECK(clear.find("wr32(kTrainingCombatRecordBase") == std::string::npos);

    const std::string seams = sourceSlice(
        inl, "static bool trainingVerifyNativeSeams() {",
        "\nstatic bool trainingVerifyGuardScanners()");
    CHECK(!seams.empty());
    CHECK(seams.find("kComboTeardownSig") != std::string::npos);
    CHECK(seams.find("kTrainingComboTeardownAddr") != std::string::npos);
    CHECK(seams.find("se::verifyCodeBytes(") != std::string::npos);
    CHECK(clear.find("rangeReadable(") == std::string::npos);

    const std::string readout = sourceSlice(
        inl, "static bool trainingClearComboReadout() {",
        "\nconstexpr uintptr_t kTrainingActorUnlinkAddr = 0x42DB00u;");
    CHECK(!readout.empty());
    CHECK(readout.find(
              "trainingClearNativeComboState(trainingFighter(player, false));")
          != std::string::npos);
    CHECK(readout.find("trainingAlternateFormActor(player)")
          != std::string::npos);
    CHECK(readout.find("trainingClearNativeComboState(alternate);")
          != std::string::npos);
    CHECK(readout.find("rd32(record + kTrainingComboHitsOffset) != 0u")
          != std::string::npos);

    const std::string reset = sourceSlice(
        inl, "static bool trainingResetPairToPreset(PositionPreset preset) {",
        "\n}");
    CHECK(!reset.empty());
    const std::size_t restorableCall =
        reset.find("trainingRoundCharacterStateRestorable()");
    const std::size_t captureResourcesCall =
        reset.find("trainingCaptureResetResourceState(resources)");
    const std::size_t despawnCall =
        reset.find("trainingDespawnTransientActors()");
    const std::size_t restoreActorsCall =
        reset.find("trainingRestoreRoundCharacterActors()");
    const std::size_t clearCall =
        reset.find("trainingClearComboReadout()");
    const std::size_t restoreRegionsCall =
        reset.find("trainingRestoreRoundCharacterRegions()");
    const std::size_t placeCall =
        reset.find("trainingApplyPositionPreset(preset)");
    const std::size_t applyResourcesCall =
        reset.find("trainingApplyResetResourceState(resources)");
    CHECK(restorableCall != std::string::npos);
    CHECK(captureResourcesCall != std::string::npos);
    CHECK(clearCall != std::string::npos);
    CHECK(despawnCall != std::string::npos);
    CHECK(restoreActorsCall != std::string::npos);
    CHECK(restoreRegionsCall != std::string::npos);
    CHECK(placeCall != std::string::npos);
    CHECK(applyResourcesCall != std::string::npos);
    CHECK(restorableCall < captureResourcesCall);
    CHECK(captureResourcesCall < despawnCall);
    CHECK(despawnCall < restoreActorsCall);
    CHECK(restoreActorsCall < clearCall);
    CHECK(clearCall < restoreRegionsCall);
    CHECK(restoreRegionsCall < placeCall);
    CHECK(placeCall < applyResourcesCall);
    const std::string neutral = sourceSlice(
        inl, "static CommandAckResult trainingApplyRoundStartNeutral(",
        "\n}");
    CHECK(!neutral.empty());
    CHECK(neutral.find(
              "trainingResetPairToPreset(PositionPreset::RoundStart)")
          != std::string::npos);
    CHECK(neutral.find("trainingClearComboReadout();")
          == std::string::npos);
}

TEST(TrainingContactPolicyOverridesDecisionsAndNeverEmulatesEffects) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    const std::string hooks = readQohSource("src/GameHooks.cpp");
    const std::string pure =
        readQohSource("src/training/TrainingContactPolicies.h");
    CHECK(!inl.empty());
    CHECK(!hooks.empty());
    CHECK(!pure.empty());

    CHECK(pure.find("0x6265") == std::string::npos);
    CHECK(pure.find("rd32(") == std::string::npos);
    CHECK(pure.find("wr32(") == std::string::npos);
    CHECK(pure.find("windows.h") == std::string::npos);
    CHECK(pure.find("MH_CreateHook") == std::string::npos);

    const std::string rng = sourceSlice(
        hooks, "unsigned int __cdecl hkRngDraw()", "\nstatic bool g_histTrace = false;");
    CHECK(!rng.empty());
    CHECK(rng.find("trainingCriticalContextArmed()") != std::string::npos);
    const std::string armed = sourceSlice(
        rng, "if (trainingCriticalContextArmed())", "\n    if (g_cosRngMode");
    CHECK(!armed.empty());
    CHECK_EQ(countOccurrences(armed, "g_origRng()"), 1u);
    CHECK(armed.find("g_privSeed") == std::string::npos);

    CHECK_EQ(countOccurrences(hooks, "&hkRngDraw"), 2u);

    const std::string parry = sourceSlice(
        inl, "static int __fastcall hkTrainingGuardActionRequest(",
        "\nstatic void trainingInstallContactPolicyHooks()");
    CHECK(!parry.empty());
    CHECK(parry.find("& 1u) == 0u") != std::string::npos);
    CHECK(parry.find("kParryMatcherActionRequest") != std::string::npos);
    CHECK(parry.find("kParryMatcherPendingFlag") != std::string::npos);
    CHECK(parry.find("kParryMatcherAcceptedResult") != std::string::npos);
    CHECK(parry.find("priorRequest") != std::string::npos);
    CHECK(parry.find("priorPending") != std::string::npos);
    CHECK(parry.find("+ 157") == std::string::npos);
    CHECK(parry.find("sub_419AA0") == std::string::npos);

    const std::string faultless = sourceSlice(
        inl, "static int __fastcall hkTrainingFaultlessDetector(",
        "\nstatic int __fastcall hkTrainingGuardActionRequest(");
    CHECK(!faultless.empty());
    CHECK(faultless.find("faultlessFamilyModeForActor") != std::string::npos);
    CHECK(faultless.find("wr32(") == std::string::npos);
    CHECK(faultless.find("1360") == std::string::npos);

    const std::string critical = sourceSlice(
        inl, "static int __fastcall hkTrainingCriticalScaler(",
        "\nstatic bool trainingCriticalContextArmed(");
    CHECK(!critical.empty());
    CHECK_EQ(countOccurrences(critical, "g_trainingOrigCriticalScaler("), 2u);
    CHECK(critical.find("*damage *= 2") == std::string::npos);
    CHECK(critical.find("/ 2") == std::string::npos);
    CHECK(critical.find("criticalPolicyClearsGuardCrushLatch")
          != std::string::npos);
    CHECK(critical.find("trainingClearGuardCrushLatch") != std::string::npos);

    const std::string install = sourceSlice(
        inl, "static void trainingInstallContactPolicyHooks()",
        "\nstatic bool installTrainingHooksOnce()");
    CHECK(!install.empty());
    CHECK(install.find("g_trainingFaultlessHookReady") != std::string::npos);
    CHECK(install.find("g_trainingParryHookReady") != std::string::npos);
    CHECK(install.find("g_trainingCriticalHookReady") != std::string::npos);
    CHECK(install.find("g_trainingHooksSupported = false") == std::string::npos);
    CHECK(install.find("TrainingDisableReason") == std::string::npos);

    const std::string active = sourceSlice(
        inl, "static bool trainingContactPolicyActive()",
        "\nstatic bool trainingGuardCrushLatchSet(");
    CHECK(!active.empty());
    CHECK(active.find("g_trainingLaunchRequested") != std::string::npos);
    CHECK(active.find("TrainingLifecycleState::Active") != std::string::npos);
    CHECK(active.find("&& onClockThread();") != std::string::npos);

    CHECK(inl.find("g_trainingContactPolicyLatched = "
                   "g_trainingContactPolicyRequested;") != std::string::npos);
}

TEST(TrainingHitboxOverlayAbiMatchesTheWrapperDeclaration) {
    CHECK(sizeof(qtraining::OverlayCommandAbi) == sizeof(EfzDx9OverlayCommand));
    CHECK(sizeof(qtraining::OverlayFrameAbi) == sizeof(EfzDx9OverlayFrame));
    CHECK(qtraining::kOverlayMaxCommands == EFZ_DX9_OVERLAY_MAX_COMMANDS);
    CHECK(qtraining::kOverlayTextCap == EFZ_DX9_OVERLAY_TEXT_CAP);
    CHECK(qtraining::kOverlayCommandRectFilled
          == EFZ_DX9_OVERLAY_COMMAND_RECT_FILLED);
    CHECK(qtraining::kOverlayCommandRect == EFZ_DX9_OVERLAY_COMMAND_RECT);
    CHECK(qtraining::kOverlayCommandText == EFZ_DX9_OVERLAY_COMMAND_TEXT);
    CHECK(qtraining::kOverlayTextShadow == EFZ_DX9_OVERLAY_TEXT_SHADOW);
    CHECK(qtraining::kOverlayTextAlignCenter
          == EFZ_DX9_OVERLAY_TEXT_ALIGN_CENTER);

    CHECK(offsetof(qtraining::OverlayCommandAbi, structSize)
          == offsetof(EfzDx9OverlayCommand, structSize));
    CHECK(offsetof(qtraining::OverlayCommandAbi, type)
          == offsetof(EfzDx9OverlayCommand, type));
    CHECK(offsetof(qtraining::OverlayCommandAbi, x)
          == offsetof(EfzDx9OverlayCommand, x));
    CHECK(offsetof(qtraining::OverlayCommandAbi, h)
          == offsetof(EfzDx9OverlayCommand, h));
    CHECK(offsetof(qtraining::OverlayCommandAbi, thickness)
          == offsetof(EfzDx9OverlayCommand, thickness));
    CHECK(offsetof(qtraining::OverlayCommandAbi, color)
          == offsetof(EfzDx9OverlayCommand, color));
    CHECK(offsetof(qtraining::OverlayCommandAbi, flags)
          == offsetof(EfzDx9OverlayCommand, flags));
    CHECK(offsetof(qtraining::OverlayCommandAbi, text)
          == offsetof(EfzDx9OverlayCommand, text));
    CHECK(offsetof(qtraining::OverlayFrameAbi, visible)
          == offsetof(EfzDx9OverlayFrame, visible));
    CHECK(offsetof(qtraining::OverlayFrameAbi, virtualWidth)
          == offsetof(EfzDx9OverlayFrame, virtualWidth));
    CHECK(offsetof(qtraining::OverlayFrameAbi, commandCount)
          == offsetof(EfzDx9OverlayFrame, commandCount));
    CHECK(offsetof(qtraining::OverlayFrameAbi, commands)
          == offsetof(EfzDx9OverlayFrame, commands));
}

TEST(TrainingHitboxOverlayCanvasMatchesTheGamesOwnSurface) {
    CHECK(qtraining::kHitboxVirtualWidth == 320.0f);
    CHECK(qtraining::kHitboxVirtualHeight == 240.0f);

    const std::string ddraw = readWorkspaceSource("ddraw_wrapper/ddraw.c");
    CHECK(!ddraw.empty());
    CHECK(ddraw.find("FRAMESRC_QOH99_SCALE_320x240") != std::string::npos);

    const std::string ops = readWorkspaceSource("ddraw_wrapper/surface_ops.c");
    CHECK(!ops.empty());
    CHECK(withoutWhitespace(ops).find("src_width!=320||src_height!=240")
          != std::string::npos);
}

TEST(TrainingHitboxOverlayProviderNeverTouchesGameMemory) {
    const std::string bridge =
        readQohSource("src/training/TrainingHitboxOverlayBridge.h");
    CHECK(!bridge.empty());

    const std::string compose = sourceSlice(
        bridge, "inline int hitboxComposeOverlayFrame(",
        "\ninline int __stdcall hitboxOverlayProviderThunk(");
    CHECK(!compose.empty());
    const std::string thunk = sourceSlice(
        bridge, "inline int __stdcall hitboxOverlayProviderThunk(",
        "\nstruct HitboxOverlayBridgeState");
    CHECK(!thunk.empty());

    const std::string bodies[] = {compose, thunk};
    for (const std::string& body : bodies) {
        const std::string compact = withoutWhitespace(body);
        CHECK(compact.find("rd8(") == std::string::npos);
        CHECK(compact.find("rd16(") == std::string::npos);
        CHECK(compact.find("rd32(") == std::string::npos);
        CHECK(compact.find("wr8(") == std::string::npos);
        CHECK(compact.find("QLOG(") == std::string::npos);
    }
    CHECK(withoutWhitespace(thunk).find(
              "if(!trainingHitboxes().read(snapshot))return0;")
          != std::string::npos);
}

TEST(TrainingHitboxReaderRunsOnTheGameThreadAndPublishesFinishedRects) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());

    const std::string tick = sourceSlice(
        inl, "static void trainingOuterTick() {", "\n    const uint32_t scene");
    CHECK(!tick.empty());
    CHECK(tick.find("trainingPublishHitboxSnapshot();") != std::string::npos);
    CHECK(tick.find("trainingTryRegisterHitboxOverlay();") != std::string::npos);

    const std::string publish = sourceSlice(
        inl, "static void trainingPublishHitboxSnapshot(bool fromRenderPhase) {",
        "\nstatic void trainingTryRegisterHitboxOverlay(");
    CHECK(!publish.empty());
    CHECK(publish.find("publishHidden();") != std::string::npos);
    CHECK(publish.find("g_trainingHitboxOverlayVisible = false;")
          != std::string::npos);

    CHECK(inl.find("qtraining::HitboxBoxSource::Secondary, 29u, 30u, false")
          != std::string::npos);
    CHECK(inl.find("qtraining::HitboxBoxSource::Attack,    31u, 32u, true")
          != std::string::npos);

    const std::string actorBoxes = sourceSlice(
        inl, "static void trainingReadActorBoxes(",
        "\nstatic void trainingPublishHitboxSnapshot(");
    CHECK(!actorBoxes.empty());
    const std::string compactBoxes = withoutWhitespace(actorBoxes);
    CHECK(compactBoxes.find("hitboxAttackActive(") != std::string::npos);
    CHECK(compactBoxes.find("rd16(actor+160u)") != std::string::npos);
    CHECK(compactBoxes.find("rd8(frame+14u)") == std::string::npos);

    CHECK(publish.find("rangeReadable(") != std::string::npos);
    const std::string actor = sourceSlice(
        inl, "static void trainingReadActorBoxes(",
        "\nstatic void trainingPublishHitboxSnapshot(");
    CHECK(!actor.empty());
    CHECK(actor.find("trainingCheckedScaledPointer(") != std::string::npos);
    CHECK(actor.find("rangeReadable(") != std::string::npos);
}

TEST(TrainingHitboxModesNeverRaiseBothDisplays) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    CHECK(inl.find("qtraining::hitboxVanillaFlagValue(g_trainingHitboxMode)")
          != std::string::npos);
    CHECK(withoutWhitespace(inl).find("g_trainingHitboxRequested")
          == std::string::npos);

    CHECK(qtraining::kHitboxDefaultRenderer
          == qtraining::HitboxDisplayMode::Custom);
    qtraining::TrainingCommandV1 command{};
    command.commandKind =
        static_cast<uint32_t>(qtraining::TrainingCommandKind::SetHitboxDisplay);
    command.value = 2u;
    CHECK(qtraining::validCommandPayload(command));
    command.value = 3u;
    CHECK(!qtraining::validCommandPayload(command));
}

TEST(NativeTrainingMenuNeverOffersTheUnreleasedGuardPredictor) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    const std::string values = sourceSlice(
        wrapper, "const int kNativeGuardModeValues[] = {", "};");
    CHECK(!values.empty());
    CHECK(values.find("QOH99_TRAINING_GUARD_AUTO,") == std::string::npos);
    CHECK(values.find("QOH99_TRAINING_GUARD_OFF") != std::string::npos);
    CHECK(values.find("QOH99_TRAINING_GUARD_AUTO_HEURISTIC")
          != std::string::npos);

    CHECK(wrapper.find("out.choiceMapped(\"Guard mode\"") != std::string::npos);
    CHECK(wrapper.find("kNativeGuardModeValues,") != std::string::npos);
}

TEST(NativeTrainingMenuSwitchIsReversibleFromBothSides) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    CHECK(wrapper.find("kNativeDiagnosticsRowTag") != std::string::npos);
    const std::string sidebar = sourceSlice(
        wrapper, "TrainingMenuPage DrawTrainingMenuSidebar(",
        "\n    for (const TrainingMenuPageInfo &info : kTrainingMenuPages)\n    {\n        if (info.page != selected) continue;");
    CHECK(!sidebar.empty());
    CHECK(sidebar.find("TrainingMenuSetNativeStyle(true)") != std::string::npos);

    CHECK(wrapper.find("bool &TrainingMenuNativeStyleRef()") != std::string::npos);
    const std::string setter = sourceSlice(
        wrapper, "void TrainingMenuSetNativeStyle(bool nativeStyle)\n{", "\n}");
    CHECK(!setter.empty());
    CHECK(setter.find("TrainingMenuNativeStyleRef() = nativeStyle;")
          != std::string::npos);
    CHECK(setter.find("OverlayIniSet(\"MenuStyle\"") != std::string::npos);
}

TEST(NativeTrainingMenuDiagnosticsSwitchAlsoEnablesTheAdvancedPages) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string handler = sourceSlice(
        wrapper, "if (row.slot == kNativeDiagnosticsRowTag)", "return;");
    CHECK(!handler.empty());
    CHECK(handler.find("OverlayIniSet(\"MenuAdvanced\", 1)") != std::string::npos);
    CHECK(handler.find("TrainingMenuSetNativeStyle(false)") != std::string::npos);

    CHECK(wrapper.find("info.page != kTrainingPageDiagnostics")
          != std::string::npos);
}

TEST(NativeTrainingMenuBuildsEveryPageTheSidebarOffers) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string dispatch = sourceSlice(
        wrapper, "NativeMenuPageBuild build;", "\n    const int firstRow = NativeMenuFirstSelectable(");
    CHECK(!dispatch.empty());

    static const char *const kPages[] = {
        "kTrainingPageQuick",      "kTrainingPageDummy",
        "kTrainingPageReactions",  "kTrainingPageRecording",
        "kTrainingPageView",       "kTrainingPageKeys",
        "kTrainingPageHelp",       "kTrainingPageDiagnostics",
    };
    for (const char *page : kPages) {
        CHECK(dispatch.find(page) != std::string::npos);
    }
}

TEST(NativeTrainingMenuDrawsPrimitivesRatherThanImGuiWidgets) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string draw = sourceSlice(
        wrapper, "bool DrawNativeTrainingMenu(", "\nvoid BuildDebugMenu(");
    CHECK(!draw.empty());
    const std::string compact = withoutWhitespace(draw);
    CHECK(compact.find("ImGui::Button(") == std::string::npos);
    CHECK(compact.find("ImGui::Checkbox(") == std::string::npos);
    CHECK(compact.find("ImGui::Combo(") == std::string::npos);
    CHECK(compact.find("ImGui::Selectable(") == std::string::npos);
    CHECK(compact.find("ImGui::Begin(") == std::string::npos);
    CHECK(draw.find("GetForegroundDrawList()") != std::string::npos);

    CHECK(wrapper.find("g_trainingPixelFont = io.Fonts->AddFontDefault(")
          != std::string::npos);
    CHECK(draw.find("ImFont *font = g_trainingPixelFont;")
          != std::string::npos);
    CHECK(draw.find("if (font == nullptr) font = ImGui::GetFont();")
          != std::string::npos);
    CHECK(draw.find("qi18n::language() != qi18n::Language::English")
          != std::string::npos);
}

TEST(NativeTrainingMenuAnchorsToTheGameRectangle) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string metrics = sourceSlice(
        wrapper, "NativeMenuMetrics NativeMenuComputeMetrics(", "\n}");
    CHECK(!metrics.empty());
    CHECK(metrics.find("params.dstW") != std::string::npos);
    CHECK(metrics.find("params.dstH") != std::string::npos);
    CHECK(metrics.find("params.dstX") != std::string::npos);
    CHECK(metrics.find("params.dstY") != std::string::npos);
    CHECK(metrics.find("13.0f * scale") != std::string::npos);
    CHECK(metrics.find("std::floor(frameH / 240.0f)") != std::string::npos);
}

TEST(NativeTrainingMenuFitsAndClipsAtEveryWindowSize) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    const std::string metrics = sourceSlice(
        wrapper, "NativeMenuMetrics NativeMenuComputeMetrics(", "\n}");
    CHECK(!metrics.empty());
    CHECK(metrics.find("scale -= 1.0f;") != std::string::npos);
    CHECK(metrics.find("if (scale <= 1.0f)") != std::string::npos);
    CHECK(metrics.find("NativeMenuTabLineCount(") != std::string::npos);
    CHECK(metrics.find("kNativeMenuMinBodyRows") != std::string::npos);

    const std::string draw = sourceSlice(
        wrapper, "bool DrawNativeTrainingMenu(", "\nvoid BuildDebugMenu(");
    CHECK(!draw.empty());
    CHECK(draw.find("PushClipRect(") != std::string::npos);
    CHECK(draw.find("PopClipRect()") != std::string::npos);

    const std::string tabs = sourceSlice(
        wrapper, "void NativeMenuDrawTabs(", "\n}");
    CHECK(!tabs.empty());
    CHECK(tabs.find("y += m.lineHeight;") != std::string::npos);

    CHECK(draw.find("visibleRows") != std::string::npos);
    CHECK(draw.find("g_nativeMenu.scroll") != std::string::npos);
}

TEST(NativeTrainingMenuReachesTabsWithUpAndSwitchesThemWithLeftRight) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string draw = sourceSlice(
        wrapper, "bool DrawNativeTrainingMenu(", "\nvoid BuildDebugMenu(");
    CHECK(!draw.empty());

    CHECK(draw.find("g_nativeMenu.tabFocus && in.left") != std::string::npos);
    CHECK(draw.find("g_nativeMenu.tabFocus && in.right") != std::string::npos);
    CHECK(draw.find("stepPagePrev") < draw.find("NativeMenuPageBuild build;"));

    CHECK(draw.find("if (g_nativeMenu.row == firstRow) g_nativeMenu.tabFocus = true;")
          != std::string::npos);
    CHECK(draw.find("g_nativeMenu.tabFocus = false;") != std::string::npos);

    CHECK(draw.find("if (!g_nativeMenu.tabFocus && g_nativeMenu.row >= 0")
          != std::string::npos);
    CHECK(draw.find("!g_nativeMenu.tabFocus && i == g_nativeMenu.row")
          != std::string::npos);

    CHECK(draw.find("else if (!hasSelectableRow)") != std::string::npos);
    CHECK(draw.find("if (!g_nativeMenu.tabFocus)") != std::string::npos);
    CHECK(draw.find("cursorLine < g_nativeMenu.scroll") != std::string::npos);
}

TEST(NativeTrainingMenuIgnoresARestingPointerUntilItMovesOrClicks) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string draw = sourceSlice(
        wrapper, "bool DrawNativeTrainingMenu(", "\nvoid BuildDebugMenu(");
    CHECK(!draw.empty());

    CHECK(draw.find("g_nativeMenu.mouseEngaged = false;") != std::string::npos);
    CHECK(draw.find("g_nativeMenu.mouseEngaged = true;") != std::string::npos);
    CHECK(draw.find("if (keyboardUsed) g_nativeMenu.mouseEngaged = false;")
          != std::string::npos);
    CHECK(draw.find("mouseMoved || mouseClicked") != std::string::npos);

    CHECK(draw.find("if (mouseActive && NativeRowSelectable(row)")
          != std::string::npos);
    CHECK(draw.find(
              "const bool clicked = mouseClicked && mouseActive && "
              "!paletteModal;")
          != std::string::npos);

    CHECK(draw.find("const ImVec2 &mouse = mousePos;") != std::string::npos);
}

TEST(NativeTrainingMenuKeepsEveryTabOnOneRow) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string metrics = sourceSlice(
        wrapper, "NativeMenuMetrics NativeMenuComputeMetrics(", "\n}");
    CHECK(!metrics.empty());
    CHECK(metrics.find("m.tabLines == 1") != std::string::npos);
    CHECK(metrics.find("m.tabLines <= 2") == std::string::npos);
}

TEST(TrainingGuardModeNamesAreBehaviouralAndSharedByBothMenus) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    const std::string names = sourceSlice(
        wrapper, "const char *const kNativeGuardModeNames[] = {", "};");
    CHECK(!names.empty());
    CHECK(names.find("All") != std::string::npos);
    CHECK(names.find("Adaptive") != std::string::npos);
    CHECK(names.find("Record") == std::string::npos);
    CHECK(names.find("posture") == std::string::npos);
    CHECK(names.find("Auto") == std::string::npos);

    const std::string classic = sourceSlice(
        wrapper, "static const char *kGuardModes[] = {", "};");
    CHECK(!classic.empty());
    CHECK(classic.find("Auto") == std::string::npos);
    CHECK(classic.find("All") != std::string::npos);
    CHECK(classic.find("Adaptive") != std::string::npos);

    const std::string values = sourceSlice(
        wrapper, "const int kNativeGuardModeValues[] = {", "};");
    CHECK(!values.empty());
    CHECK(values.find("QOH99_TRAINING_GUARD_AUTO_RECORD")
          < values.find("QOH99_TRAINING_GUARD_AUTO_HEURISTIC"));
}

TEST(NativeTrainingMenuGivesTriggersRealSubmenus) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string triggers = sourceSlice(
        wrapper, "void NativeMenuBuildTriggers(", "\n}");
    CHECK(!triggers.empty());

    CHECK(triggers.find("out.submenu(") != std::string::npos);
    CHECK(triggers.find("NativeMenuTriggerSummary(") != std::string::npos);
    CHECK(triggers.find("out.action(\"< Back\"") != std::string::npos);
    CHECK(triggers.find("out.submenu(\"Move\"")
          != std::string::npos);
    CHECK(triggers.find("out.action(\"< Back\"")
          < triggers.find("out.submenu(\"Move\""));

    const std::string summary = sourceSlice(
        wrapper, "void NativeMenuTriggerSummary(", "\n}");
    CHECK(!summary.empty());
    CHECK(summary.find("triggerMoveIndex[event] <= 0") != std::string::npos);
    CHECK(summary.find("triggerMacroSlot[event] <= 0") != std::string::npos);

    const std::string draw = sourceSlice(
        wrapper, "bool DrawNativeTrainingMenu(", "\nvoid BuildDebugMenu(");
    CHECK(!draw.empty());
    CHECK(draw.find("if (g_nativeMenu.submenu >= 0)") != std::string::npos);
    CHECK(draw.find("g_nativeMenu.submenu = -1;") != std::string::npos);
    CHECK(draw.find("TRAINING  >  %s") != std::string::npos);

    const std::string commit = sourceSlice(
        wrapper, "void NativeMenuCommitRow(", "\n}");
    CHECK(!commit.empty());
    const std::size_t openTag =
        commit.find("row.slot >= kNativeTriggerOpenRowTag");
    CHECK(openTag != std::string::npos);
    CHECK(commit.find("if (!activate) return;", openTag)
          != std::string::npos);
}

TEST(OverlayMenuNavigationComesFromTheConfiguredInGameControls) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    const std::string readInput = sourceSlice(
        wrapper, "NativeMenuInput NativeMenuReadInput()", "\n}");
    CHECK(!readInput.empty());
    CHECK(withoutWhitespace(readInput).find("IsKeyPressed(ImGuiKey_Gamepad")
          == std::string::npos);
    CHECK(withoutWhitespace(wrapper).find(
              "ConfigFlags|=ImGuiConfigFlags_NavEnableGamepad")
          == std::string::npos);
    CHECK(readInput.find("GetAsyncKeyState") != std::string::npos);
    CHECK(withoutWhitespace(readInput).find("ImGui::IsKeyPressed(")
          == std::string::npos);

    CHECK(wrapper.find("EFZDX9_Presenter_SubmitMenuNavigation")
          != std::string::npos);
    CHECK(wrapper.find("if (!g_presenter.menuVisible) return FALSE;")
          != std::string::npos);
    CHECK(wrapper.find("InterlockedOr(&g_menuNavigationPending")
          != std::string::npos);
    CHECK(wrapper.find("InterlockedExchange(&g_menuNavigationPending, 0)")
          != std::string::npos);

    CHECK(wrapper.find("in.up = in.up || took(") == std::string::npos);
    CHECK(wrapper.find("in.commit = in.commit || took(") == std::string::npos);
    CHECK(wrapper.find("debtAgainstSubmitted") != std::string::npos);
    CHECK(wrapper.find("debtAgainstOwnEdge") != std::string::npos);
    CHECK(wrapper.find("const bool submitted = took(nav);")
          != std::string::npos);
    for (const char* nav : {"QOH99_MENU_NAV_UP", "QOH99_MENU_NAV_DOWN",
                            "QOH99_MENU_NAV_LEFT", "QOH99_MENU_NAV_RIGHT",
                            "QOH99_MENU_NAV_CONFIRM", "QOH99_MENU_NAV_BACK"}) {
        std::string needle = "navigate(";
        needle += nav;
        CHECK(wrapper.find(needle) != std::string::npos);
    }
    CHECK(wrapper.find("kMenuDuplicateFrames") != std::string::npos);

    const std::string def = readWorkspaceSource("ddraw_wrapper/ddraw.def");
    CHECK(!def.empty());
    CHECK(def.find("EFZDX9_Presenter_SubmitMenuNavigation") != std::string::npos);
    CHECK(def.find("EFZDX9_Presenter_ToggleMenu") != std::string::npos);

    const std::string bridge =
        readQohSource("src/input/MenuNavigationBridge.h");
    CHECK(!bridge.empty());
    CHECK(bridge.find("if (slot.down) return 0u;") != std::string::npos);
    CHECK(bridge.find("kMenuNavRepeatDelayMs") == std::string::npos);
    CHECK(bridge.find("kMenuNavRepeatIntervalMs") == std::string::npos);
    CHECK(wrapper.find("if (state.down) return false;") != std::string::npos);
    CHECK(bridge.find("void reset()") != std::string::npos);

    const std::string hooks = readQohSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    CHECK(hooks.find("s_menuNav.step(menuMask") != std::string::npos);
    CHECK(hooks.find("qinput::menuOverlayVisible()") != std::string::npos);
    CHECK(hooks.find("s_menuNav.reset();") != std::string::npos);
}

TEST(OverlayMenuPadOpenIsItsOwnActionSoItCannotDoubleFire) {
    const std::string contract =
        readQohSource("src/input/ModeControlContract.h");
    CHECK(!contract.empty());
    CHECK(contract.find("OpenOverlayMenuPad = 4,") != std::string::npos);
    CHECK(contract.find("\"openOverlayMenuPad\"") != std::string::npos);
    const std::string descriptor = sourceSlice(
        contract, "{ModeControlAction::OpenOverlayMenuPad,", "},");
    CHECK(!descriptor.empty());
    CHECK(descriptor.find("KeyboardOrSelectedControllerDigital")
          != std::string::npos);

    const std::string original = sourceSlice(
        contract, "{ModeControlAction::ToggleOverlayMenu,", "},");
    CHECK(!original.empty());
    CHECK(original.find("KeyboardOnly") != std::string::npos);

    CHECK(contract.find("static_cast<uint16_t>(ModeControlAction::Count) == 64")
          != std::string::npos);

    const std::string hooks = readQohSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    CHECK(hooks.find("qinput::ModeControlAction::OpenOverlayMenuPad")
          != std::string::npos);
    CHECK(hooks.find("qinput::menuOverlayToggle();") != std::string::npos);
}

TEST(NativeTrainingMenuOffersTheWholeTriggerResponseNotJustRecordings) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string triggers = sourceSlice(
        wrapper, "void NativeMenuBuildTriggers(", "\n}");
    CHECK(!triggers.empty());

    CHECK(triggers.find("out.heading(\"MOVES\")") != std::string::npos);
    CHECK(triggers.find("out.heading(\"RECORDINGS\")") != std::string::npos);
    CHECK(triggers.find("out.action(\"Off\"") != std::string::npos);

    CHECK(triggers.find("TrainingMoveWasAlreadyListed(") != std::string::npos);
    CHECK(triggers.find("qtraining::trainingMoveCount(") != std::string::npos);
    CHECK(triggers.find("\" (empty)\"") != std::string::npos);

    const std::string summary = sourceSlice(
        wrapper, "void NativeMenuTriggerSummary(", "\n}");
    CHECK(!summary.empty());
    CHECK(summary.find("TrainingReactionPreview(") != std::string::npos);
    CHECK(summary.find("triggerMoveIndex[event] <= 0") != std::string::npos);

    const std::string commit = sourceSlice(
        wrapper, "void NativeMenuCommitRow(", "\n}");
    CHECK(!commit.empty());
    const std::size_t setResponse =
        commit.find("row.slot == kNativeTriggerSetResponseTag");
    CHECK(setResponse != std::string::npos);
    CHECK(commit.find("g_trainingUi.triggerMoveIndex[event] = 0;", setResponse)
          != std::string::npos);
    CHECK(commit.find("g_trainingUi.triggerMacroSlot[event] = 0;", setResponse)
          != std::string::npos);
    CHECK(commit.find("EnqueueTrainingTriggerConfig(status, event);", setResponse)
          != std::string::npos);

    const std::string draw = sourceSlice(
        wrapper, "bool DrawNativeTrainingMenu(", "\nvoid BuildDebugMenu(");
    CHECK(!draw.empty());
    CHECK(draw.find("if (g_nativeMenu.submenuChild)") < draw.find(
              "else if (g_nativeMenu.submenu >= 0)"));

    CHECK(wrapper.find("kNativeMenuMaxRows = 160") != std::string::npos);
}

TEST(OverlayMenuPadOpenDefaultsToStart) {
    const std::string policy = readQohSource("src/input/ModeControlPolicy.cpp");
    CHECK(!policy.empty());
    CHECK(policy.find("ProfileBinding padButton(") != std::string::npos);
    CHECK(policy.find("ProbeKind::GamepadButton") != std::string::npos);
    CHECK(policy.find("addPad(file, ModeControlAction::OpenOverlayMenuPad, 6)")
          != std::string::npos);

    const std::string contract =
        readQohSource("src/input/ModeControlContract.h");
    CHECK(!contract.empty());
    const std::string original = sourceSlice(
        contract, "{ModeControlAction::ToggleOverlayMenu,", "},");
    CHECK(!original.empty());
    CHECK(original.find("KeyboardOnly") != std::string::npos);
}

TEST(NativeTrainingMenuListsEveryTriggerEventInAbiOrder) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    CHECK(wrapper.find("IM_ARRAYSIZE(kNativeTriggerEventNames)")
          != std::string::npos);
    CHECK(wrapper.find("== QOH99_TRAINING_TRIGGER_EVENT_COUNT")
          != std::string::npos);

    const std::string names = sourceSlice(
        wrapper, "const char *const kNativeTriggerEventNames[] = {", "};");
    CHECK(!names.empty());
    CHECK(names.find("Getting hit") < names.find("Waking up"));
    CHECK(names.find("Waking up") < names.find("Teching in the air"));
    CHECK(names.find("Teching in the air") < names.find("Teching on the ground"));
    CHECK(names.find("Teching on the ground") < names.find("Blocking (guard cancel)"));
    CHECK(names.find("Blocking (guard cancel)") != std::string::npos);

    const std::string triggers = sourceSlice(
        wrapper, "void NativeMenuBuildTriggers(", "\n}");
    CHECK(!triggers.empty());
    CHECK(triggers.find("constexpr int events = QOH99_TRAINING_TRIGGER_EVENT_COUNT")
          != std::string::npos);
}

TEST(NativeTrainingMenuSetsResourceValuesInTheGamesOwnUnits) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string commit = sourceSlice(
        wrapper, "void NativeMenuCommitRow(", "\n}");
    CHECK(!commit.empty());
    const std::size_t setValue =
        commit.find("row.slot >= kNativeSetValueRowTag");
    CHECK(setValue != std::string::npos);

    CHECK(commit.find("status.lifeSetMax", setValue) != std::string::npos);
    CHECK(commit.find("QOH99_TRAINING_EX_STOCKS_FULL", setValue)
          != std::string::npos);
    CHECK(commit.find("QOH99_TRAINING_GUARD_GAUGE_FULL", setValue)
          != std::string::npos);
    CHECK(commit.find("if (value < 1u) value = 1u;", setValue)
          != std::string::npos);
}

TEST(NativeTrainingMenuReachesThePlayerFacingCommandSurface) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::size_t start = wrapper.find("bool &TrainingMenuNativeStyleRef()");
    const std::size_t end = wrapper.find("void BuildDebugMenu(");
    CHECK(start != std::string::npos);
    CHECK(end != std::string::npos);
    CHECK(start < end);
    const std::string native = wrapper.substr(start, end - start);

    static const char *const kRequired[] = {
        "QOH99_TRAINING_COMMAND_TOGGLE_PAUSE",
        "QOH99_TRAINING_COMMAND_FRAME_STEP",
        "QOH99_TRAINING_COMMAND_RESET_ROUND_START_NEUTRAL",
        "QOH99_TRAINING_COMMAND_REFILL_LIFE",
        "QOH99_TRAINING_COMMAND_SET_HEALTH_POLICY",
        "QOH99_TRAINING_COMMAND_SET_EX_POLICY",
        "QOH99_TRAINING_COMMAND_SET_GUARD_GAUGE_POLICY",
        "QOH99_TRAINING_COMMAND_SET_LIFE_VALUE",
        "QOH99_TRAINING_COMMAND_SET_EX_METER",
        "QOH99_TRAINING_COMMAND_SET_GUARD_GAUGE_VALUE",
        "QOH99_TRAINING_COMMAND_SET_EX_RESTORE_TARGET",
        "QOH99_TRAINING_COMMAND_SET_GUARD_MODE",
        "QOH99_TRAINING_COMMAND_SET_STANCE",
        "QOH99_TRAINING_COMMAND_SET_CONTACT_POLICY",
        "QOH99_TRAINING_COMMAND_SET_RECOVERY_POLICY",
        "QOH99_TRAINING_COMMAND_SET_TERMINAL_RECOVERY",
        "QOH99_TRAINING_COMMAND_SET_THROW_TECH_CHANCE",
        "QOH99_TRAINING_COMMAND_TOGGLE_RECORD",
        "QOH99_TRAINING_COMMAND_SAVE_STATE",
        "QOH99_TRAINING_COMMAND_SET_PLAYBACK_MODE",
        "QOH99_TRAINING_COMMAND_SET_FACING_POLICY",
        "QOH99_TRAINING_COMMAND_SET_MACRO_AFTER_RESET",
        "QOH99_TRAINING_COMMAND_SET_METER_CHARGE_CONFIG",
        "QOH99_TRAINING_COMMAND_SET_RECOVERY_TIMING",
        "QOH99_TRAINING_COMMAND_SET_HITBOX_DISPLAY",
        "QOH99_TRAINING_COMMAND_SET_FRAME_ADVANTAGE_ENABLED",
        "QOH99_TRAINING_COMMAND_SET_NATIVE_HUD_MASK",
    };
    for (const char *command : kRequired) {
        CHECK(native.find(command) != std::string::npos);
    }

    CHECK(native.find("EnqueueTrainingTriggerConfig(") != std::string::npos);

    CHECK(native.find("QOH99_TRAINING_COMMAND_SET_DEBUG_OVERLAY_ENABLED")
          == std::string::npos);
    CHECK(native.find("QOH99_TRAINING_COMMAND_SET_FRAME_TRACE_ENABLED")
          == std::string::npos);
    CHECK(native.find("QOH99_TRAINING_COMMAND_SET_STATE_CHANGE_LOG_ENABLED")
          == std::string::npos);
}

TEST(NativeTrainingMenuResendsWholeConfigWordsFromTheRuntimeEcho) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string commit = sourceSlice(
        wrapper, "void NativeMenuCommitRow(", "\n}");
    CHECK(!commit.empty());

    const std::size_t charge =
        commit.find("row.slot == kNativeMeterChargeSideRowTag");
    CHECK(charge != std::string::npos);
    CHECK(commit.find("status.meterChargeTarget", charge) != std::string::npos);
    CHECK(commit.find("status.meterChargeInterval", charge)
          != std::string::npos);

    const std::size_t timing =
        commit.find("row.slot >= kNativeRecoveryTimingRowTag");
    CHECK(timing != std::string::npos);
    CHECK(commit.find("status.recoveryDelaySteps", timing) != std::string::npos);
    CHECK(commit.find("status.recoveryRandomMinSteps", timing)
          != std::string::npos);
    CHECK(commit.find("status.recoveryRandomMaxSteps", timing)
          != std::string::npos);

    CHECK(commit.find("if (values[2] < values[1])", timing) != std::string::npos);
}

TEST(NativeTrainingMenuShowsRecoveryTimingOnlyWhenADelayModeUsesIt) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string dummy = sourceSlice(
        wrapper, "void NativeMenuBuildDummy(", "\n}");
    CHECK(!dummy.empty());

    CHECK(dummy.find("QOH99_TRAINING_RECOVERY_FIXED_DELAY") != std::string::npos);
    CHECK(dummy.find("QOH99_TRAINING_RECOVERY_RANDOM_DELAY")
          != std::string::npos);
    CHECK(dummy.find("usesFixed") != std::string::npos);
    CHECK(dummy.find("usesRandom") != std::string::npos);
    CHECK(dummy.find("if (usesFixed)") != std::string::npos);
    CHECK(dummy.find("if (usesRandom)") != std::string::npos);
    CHECK(dummy.find("status.recoveryModeAir") != std::string::npos);
    CHECK(dummy.find("status.recoveryModeGround") != std::string::npos);
    CHECK(dummy.find("status.recoveryModeWall") != std::string::npos);
    CHECK(dummy.find("status.recoveryModeTerminal") != std::string::npos);
    CHECK(dummy.find("QOH99_TRAINING_RECOVERY_SLOT_THROW_ESCAPE")
          == std::string::npos);
}

TEST(NativeTrainingMenuHintsDescribeOnlyTheSelectedOption) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    CHECK(wrapper.find("const char *const *choiceHints;") != std::string::npos);
    const std::string draw = sourceSlice(
        wrapper, "bool DrawNativeTrainingMenu(", "\nvoid BuildDebugMenu(");
    CHECK(!draw.empty());
    CHECK(draw.find("row.choiceHints[row.value]") != std::string::npos);
    CHECK(draw.find("NativeMenuTextWrapped(dl, font, m.fontSize,")
          != std::string::npos);
    CHECK(wrapper.find("CalcWordWrapPositionA") != std::string::npos);

    CHECK(wrapper.find("struct NativeTextScroll") != std::string::npos);
    CHECK(wrapper.find("kNativeTextScrollHoldMs") != std::string::npos);
    CHECK(wrapper.find("kNativeTextScrollStepMs") != std::string::npos);
    CHECK_EQ(countOccurrences(draw, "&footerScroll"), 2u);
    CHECK(wrapper.find("NativeTextScroll *scroll = nullptr") !=
          std::string::npos);
    CHECK(wrapper.find("const float markX = pos.x + wrapWidth - markWidth;")
          != std::string::npos);

    static const char *const kTables[] = {
        "kNativeGuardModeHints",
        "kNativeHitboxStyleHints",
        "kNativeContactModeHints",
        "kNativeRecoveryHints",
        "kNativeTerminalHints",
        "kNativeRestoreHints",
        "kNativeStanceHints",
        "kNativeFacingHints",
        "kNativePlaybackHints",
    };
    for (const char *table : kTables) {
        CHECK(wrapper.find(table) != std::string::npos);
    }

    CHECK(wrapper.find("All reads the move and blocks it correctly. Adaptive ")
          == std::string::npos);
}

TEST(NativeTrainingMenuLegendNamesTheRealConfirmAndBackButtons) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    CHECK(wrapper.find("EFZDX9_Presenter_SetMenuNavigationLabels")
          != std::string::npos);
    CHECK(wrapper.find("g_menuConfirmLabel[24] = \"A\"") != std::string::npos);
    CHECK(wrapper.find("g_menuBackLabel[24] = \"B\"") != std::string::npos);
    const std::string draw = sourceSlice(
        wrapper, "bool DrawNativeTrainingMenu(", "\nvoid BuildDebugMenu(");
    CHECK(!draw.empty());
    CHECK(draw.find("g_menuConfirmLabel") != std::string::npos);
    CHECK(draw.find("g_menuBackLabel") != std::string::npos);

    const std::string def = readWorkspaceSource("ddraw_wrapper/ddraw.def");
    CHECK(def.find("EFZDX9_Presenter_SetMenuNavigationLabels")
          != std::string::npos);

    const std::string bridge =
        readQohSource("src/input/MenuNavigationBridge.h");
    CHECK(!bridge.empty());
    CHECK(bridge.find("FightAction::A") != std::string::npos);
    CHECK(bridge.find("FightAction::B") != std::string::npos);
    CHECK(bridge.find("GetKeyNameTextA") != std::string::npos);
    CHECK(bridge.find("lastConfirm") != std::string::npos);

    const std::string commit = sourceSlice(
        wrapper, "void NativeMenuCommitRow(", "\n}");
    CHECK(!commit.empty());
    const std::size_t back = commit.find("row.slot == kNativeMenuBackRowTag");
    CHECK(back != std::string::npos);
    CHECK(commit.find("if (!activate) return;", back) != std::string::npos);
}

TEST(NativeTrainingMenuTagRangesCannotOverlap) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    CHECK(wrapper.find("kNativeTriggerOpenRowTag = 0xF010u") != std::string::npos);
    CHECK(wrapper.find("kNativeRecoveryTimingRowTag = 0xF020u")
          != std::string::npos);

    CHECK(wrapper.find("trigger-open range runs into the recovery-timing tags")
          != std::string::npos);
    CHECK(wrapper.find("recovery-timing range runs into the HUD guard tag")
          != std::string::npos);
    CHECK(wrapper.find("HUD guard tag runs into the trigger-slot tags")
          != std::string::npos);
    CHECK(wrapper.find("native-HUD range runs into the set-value tags")
          != std::string::npos);
    CHECK(wrapper.find("set-value range runs into the meter-charge tags")
          != std::string::npos);
    CHECK(wrapper.find("meter-charge tags run into the trigger-open range")
          != std::string::npos);

    const std::string commit = sourceSlice(
        wrapper, "void NativeMenuCommitRow(", "\n}");
    CHECK(!commit.empty());
    CHECK(commit.find("kNativeTriggerOpenRowTag + 0x40u") == std::string::npos);
    CHECK(commit.find("+ QOH99_TRAINING_TRIGGER_EVENT_COUNT)")
          != std::string::npos);
}

TEST(NativeTrainingMenuOnlyPollsWhileVisibleAndFocused) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string readInput = sourceSlice(
        wrapper, "NativeMenuInput NativeMenuReadInput()", "\n}");
    CHECK(!readInput.empty());

    CHECK(readInput.find("if (!g_presenter.menuVisible) return idle;")
          != std::string::npos);
    CHECK(readInput.find("GetForegroundWindow() != g_presenter.window")
          != std::string::npos);

    CHECK(readInput.find("kPollIntervalMs") != std::string::npos);
    CHECK(readInput.find("nowMs - lastPollMs < kPollIntervalMs")
          != std::string::npos);
    CHECK(readInput.find("kPollIntervalMs")
          < readInput.find("GetAsyncKeyState("));
}

TEST(EveryWrapperExportTheDllResolvesIsListedInTheDefFile) {
    const std::string bridge =
        readQohSource("src/input/MenuNavigationBridge.h");
    CHECK(!bridge.empty());
    const std::string def = readWorkspaceSource("ddraw_wrapper/ddraw.def");
    CHECK(!def.empty());

    const std::string needle = "\"EFZDX9_";
    std::size_t at = 0;
    int found = 0;
    for (;;) {
        const std::size_t start = bridge.find(needle, at);
        if (start == std::string::npos) break;
        const std::size_t from = start + 1;
        const std::size_t end = bridge.find('"', from);
        if (end == std::string::npos) break;
        const std::string name = bridge.substr(from, end - from);
        at = end + 1;
        ++found;
        CHECK(def.find(name) != std::string::npos);
    }

    CHECK(found >= 4);
}

TEST(CustomPalettesStayPresentationOnlyAndGated) {
    const std::string hooks = readWorkspaceSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());

    CHECK(hooks.find("netPaletteExchangeTick") != std::string::npos);
    const std::size_t exchange = hooks.find("static void netPaletteExchangeTick");
    CHECK(exchange != std::string::npos);
    const std::size_t canonical = hooks.find("netCanonicalRematchTick(frame)");
    CHECK(canonical != std::string::npos);
    const std::size_t harness = hooks.find("void harnessFrontendPressTick");
    CHECK(harness != std::string::npos);
    const std::size_t harnessEnd = hooks.find('}', harness);
    CHECK(harnessEnd != std::string::npos);
    CHECK(hooks.substr(harness, harnessEnd - harness)
              .find("netPaletteExchangeTick") != std::string::npos);

    CHECK(hooks.find("a peer may only recolour its own fighter")
          != std::string::npos);
    CHECK(hooks.find("inbound.side == localSide") != std::string::npos);

    CHECK(hooks.find("paletteCustomConfirmTranslate") != std::string::npos);
    CHECK(hooks.find("kSelectorLive") != std::string::npos);
    const std::size_t translate =
        hooks.find("static int paletteCustomConfirmTranslate");
    CHECK(translate != std::string::npos);
    const std::size_t translateEnd =
        hooks.find("\nstatic", translate + 1);
    CHECK(translateEnd != std::string::npos);
    const std::string body =
        hooks.substr(translate, translateEnd - translate);
    CHECK(body.find("kSelectorLive") != std::string::npos);
    CHECK(body.find("kSelectConfirmState = 0x8716D0") != std::string::npos);
    CHECK(hooks.find("0x4AA454") != std::string::npos);
    CHECK(body.find("0x4AA454") == std::string::npos);

    const std::string types =
        readWorkspaceSource("src/replay/ReplayTypes.h");
    CHECK(!types.empty());
    CHECK(types.find("Palette = 10") != std::string::npos);
    CHECK(types.find("&& kind != ChunkKindV2::Palette") != std::string::npos);

    const std::string bridge =
        readWorkspaceSource("src/game/PaletteBridge.h");
    CHECK(!bridge.empty());
    CHECK(bridge.find("EFZDDRAW_QueryPalette") != std::string::npos);
    CHECK(bridge.find("EFZDDRAW_SubmitPalette") != std::string::npos);
    const std::string def = readWorkspaceSource("ddraw_wrapper/ddraw.def");
    CHECK(def.find("EFZDDRAW_QueryPalette") != std::string::npos);
    CHECK(def.find("EFZDDRAW_SubmitPalette") != std::string::npos);
    CHECK(def.find("EFZDX9_Presenter_IsMenuVisible @22") != std::string::npos);
}

TEST(CustomPaletteOwnershipIsDeclaredPerRole) {
    const std::string hooks =
        readWorkspaceSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    const std::size_t at = hooks.find("static void paletteOwnershipTick");
    CHECK(at != std::string::npos);
    const std::size_t end = hooks.find("\nstatic", at + 1);
    CHECK(end != std::string::npos);
    const std::string body = hooks.substr(at, end - at);

    CHECK(body.find("paletteDeclareOwnership") != std::string::npos);
    CHECK(body.find("RunMode::Spectator") != std::string::npos);
    CHECK(body.find("RunMode::Replay") != std::string::npos);
    CHECK(body.find("g_netLocalCtrl") != std::string::npos);

    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    CHECK(wrapper.find("g_paletteLocalSlotMask") != std::string::npos);
    CHECK(wrapper.find("does not own that slot") != std::string::npos);
    CHECK(wrapper.find("EFZDDRAW_SetPaletteOwnership") != std::string::npos);
    const std::string def = readWorkspaceSource("ddraw_wrapper/ddraw.def");
    CHECK(def.find("EFZDDRAW_SetPaletteOwnership @25") != std::string::npos);
}

TEST(SpectatorReplayHotkeysReachTheViewerThroughTheWrapper) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    CHECK(wrapper.find("EFZDX9_RegisterSpectatorHotkeySink")
          != std::string::npos);
    CHECK(wrapper.find("g_presenter.spectatorHotkeySink != nullptr")
          != std::string::npos);
    const std::string ddraw = readWorkspaceSource("ddraw_wrapper/ddraw.c");
    CHECK(ddraw.find("EFZDDRAW_QueryWrapperHotkey") != std::string::npos);
    const std::string def = readWorkspaceSource("ddraw_wrapper/ddraw.def");
    CHECK(def.find("EFZDX9_RegisterSpectatorHotkeySink @26")
          != std::string::npos);
    CHECK(def.find("EFZDDRAW_QueryWrapperHotkey @27") != std::string::npos);
    const std::string hooks = readWorkspaceSource(
        "src/GameHooks.cpp");
    CHECK(hooks.find("\"EFZDX9_RegisterSpectatorHotkeySink\"")
          != std::string::npos);
    CHECK(hooks.find("\"EFZDDRAW_QueryWrapperHotkey\"") != std::string::npos);
    CHECK(hooks.find("query(\"ReplayFaster\"") != std::string::npos);
    CHECK(hooks.find("case kWrapperHotkeyFaster:") != std::string::npos);
    CHECK(hooks.find("submit(qreplay::PlaybackCommand::Exit, 0ull);")
          != std::string::npos);
    CHECK(hooks.find("press %s to skip to %s") != std::string::npos);
}

TEST(DelayAndRollbackAreChosenOnTheMeasuredScreenAndNowhereElse) {
    const std::string caster =
        readWorkspaceSource("src/client/Main.cpp");
    CHECK(!caster.empty());
    CHECK(caster.find("settingsDelayRollbackMenu") == std::string::npos);
    CHECK(caster.find("\"Delay and rollback...\",") == std::string::npos);
    CHECK(caster.find("lastDelay") == std::string::npos);
    CHECK(caster.find("lastRollback") == std::string::npos);
    CHECK(caster.find("delay = myLow <= authMaxDelay ? myLow : authMaxDelay;")
          != std::string::npos);
    CHECK(caster.find(
              "rollback = myLowR <= authMaxRollback ? myLowR : "
              "authMaxRollback;") != std::string::npos);
    CHECK(caster.find("s2.delay = localDelay; s2.rollback = localRollback;")
          != std::string::npos);
    CHECK(caster.find("localRollback = floored;") != std::string::npos);
    CHECK(caster.find("delay    = flagDelay    >= 0 ? flagDelay    : s.delay;")
          != std::string::npos);

    const std::string hooks =
        readWorkspaceSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    CHECK(hooks.find("inline uint32_t netRouteLiveFloorFrames() {")
          != std::string::npos);
    CHECK(hooks.find("rttMicros = ping.meanMicros;") != std::string::npos);
    CHECK(hooks.find("return static_cast<uint32_t>(qnet::oneWayFrames(")
          != std::string::npos);
    const std::size_t delayFloor =
        hooks.find("inline uint8_t netMinimumLocalDelayForRoute() {");
    const std::size_t rollbackFloor =
        hooks.find("inline uint8_t netMinimumLocalRollbackForRoute() {");
    CHECK(delayFloor != std::string::npos);
    CHECK(rollbackFloor != std::string::npos);
    CHECK(hooks.find("const uint32_t required = netRouteLiveFloorFrames();",
                     delayFloor) < delayFloor + 400u);
    CHECK(hooks.find("const uint32_t required = netRouteLiveFloorFrames();",
                     rollbackFloor) < rollbackFloor + 400u);

    const std::string readme = readWorkspaceSource("README.md");
    CHECK(!readme.empty());
    CHECK(readme.find("Settings -> Delay and rollback...")
          == std::string::npos);
    CHECK(readme.find("There is nothing to configure beforehand.")
          != std::string::npos);
    CHECK(readme.find("Both can be changed at") != std::string::npos);
}

TEST(TuningFloorsAreThePhysicalMinimumAndMoveWithThePeer) {
    CHECK(qgame::kMaximumGameplayRollback == 30);
    CHECK(qgame::kMaximumGameplayDelay == 15);
    CHECK(qnet::kCapabilityRollback
          == static_cast<int>(qgame::kMaximumGameplayRollback));
    CHECK(qnet::kCapabilityDelay
          == static_cast<int>(qgame::kMaximumGameplayDelay));
    const std::string hooks =
        readWorkspaceSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    CHECK(hooks.find("rollback ceiling exceeds the savestate pool depth")
          != std::string::npos);
    CHECK(hooks.find("constexpr int kRollbackCap =") != std::string::npos);
    CHECK(hooks.find("g_netRemoteRollback = (remoteRollback < 0) ? 0")
          != std::string::npos);

    const std::string caster =
        readWorkspaceSource("src/client/Main.cpp");
    CHECK(!caster.empty());
    for (const char* form : {"delay > 15", "rollback > 15",
                             "delay >= 15", "rollback >= 15",
                             "delay <= 15", "rollback <= 15",
                             "delay < 15", "rollback < 15",
                             "delay == 15", "rollback == 15"}) {
        CHECK(caster.find(form) == std::string::npos);
    }
    const std::string dllMain =
        readWorkspaceSource("src/DllMain.cpp");
    CHECK(!dllMain.empty());
    for (const char* form : {"delay > 15", "rollback > 15",
                             "Delay > 15", "Rollback > 15",
                             "delay <= 15", "rollback <= 15"}) {
        CHECK(dllMain.find(form) == std::string::npos);
    }
    CHECK(dllMain.find("qgame::kMaximumGameplayRollback")
          != std::string::npos);
    CHECK(caster.find("authMaxRollback = qnet::kCapabilityRollback")
          != std::string::npos);
    CHECK(caster.find("kPreviousCapabilityRollback = 15")
          != std::string::npos);

    CHECK(caster.find("const int floorRequirement =") != std::string::npos);
    CHECK(caster.find("worseRequirement(rec.floorFrames, peerRequiredFrames)")
          != std::string::npos);
    CHECK(caster.find("hostRequiredOwn = worseRequirement(") 
          != std::string::npos);
    CHECK(caster.find("recommendation.floorFrames, clientReported")
          != std::string::npos);
    CHECK(caster.find("clientRequiredOwn = recommendation.floorFrames;")
          != std::string::npos);
    CHECK(caster.find("requiredFramesWire(measured.floorFrames)")
          != std::string::npos);
    CHECK(caster.find("requiredOwnReceive") == std::string::npos);
    CHECK(caster.find("requiredPeerReceive") == std::string::npos);

    CHECK(caster.find("ctrl.sendMsg(MSG_SETTINGS_PENDING, &mirror,")
          != std::string::npos);
    CHECK(caster.find("const bool peerLive = ctx.havePending;")
          != std::string::npos);
    CHECK(caster.find("floorRequirement + 1 - peerDelayNow, 0, authMaxRollback")
          != std::string::npos);
    CHECK(caster.find("Your opponent is on input delay ")
          != std::string::npos);

    CHECK(qnet::kProtocolVersion == 21u);
    const std::string contract =
        readWorkspaceSource("src/ipc/BuildContract.h");
    CHECK(contract.find("_CTRL21_") != std::string::npos);
}

TEST(TrainingDummyIsARoleAndTheSwapExchangesIt) {
    const std::string integration = readWorkspaceSource(
        "src/training/TrainingGameIntegration.inl");
    CHECK(!integration.empty());
    CHECK(integration.find("static uint32_t trainingDummySlot() {")
          != std::string::npos);
    CHECK(integration.find("static uint32_t trainingHumanSlot() {")
          != std::string::npos);

    CHECK(countOccurrences(integration, "trainingFighter(0u, false)") == 2u);
    CHECK(countOccurrences(integration, "trainingFighter(1u, false)") == 2u);
    CHECK(countOccurrences(integration, "trainingFighter(0u, true)") == 1u);
    CHECK(countOccurrences(integration, "trainingFighter(1u, true)") == 1u);

    CHECK(integration.find(
              "const uintptr_t defender = "
              "trainingFighter(trainingPolicySlot(), false);")
          != std::string::npos);

    CHECK(integration.find("const bool swap = g_trainingPlayerSwap;")
          != std::string::npos);
    CHECK(integration.find("input.dummyIsP1 = swap;") != std::string::npos);
    CHECK(integration.find(
              "swap ? g_trainingPhysical.p2Mask : g_trainingPhysical.p1Mask")
          != std::string::npos);
    CHECK(integration.find("input.macroOverrideP1 = swap ? macro.overrideP2")
          != std::string::npos);

    CHECK(integration.find(
              "if (g_trainingMacros.state() != MacroState::Idle)")
          != std::string::npos);
    CHECK(integration.find("g_trainingRecoveryP1.reset(g_trainingGeneration);")
          != std::string::npos);
    CHECK(integration.find("status.playerSwap = g_trainingPlayerSwap")
          != std::string::npos);

    const std::size_t macroReset =
        integration.find("g_trainingMacros.reset();");
    const std::size_t swapReset =
        integration.find("g_trainingPlayerSwap = false;",
                         macroReset == std::string::npos ? 0u : macroReset);
    CHECK(macroReset != std::string::npos);
    CHECK(swapReset != std::string::npos);
    CHECK(swapReset > macroReset);

    const std::string macros = readWorkspaceSource(
        "src/training/TrainingMacros.cpp");
    CHECK(!macros.empty());
    CHECK(macros.find("result.p2Mask = input.recordSourceMask;")
          != std::string::npos);
}

TEST(TrainingDummySettingsFollowTheUndrivenFighter) {
    const std::string integration = readWorkspaceSource(
        "src/training/TrainingGameIntegration.inl");
    CHECK(!integration.empty());
    CHECK(integration.find("static uint32_t trainingPolicySlot() {")
          != std::string::npos);
    CHECK(integration.find("static uint32_t trainingPolicyPressureSlot() {")
          != std::string::npos);

    CHECK(integration.find("static bool g_trainingPolicyFollowsHuman = false;")
          != std::string::npos);
    const std::string compactIntegration = withoutWhitespace(integration);
    CHECK(compactIntegration.find(
              "boolpolicyFollowsHuman=false;if(macroRecordOwnership){")
          != std::string::npos);

    CHECK(compactIntegration.find(
              "if(policyFollowsHuman!=g_trainingPolicyFollowsHuman){"
              "g_trainingPolicyFollowsHuman=policyFollowsHuman;")
          != std::string::npos);

    const std::string decision = sourceSlice(
        integration, "static qtraining::GuardDecision trainingGuardDecision(",
        "\nstatic qtraining::DummyPostureResult trainingDummyPosture(");
    CHECK(!decision.empty());
    CHECK(decision.find("trainingPolicySlot()") != std::string::npos);
    CHECK(decision.find("trainingPolicyPressureSlot()") != std::string::npos);
    CHECK(decision.find("trainingDummySlot()") == std::string::npos);
    CHECK(decision.find("trainingHumanSlot()") == std::string::npos);
    CHECK(integration.find("trainingDummySlot()") != std::string::npos);
    CHECK(integration.find("trainingHumanSlot()") != std::string::npos);

    const std::string contact = sourceSlice(
        integration, "static void trainingRecordContact(",
        "\n    uint8_t attackerOwner = 0u;");
    CHECK(!contact.empty());
    CHECK(contact.find(
              "const uintptr_t defender =\n"
              "            trainingFighter(trainingPolicySlot(), false);")
          != std::string::npos);
    CHECK(contact.find("trainingDummySlot()") == std::string::npos);

    const std::string resetSettings = sourceSlice(
        integration, "static void trainingResetPlayerSettings() {",
        "\n}");
    CHECK(!resetSettings.empty());
    CHECK(resetSettings.find("g_trainingPolicyFollowsHuman = false;")
          != std::string::npos);
    CHECK(countOccurrences(integration,
                           "g_trainingPolicyFollowsHuman = false;") == 4u);

    const std::string header = readWorkspaceSource(
        "src/training/TrainingInput.h");
    CHECK(!header.empty());
    CHECK(header.find("constexpr uint32_t trainingPolicyLane(bool "
                      "humanDrivesDummyLane,")
          != std::string::npos);
    CHECK(withoutWhitespace(header).find(
              "returntrainingDrivenLane(humanDrivesDummyLane,dummyIsP1)"
              "==kTrainingDrivenLaneP1"
              "?kTrainingDrivenLaneP2:kTrainingDrivenLaneP1;")
          != std::string::npos);
}

TEST(SpectatedSessionsRecordThroughTheV2Recorder) {
    const std::string hooks =
        readWorkspaceSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    const std::string seam = sourceSlice(
        hooks, "int doSpectator(FrameDispatchFn orig) {",
        "\nstatic qgame::NativeHudOfflineOwner nativeHudOfflineOwner() {");
    CHECK(!seam.empty());
    const std::string compact = withoutWhitespace(seam);

    CHECK(compact.find("netReplayMaybeStageFreshGameV2(frame);")
          != std::string::npos);
    CHECK(compact.find("meta.canonicalActions[0]=record.playerOneInput;")
          != std::string::npos);
    CHECK(compact.find("meta.canonicalActions[1]=record.playerTwoInput;")
          != std::string::npos);
    for (const char* field : {"meta.replayLifecycleAfter=",
                              "meta.replayRoundTransition=",
                              "meta.replayRoster=netReplayRosterV2();",
                              "meta.replayStage=netReplayStageV2();",
                              "meta.replayDispatcherResult=tickResult;"}) {
        CHECK(compact.find(field) != std::string::npos);
    }

    const std::size_t commit =
        compact.find("qgame::spectatorViewerCommitConfirmed(");
    const std::size_t publish =
        compact.find("qgame::replay::replayRecordConfirmedV2(replayFrame);");
    CHECK(commit != std::string::npos);
    CHECK(publish != std::string::npos);
    CHECK(commit < publish);

    CHECK(compact.find("netSpectatorNameRecorderLanes();") != std::string::npos);
    CHECK(countOccurrences(seam, "netSpectatorNameRecorderLanes();") == 2u);
    CHECK(withoutWhitespace(hooks).find(
              "qgame::replay::replayRecordSetSourceLaneNamesV2(names);")
          != std::string::npos);

    const std::string runtime = readWorkspaceSource(
        "src/game/replay/ReplayRuntime.cpp");
    CHECK(!runtime.empty());
    const std::string naming = sourceSlice(
        runtime, "bool replayRecordSetSourceLaneNamesV2(", "\n}\n");
    CHECK(!naming.empty());
    const std::string compactNaming = withoutWhitespace(naming);
    CHECK(compactNaming.find("r.epochTracker.hasPendingLaunch()")
          != std::string::npos);
    CHECK(compactNaming.find(
              "!=qreplay::ReplayGameRecordStateV2::WaitingForFreshGame")
          != std::string::npos);

    const std::string caster =
        readWorkspaceSource("src/client/Main.cpp");
    CHECK(!caster.empty());
    CHECK(withoutWhitespace(caster).find("boolsaveSpectatorSessions=true;")
          != std::string::npos);
}

TEST(InteractiveStartupAppliesTheSavedLanguage) {
    const std::string main =
        readWorkspaceSource("src/client/Main.cpp");
    CHECK(!main.empty());
    const std::string body = sourceSlice(main, "int interactive() {", "\n}\n");
    CHECK(!body.empty());
    const std::string compact = withoutWhitespace(body);

    const std::size_t load = compact.find("s.load();");
    const std::size_t apply =
        compact.find("qi18n::setLanguageFromIniValue(s.language);");
    CHECK(load != std::string::npos);
    CHECK(apply != std::string::npos);
    CHECK(load < apply);

    CHECK(withoutWhitespace(main).find(
              "qi18n::setLanguageFromIniValue(a.s.language);")
          != std::string::npos);
}

TEST(OfflineVersusReturnsToCharacterSelectOnTheOnlineBinding) {
    const std::string hooks =
        readWorkspaceSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    const std::string compact = withoutWhitespace(hooks);

    const std::string contract =
        readWorkspaceSource("src/input/ModeControlContract.h");
    CHECK(!contract.empty());
    CHECK(contract.find("OnlineReturnCharacterSelect") != std::string::npos);
    CHECK(withoutWhitespace(contract).find("OfflineReturnCharacterSelect")
          == std::string::npos);
    CHECK(contract.find(
              "Return to character select (online or offline VS)")
          != std::string::npos);

    const std::string evaluator = sourceSlice(
        hooks, "qinput::ModeControlEvaluation netEvaluateOnlineModeControls() {",
        "\n}\n");
    CHECK(!evaluator.empty());
    CHECK(withoutWhitespace(evaluator).find("RunMode::Observe")
          == std::string::npos);

    const std::string tick = sourceSlice(
        hooks, "void netOfflineFrameTick() {", "\n}\n");
    CHECK(!tick.empty());
    const std::string compactTick = withoutWhitespace(tick);
    CHECK(compactTick.find(
              "constboolofflineReturnEligible="
              "g_mode==RunMode::Observe&&offlineManagedWanted()"
              "&&!g_netActive&&!g_replayActive&&!g_replayRoleActive"
              "&&!g_trainingLaunchRequested"
              "&&!qgame::directBootUnattendedFrontendActive();")
          != std::string::npos);
    CHECK(compactTick.find("if(offlineReturnEligible){") != std::string::npos);
    CHECK(compactTick.find(
              "netOfflineReturnToCharacterSelectTick(offlineReturnEligible"
              "&&(modeEvaluation.edges&qinput::modeControlBit(")
          != std::string::npos);

    const std::string arm = sourceSlice(
        hooks, "void netOfflineReturnToCharacterSelectTick(const bool edge) {",
        "\n}\n");
    CHECK(!arm.empty());
    const std::string compactArm = withoutWhitespace(arm);
    CHECK(compactArm.find(
              "qgame::onlineReturnToCharacterSelectEligible(rd32(kSceneStateAddr))")
          != std::string::npos);
    CHECK(compactArm.find("qgame::replay::replayAbandonOpenGameV2(")
          != std::string::npos);
    CHECK(compactArm.find("g_offlineRecorderRebaseline=true;")
          != std::string::npos);
    CHECK(compactArm.find("qreplay::TerminalReason::ProcessExit")
          != std::string::npos);
    CHECK(compactArm.find("g_offlineReturnToCharacterSelectPending=true;")
          != std::string::npos);
    CHECK(compactArm.find("if(!edge)return;") != std::string::npos);

    CHECK(compactArm.find("if(netOfflineRematchTheaterFlipped()){")
          != std::string::npos);
    CHECK(compactArm.find(
              "if(!qnetplay::rematchTheaterHumanVersusHuman(observed)){")
          != std::string::npos);
    CHECK(compactArm.find("elseif(netRecorderArmed()){") != std::string::npos);
    CHECK(compactArm.find("netRecorderCloseMatchSegment(") != std::string::npos);

    CHECK(compactTick.find(
              "enabledTraining&=~qinput::modeControlBit("
              "qinput::ModeControlAction::TrainingReturnCharacterSelect);")
          != std::string::npos);
    const std::size_t standDown = compactTick.find(
        "enabledTraining&=~qinput::modeControlBit(");
    const std::size_t armOnline = compactTick.find(
        "enabledTraining|=qinput::modeControlBit("
        "qinput::ModeControlAction::OnlineReturnCharacterSelect);");
    CHECK(standDown != std::string::npos);
    CHECK(armOnline != std::string::npos);
    CHECK(standDown < armOnline);

    CHECK(compact.find("if(g_offlineReturnToCharacterSelectPending){"
                       "g_offlineReturnToCharacterSelectPending=false;"
                       "if(qgame::onlineReturnToCharacterSelectEligible("
                       "rd32(kSceneStateAddr))){")
          != std::string::npos);

    const std::string seam = sourceSlice(
        hooks, "void netOfflineRecorderCommitV2() {", "\n}\n");
    CHECK(withoutWhitespace(seam).find(
              "if(liveScene!=2u){s_staged=false;s_opened=false;"
              "s_havePendingCapture=false;}")
          != std::string::npos);
}

TEST(OfflineSessionsPublishThroughTheV2Recorder) {
    const std::string hooks =
        readWorkspaceSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    const std::string compact = withoutWhitespace(hooks);

    CHECK(compact.find("voidnetOfflineFrameTick(){"
                       "(void)netApplyOfflineInputBindings();"
                       "netOfflineRecorderCommit();"
                       "netOfflineRecorderCommitV2();")
          != std::string::npos);

    const std::string seam = sourceSlice(
        hooks, "void netOfflineRecorderCommitV2() {", "\n}\n");
    CHECK(!seam.empty());
    const std::string compactSeam = withoutWhitespace(seam);
    CHECK(compactSeam.find("if(!qgame::replay::replayRecorderV2Active())return;")
          != std::string::npos);
    CHECK(compactSeam.find("netRecorderArmed()") == std::string::npos);
    CHECK(compactSeam.find("if(g_netActive)return;") != std::string::npos);
    CHECK(compactSeam.find(
              "qgame::replay::replayRecordStageFreshGameV2(s_pendingCapture)")
          != std::string::npos);
    CHECK(compactSeam.find("qgame::replay::replayRecordConfirmedV2(frame)")
          != std::string::npos);
    const std::size_t liveRead =
        compactSeam.find("constuint32_tliveScene=rd32(kSceneStateAddr);");
    const std::size_t counterRead =
        compactSeam.find("constuint32_tcounter=rd32(0x4AB778);");
    CHECK(liveRead != std::string::npos);
    CHECK(counterRead != std::string::npos);
    CHECK(liveRead < counterRead);
    const std::size_t barrier =
        compactSeam.find("constboolenteredSetup=liveScene==2u&&liveSub==-1");
    CHECK(barrier != std::string::npos);
    CHECK(barrier < counterRead);
    CHECK(compactSeam.find("if(enteredSetup&&!s_havePendingCapture){")
          != std::string::npos);
    CHECK(compactSeam.find("constboolatLaunch=") == std::string::npos);

    CHECK(compactSeam.find(
              "if(s_havePendingCapture&&!s_staged&&s_have"
              "&&s_before.scene==2u&&s_before.matchSub==-1){")
          != std::string::npos);

    CHECK(compactSeam.find("constboolstepped=counter!=s_lastCounter;")
          != std::string::npos);
    CHECK(compactSeam.find("after==s_before") != std::string::npos);
    CHECK(compactSeam.find("s_unsteppedRun") != std::string::npos);

    CHECK(compactSeam.find("capture.selectorProof=s_charSelectProof;")
          != std::string::npos);
    CHECK(compactSeam.find("if(liveScene==3u){s_charSelectProof=")
          != std::string::npos);
    CHECK(compactSeam.find("s_proofBefore") == std::string::npos);

    CHECK(compactSeam.find(
              "s_pendingCapture.launchSourceFrame=s_sourceFrame;")
          != std::string::npos);
    CHECK(compactSeam.find("capture.launchSourceFrame=s_sourceFrame-1u;")
          == std::string::npos);
    CHECK(compactSeam.find(
              "capture.layoutGeneration=kOfflineLayoutGeneration;")
          != std::string::npos);
    CHECK(compactSeam.find("capture.layoutGeneration=0u;")
          == std::string::npos);
    CHECK(withoutWhitespace(hooks).find(
              "constexpruint32_tkOfflineLayoutGeneration=1u;")
          != std::string::npos);
    CHECK(compactSeam.find("netReplayLifecycleV2(kOfflineLayoutGeneration)")
          != std::string::npos);
    CHECK(compactSeam.find("netReplayLifecycleV2(0u)") == std::string::npos);
    const std::string runtimeV2 = readWorkspaceSource(
        "src/game/replay/ReplayRuntime.cpp");
    CHECK(!runtimeV2.empty());
    CHECK(withoutWhitespace(runtimeV2).find(
              "if(capture.layoutGeneration==0u")
          != std::string::npos);

    const std::string armed = sourceSlice(
        hooks, "bool netRecorderArmed() {", "\n}\n");
    CHECK(!armed.empty());
    CHECK(withoutWhitespace(armed).find(
              "&&!qgame::replay::replayRecorderV2Active()")
          != std::string::npos);
}

TEST(TrainingSaveStatesActOnTheSelectedSlotInBothMenus) {
    const std::string runtime = readWorkspaceSource(
        "src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());
    const std::string compactRuntime = withoutWhitespace(runtime);
    const std::string saveCase = sourceSlice(
        runtime, "case TrainingCommandKind::SaveState: {",
        "case TrainingCommandKind::LoadState:");
    CHECK(!saveCase.empty());
    CHECK(withoutWhitespace(saveCase).find(
              "trainingSaveState(command.slotOrSetting)")
          != std::string::npos);
    const std::string loadCase = sourceSlice(
        runtime, "case TrainingCommandKind::LoadState: {",
        "case TrainingCommandKind::ClearState:");
    CHECK(!loadCase.empty());
    CHECK(withoutWhitespace(loadCase).find(
              "trainingLoadState(command.slotOrSetting)")
          != std::string::npos);
    CHECK(compactRuntime.find(
              "caseTrainingCommandKind::SelectStateSlot:")
          != std::string::npos);

    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string native = sourceSlice(
        wrapper, "void NativeMenuBuildRecording(",
        "\nconst char *const kNativeHitboxStyleNames[");
    CHECK(!native.empty());
    const std::string compactNative = withoutWhitespace(native);
    CHECK(compactNative.find(
              "constDWORDstateSlot=(std::min)(status.selectedStateSlot,")
          != std::string::npos);
    for (const char* command : {"QOH99_TRAINING_COMMAND_SAVE_STATE",
                                "QOH99_TRAINING_COMMAND_LOAD_STATE",
                                "QOH99_TRAINING_COMMAND_CLEAR_STATE"}) {
        const std::string sends =
            std::string(command) + "," + "stateSlot,0,";
        CHECK(compactNative.find(sends) != std::string::npos);
        CHECK(compactNative.find(std::string(command) + ",0,0,")
              == std::string::npos);
    }

    CHECK(compactNative.find("QOH99_TRAINING_COMMAND_CLEAR_MACRO,macroSlot,0,")
          != std::string::npos);
    CHECK(compactNative.find("QOH99_TRAINING_COMMAND_CLEAR_MACRO,0,0,")
          == std::string::npos);

    CHECK(compactNative.find("QOH99_TRAINING_COMMAND_TOGGLE_RECORD,0,0,")
          != std::string::npos);

    CHECK(withoutWhitespace(wrapper).find(
              "EnqueueTrainingCommand(status,"
              "QOH99_TRAINING_COMMAND_SAVE_STATE,slot,0);")
          != std::string::npos);
}

TEST(CustomPaletteLifetimesSurviveRematchAndPlaybackBoot) {
    const std::string hooks =
        readWorkspaceSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    CHECK(hooks.find("replayRecordingActive()") != std::string::npos);

    const std::string worker = readWorkspaceSource(
        "src/game/replay/ReplayIpcWorker.cpp");
    CHECK(!worker.empty());
    CHECK(worker.find("paletteHoldFromReplay") == std::string::npos);
    CHECK(worker.find("paletteClearReplayHold") == std::string::npos);
    CHECK(worker.find("prebuffer->palettes = s.playbackPalettesV3;") != std::string::npos);
    CHECK(worker.find("paletteSubmitToWrapper") == std::string::npos);
    const std::string runtime = readWorkspaceSource(
        "src/game/replay/ReplayRuntime.cpp");
    const std::string install = sourceSlice(runtime, "bool installPlaybackPrebufferV3(", "\n}");
    CHECK(install.find("paletteClearReplayHold") < install.find("paletteHoldFromReplay"));
    CHECK(install.find("paletteHoldFromReplay") != std::string::npos);
    const std::string bridge =
        readWorkspaceSource("src/game/PaletteBridge.h");
    CHECK(bridge.find("paletteSubmitToWrapper") != std::string::npos);
    CHECK(hooks.find("paletteSubmitToWrapper") != std::string::npos);

    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");


    CHECK(wrapper.find("re-applied the custom palette after the")
          != std::string::npos);
}

TEST(PaletteEditorPointerRulesTabOwnershipAndMirrorUnlock) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());

    CHECK(wrapper.find("void PaletteStudioTouch") != std::string::npos);
    CHECK(wrapper.find("PaletteStudioTouch(st.slot)") != std::string::npos);
    CHECK(wrapper.find("PalettePublishSlot(st.slot)") == std::string::npos);
    CHECK(wrapper.find("PalettePublishSlot(g_paletteStudio.slot)")
          == std::string::npos);

    const std::size_t toggleAt = wrapper.find("void PaletteAutoToggle()");
    CHECK(toggleAt != std::string::npos);
    const std::string toggle = wrapper.substr(toggleAt, 1200);
    CHECK(toggle.find("g_paletteLocalSlotMask != 0xFu") != std::string::npos);
    CHECK(toggle.find("continue;") != std::string::npos);

    CHECK(wrapper.find("kQohColourStepSig") != std::string::npos);
    CHECK(wrapper.find("armedVariant >= 0) continue;") == std::string::npos);
    CHECK(wrapper.find("if (PaletteArmedSeatWearsCustom(seat)) continue;")
          != std::string::npos);
    CHECK(wrapper.find("static bool PaletteVariantFileExists(") != std::string::npos);
    CHECK(wrapper.find("if (g_paletteLocalSlotMask != 0xFu) return true;")
          != std::string::npos);
    CHECK(wrapper.find("there is no %s_%d.pal in the palettes folder")
          != std::string::npos);
    CHECK(wrapper.find("is not the build this hook was verified on")
          != std::string::npos);

    CHECK(wrapper.find("&& !st.pickerOpen;") != std::string::npos);
    CHECK(wrapper.find("if (action) return 1;") != std::string::npos);

    CHECK(wrapper.find("bool PaletteStudioUndo") != std::string::npos);
    CHECK(wrapper.find("bool PaletteStudioRedo") != std::string::npos);
    CHECK(wrapper.find("kPaletteUndoBurstMs") != std::string::npos);
    CHECK(wrapper.find("PaletteUndoExternalSync") != std::string::npos);
    CHECK(wrapper.find("PaletteUndoRecord(slot);\n    PalettePublishSlot(slot);")
          != std::string::npos);

    CHECK(wrapper.find("NativeMenuPaletteSpriteRefresh") != std::string::npos);
    CHECK(wrapper.find("PaletteSpriteTransform") != std::string::npos);
    CHECK(wrapper.find("if (pix == 0) continue;") != std::string::npos);
    CHECK(wrapper.find("NativeMenuPalettePreviewAnchor") == std::string::npos);

    CHECK(wrapper.find("PaletteSaveVariantAs") == std::string::npos);
    CHECK(wrapper.find("fileVariant") == std::string::npos);
    const std::string hooks =
        readWorkspaceSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    CHECK(hooks.find("UPDATING (live edit)") == std::string::npos);
    CHECK(hooks.find("if (g_netPaletteAppliedEpoch == g_netEpoch) continue;")
          != std::string::npos);
}

TEST(SpectatorChainedSetEventsAreProducerFolded) {
    const std::string publisher = readWorkspaceSource(
        "src/game/spectator/SpectatorPublisher.cpp");
    CHECK(!publisher.empty());
    const std::size_t fold =
        publisher.find("bool publishChainedSetEvent");
    CHECK(fold != std::string::npos);
    const std::size_t foldEnd = publisher.find("\n}", fold);
    CHECK(foldEnd != std::string::npos);
    const std::string body = publisher.substr(fold, foldEnd - fold);
    const std::size_t space = body.find("kChainedEventSlots) return false");
    const std::size_t digest = body.find("advanceSetEventDigest");
    CHECK(space != std::string::npos);
    CHECK(digest != std::string::npos);
    CHECK(space < digest);
    CHECK(publisher.find("SidecarMessageKind::ChainedSetEvent")
          != std::string::npos);
    CHECK(publisher.find("event.recordCursor <= frontier")
          != std::string::npos);

    const std::string sidecar = readWorkspaceSource(
        "src/client/spectator/SpectatorSidecar.cpp");
    CHECK(!sidecar.empty());
    CHECK(sidecar.find("drainHeldSetEvents") != std::string::npos);
    CHECK(sidecar.find("front.recordCursor > _committedRecords")
          != std::string::npos);
    std::size_t bounds = 0u;
    std::size_t at = 0u;
    for (;;) {
        at = sidecar.find("PublicationWireKind::ChainedSetEvent)", at);
        if (at == std::string::npos) break;
        ++bounds;
        ++at;
    }
    CHECK(bounds >= 2u);
    CHECK(sidecar.find("bridgeSetEventToArchiveV3") == std::string::npos);

    const std::string hooks =
        readWorkspaceSource("src/GameHooks.cpp");
    CHECK(hooks.find("publishChainedSetEvent") != std::string::npos);
    CHECK(hooks.find("hold.matchId != currentMatch") != std::string::npos);
    CHECK(hooks.find("playback < hold.recordCursor") != std::string::npos);
}

TEST(NativeTrainingMenuViewPageOffersEveryNativeHudGroup) {
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string hud = sourceSlice(
        wrapper, "static const NativeHudRow kHudRows[] = {", "};");
    CHECK(!hud.empty());

    static const char *const kGroups[] = {
        "QOH99_TRAINING_HUD_GLOBAL_BOTTOM_ELAPSED_TIME",
        "QOH99_TRAINING_HUD_GLOBAL_TOP_ROUND_TIMER",
        "QOH99_TRAINING_HUD_GLOBAL_FPS",
        "QOH99_TRAINING_HUD_PLAYER_SCORE",
        "QOH99_TRAINING_HUD_PLAYER_PORTRAIT_LIFE",
        "QOH99_TRAINING_HUD_PLAYER_NAME",
        "QOH99_TRAINING_HUD_PLAYER_ROUND_WINS",
        "QOH99_TRAINING_HUD_PLAYER_EX",
        "QOH99_TRAINING_HUD_PLAYER_COMBO",
    };
    for (const char *group : kGroups) {
        CHECK(hud.find(group) != std::string::npos);
    }
    CHECK(hud.find("{\"P1 name\"") != std::string::npos);
    CHECK(hud.find("{\"P2 name\"") != std::string::npos);
    CHECK(hud.find("{\"P1 combo readout\"") != std::string::npos);
    CHECK(hud.find("{\"P2 combo readout\"") != std::string::npos);

    CHECK(hud.find("Elapsed time (bottom)") != std::string::npos);
    CHECK(hud.find("Round timer (top)") != std::string::npos);

    CHECK(wrapper.find("Guard gauges (both)") != std::string::npos);
    const std::string commit = sourceSlice(
        wrapper, "if (row.slot == kNativeHudGuardRowTag)", "return;");
    CHECK(!commit.empty());
    CHECK(commit.find("side < 2u") != std::string::npos);
    CHECK(commit.find("QOH99_TRAINING_COMMAND_SET_NATIVE_HUD_MASK")
          != std::string::npos);
    CHECK(commit.find("const DWORD wanted =") != std::string::npos);
}

TEST(TrainingRoundCharacterSnapshotCapturesActorsAndMutableChildren) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());
    const std::string compact = withoutWhitespace(inl);

    CHECK(inl.find("struct TrainingRoundCharacterRegion")
          != std::string::npos);
    CHECK(inl.find("struct TrainingRoundCharacterState")
          != std::string::npos);
    CHECK(inl.find(
              "static TrainingRoundCharacterState "
              "g_trainingRoundCharacterState{};")
          != std::string::npos);
    CHECK(compact.find("kTrainingRoundCharacterActorCapacity=4u")
          != std::string::npos);
    CHECK(compact.find("actorBytes[kTrainingRoundCharacterActorCapacity]"
                       "[addr::kFighterSize]")
          != std::string::npos);
    CHECK(compact.find("kTrainingRoundCharacterSidecarBase=0x625680u")
          != std::string::npos);
    CHECK(compact.find("kTrainingRoundCharacterSidecarSize=0x3D0u")
          != std::string::npos);
    CHECK(compact.find(
              "sidecarBytes[qtraining::kTrainingRefillPlayerCount]"
              "[kTrainingRoundCharacterSidecarSize]")
          != std::string::npos);

    const std::string ready = withoutWhitespace(sourceSlice(
        inl, "static bool trainingRoundCharacterCaptureReady() {", "\n}"));
    CHECK(!ready.empty());
    CHECK(ready.find("trainingFighter(player,false)") != std::string::npos);
    CHECK(ready.find("kTrainingFighterActionStateOffset)!=0u")
          != std::string::npos);
    CHECK(ready.find("kTrainingFighterDispatchActionOffset")
          == std::string::npos);
    CHECK(ready.find("trainingExactIdleAction") == std::string::npos);

    const std::string capture = sourceSlice(
        inl, "static bool trainingCaptureRoundCharacterState() {", "\n}");
    CHECK(!capture.empty());
    CHECK(capture.find("trainingFighter(player, false)")
          != std::string::npos);
    CHECK(capture.find("trainingFighter(player, true)")
          != std::string::npos);
    CHECK(capture.find("trainingCaptureRoundCharacterActor(active)")
          != std::string::npos);
    CHECK(capture.find("trainingCaptureRoundCharacterActor(alternate)")
          != std::string::npos);
    CHECK(capture.find("kTrainingRoundCharacterSidecarBase")
          != std::string::npos);
    CHECK(capture.find(
              "g_trainingRoundCharacterState.sidecarBytes[player]")
          != std::string::npos);

    const std::string actor = sourceSlice(
        inl, "static bool trainingCaptureRoundCharacterActor(uintptr_t actor) {",
        "\n}");
    CHECK(!actor.empty());
    CHECK(actor.find("addr::kFighterSize") != std::string::npos);
    CHECK(actor.find("kTrainingFighterCommandChildOffset")
          != std::string::npos);
    CHECK(actor.find("kTrainingRoundCharacterCommandSize")
          != std::string::npos);
    CHECK(actor.find("kTrainingCommandDirectionKillOffset")
          != std::string::npos);
    CHECK(actor.find("std::memset(saved.bytes.data() + "
                     "kTrainingCommandDirectionKillOffset")
          != std::string::npos);
    CHECK(actor.find("kTrainingFighterFrameTableOffset")
          != std::string::npos);
    CHECK(actor.find("kTrainingFighterFrameCountOffset")
          != std::string::npos);
    CHECK(actor.find("static_cast<size_t>(frameCount) << 6u")
          != std::string::npos);

    const std::string region = sourceSlice(
        inl, "static bool trainingCaptureRoundCharacterRegion(",
        "\nstatic bool trainingCaptureRoundCharacterActor(");
    CHECK(!region.empty());
    CHECK(region.find("saved.address == address") != std::string::npos);
    CHECK(region.find("saved.bytes.size() == size") != std::string::npos);
    CHECK(region.find("std::memcpy(") != std::string::npos);

    const std::string restorable = sourceSlice(
        inl, "static bool trainingRoundCharacterStateRestorable() {", "\n}");
    const std::string restoreActors = sourceSlice(
        inl, "static void trainingRestoreRoundCharacterActors() {", "\n}");
    const std::string restoreRegions = sourceSlice(
        inl, "static void trainingRestoreRoundCharacterRegions() {", "\n}");
    CHECK(!restorable.empty());
    CHECK(!restoreActors.empty());
    CHECK(!restoreRegions.empty());
    CHECK(restoreRegions.find("kTrainingRoundCharacterSidecarBase")
          != std::string::npos);
    CHECK(restoreRegions.find("saved.sidecarBytes[player]")
          != std::string::npos);
    for (const char* resource : {"kBattleLife", "trainingExRecordAddress(",
                                 "trainingGuardGaugeAddress("}) {
        CHECK(capture.find(resource) == std::string::npos);
        CHECK(restorable.find(resource) == std::string::npos);
        CHECK(restoreActors.find(resource) == std::string::npos);
        CHECK(restoreRegions.find(resource) == std::string::npos);
    }
}

TEST(ReversalScheduleIsSeparableAndCorrectsItselfPerMove) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());

    CHECK(inl.find("static_cast<int64_t>(commitIndex) + 1") != std::string::npos);
    CHECK(inl.find("+ kTrainingChainLastTickIsActionable + lead")
          != std::string::npos);
    CHECK(inl.find("trainingReversalLead(") != std::string::npos);

    CHECK(inl.find("constexpr int32_t kTrainingChainLastTickIsActionable")
          != std::string::npos);
    CHECK_EQ(countOccurrences(inl, "kTrainingChainLastTickIsActionable"),
             std::size_t{6});
    CHECK(inl.find("== 1 + kTrainingChainLastTickIsActionable")
          != std::string::npos);

    const std::string slot = sourceSlice(
        inl, "static TrainingReversalLeadSlot* trainingReversalLeadSlot(",
        "\n}");
    CHECK(!slot.empty());
    CHECK(slot.find("slot.character == character") != std::string::npos);
    CHECK(slot.find("slot.moveIndex == moveIndex") != std::string::npos);

    const std::string observe = sourceSlice(
        inl, "static void trainingReversalLeadObserve(", "\n}");
    CHECK(!observe.empty());
    CHECK(observe.find("kTrainingReversalLeadMax") != std::string::npos);
    CHECK(inl.find("constexpr int32_t kTrainingReversalLeadMax")
          != std::string::npos);
    CHECK(observe.find("if (delta == 0)") != std::string::npos);
    CHECK(observe.find("trainingReversalLeadSlot(character, moveIndex, false)")
          != std::string::npos);

    CHECK(inl.find("watch.prebuffered") != std::string::npos);
    CHECK(inl.find("g_trainingWakeupReversal.predictedStartStep != 0u")
          != std::string::npos);
    CHECK(inl.find("dummy.action == watch.expectedAction")
          != std::string::npos);

    CHECK(inl.find("predictedStart,predictedAction") != std::string::npos);
    CHECK(inl.find("reversal_trace.csv") != std::string::npos);
    CHECK(inl.find("kTrainingCommandDirDwellOffset") != std::string::npos);
    CHECK(inl.find("kTrainingCommandButtonCountOffset") != std::string::npos);
    CHECK(inl.find("kTrainingReversalTraceRowCap") != std::string::npos);
    const std::string row = sourceSlice(
        inl, "static void trainingReversalTraceRow(const char* phase) {",
        "\n}");
    CHECK(!row.empty());
    CHECK(row.find("wr8(") == std::string::npos);
    CHECK(row.find("wr32(") == std::string::npos);
}

TEST(TrainingRoundCharacterSnapshotOwnsRestoreAndLifecycleBoundaries) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());

    const std::string restorable = sourceSlice(
        inl, "static bool trainingRoundCharacterStateRestorable() {", "\n}");
    CHECK(!restorable.empty());
    CHECK(restorable.find("saved.generation != g_trainingGeneration")
          != std::string::npos);
    CHECK(restorable.find("originalOrder") != std::string::npos);
    CHECK(restorable.find("swappedOrder") != std::string::npos);

    const std::string restoreActors = sourceSlice(
        inl, "static void trainingRestoreRoundCharacterActors() {", "\n}");
    CHECK(!restoreActors.empty());
    const std::size_t activeTable =
        restoreActors.find("wr32(addr::kFighterPtrTable");
    const std::size_t backupTable =
        restoreActors.find("wr32(addr::kFighterBackupTable");
    const std::size_t actors =
        restoreActors.find("saved.actorBytes[index]");
    CHECK(activeTable != std::string::npos);
    CHECK(backupTable != std::string::npos);
    CHECK(actors != std::string::npos);
    CHECK(activeTable < actors);
    CHECK(backupTable < actors);
    const std::string restoreRegions = sourceSlice(
        inl, "static void trainingRestoreRoundCharacterRegions() {", "\n}");
    CHECK(!restoreRegions.empty());
    CHECK(restoreRegions.find("region.bytes.data()")
          != std::string::npos);
    CHECK(restoreRegions.find("saved.sidecarBytes[player]")
          != std::string::npos);

    const std::string admission = sourceSlice(
        inl, "static int trainingAdmitAfterNativeGate(int nativeResult) {",
        "\nstatic int __cdecl hkTrainingStepGate()");
    CHECK(!admission.empty());
    const std::size_t baseline =
        admission.find("trainingCaptureBaseline()");
    const std::size_t invariants =
        admission.find("trainingInvariantsHold()");
    const std::size_t capture =
        admission.find("trainingCaptureRoundCharacterState()");
    const std::size_t actionPoll =
        admission.find("++g_trainingPollSerial");
    CHECK(baseline != std::string::npos);
    CHECK(invariants != std::string::npos);
    CHECK(capture != std::string::npos);
    CHECK(actionPoll != std::string::npos);
    CHECK(baseline < capture);
    CHECK(invariants < capture);
    CHECK(capture < actionPoll);
    CHECK(admission.find(
              "const bool characterCapturePending =")
          != std::string::npos);
    CHECK(admission.find(
              "g_trainingOperationNeutral || characterCapturePending")
          != std::string::npos);
    CHECK(admission.find(
              "g_trainingOperationNeutral = characterCapturePending;")
          != std::string::npos);

    const std::string generation = sourceSlice(
        inl, "static void trainingBeginNewGeneration() {",
        "\nstatic bool trainingStateProbeEnabled(");
    CHECK(!generation.empty());
    CHECK(generation.find("trainingInvalidateRoundCharacterState();")
          != std::string::npos);
    const std::string outer = sourceSlice(
        inl, "static void trainingOuterTick() {",
        "\nstatic bool trainingInCharacterSelectFrontend()");
    CHECK(!outer.empty());
    const std::size_t roundExit = outer.find(
        "g_trainingLastScene == 2u && g_trainingLastMatchSub == 0u");
    const std::size_t invalidate =
        outer.find("trainingInvalidateRoundCharacterState();", roundExit);
    const std::size_t baselineReset =
        outer.find("g_trainingBaseline = TrainingInvariantBaseline{};",
                   roundExit);
    CHECK(roundExit != std::string::npos);
    CHECK(invalidate != std::string::npos);
    CHECK(baselineReset != std::string::npos);
    CHECK(invalidate < baselineReset);
    const std::size_t armStart =
        inl.find("static void trainingArmInternal(bool enabled) {");
    CHECK(armStart != std::string::npos);
    CHECK(inl.find("trainingInvalidateRoundCharacterState();", armStart)
          != std::string::npos);

    for (const char* retired : {"trainingForceFighterIdle(",
                                "trainingRequestBothFightersIdle(",
                                "trainingBreakThrowLink(",
                                "trainingClearFighterMotion(",
                                "trainingAdvanceBothFightersIdleEnforcement("}) {
        CHECK(inl.find(retired) == std::string::npos);
    }
}

TEST(TrainingResetDespawnsTransientActorsBeforeRestoringCharacters) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());

    const std::string despawn = sourceSlice(
        inl, "static uint32_t trainingDespawnTransientActors() {", "\n}");
    CHECK(!despawn.empty());
    CHECK(despawn.find("unlink(reinterpret_cast<void*>(actor));")
          != std::string::npos);
    CHECK(despawn.find("wr8(actor + kTrainingFighterCharacterOffset, 0xFFu);")
          != std::string::npos);
    CHECK(despawn.find("unlink(reinterpret_cast<void*>(actor));")
          < despawn.find(
                "wr8(actor + kTrainingFighterCharacterOffset, 0xFFu);"));
    CHECK(despawn.find("addr::kEffectPool") != std::string::npos);
    CHECK(despawn.find("addr::kActorClonePool") != std::string::npos);
    CHECK(despawn.find("kHelperCloneP1") == std::string::npos);
    CHECK(despawn.find("kHelperCloneP2") == std::string::npos);
    CHECK(despawn.find("trainingDespawnCharacterAuxActors()")
          != std::string::npos);
    const std::string aux = sourceSlice(
        inl, "static uint32_t trainingDespawnCharacterAuxActors() {",
        "\nstatic uint32_t trainingDespawnTransientActors()");
    CHECK(!aux.empty());
    CHECK(inl.find("kTrainingCharacterAuxActorBase = 0x8737CCu")
          != std::string::npos);
    CHECK(inl.find("kTrainingCharacterAuxActorCount = 25u")
          != std::string::npos);
    CHECK(inl.find("kTrainingKaedeAuxActorCharacter = 34u")
          != std::string::npos);
    CHECK(inl.find("kTrainingKaedeAuxActorAction = 120u")
          != std::string::npos);
    CHECK(inl.find("kTrainingObj1AuxActorCharacter = 100u")
          != std::string::npos);
    CHECK(inl.find("kTrainingObj1AuxActorAction = 50u")
          != std::string::npos);
    CHECK(aux.find("kTrainingFighterCharacterOffset")
          != std::string::npos);
    CHECK(aux.find("kTrainingFighterActionStateOffset")
          != std::string::npos);
    CHECK(aux.find("wr8(actor + kTrainingFighterCharacterOffset, 0xFFu);")
          != std::string::npos);
    CHECK(aux.find("kTrainingActorUnlinkAddr") == std::string::npos);
    CHECK(aux.find("unlink(") == std::string::npos);
    const std::string seams = sourceSlice(
        inl, "static bool trainingVerifyNativeSeams() {",
        "\nstatic bool trainingVerifyGuardScanners()");
    CHECK(!seams.empty());
    CHECK(seams.find("kActorUnlinkSig") != std::string::npos);
    CHECK(seams.find("kTrainingActorUnlinkAddr") != std::string::npos);
    CHECK(seams.find("se::verifyCodeBytes(") != std::string::npos);
    CHECK(despawn.find("g_trainingActorUnlinkQualified")
          == std::string::npos);
    CHECK(despawn.find("rangeReadable(") == std::string::npos);
    CHECK(inl.find("trainingQualifyActorUnlink") == std::string::npos);
    CHECK(inl.find("kTrainingActorUnlinkAddr = 0x42DB00u")
          != std::string::npos);

    const std::string reset = sourceSlice(
        inl, "static bool trainingResetPairToPreset(PositionPreset preset) {",
        "\n}");
    CHECK(!reset.empty());
    const std::size_t despawnCall =
        reset.find("trainingDespawnTransientActors()");
    const std::size_t restoreCall =
        reset.find("trainingRestoreRoundCharacterActors()");
    CHECK(despawnCall != std::string::npos);
    CHECK(restoreCall != std::string::npos);
    CHECK(despawnCall < restoreCall);
    CHECK(inl.find("trainingClearFighterMotion(") == std::string::npos);
    CHECK(inl.find("trainingBreakThrowLink(") == std::string::npos);
}

TEST(ReplayHitboxesReuseTheRenderPhaseSnapshotWhilePaused) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    const std::string publish = sourceSlice(
        inl, "static void trainingPublishHitboxSnapshot(bool fromRenderPhase) {",
        "\nstatic void trainingTryRegisterHitboxOverlay(");
    const std::string toggle = sourceSlice(
        inl, "static void replaySetCustomHitboxes(bool enabled) {", "\n}");
    const std::string release = sourceSlice(
        inl, "static void replayReleaseCustomHitboxes() {", "\n}");
    CHECK(!toggle.empty());
    CHECK(!release.empty());
    CHECK(publish.find("PresentationDisposition::SuppressHistorical")
          < publish.find("trainingReadHitboxCamera(camera)"));
    CHECK(publish.find("g_replayHitboxSnapshot = snapshot;")
          != std::string::npos);
    CHECK(publish.find("snapshot.visible = g_replayHitboxesEnabled ? 1u : 0u;")
          != std::string::npos);
    CHECK(toggle.find("wr8(kTrainingHitboxAddr, 0u)") != std::string::npos);
    CHECK(toggle.find("trainingHitboxes().publish(g_replayHitboxSnapshot)")
          != std::string::npos);
    CHECK(toggle.find("trainingReadActorBoxes") == std::string::npos);
    CHECK(release.find("g_replayHitboxSnapshotValid = false;")
          != std::string::npos);
    CHECK(release.find("hitboxReleaseOverlay()") != std::string::npos);
}

TEST(ReplayDisplayFlagsKeepCaptureDefaultsSeparateFromTraining) {
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_DISPLAY_INPUTS), qipc::kReplayShowInputs);
    CHECK_EQ(static_cast<unsigned>(QOH99_REPLAY_DISPLAY_TRANSPORT), qipc::kReplayShowTransport);
    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    const std::string def = readWorkspaceSource("ddraw_wrapper/ddraw.def");
    const std::string set = sourceSlice(
        wrapper, "extern \"C\" void __stdcall EFZDX9_SetReplayDisplayFlags(",
        "\n}");
    const std::string query = sourceSlice(
        wrapper, "extern \"C\" DWORD __stdcall EFZDX9_QueryReplayDisplayFlags(",
        "\n}");
    const std::string draw = sourceSlice(
        wrapper, "void DrawReplayTransport(", "\nvoid DrawTrainingFrameAdvantageOverlay(");
    CHECK(!set.empty());
    CHECK(!query.empty());
    CHECK(wrapper.find("bool g_inputDisplayVisible = true;") != std::string::npos);
    CHECK(set.find("QOH99_REPLAY_DISPLAY_INPUTS") != std::string::npos);
    CHECK(set.find("QOH99_REPLAY_DISPLAY_TRANSPORT") != std::string::npos);
    CHECK(query.find("g_inputDisplayVisible") != std::string::npos);
    CHECK(query.find("g_replayTransportVisible") != std::string::npos);
    CHECK(draw.find("if (!g_replayTransportVisible)") != std::string::npos);
    CHECK(wrapper.find("Show replay frame and speed##replay_transport_visible")
          != std::string::npos);
    CHECK(def.find("EFZDX9_SetReplayDisplayFlags @28") != std::string::npos);
    CHECK(def.find("EFZDX9_QueryReplayDisplayFlags @29") != std::string::npos);
}

TEST(TrainingHitboxOverlayReadsAtTheGamesOwnDrawPhase) {
    const std::string inl =
        readQohSource("src/training/TrainingGameIntegration.inl");
    CHECK(!inl.empty());

    const std::string publish = sourceSlice(
        inl, "static void trainingPublishHitboxSnapshot(bool fromRenderPhase) {",
        "\nstatic void trainingTryRegisterHitboxOverlay(");
    CHECK(!publish.empty());

    CHECK(publish.find("trainingHitboxes().publish(snapshot)")
          != std::string::npos);
    CHECK(inl.find("g_trainingHitboxHeld") == std::string::npos);

    CHECK(inl.find("kTrainingPreBoxRenderFn = 0x419C70u") != std::string::npos);
    CHECK(inl.find("static int hkTrainingPreBoxRender()") != std::string::npos);
    const std::string detour = sourceSlice(
        inl, "static int hkTrainingPreBoxRender() {", "\n}");
    CHECK(!detour.empty());
    CHECK(detour.find("g_trainingOrigPreBoxRender()")
          < detour.find("trainingPublishHitboxSnapshot(true)"));

    CHECK(publish.find(
              "if (g_trainingHitboxRenderPhaseHooked && !fromRenderPhase) "
              "return;") != std::string::npos);

    CHECK(publish.find("publishHidden()") < publish.find(
              "if (g_trainingHitboxRenderPhaseHooked && !fromRenderPhase)"));

    CHECK(inl.find("static void trainingInstallRenderPhaseHook()")
          != std::string::npos);
    const std::string install = sourceSlice(
        inl, "static void trainingInstallRenderPhaseHook() {", "\n}");
    CHECK(!install.empty());
    CHECK(install.find("se::verifyCodeBytes(kTrainingPreBoxRenderFn")
          != std::string::npos);
    CHECK(install.find("falls back to the outer tick") != std::string::npos);

    CHECK(publish.find("if (!fromRenderPhase) {") != std::string::npos);
    CHECK(install.find("trainingHitboxDropHeldFrame();") != std::string::npos);
}

TEST(SpectatorSetSeekCheckpointIsTheCanonicalSlotAtItsPublication) {
    const std::string hooks =
        readWorkspaceSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());

    const std::size_t captureFn =
        hooks.find("static void spectatorSetCheckpointCaptureAtPublish");
    CHECK(captureFn != std::string::npos);
    const std::size_t captureBody = hooks.find("copySlotTo", captureFn);
    CHECK(captureBody != std::string::npos);
    const std::size_t captureCall =
        hooks.find("spectatorSetCheckpointCaptureAtPublish(\n"
                   "                    confirmed.frame, meta);");
    CHECK(captureCall != std::string::npos);
    const std::size_t publishAfter =
        hooks.find("publishConfirmed(sidecar)", captureCall);
    CHECK(publishAfter != std::string::npos);
    CHECK(hooks.find("meta.spectatorPreHash,\n"
                     "            bootDescriptor") != std::string::npos);

    CHECK(hooks.find("g_rb.loadFromForeign(state->bytes.data())")
          != std::string::npos);
    CHECK(hooks.find("netRestoreCheckpointAudioRegion")
          != std::string::npos);
    const std::size_t verify = hooks.find(
        "restoredHash != g_spectatorSkipCheckpoint.stateHash");
    CHECK(verify != std::string::npos);
    const std::size_t rebind = hooks.find(
        "spectatorViewerResumeAtCheckpoint", verify);
    CHECK(rebind != std::string::npos);
    const std::size_t transients = hooks.find(
        "g_netBootstrapTransientsEpoch = g_netEpoch;", verify);
    CHECK(transients != std::string::npos);

    const std::string rollback =
        readWorkspaceSource("src/RollbackManager.cpp");
    CHECK(!rollback.empty());
    CHECK(rollback.find("bool RollbackManager::loadFromForeign")
          != std::string::npos);
    CHECK(rollback.find("descriptorRelocations") != std::string::npos);
    CHECK(rollback.find("inactiveOwnerSlot") != std::string::npos);
    CHECK(rollback.find("pr.ownerCaptured && _ptrExpectedPresent[i] != 0")
          != std::string::npos);

    const std::string ipc =
        readWorkspaceSource("src/ipc/IpcConfig.h");
    CHECK(!ipc.empty());
    CHECK(ipc.find("must not request a viewer checkpoint")
          != std::string::npos);
    const std::size_t disabledBranch =
        ipc.find("disabled client spectator publisher carries channel state");
    CHECK(disabledBranch != std::string::npos);
    const std::size_t reservedScan =
        ipc.rfind("const bool hasReserved", disabledBranch);
    CHECK(reservedScan != std::string::npos);
    CHECK(ipc.substr(reservedScan, disabledBranch - reservedScan)
              .find("spectatorWantsNewestCheckpoint")
          == std::string::npos);
}

TEST(SessionEventCommitAckNeverEchoesAndOnlyClearsItsOwnEvent) {
    const std::string core = readWorkspaceSource(
        "src/netplay/NetplayCore.cpp");
    CHECK(!core.empty());
    const std::size_t resend = core.find(
        "!= wire::SessionEventStage::CommitAck");
    CHECK(resend != std::string::npos);
    const std::size_t cadence = core.rfind("readyResendMs", resend);
    CHECK(cadence != std::string::npos);
    CHECK(resend - cadence < 200u);

    const std::string hooks =
        readWorkspaceSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    const std::size_t ackCase = hooks.find(
        "case SessionEventStage::CommitAck:");
    CHECK(ackCase != std::string::npos);
    const std::size_t guardedClear = hooks.find(
        "inbound.eventId == g_sessionEvents.lastAppliedId()", ackCase);
    const std::size_t nextCase = hooks.find("case SessionEventStage::",
                                            ackCase + 8u);
    CHECK(guardedClear != std::string::npos);
    CHECK(nextCase != std::string::npos);
    CHECK(guardedClear < nextCase);
    CHECK(hooks.find("quiesce id=%u kind=%u REFUSED") != std::string::npos);
}

TEST(SpectatorsFollowAnF1ReturnThroughTheEventChain) {
    const std::string hooks =
        readWorkspaceSource("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    const std::size_t helper =
        hooks.find("static bool spectatorSetEventPublishF1Return()");
    CHECK(helper != std::string::npos);
    const std::size_t resultFirst =
        hooks.find("SetEventKind::MatchResult", helper);
    const std::size_t returnAfter =
        hooks.find("SetEventKind::ReturnedToCharacterSelect", helper);
    CHECK(resultFirst != std::string::npos);
    CHECK(returnAfter != std::string::npos);
    CHECK(resultFirst < returnAfter);
    const std::size_t finalize =
        hooks.find("void netFinalizeCommittedCharacterSelectReturn()");
    CHECK(finalize != std::string::npos);
    const std::size_t guardedSeal = hooks.find(
        "if (!spectatorSetEventPublishF1Return())", finalize);
    CHECK(guardedSeal != std::string::npos);
    CHECK(hooks.find("record.record != g_spectatorReturnAtRecord")
          != std::string::npos);
    CHECK(hooks.find("F1 return executed at record") != std::string::npos);
    const std::size_t bind = hooks.find(
        "g_spectatorReturnScanCursor =\n"
        "                g_spectatorSkipCheckpoint.eventCursor;");
    CHECK(bind != std::string::npos);
}

TEST(TrainingStatusMessagesFadeEventsAndDeriveModes) {
    const std::string runtime = readWorkspaceSource(
        "src/training/TrainingGameIntegration.inl");
    CHECK(!runtime.empty());
    const std::string compactRuntime = withoutWhitespace(runtime);

    const std::size_t derive =
        compactRuntime.find("switch(g_trainingMacros.state()){");
    CHECK(derive != std::string::npos);
    CHECK(compactRuntime.find("status.statusModeKind", derive)
          != std::string::npos);

    for (const char *raise : {
             "qtraining::StatusEvent::PositionReset",
             "qtraining::StatusEvent::RoundReset",
             "qtraining::StatusEvent::StateSaved",
             "qtraining::StatusEvent::StateLoaded",
             "qtraining::StatusEvent::StateCleared",
             "qtraining::StatusEvent::StateSlotSelected",
             "qtraining::StatusEvent::MacroSlotSelected",
             "qtraining::StatusEvent::MacroCleared",
             "qtraining::StatusEvent::MacroRecorded",
             "qtraining::StatusEvent::MacroStopped"}) {
        CHECK(runtime.find(raise) != std::string::npos);
    }
    const char *const savedRaise =
        "trainingRaiseStatusEvent(qtraining::StatusEvent::StateSaved";
    std::size_t saidSaved = 0;
    for (std::size_t at = runtime.find(savedRaise);
         at != std::string::npos;
         at = runtime.find(savedRaise, at + 1)) {
        ++saidSaved;
    }
    CHECK(saidSaved >= 2u);

    const std::string macros = readWorkspaceSource(
        "src/training/TrainingMacros.cpp");
    CHECK(!macros.empty());
    const std::string compactMacros = withoutWhitespace(macros);
    CHECK(compactMacros.find("caseMacroState::PreRecord:") != std::string::npos);
    const std::string preRecord = sourceSlice(
        macros, "bool TrainingMacros::requestPreRecord()", "\n}\n");
    CHECK(!preRecord.empty());
    CHECK(withoutWhitespace(preRecord).find("state_!=MacroState::Idle")
          != std::string::npos);

    const std::string wrapper =
        readWorkspaceSource("ddraw_wrapper/render_dx9_imgui.cpp");
    CHECK(!wrapper.empty());
    const std::string overlay = sourceSlice(
        wrapper, "void DrawTrainingStatusMessages(", "\n}\n");
    CHECK(!overlay.empty());
    const std::string compactOverlay = withoutWhitespace(overlay);
    CHECK(compactOverlay.find("!TrainingHasStatusMessages(status)")
          != std::string::npos);
    CHECK(compactOverlay.find("eventAlpha") != std::string::npos);
    CHECK(compactOverlay.find("status.statusEventSequence!=lastSequence")
          != std::string::npos);
    CHECK(compactOverlay.find("modeAlpha") == std::string::npos);
    CHECK(wrapper.find("DrawTrainingFrameAdvantageOverlay(params);\n"
                       "        DrawTrainingStatusMessages(params);")
          != std::string::npos);
}
