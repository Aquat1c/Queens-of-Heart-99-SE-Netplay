#include "TestHarness.h"

#include <cstdio>
#include <string>

#include "input/InputCompiler.h"
#include "input/InputProfile.h"
#include "input/ModeControlPolicy.h"
#include "input/NativeBindingTable.h"

using namespace qinput;

QNP_TEST(ProfileDefaultsCompileWithNoControllerPresent) {
    const ControlsFile file = makeDefaultControlsFile();
    QNP_REQUIRE(!file.profiles.empty());
    const InputProfile& profile = file.profiles[0];
    QNP_CHECK(profile.sourceMode == CompiledSourceMode::Keyboard);
    QNP_CHECK(profile.device.guid.empty());

    const CompileResult compiled = compileInputProfile(profile, false, &file);
    QNP_CHECK(compiled.ok);
    QNP_CHECK(compiled.error.empty());
    QNP_CHECK(compiled.config.probeCount == 0u);

    for (uint16_t i = 0; i < compiled.modeControls.count; ++i) {
        QNP_CHECK(compiled.modeControls.bindings[i].binding.type
                  == CompiledBindingType::KeyboardScan);
    }
    QNP_CHECK(compiled.modeControls.count > 0u);

    QNP_CHECK(!file.modeControls[static_cast<size_t>(
        ModeControlAction::OpenOverlayMenuPad)].empty());
    InputProfile withDevice = profile;
    withDevice.device.guid = "0300000000000000deadbeef00000000";
    withDevice.mapping = "0300000000000000deadbeef00000000,Test Pad,a:b0,";
    const CompileResult padded =
        compileInputProfile(withDevice, false, &file);
    QNP_CHECK(padded.ok);
    QNP_CHECK(padded.config.probeCount == 1u);
    bool sawPad = false;
    for (uint16_t i = 0; i < padded.modeControls.count; ++i) {
        if (padded.modeControls.bindings[i].binding.type
            == CompiledBindingType::ProbeButton) {
            sawPad = true;
        }
    }
    QNP_CHECK(sawPad);
}

QNP_TEST(ProfileRoundTrip) {
    ControlsFile file{};
    file.activeProfileId = "default-keyboard";
    file.profiles.push_back(makeDefaultKeyboardProfile());
    const std::string text = serializeControlsFile(file);
    ControlsFile parsed{};
    std::string error;
    QNP_REQUIRE(parseControlsFile(text, parsed, error));
    QNP_CHECK(parsed.schemaVersion == kControlsSchemaVersion);
    QNP_CHECK(parsed.activeProfileId == "default-keyboard");
    QNP_REQUIRE(parsed.profiles.size() == 1);
    const InputProfile& profile = parsed.profiles[0];
    QNP_CHECK(profile.id == "default-keyboard");
    QNP_CHECK(profile.sourceMode == CompiledSourceMode::Keyboard);
    for (int action = 0; action < 8; ++action)
        QNP_CHECK(profile.actions[action].size() == 1);
    QNP_CHECK(serializeControlsFile(parsed) == text);
}

