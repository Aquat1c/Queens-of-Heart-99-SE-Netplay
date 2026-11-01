#include "TestHarness.h"

#include "input/BindingEvaluator.h"
#include "input/InputCompiler.h"
#include "input/InputProfile.h"

#include <cstdio>
#include <memory>

using namespace qinput;

namespace {

struct Fixture {
    InputLaunchConfig config{};
    InputEvaluator evaluator;
    uint32_t keyBits[8] = {};
    uint32_t probes[kMaxControllerProbes] = {};
    uint32_t keyboardSerial = 1;
    uint32_t controllerSerial = 1;
    uint32_t keyboardGeneration = 1;
    uint32_t controllerGeneration = 1;
    bool keyboardHealthy = true;
    bool controllerHealthy = true;
    bool focusActive = true;
    uint32_t focusGeneration = 1;

    void configure() { evaluator.configure(&config); }

    void holdKey(uint8_t dik, bool held) {
        if (held)
            keyBits[dik >> 5] |= 1u << (dik & 31u);
        else
            keyBits[dik >> 5] &= ~(1u << (dik & 31u));
    }

    EvaluateResult step() {
        ++keyboardSerial;
        ++controllerSerial;
        KeyboardView keyboard{keyBits, keyboardHealthy,
                              keyboardGeneration, keyboardSerial};
        ControllerView controller{probes, config.probeCount,
                                  controllerHealthy, controllerGeneration,
                                  controllerSerial};
        FocusView focus{focusActive, focusGeneration};
        return evaluator.evaluate(keyboard, controller, focus);
    }

    void arm() {
        (void)step();
        (void)step();
        (void)step();
        (void)step();
    }
};

InputLaunchConfig keyboardOnlyConfig() {
    const CompileResult compiled =
        compileInputProfile(makeDefaultKeyboardProfile(), false);
    return compiled.config;
}

InputLaunchConfig axisConfig(SocdPolicy horizontal = SocdPolicy::Native) {
    InputProfile profile = makeDefaultKeyboardProfile();
    profile.sourceMode = CompiledSourceMode::KeyboardAndController;
    profile.device.guid = "testguid";
    profile.device.vendor = 1;
    profile.device.product = 2;
    profile.device.version = 3;
    profile.horizontalSocd = horizontal;
    ProfileBinding leftAxis{};
    leftAxis.type = CompiledBindingType::ProbeAxis;
    leftAxis.probeKind = ProbeKind::RawAxis;
    leftAxis.token = 0;
    leftAxis.polarity = AxisPolarity::Negative;
    profile.actions[2].push_back(leftAxis);
    ProfileBinding rightAxis = leftAxis;
    rightAxis.polarity = AxisPolarity::Positive;
    profile.actions[3].push_back(rightAxis);
    ProfileBinding button{};
    button.type = CompiledBindingType::ProbeButton;
    button.probeKind = ProbeKind::RawButton;
    button.token = 3;
    profile.actions[4].push_back(button);
    const CompileResult compiled = compileInputProfile(profile, false);
    return compiled.config;
}

}

QNP_TEST(BindingsSemanticDpadClusterCompletesAllFourDirections) {
    InputProfile profile{};
    profile.sourceMode = CompiledSourceMode::Controller;
    profile.device.guid = "0300c6b1790000001100000000000000";
    profile.device.normalizedName = "SWITCH CO.,LTD. USB Gamepad";
    profile.mapping = "0300c6b1790000001100000000000000,USB Gamepad,a:b1,b:b2,"
                      "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,";

    for (int direction = 0; direction < 4; ++direction) {
        ProfileBinding pad{};
        pad.type = CompiledBindingType::ProbeButton;
        pad.probeKind = ProbeKind::GamepadButton;
        pad.token = static_cast<uint8_t>(11 + direction);
        profile.actions[direction].push_back(pad);
    }
    const uint8_t attacks[4] = {1u, 0u, 9u, 3u};
    for (int i = 0; i < 4; ++i) {
        ProfileBinding pad{};
        pad.type = CompiledBindingType::ProbeButton;
        pad.probeKind = ProbeKind::GamepadButton;
        pad.token = attacks[i];
        profile.actions[4 + i].push_back(pad);
    }

    const CompileResult compiled = compileInputProfile(profile, false);
    if (!compiled.ok)
        std::printf("      compile refused: %s\n", compiled.error.c_str());
    QNP_CHECK(compiled.error.find("no binding for action") == std::string::npos);
    QNP_CHECK(compiled.ok);
}