QNP_TEST(ProfileSchemaOneModeControlMigrationNeverGuessesAPlayer) {
    const std::string text =
        "{\"schema\":1,\"profiles\":["
        "{\"id\":\"p1\",\"name\":\"one\",\"source\":\"keyboard\","
        "\"sessionControls\":{\"Pause\":[{\"type\":\"KeyboardScan\",\"dik\":57}],"
        "\"Back\":[{\"type\":\"KeyboardScan\",\"dik\":14}],"
        "\"DelayDecrease\":[{\"type\":\"KeyboardScan\",\"dik\":12}]}},"
        "{\"id\":\"p2\",\"name\":\"two\",\"source\":\"keyboard\","
        "\"sessionControls\":{\"Pause\":[{\"type\":\"KeyboardScan\",\"dik\":58}],"
        "\"Back\":[{\"type\":\"KeyboardScan\",\"dik\":1}],"
        "\"DelayDecrease\":[{\"type\":\"KeyboardScan\",\"dik\":12}]}}]}";
    ControlsFile parsed{};
    std::string error;
    QNP_REQUIRE(parseControlsFile(text, parsed, error));
    QNP_CHECK(parsed.schemaVersion == 2u);
    QNP_CHECK(modeControlDescriptor(ModeControlAction::LeaveMatch) != nullptr);
    const auto& delay = parsed.modeControls[static_cast<size_t>(
        ModeControlAction::InputDelayDecrease)];
    QNP_REQUIRE(delay.size() == 1u);
    QNP_CHECK(delay[0].dik == 12u);
    QNP_CHECK(parsed.modeControls[static_cast<size_t>(
                  ModeControlAction::LeaveMatch)].empty());
    QNP_REQUIRE(parsed.pendingModeMigrations.size() == 1u);
    QNP_CHECK(parsed.pendingModeMigrations[0].action
              == ModeControlAction::LeaveMatch);
    QNP_CHECK(parsed.pendingModeMigrations[0].playerBindings[0][0].dik == 14u);
    QNP_CHECK(parsed.pendingModeMigrations[0].playerBindings[1][0].dik == 1u);
}

QNP_TEST(ProfileSchemaTwoExplicitEmptyModeControlsStayUnbound) {
    ControlsFile parsed{};
    std::string error;
    QNP_REQUIRE(parseControlsFile(
        "{\"schema\":2,\"profiles\":[],\"modeControls\":{"
        "\"togglePresentationMode\":[],\"trainingChargeExFraction\":[],"
        "\"replayTakeoverExit\":[]}}",
        parsed, error));
    QNP_CHECK(parsed.modeControls[static_cast<size_t>(
                  ModeControlAction::TogglePresentationMode)].empty());
    QNP_CHECK(parsed.modeControls[static_cast<size_t>(
                  ModeControlAction::TrainingChargeExFraction)].empty());
    QNP_CHECK(parsed.modeControls[static_cast<size_t>(
                  ModeControlAction::ReplayTakeoverExit)].empty());
    QNP_REQUIRE(parsed.modeControls[static_cast<size_t>(
                    ModeControlAction::ReplayTakeoverP1)].size() == 1u);
    QNP_CHECK(parsed.modeControls[static_cast<size_t>(
                  ModeControlAction::ReplayTakeoverP1)][0].dik == 0x21u);
    QNP_REQUIRE(parsed.modeControls[static_cast<size_t>(
                    ModeControlAction::ReplayToggleCollisionDisplay)].size()
                == 1u);
    QNP_CHECK(parsed.modeControls[static_cast<size_t>(
                  ModeControlAction::ReplayToggleCollisionDisplay)][0].dik
              == 0x41u);
    QNP_REQUIRE(parsed.modeControls[static_cast<size_t>(
                    ModeControlAction::OnlineReturnCharacterSelect)].size()
                == 1u);
    QNP_CHECK(parsed.modeControls[static_cast<size_t>(
                  ModeControlAction::OnlineReturnCharacterSelect)][0].dik
              == 0x3Bu);
    QNP_CHECK(parsed.pendingModeMigrations.empty());
}

QNP_TEST(ProfileAllBindingTokenTypes) {
    const std::string text =
        "{\"schema\":1,\"profiles\":[{\"id\":\"p\",\"name\":\"n\","
        "\"source\":\"keyboard+controller\","
        "\"device\":{\"guid\":\"g\",\"vendor\":1,\"product\":2,"
        "\"version\":3},"
        "\"actions\":{"
        "\"Up\":[{\"type\":\"KeyboardScan\",\"dik\":200},"
        "{\"type\":\"GamepadButton\",\"button\":11},"
        "{\"type\":\"GamepadAxis\",\"axis\":1,\"polarity\":\"negative\","
        "\"pressQ15\":16384,\"releaseQ15\":11469}],"
        "\"Down\":[{\"type\":\"RawJoystickHat\",\"hat\":0,"
        "\"direction\":\"Down\"}],"
        "\"Left\":[{\"type\":\"RawJoystickAxis\",\"axis\":0,"
        "\"polarity\":\"negative\"}],"
        "\"Right\":[{\"type\":\"RawJoystickButton\",\"button\":7}],"
        "\"A\":[{\"type\":\"KeyboardScan\",\"dik\":44}],"
        "\"B\":[{\"type\":\"KeyboardScan\",\"dik\":45}],"
        "\"C\":[{\"type\":\"KeyboardScan\",\"dik\":46}],"
        "\"D\":[{\"type\":\"KeyboardScan\",\"dik\":47}]},"
        "\"mapping\":\"m\"}]}";
    ControlsFile parsed{};
    std::string error;
    QNP_REQUIRE(parseControlsFile(text, parsed, error));
    const InputProfile& profile = parsed.profiles[0];
    QNP_CHECK(profile.actions[0].size() == 3);
    QNP_CHECK(profile.actions[0][1].probeKind
              == ProbeKind::GamepadButton);
    QNP_CHECK(profile.actions[1][0].hatBit == kHatDown);
    QNP_CHECK(profile.actions[2][0].polarity == AxisPolarity::Negative);
}

QNP_TEST(ProfileRejectsMalformedInput) {
    ControlsFile parsed{};
    std::string error;
    QNP_CHECK(!parseControlsFile(
        "{\"schema\":1,\"bogus\":1,\"profiles\":[]}", parsed, error));
    QNP_CHECK(!parseControlsFile(
        "{\"schema\":1,\"schema\":1,\"profiles\":[]}", parsed, error));
    QNP_CHECK(!parseControlsFile(
        "{\"schema\":1.5,\"profiles\":[]}", parsed, error));
    QNP_CHECK(!parseControlsFile(
        "{\"schema\":999,\"profiles\":[]}", parsed, error));
    QNP_CHECK(!parseControlsFile(
        "{\"schema\":1,\"profiles\":[]}x", parsed, error));
    QNP_CHECK(!parseControlsFile(
        "{\"schema\":1,\"profiles\":[{\"id\":\"p\",\"name\":\"n\","
        "\"source\":\"keyboard\",\"actions\":{\"Up\":[{\"type\":"
        "\"GamepadAxis\",\"axis\":0,\"polarity\":\"positive\","
        "\"pressQ15\":1000,\"releaseQ15\":1000}]}}]}",
        parsed, error));
    QNP_CHECK(!parseControlsFile(
        "{\"schema\":1,\"profiles\":[{\"id\":\"p\",\"name\":\"n\","
        "\"source\":\"keyboard\",\"sessionControls\":{\"Pause\":[{"
        "\"type\":\"GamepadAxis\",\"axis\":0,\"polarity\":\"positive\"}"
        "]}}]}",
        parsed, error));
    QNP_CHECK(!parseControlsFile(
        "{\"schema\":1,\"profiles\":[{\"id\":\"p\",\"name\":\"a\","
        "\"source\":\"keyboard\"},{\"id\":\"p\",\"name\":\"b\","
        "\"source\":\"keyboard\"}]}",
        parsed, error));
}

QNP_TEST(ProfileCompilerDefaults) {
    const CompileResult result =
        compileInputProfile(makeDefaultKeyboardProfile(), false);
    QNP_REQUIRE(result.ok);
    QNP_CHECK(result.config.probeCount == 0);
    QNP_CHECK(validateInputLaunchConfig(result.config, false) == nullptr);
    QNP_CHECK(result.modeControls.count > 0u);
    bool sawDelayDecrease = false;
    for (uint16_t i = 0; i < result.modeControls.count; ++i) {
        const auto& binding = result.modeControls.bindings[i];
        if (binding.action == ModeControlAction::InputDelayDecrease) {
            sawDelayDecrease = true;
            QNP_CHECK(binding.binding.type
                      == CompiledBindingType::KeyboardScan);
            QNP_CHECK(binding.binding.dik == 0x0Cu);
        }
    }
    QNP_CHECK(sawDelayDecrease);
}