QNP_TEST(BindingsAxisNormalizationBoundaries) {
    CompiledAxisCalibration bipolar{};
    bipolar.minimum = -32768;
    bipolar.center = 0;
    bipolar.maximum = 32767;
    QNP_CHECK(normalizeAxisMagnitudeQ15(-32768, bipolar,
                                        AxisPolarity::Negative) == 32767);
    QNP_CHECK(normalizeAxisMagnitudeQ15(32767, bipolar,
                                        AxisPolarity::Positive) == 32767);
    QNP_CHECK(normalizeAxisMagnitudeQ15(0, bipolar,
                                        AxisPolarity::Positive) == 0);
    QNP_CHECK(normalizeAxisMagnitudeQ15(-100, bipolar,
                                        AxisPolarity::Positive) == 0);
    bipolar.deadzoneQ15 = 4096;
    CompiledAxisCalibration deadzoned = bipolar;
    const int32_t atDeadzone = normalizeAxisMagnitudeQ15(
        4096, deadzoned, AxisPolarity::Positive);
    QNP_CHECK(atDeadzone == 0);
    const int32_t aboveDeadzone = normalizeAxisMagnitudeQ15(
        4098, deadzoned, AxisPolarity::Positive);
    QNP_CHECK(aboveDeadzone > 4096);
    CompiledAxisCalibration offCenter{};
    offCenter.minimum = -1000;
    offCenter.center = 200;
    offCenter.maximum = 2000;
    QNP_CHECK(normalizeAxisMagnitudeQ15(2000, offCenter,
                                        AxisPolarity::Positive) == 32767);
    QNP_CHECK(normalizeAxisMagnitudeQ15(-1000, offCenter,
                                        AxisPolarity::Negative) == 32767);
    offCenter.invert = 1;
    QNP_CHECK(normalizeAxisMagnitudeQ15(-1000, offCenter,
                                        AxisPolarity::Positive) == 32767);
    CompiledAxisCalibration trigger{};
    trigger.minimum = 0;
    trigger.center = 0;
    trigger.maximum = 32767;
    trigger.unipolar = 1;
    QNP_CHECK(normalizeAxisMagnitudeQ15(32767, trigger,
                                        AxisPolarity::Positive) == 32767);
    QNP_CHECK(normalizeAxisMagnitudeQ15(0, trigger,
                                        AxisPolarity::Positive) == 0);
    QNP_CHECK(normalizeAxisMagnitudeQ15(20000, trigger,
                                        AxisPolarity::Negative) == 0);
    trigger.invert = 1;
    QNP_CHECK(normalizeAxisMagnitudeQ15(0, trigger,
                                        AxisPolarity::Positive) == 32767);
    QNP_CHECK(normalizeAxisMagnitudeQ15(INT32_MAX, bipolar,
                                        AxisPolarity::Positive) == 32767);
    QNP_CHECK(normalizeAxisMagnitudeQ15(INT32_MIN, bipolar,
                                        AxisPolarity::Negative) == 32767);
}