QNP_TEST(ProfileCompilerRejections) {
    {
        InputProfile profile = makeDefaultKeyboardProfile();
        profile.actions[4][0].dik = 0x0Cu;
        QNP_CHECK(!compileInputProfile(profile, false).ok);
    }
    {
        InputProfile profile = makeDefaultKeyboardProfile();
        profile.actions[7].clear();
        QNP_CHECK(!compileInputProfile(profile, false).ok);
    }
    {
        InputProfile profile = makeDefaultKeyboardProfile();
        profile.actions[5][0].dik = profile.actions[4][0].dik;
        QNP_CHECK(!compileInputProfile(profile, false).ok);
    }
    {
        InputProfile profile = makeDefaultKeyboardProfile();
        ControlsFile controls = makeDefaultControlsFile();
        ProfileBinding leave{};
        leave.type = CompiledBindingType::KeyboardScan;
        leave.dik = profile.actions[4][0].dik;
        controls.modeControls[static_cast<size_t>(
            ModeControlAction::LeaveMatch)].push_back(leave);
        QNP_CHECK(!compileInputProfile(profile, false, &controls).ok);
    }
    {
        InputProfile profile = makeDefaultKeyboardProfile();
        ControlsFile controls = makeDefaultControlsFile();
        ProfileBinding key{};
        key.type = CompiledBindingType::KeyboardScan;
        key.dik = 0x44;
        controls.modeControls[static_cast<size_t>(
            ModeControlAction::LeaveMatch)].push_back(key);
        controls.modeControls[static_cast<size_t>(
            ModeControlAction::InputDelayDecrease)].push_back(key);
        QNP_CHECK(!compileInputProfile(profile, false, &controls).ok);
    }
    {
        InputProfile profile = makeDefaultKeyboardProfile();
        profile.sourceMode = CompiledSourceMode::KeyboardAndController;
        profile.device.guid = "g";
        int token = 0;
        for (int action = 0; action < 8; ++action) {
            profile.actions[action].clear();
            for (uint32_t i = 0; i < kMaxBindingsPerAction; ++i) {
                ProfileBinding binding{};
                binding.type = CompiledBindingType::ProbeButton;
                binding.probeKind = ProbeKind::RawButton;
                binding.token = static_cast<uint8_t>(token++);
                profile.actions[action].push_back(binding);
            }
        }
        ProfileBinding extra{};
        extra.type = CompiledBindingType::ProbeButton;
        extra.probeKind = ProbeKind::RawButton;
        extra.token = static_cast<uint8_t>(token);
        ControlsFile controls{};
        controls.modeControls[static_cast<size_t>(
            ModeControlAction::LeaveMatch)].push_back(extra);
        const CompileResult result =
            compileInputProfile(profile, false, &controls);
        QNP_CHECK(!result.ok);
    }
}

QNP_TEST(NativeBindingPlanExpressesKeyboardProfile) {
    const qinput::CompileResult compiled =
        compileInputProfile(makeDefaultKeyboardProfile(), false);
    QNP_REQUIRE(compiled.ok);
    constexpr uint32_t kKeyboardCallback = 0x484B00u;
    const NativeBindingPlan plan =
        buildNativeBindingPlan(compiled.config, kKeyboardCallback);
    QNP_CHECK(plan.anyExpressed);
    QNP_CHECK(plan.unexpressedMask == 0);
    const uint8_t expected[8] = {0xC8, 0xD0, 0xCB, 0xCD,
                                 0x2C, 0x2D, 0x2E, 0x2F};
    for (uint32_t action = 0; action < 8; ++action) {
        QNP_CHECK(plan.actions[action].sourceType
                  == kNativeSourceKeyboard);
        QNP_CHECK(plan.actions[action].codeOrMask == expected[action]);
        QNP_CHECK(plan.actions[action].callback == kKeyboardCallback);
    }
    QNP_CHECK(nativeDescriptorOffset(0, 0) == 0u);
    QNP_CHECK(nativeDescriptorOffset(0, 7) == 84u);
    QNP_CHECK(nativeDescriptorOffset(1, 0) == 96u);
    QNP_CHECK(nativeDescriptorOffset(3, 7) == kNativeBindingTableBytes - 12u);
}

QNP_TEST(NativeBindingPlanReportsControllerOnlyActions) {
    InputProfile profile = makeDefaultKeyboardProfile();
    profile.sourceMode = CompiledSourceMode::KeyboardAndController;
    profile.device.guid = "g";
    ProfileBinding button{};
    button.type = CompiledBindingType::ProbeButton;
    button.probeKind = ProbeKind::RawButton;
    button.token = 2;
    profile.actions[4].clear();
    profile.actions[4].push_back(button);
    const qinput::CompileResult compiled =
        compileInputProfile(profile, false);
    QNP_REQUIRE(compiled.ok);
    const NativeBindingPlan plan =
        buildNativeBindingPlan(compiled.config, 0x484B00u);
    QNP_CHECK(plan.anyExpressed);
    QNP_CHECK(plan.unexpressedMask == (1u << 4));
    QNP_CHECK(plan.actions[4].codeOrMask == 0u);
}

QNP_TEST(TriggerOnAModeControlSurvivesTheControlsFileRoundTrip) {
    ControlsFile file{};
    InputProfile profile = makeDefaultKeyboardProfile();
    file.activeProfileId = profile.id;
    file.profiles.push_back(profile);

    ProfileBinding trigger{};
    trigger.type = CompiledBindingType::ProbeAxis;
    trigger.probeKind = ProbeKind::GamepadAxis;
    trigger.token = 4;
    trigger.polarity = AxisPolarity::Positive;
    trigger.pressQ15 = 16384;
    trigger.releaseQ15 = 11469;
    std::string error;
    QNP_REQUIRE(validModeControlBinding(ModeControlAction::LeaveMatch,
                                        trigger, error));
    file.modeControls[static_cast<size_t>(ModeControlAction::LeaveMatch)]
        .push_back(trigger);

    ControlsFile reloaded{};
    const bool parsed =
        parseControlsFile(serializeControlsFile(file), reloaded, error);
    QNP_CHECK(parsed);
    if (!parsed) {
        std::printf("  the writer produced a file the reader refused: %s\n",
                    error.c_str());
        return;
    }
    const auto& back = reloaded.modeControls[
        static_cast<size_t>(ModeControlAction::LeaveMatch)];
    QNP_REQUIRE(back.size() == 1u);
    QNP_CHECK(back[0].type == CompiledBindingType::ProbeAxis);
    QNP_CHECK(back[0].probeKind == ProbeKind::GamepadAxis);
    QNP_CHECK(back[0].token == 4);
    QNP_CHECK(back[0].polarity == AxisPolarity::Positive);

    ControlsFile sticks = file;
    sticks.modeControls[static_cast<size_t>(ModeControlAction::LeaveMatch)][0]
        .token = 0;
    ControlsFile rejected{};
    std::string stickError;
    QNP_CHECK(!parseControlsFile(serializeControlsFile(sticks), rejected,
                                 stickError));
    QNP_CHECK(stickError.find("stick") != std::string::npos);
}

QNP_TEST(EveryOptionalProfileFieldSurvivesItsOwnRoundTrip) {
    InputProfile profile = makeDefaultKeyboardProfile();
    profile.device.guid = "030003f05e040000ea02000000007200";
    profile.device.vendor = 0x054C;
    profile.device.product = 0x0CE6;
    profile.device.normalizedName = "DualSense Wireless Controller";
    profile.device.pathFingerprint = 0x1122334455667788ull;
    profile.device.serialHash = 0xFEEDFACECAFEBEEFull;
    profile.mapping = "030003f05e040000ea02000000007200,DualSense,a:b0";
    profile.probeBackend = "sdl";

    ControlsFile file{};
    file.activeProfileId = profile.id;
    file.profiles.push_back(profile);

    ControlsFile reloaded{};
    std::string error;
    const bool parsed =
        parseControlsFile(serializeControlsFile(file), reloaded, error);
    QNP_CHECK(parsed);
    if (!parsed) {
        std::printf("  parse error: %s\n", error.c_str());
        return;
    }
    QNP_REQUIRE(reloaded.profiles.size() == 1);
    QNP_CHECK(reloaded.profiles[0].probeBackend == "sdl");
    QNP_CHECK(reloaded.profiles[0].mapping == profile.mapping);
    QNP_CHECK(reloaded.profiles[0].device.serialHash
              == profile.device.serialHash);

    file.profiles[0].probeBackend = "dinput8";
    QNP_CHECK(parseControlsFile(serializeControlsFile(file), reloaded, error));
    QNP_REQUIRE(reloaded.profiles.size() == 1);
    QNP_CHECK(reloaded.profiles[0].probeBackend == "dinput8");

    file.profiles[0].probeBackend.clear();
    QNP_CHECK(parseControlsFile(serializeControlsFile(file), reloaded, error));
    QNP_REQUIRE(reloaded.profiles.size() == 1);
    QNP_CHECK(reloaded.profiles[0].probeBackend.empty());
}