QNP_TEST(BindingsKeyboardPressHoldRelease) {
    auto fixture = std::make_unique<Fixture>();
    fixture->config = keyboardOnlyConfig();
    fixture->configure();
    fixture->arm();
    QNP_CHECK(fixture->step().mask == 0);
    fixture->holdKey(0x2C, true);
    QNP_CHECK(fixture->step().mask == kCompactA);
    QNP_CHECK(fixture->step().mask == kCompactA);
    fixture->holdKey(0x2C, false);
    QNP_CHECK(fixture->step().mask == 0);
}

QNP_TEST(BindingsAxisHysteresisAndReversal) {
    auto fixture = std::make_unique<Fixture>();
    fixture->config = axisConfig();
    fixture->configure();
    fixture->arm();
    uint8_t axisSlot = 0xFF;
    for (uint32_t i = 0; i < fixture->config.probeCount; ++i) {
        if (fixture->config.probes[i].kind == ProbeKind::RawAxis)
            axisSlot = static_cast<uint8_t>(i);
    }
    QNP_REQUIRE(axisSlot != 0xFF);
    fixture->probes[axisSlot] = static_cast<uint32_t>(16383);
    QNP_CHECK((fixture->step().mask
               & (kCompactLeft | kCompactRight)) == 0);
    fixture->probes[axisSlot] = static_cast<uint32_t>(16384);
    QNP_CHECK((fixture->step().mask & kCompactRight) != 0);
    fixture->probes[axisSlot] = static_cast<uint32_t>(12000);
    QNP_CHECK((fixture->step().mask & kCompactRight) != 0);
    fixture->probes[axisSlot] = static_cast<uint32_t>(11469);
    QNP_CHECK((fixture->step().mask & kCompactRight) == 0);
    fixture->probes[axisSlot] = static_cast<uint32_t>(16384);
    (void)fixture->step();
    fixture->probes[axisSlot] = static_cast<uint32_t>(-32768);
    const EvaluateResult reversed = fixture->step();
    QNP_CHECK((reversed.mask & kCompactLeft) != 0);
    QNP_CHECK((reversed.mask & kCompactRight) == 0);
}

QNP_TEST(BindingsFocusLossAndRegain) {
    auto fixture = std::make_unique<Fixture>();
    fixture->config = keyboardOnlyConfig();
    fixture->configure();
    fixture->arm();
    fixture->holdKey(0x2C, true);
    QNP_CHECK(fixture->step().mask == kCompactA);
    fixture->focusActive = false;
    ++fixture->focusGeneration;
    QNP_CHECK(fixture->step().mask == 0);
    fixture->focusActive = true;
    ++fixture->focusGeneration;
    QNP_CHECK(fixture->step().mask == 0);
    QNP_CHECK(fixture->step().mask == 0);
    fixture->holdKey(0x2C, false);
    (void)fixture->step();
    (void)fixture->step();
    (void)fixture->step();
    fixture->holdKey(0x2C, true);
    QNP_CHECK(fixture->step().mask == kCompactA);
}

QNP_TEST(BindingsSameGenerationStallHoldsTheLastInput) {
    auto fixture = std::make_unique<Fixture>();
    fixture->config = axisConfig();
    fixture->configure();
    fixture->arm();
    uint8_t buttonSlot = 0xFF;
    for (uint32_t i = 0; i < fixture->config.probeCount; ++i) {
        if (fixture->config.probes[i].kind == ProbeKind::RawButton)
            buttonSlot = static_cast<uint8_t>(i);
    }
    QNP_REQUIRE(buttonSlot != 0xFF);

    fixture->probes[buttonSlot] = 1;
    QNP_CHECK((fixture->step().mask & kCompactA) != 0);

    fixture->controllerHealthy = false;
    for (uint32_t i = 0; i < kSourceStallHoldEvaluations / 2u; ++i) {
        const EvaluateResult held = fixture->step();
        QNP_CHECK((held.mask & kCompactA) != 0);
        QNP_CHECK(held.controllerGate == SourceGateState::Armed);
    }
    fixture->controllerHealthy = true;
    EvaluateResult result = fixture->step();
    QNP_CHECK((result.mask & kCompactA) != 0);
    QNP_CHECK(result.controllerGate == SourceGateState::Armed);
    fixture->probes[buttonSlot] = 0;
    QNP_CHECK((fixture->step().mask & kCompactA) == 0);

    fixture->probes[buttonSlot] = 1;
    QNP_CHECK((fixture->step().mask & kCompactA) != 0);
    fixture->controllerHealthy = false;
    for (uint32_t i = 0; i + 1u < kSourceStallHoldEvaluations; ++i)
        QNP_CHECK((fixture->step().mask & kCompactA) != 0);
    fixture->controllerHealthy = true;
    QNP_CHECK(fixture->step().controllerGate == SourceGateState::Armed);
}