QNP_TEST(ControlsFilePreservesAllPlayersAndEscapesJsonStrings) {
    ControlsFile file{};
    for (int player = 0; player < 4; ++player) {
        InputProfile profile = makeDefaultKeyboardProfile();
        profile.id = "player-" + std::to_string(player + 1);
        profile.name = "Player " + std::to_string(player + 1)
                     + " \"quoted\" \\ path\tlabel";
        profile.device.guid = "guid\\\"" + std::to_string(player + 1);
        profile.device.normalizedName = "Pad\n" + std::to_string(player + 1);
        file.activeByPlayer[player] = profile.id;
        file.profiles.push_back(profile);
    }
    file.activeProfileId = file.activeByPlayer[0];

    const std::string serialized = serializeControlsFile(file);
    ControlsFile parsed{};
    std::string error;
    QNP_REQUIRE(parseControlsFile(serialized, parsed, error));
    QNP_REQUIRE(parsed.profiles.size() == 4u);
    for (int player = 0; player < 4; ++player) {
        QNP_CHECK(parsed.activeByPlayer[player]
                  == file.activeByPlayer[player]);
        QNP_CHECK(parsed.profiles[static_cast<size_t>(player)].id
                  == file.profiles[static_cast<size_t>(player)].id);
        QNP_CHECK(parsed.profiles[static_cast<size_t>(player)].name
                  == file.profiles[static_cast<size_t>(player)].name);
        QNP_CHECK(parsed.profiles[static_cast<size_t>(player)].device.guid
                  == file.profiles[static_cast<size_t>(player)].device.guid);
        QNP_CHECK(parsed.profiles[static_cast<size_t>(player)]
                      .device.normalizedName
                  == file.profiles[static_cast<size_t>(player)]
                         .device.normalizedName);
    }
    QNP_CHECK(serializeControlsFile(parsed) == serialized);

    std::string escaped = serialized;
    const std::string oldName =
        "\"name\":\"Player 1 \\\"quoted\\\" \\\\ path\\tlabel\"";
    const std::string newName =
        "\"name\":\"Player \\u0031 \\uD83D\\uDE80\"";
    const size_t nameAt = escaped.find(oldName);
    QNP_REQUIRE(nameAt != std::string::npos);
    escaped.replace(nameAt, oldName.size(), newName);
    QNP_REQUIRE(parseControlsFile(escaped, parsed, error));
    QNP_CHECK(parsed.profiles[0].name
              == std::string("Player 1 \xF0\x9F\x9A\x80"));
}