QNP_TEST(BindingsFocusLossStillRequiresNeutral) {
    auto fixture = std::make_unique<Fixture>();
    fixture->config = axisConfig();
    fixture->config.focusPolicy = FocusPolicy::GameWindowOnly;
    fixture->configure();
    fixture->arm();
    uint8_t buttonSlot = 0xFF;
    for (uint32_t i = 0; i < fixture->config.probeCount; ++i) {
        if (fixture->config.probes[i].kind == ProbeKind::RawButton)
            buttonSlot = static_cast<uint8_t>(i);
    }
    QNP_REQUIRE(buttonSlot != 0xFF);
    fixture->probes[buttonSlot] = 1;
    QNP_CHECK((fixture->step().mask & kCompactA) != 0);

    fixture->focusActive = false;
    EvaluateResult result = fixture->step();
    QNP_CHECK((result.mask & kCompactA) == 0);
    QNP_CHECK(result.controllerGate == SourceGateState::Disconnected);
    fixture->focusActive = true;
    ++fixture->focusGeneration;
    result = fixture->step();
    QNP_CHECK((result.mask & kCompactA) == 0);
}

QNP_TEST(BindingsDisconnectReconnectGate) {
    auto fixture = std::make_unique<Fixture>();
    fixture->config = axisConfig();
    fixture->configure();
    fixture->arm();
    uint8_t buttonSlot = 0xFF;
    for (uint32_t i = 0; i < fixture->config.probeCount; ++i) {
        if (fixture->config.probes[i].kind == ProbeKind::RawButton)
            buttonSlot = static_cast<uint8_t>(i);
    }
    QNP_REQUIRE(buttonSlot != 0xFF);
    fixture->probes[buttonSlot] = 1;
    QNP_CHECK((fixture->step().mask & kCompactA) != 0);
    fixture->controllerHealthy = false;
    fixture->holdKey(0x2C, false);
    EvaluateResult result = fixture->step();
    QNP_CHECK((result.mask & kCompactA) != 0);
    QNP_CHECK(result.controllerGate == SourceGateState::Armed);
    for (uint32_t i = 0; i < kSourceStallHoldEvaluations; ++i)
        result = fixture->step();
    QNP_CHECK((result.mask & kCompactA) == 0);
    QNP_CHECK(result.controllerGate == SourceGateState::Disconnected);
    fixture->holdKey(0x11, true);
    fixture->holdKey(0xC8, true);
    result = fixture->step();
    QNP_CHECK((result.mask & kCompactUp) != 0);
    fixture->holdKey(0xC8, false);
    fixture->controllerHealthy = true;
    ++fixture->controllerGeneration;
    result = fixture->step();
    QNP_CHECK((result.mask & kCompactA) == 0);
    (void)fixture->step();
    QNP_CHECK((fixture->step().mask & kCompactA) == 0);
    fixture->probes[buttonSlot] = 0;
    (void)fixture->step();
    (void)fixture->step();
    (void)fixture->step();
    fixture->probes[buttonSlot] = 1;
    QNP_CHECK((fixture->step().mask & kCompactA) != 0);
}

QNP_TEST(BindingsSocdPolicies) {
    {
        auto fixture = std::make_unique<Fixture>();
        fixture->config = keyboardOnlyConfig();
        fixture->configure();
        fixture->arm();
        fixture->holdKey(0xCB, true);
        fixture->holdKey(0xCD, true);
        const uint16_t mask = fixture->step().mask;
        QNP_CHECK((mask & kCompactLeft) != 0);
        QNP_CHECK((mask & kCompactRight) != 0);
    }
    {
        InputProfile profile = makeDefaultKeyboardProfile();
        profile.horizontalSocd = SocdPolicy::Neutral;
        auto fixture = std::make_unique<Fixture>();
        fixture->config = compileInputProfile(profile, false).config;
        fixture->configure();
        fixture->arm();
        fixture->holdKey(0xCB, true);
        fixture->holdKey(0xCD, true);
        const uint16_t mask = fixture->step().mask;
        QNP_CHECK((mask & (kCompactLeft | kCompactRight)) == 0);
    }
    {
        InputProfile profile = makeDefaultKeyboardProfile();
        profile.horizontalSocd = SocdPolicy::LastInput;
        auto fixture = std::make_unique<Fixture>();
        fixture->config = compileInputProfile(profile, false).config;
        fixture->configure();
        fixture->arm();
        fixture->holdKey(0xCB, true);
        QNP_CHECK((fixture->step().mask & kCompactLeft) != 0);
        fixture->holdKey(0xCD, true);
        const uint16_t mask = fixture->step().mask;
        QNP_CHECK((mask & kCompactRight) != 0);
        QNP_CHECK((mask & kCompactLeft) == 0);
        fixture->holdKey(0xCB, false);
        fixture->holdKey(0xCD, false);
        (void)fixture->step();
        fixture->holdKey(0xCB, true);
        fixture->holdKey(0xCD, true);
        QNP_CHECK((fixture->step().mask
                   & (kCompactLeft | kCompactRight)) == 0);
    }
    {
        InputProfile profile = makeDefaultKeyboardProfile();
        profile.verticalSocd = SocdPolicy::UpPriority;
        auto fixture = std::make_unique<Fixture>();
        fixture->config = compileInputProfile(profile, false).config;
        fixture->configure();
        fixture->arm();
        fixture->holdKey(0xC8, true);
        fixture->holdKey(0xD0, true);
        const uint16_t mask = fixture->step().mask;
        QNP_CHECK((mask & kCompactUp) != 0);
        QNP_CHECK((mask & kCompactDown) == 0);
    }
}

QNP_TEST(BindingsNeutralSocdIsNotPhysicalNeutral) {
    InputProfile profile = makeDefaultKeyboardProfile();
    profile.horizontalSocd = SocdPolicy::Neutral;
    auto fixture = std::make_unique<Fixture>();
    fixture->config = compileInputProfile(profile, false).config;
    fixture->configure();
    fixture->arm();
    fixture->holdKey(0xCB, true);
    fixture->holdKey(0xCD, true);
    const EvaluateResult result = fixture->step();
    QNP_CHECK((result.mask & (kCompactLeft | kCompactRight)) == 0);
    QNP_CHECK(!result.physicalNeutralBeforeSocd);
}

QNP_TEST(BindingsSafelyUnavailableSource) {
    auto fixture = std::make_unique<Fixture>();
    fixture->config = axisConfig();
    fixture->configure();
    fixture->controllerHealthy = false;
    fixture->arm();
    const EvaluateResult result = fixture->step();
    QNP_CHECK(result.physicalNeutralBeforeSocd);
    QNP_CHECK(result.keyboardGate == SourceGateState::Armed);
    QNP_CHECK(result.controllerGate == SourceGateState::Disconnected);
    fixture->holdKey(0x2C, true);
    QNP_CHECK((fixture->step().mask & kCompactA) != 0);
}