QNP_TEST(ControllerBindingSurvivesSaveAndReload) {
    InputProfile profile = makeDefaultKeyboardProfile();
    profile.device.guid = "030003f05e040000ea02000000007200";
    profile.device.vendor = 0x054C;
    profile.device.product = 0x0CE6;
    profile.device.normalizedName = "DualSense Wireless Controller";
    ProfileBinding button{};
    button.type = CompiledBindingType::ProbeButton;
    button.probeKind = ProbeKind::GamepadButton;
    button.token = 0;
    profile.actions[4].push_back(button);
    ProfileBinding raw{};
    raw.type = CompiledBindingType::ProbeButton;
    raw.probeKind = ProbeKind::RawButton;
    raw.token = 2;
    profile.actions[5].push_back(raw);
    profile.device.pathFingerprint = 0x1122334455667788ull;
    profile.mapping =
        "030003f05e040000ea02000000007200,DualSense,a:b0,b:b1";
    ControlsFile file{};
    file.activeProfileId = profile.id;
    file.profiles.push_back(profile);
    const std::string text = serializeControlsFile(file);
    ControlsFile reloaded{};
    std::string error;
    const bool parsed = parseControlsFile(text, reloaded, error);
    QNP_CHECK(parsed);
    if (!parsed) { std::printf("  parse error: %s\n", error.c_str()); return; }
    QNP_REQUIRE(reloaded.profiles.size() == 1);
    QNP_CHECK(reloaded.profiles[0].actions[4].size() == 2);
    QNP_CHECK(reloaded.profiles[0].actions[5].size() == 2);
    QNP_CHECK(reloaded.profiles[0].mapping == profile.mapping);
    QNP_CHECK(reloaded.profiles[0].device.pathFingerprint
              == profile.device.pathFingerprint);
    QNP_CHECK(reloaded.profiles[0].device.guid == profile.device.guid);
    const CompileResult compiled =
        compileInputProfile(reloaded.profiles[0], false);
    QNP_CHECK(compiled.ok);
    if (!compiled.ok)
        std::printf("  compile error: %s\n", compiled.error.c_str());
    QNP_CHECK(compiled.config.probeCount == 3);
}

QNP_TEST(ProfileCompilerSpectator) {
    const CompileResult result =
        compileInputProfile(makeDefaultKeyboardProfile(), true);
    QNP_REQUIRE(result.ok);
    QNP_CHECK(result.config.probeCount == 0);
    for (const CompiledActionBindings& action : result.config.actions)
        QNP_CHECK(action.count == 0);
    QNP_CHECK(validateInputLaunchConfig(result.config, true) == nullptr);
    const CompileResult player =
        compileInputProfile(makeDefaultKeyboardProfile(), false);
    QNP_CHECK(validateInputLaunchConfig(player.config, true) != nullptr);
}

QNP_TEST(ProfileCompilerProbeUnionDeduplicated) {
    InputProfile profile = makeDefaultKeyboardProfile();
    profile.sourceMode = CompiledSourceMode::KeyboardAndController;
    profile.device.guid = "g";
    ProfileBinding button{};
    button.type = CompiledBindingType::ProbeButton;
    button.probeKind = ProbeKind::RawButton;
    button.token = 5;
    profile.actions[4].push_back(button);
    ProfileBinding leaveButton = button;
    leaveButton.token = 9;
    ControlsFile controls{};
    controls.modeControls[static_cast<size_t>(
        ModeControlAction::LeaveMatch)].push_back(leaveButton);
    const CompileResult result =
        compileInputProfile(profile, false, &controls);
    QNP_REQUIRE(result.ok);
    QNP_CHECK(result.config.probeCount == 2);
}

QNP_TEST(RawProfileIgnoresIncompatibleSemanticModeDefaults) {
    ControlsFile controls = makeDefaultControlsFile();
    QNP_REQUIRE(!controls.profiles.empty());
    InputProfile& profile = controls.profiles[0];
    profile.sourceMode = CompiledSourceMode::KeyboardAndController;
    profile.device.guid = "raw-guid";
    profile.mapping.clear();
    profile.probeBackend = "dinput8";
    ProfileBinding raw{};
    raw.type = CompiledBindingType::ProbeButton;
    raw.probeKind = ProbeKind::RawButton;
    raw.token = 2;
    profile.actions[4].push_back(raw);

    const CompileResult compiled =
        compileInputProfile(profile, false, &controls);
    QNP_REQUIRE(compiled.ok);
    QNP_CHECK(compiled.config.probeCount == 1u);
    QNP_CHECK(compiled.config.probes[0].kind == ProbeKind::RawButton);
    QNP_CHECK(compiled.config.probeBackend
              == CompiledProbeBackend::DirectInput8);
}

QNP_TEST(CompiledProbeBackendIsValidatedAtTheLaunchBoundary) {
    InputLaunchConfig config{};
    config.sourceMode = CompiledSourceMode::KeyboardAndController;
    config.probeCount = 1;
    config.probes[0].kind = ProbeKind::RawButton;
    std::snprintf(config.device.guid, sizeof(config.device.guid), "g");
    for (CompiledActionBindings& action : config.actions) {
        action.count = 1;
        action.bindings[0].type = CompiledBindingType::KeyboardScan;
        action.bindings[0].dik = 0x2Cu;
    }

    config.probeBackend = CompiledProbeBackend::DirectInput8;
    QNP_CHECK(validateInputLaunchConfig(config, false) == nullptr);
    config.probeBackend = static_cast<CompiledProbeBackend>(99u);
    QNP_CHECK(validateInputLaunchConfig(config, false) != nullptr);

    config.probeBackend = CompiledProbeBackend::DirectInput8;
    config.probes[0].kind = ProbeKind::GamepadButton;
    config.mappingLength = 1;
    config.mapping[0] = 'm';
    QNP_CHECK(validateInputLaunchConfig(config, false) != nullptr);

    config.probeCount = 0;
    config.mappingLength = 0;
    QNP_CHECK(validateInputLaunchConfig(config, false) != nullptr);
}

QNP_TEST(CompiledProbeKindsDecideWhichBackendCanServeTheProfile) {
    using qinput::ProbeKind;
    using qinput::CompiledProbe;

    QNP_CHECK(qinput::compiledProbeNeedsStandardizedBackend(
        ProbeKind::GamepadButton));
    QNP_CHECK(qinput::compiledProbeNeedsStandardizedBackend(
        ProbeKind::GamepadAxis));
    QNP_CHECK(!qinput::compiledProbeNeedsStandardizedBackend(
        ProbeKind::RawButton));
    QNP_CHECK(!qinput::compiledProbeNeedsStandardizedBackend(
        ProbeKind::RawAxis));
    QNP_CHECK(!qinput::compiledProbeNeedsStandardizedBackend(
        ProbeKind::RawHat));
    QNP_CHECK(!qinput::compiledProbeNeedsStandardizedBackend(ProbeKind::None));

    CompiledProbe raw[3] = {};
    raw[0].kind = ProbeKind::RawButton;
    raw[1].kind = ProbeKind::RawAxis;
    raw[2].kind = ProbeKind::RawHat;
    QNP_CHECK(!qinput::compiledProbesRequireStandardizedBackend(raw, 3));

    CompiledProbe mixed[3] = {};
    mixed[0].kind = ProbeKind::RawButton;
    mixed[1].kind = ProbeKind::GamepadAxis;
    mixed[2].kind = ProbeKind::RawHat;
    QNP_CHECK(qinput::compiledProbesRequireStandardizedBackend(mixed, 3));

    QNP_CHECK(!qinput::compiledProbesRequireStandardizedBackend(raw, 0));
    QNP_CHECK(!qinput::compiledProbesRequireStandardizedBackend(nullptr, 3));

    CompiledProbe tail[4] = {};
    tail[0].kind = ProbeKind::RawButton;
    tail[3].kind = ProbeKind::GamepadButton;
    QNP_CHECK(!qinput::compiledProbesRequireStandardizedBackend(tail, 1));
    QNP_CHECK(qinput::compiledProbesRequireStandardizedBackend(tail, 4));

    QNP_CHECK(!qinput::compiledControllerNeedsSdl(
        raw, 3, qinput::CompiledProbeBackend::DirectInput8));
    QNP_CHECK(qinput::compiledControllerNeedsSdl(
        raw, 3, qinput::CompiledProbeBackend::Sdl));
    QNP_CHECK(qinput::compiledControllerNeedsSdl(
        raw, 3, qinput::CompiledProbeBackend::Unknown));
    QNP_CHECK(qinput::compiledControllerNeedsSdl(
        mixed, 3, qinput::CompiledProbeBackend::DirectInput8));
    QNP_CHECK(!qinput::compiledControllerNeedsSdl(
        raw, 0, qinput::CompiledProbeBackend::Sdl));
}
