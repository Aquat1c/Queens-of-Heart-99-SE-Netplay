#include "TestHarness.h"

#include "input/DeviceIdentity.h"
#include "input/InputCompiler.h"
#include "input/InputProfile.h"
#include "input/ControllerPreview.h"

#include <cstring>
#include "input/DeviceMigration.h"

using namespace qinput;

namespace {

DeviceCandidate candidate(const char* guid, uint16_t vendor,
                          uint16_t product, uint16_t version,
                          const char* name) {
    DeviceCandidate result{};
    std::snprintf(result.guid, sizeof(result.guid), "%s", guid);
    result.vendor = vendor;
    result.product = product;
    result.version = version;
    std::snprintf(result.normalizedName, sizeof(result.normalizedName),
                  "%s", name);
    return result;
}

CompiledDeviceMatcher matcher(const char* guid, uint16_t vendor,
                              uint16_t product, uint16_t version,
                              const char* name) {
    CompiledDeviceMatcher result{};
    std::snprintf(result.guid, sizeof(result.guid), "%s", guid);
    result.vendor = vendor;
    result.product = product;
    result.version = version;
    std::snprintf(result.normalizedName, sizeof(result.normalizedName),
                  "%s", name);
    return result;
}

}

QNP_TEST(IdentitySerialReconnect) {
    CompiledDeviceMatcher m = matcher("guid", 1, 2, 3, "Pad");
    m.hasSerial = 1;
    m.serialHash = identityStringHash("SN123");
    DeviceCandidate candidates[2] = {
        candidate("guid", 1, 2, 3, "Pad"),
        candidate("guid", 1, 2, 3, "Pad"),
    };
    candidates[1].hasSerial = true;
    candidates[1].serialHash = identityStringHash("SN123");
    candidates[1].liveHandle = 99;
    const IdentityResolution result =
        resolveDeviceIdentity(m, candidates, 2);
    QNP_CHECK(result.outcome == IdentityOutcome::Unique);
    QNP_CHECK(result.candidateIndex == 1);
}

QNP_TEST(IdentityDuplicatesAmbiguous) {
    const CompiledDeviceMatcher m = matcher("guid", 1, 2, 3, "Pad");
    DeviceCandidate candidates[2] = {
        candidate("guid", 1, 2, 3, "Pad"),
        candidate("guid", 1, 2, 3, "Pad"),
    };
    QNP_CHECK(resolveDeviceIdentity(m, candidates, 2).outcome
              == IdentityOutcome::Ambiguous);
    DeviceCandidate reversed[2] = {candidates[1], candidates[0]};
    QNP_CHECK(resolveDeviceIdentity(m, reversed, 2).outcome
              == IdentityOutcome::Ambiguous);
}

QNP_TEST(IdentityClassFallbackUnique) {
    const CompiledDeviceMatcher m = matcher("guid", 1, 2, 3, "Pad");
    DeviceCandidate candidates[2] = {
        candidate("guid", 1, 2, 3, "Pad"),
        candidate("other", 4, 5, 6, "Other"),
    };
    const IdentityResolution result =
        resolveDeviceIdentity(m, candidates, 2);
    QNP_CHECK(result.outcome == IdentityOutcome::Unique);
    QNP_CHECK(result.candidateIndex == 0);
}

QNP_TEST(IdentityRouteSurvivesABackendRename) {
    CompiledDeviceMatcher m =
        matcher("030000000d0f0000c100000072056800", 0x0F0D, 0x00C1, 0x0572,
                "Nintendo Switch Pro Controller");
    m.hasPathFingerprint = 1;
    m.pathFingerprint = 0x6FFA13768921428Full;

    DeviceCandidate renamed =
        candidate("0300cb1c0d0f0000c100000072056800", 0x0F0D, 0x00C1, 0x0572,
                  "SWITCH CO.,LTD. Pro Controller");
    renamed.hasPathFingerprint = true;
    renamed.pathFingerprint = 0x6FFA13768921428Full;

    const IdentityResolution result = resolveDeviceIdentity(m, &renamed, 1);
    QNP_CHECK(result.outcome == IdentityOutcome::Unique);
    QNP_CHECK(result.tier == IdentityMatchTier::PathFingerprint);

    DeviceCandidate other =
        candidate("0300c6b1790000001100000000000000", 0x0079, 0x0011, 0x0000,
                  "SWITCH CO.,LTD. USB Gamepad");
    other.hasPathFingerprint = true;
    other.pathFingerprint = 0x6FFA13768921428Full;
    QNP_CHECK(resolveDeviceIdentity(m, &other, 1).outcome
              != IdentityOutcome::Unique);

    DeviceCandidate versionless = renamed;
    versionless.version = 0;
    const IdentityResolution loose =
        resolveDeviceIdentity(m, &versionless, 1);
    QNP_CHECK(loose.outcome == IdentityOutcome::Unique);
    QNP_CHECK(loose.tier == IdentityMatchTier::PathFingerprint);

    DeviceCandidate wrongVersion = renamed;
    wrongVersion.version = 0x0100;
    QNP_CHECK(resolveDeviceIdentity(m, &wrongVersion, 1).tier
              != IdentityMatchTier::PathFingerprint);
}

QNP_TEST(IdentityRouteFingerprint) {
    CompiledDeviceMatcher m = matcher("guid", 1, 2, 3, "Pad");
    m.hasPathFingerprint = 1;
    m.pathFingerprint = 0x1234;
    DeviceCandidate candidates[2] = {
        candidate("guid", 1, 2, 3, "Pad"),
        candidate("guid", 1, 2, 3, "Pad"),
    };
    candidates[0].hasPathFingerprint = true;
    candidates[0].pathFingerprint = 0x9999;
    candidates[1].hasPathFingerprint = true;
    candidates[1].pathFingerprint = 0x1234;
    const IdentityResolution result =
        resolveDeviceIdentity(m, candidates, 2);
    QNP_CHECK(result.outcome == IdentityOutcome::Unique);
    QNP_CHECK(result.candidateIndex == 1);
    candidates[0].pathFingerprint = 0x1234;
    QNP_CHECK(resolveDeviceIdentity(m, candidates, 2).outcome
              == IdentityOutcome::Ambiguous);
}

QNP_TEST(IdentityWrongClassRejected) {
    const CompiledDeviceMatcher m = matcher("guid", 1, 2, 3, "Stick");
    DeviceCandidate candidates[1] = {
        candidate("guid", 7, 8, 3, "Stick"),
    };
    QNP_CHECK(resolveDeviceIdentity(m, candidates, 1).outcome
              == IdentityOutcome::NoCandidate);
}

QNP_TEST(IdentityCrossBackendVidPidResolves) {
    const CompiledDeviceMatcher m = matcher(
        "03001e6a250900006888000000000000", 0x0925, 0x8868, 0x0100,
        "WiseGroup.,Ltd PS/SS/DC Joypad to USB BOX");
    DeviceCandidate candidates[2] = {
        candidate("006E0F0D000000000000504944564944", 0x0F0D, 0x006E, 0,
                  "HORIPAD 4 "),
        candidate("88680925000000000000504944564944", 0x0925, 0x8868, 0,
                  "PS/SS/DC Joypad to USB BOX"),
    };
    const IdentityResolution result =
        resolveDeviceIdentity(m, candidates, 2);
    QNP_CHECK(result.outcome == IdentityOutcome::Unique);
    QNP_CHECK(result.candidateIndex == 1);
    QNP_CHECK(result.tier == IdentityMatchTier::VendorProduct);
}

QNP_TEST(IdentityCrossBackendDuplicatesStayAmbiguous) {
    const CompiledDeviceMatcher m = matcher(
        "03001e6a250900006888000000000000", 0x0925, 0x8868, 0x0100,
        "WiseGroup.,Ltd PS/SS/DC Joypad to USB BOX");
    DeviceCandidate candidates[2] = {
        candidate("88680925000000000000504944564944", 0x0925, 0x8868, 0,
                  "PS/SS/DC Joypad to USB BOX"),
        candidate("88680925000000000000504944564944", 0x0925, 0x8868, 0,
                  "PS/SS/DC Joypad to USB BOX"),
    };
    QNP_CHECK(resolveDeviceIdentity(m, candidates, 2).outcome
              == IdentityOutcome::Ambiguous);
}

QNP_TEST(IdentityZeroVidPidNeverMatchesByClassAlone) {
    const CompiledDeviceMatcher m = matcher("guidA", 0, 0, 1, "Weird");
    DeviceCandidate candidates[1] = {
        candidate("guidB", 0, 0, 2, "Other Weird"),
    };
    QNP_CHECK(resolveDeviceIdentity(m, candidates, 1).outcome
              == IdentityOutcome::NoCandidate);
}

QNP_TEST(IdentityNoCandidates) {
    const CompiledDeviceMatcher m = matcher("guid", 1, 2, 3, "Pad");
    QNP_CHECK(resolveDeviceIdentity(m, nullptr, 0).outcome
              == IdentityOutcome::NoCandidate);
}

QNP_TEST(CapabilityShapeNeverParticipatesInIdentity) {
    const CompiledDeviceMatcher wanted =
        matcher("03000000", 0x2341, 0x8036, 0x0100, "LLC Arduino Leonardo");

    DeviceCandidate one =
        candidate("03000000", 0x2341, 0x8036, 0x0100, "LLC Arduino Leonardo");
    one.probeOpened = true;
    one.axisCount = 2;
    one.buttonCount = 12;
    one.hatCount = 1;

    DeviceCandidate reshaped = one;
    reshaped.buttonCount = 13;
    reshaped.isGamepad = true;

    const IdentityResolution first =
        resolveDeviceIdentity(wanted, &one, 1u);
    const IdentityResolution second =
        resolveDeviceIdentity(wanted, &reshaped, 1u);
    QNP_CHECK(first.outcome == IdentityOutcome::Unique);
    QNP_CHECK(second.outcome == IdentityOutcome::Unique);

    DeviceCandidate pair[2] = { one, one };
    pair[0].liveHandle = 4;
    pair[1].liveHandle = 5;
    const IdentityResolution tied = resolveDeviceIdentity(wanted, pair, 2u);
    QNP_CHECK(tied.outcome == IdentityOutcome::Ambiguous);
}

QNP_TEST(UsableControlsRequireSomethingToBind) {
    DeviceCandidate opened{};
    opened.probeOpened = true;
    QNP_CHECK(!candidateHasUsableControls(opened));

    opened.buttonCount = 1;
    QNP_CHECK(candidateHasUsableControls(opened));

    DeviceCandidate hatOnly{};
    hatOnly.probeOpened = true;
    hatOnly.hatCount = 1;
    QNP_CHECK(candidateHasUsableControls(hatOnly));

    DeviceCandidate axisOnly{};
    axisOnly.probeOpened = true;
    axisOnly.axisCount = 2;
    QNP_CHECK(candidateHasUsableControls(axisOnly));
}

QNP_TEST(SerialHashFromTheProfileReachesTheCompiledMatcher) {
    InputProfile profile{};
    profile.id = "pad";
    profile.name = "pad";
    profile.sourceMode = CompiledSourceMode::Controller;
    profile.device.guid = "03000000";
    profile.device.vendor = 0x2341;
    profile.device.product = 0x8036;
    profile.device.version = 0x0100;
    profile.device.normalizedName = "LLC Arduino Leonardo";
    profile.device.serialHash = 0xABCDEF0123456789ull;
    for (int action = 0; action < 8; ++action) {
        ProfileBinding binding{};
        binding.type = CompiledBindingType::ProbeButton;
        binding.probeKind = ProbeKind::RawButton;
        binding.token = static_cast<uint8_t>(action);
        profile.actions[action].push_back(binding);
    }

    const CompileResult compiled =
        compileInputProfile(profile, false);
    QNP_REQUIRE(compiled.error.empty());
    QNP_CHECK(compiled.config.device.hasSerial == 1);
    QNP_CHECK(compiled.config.device.serialHash == 0xABCDEF0123456789ull);

    DeviceCandidate right = candidate("03000000", 0x2341, 0x8036, 0x0100,
                                      "LLC Arduino Leonardo");
    right.hasSerial = true;
    right.serialHash = 0xABCDEF0123456789ull;
    DeviceCandidate wrong = right;
    wrong.serialHash = 0x1111111111111111ull;
    wrong.liveHandle = 9;

    const DeviceCandidate pair[2] = { wrong, right };
    const IdentityResolution resolution =
        resolveDeviceIdentity(compiled.config.device, pair, 2u);
    QNP_CHECK(resolution.outcome == IdentityOutcome::Unique);
    QNP_CHECK(resolution.candidateIndex == 1u);
}

QNP_TEST(IdentificationIsRequiredOnlyWhenNoKeyCanSeparateThem) {
    PreviewDevice a{};
    a.name = "LLC Arduino Leonardo";
    a.guid = "03000000";
    a.vendor = 0x2341;
    a.product = 0x8036;
    a.version = 0x0100;
    PreviewDevice b = a;

    QNP_CHECK(ControllerPreview::indistinguishable(a, b));

    a.hasSerial = true; a.serialHash = 0x1111;
    b.hasSerial = true; b.serialHash = 0x2222;
    QNP_CHECK(!ControllerPreview::indistinguishable(a, b));
    b.serialHash = 0x1111;
    QNP_CHECK(ControllerPreview::indistinguishable(a, b));

    b.hasSerial = false;
    QNP_CHECK(!ControllerPreview::indistinguishable(a, b));

    a.hasSerial = false;
    b.hasSerial = false;
    a.hasPathFingerprint = true; a.pathFingerprint = 0xAAAA;
    b.hasPathFingerprint = true; b.pathFingerprint = 0xBBBB;
    QNP_CHECK(!ControllerPreview::indistinguishable(a, b));
    b.pathFingerprint = 0xAAAA;
    QNP_CHECK(ControllerPreview::indistinguishable(a, b));

    a.hasPathFingerprint = false;
    b.hasPathFingerprint = false;
    b.product = 0x9999;
    QNP_CHECK(!ControllerPreview::indistinguishable(a, b));
}

QNP_TEST(SteamVirtualControllersAreIdentifiedForReportingOnly) {
    QNP_CHECK(qinput::isSteamVirtualController(0x28DE, 0x11FF));
    QNP_CHECK(qinput::isSteamVirtualController(0x28DE, 0x1102));

    QNP_CHECK(!qinput::isSteamVirtualController(0x2C22, 0x2303));
    QNP_CHECK(!qinput::isSteamVirtualController(0x054C, 0x09CC));
    QNP_CHECK(!qinput::isSteamVirtualController(0x045E, 0x028E));

    QNP_CHECK(qinput::isValveInputDevice(0x28DE));
    QNP_CHECK(!qinput::isValveInputDevice(0x2C22));
    QNP_CHECK(!qinput::isSteamVirtualController(0x28DE, 0x1205));
    QNP_CHECK(qinput::isValveInputDevice(0x28DE));

    QNP_CHECK(!qinput::isSteamVirtualController(0, 0));
    QNP_CHECK(!qinput::isValveInputDevice(0));
}

QNP_TEST(DeviceMigrationMovesAProfileOnlyWhenTheAnswerIsUnambiguous) {
    using namespace qinput;

    BoundDeviceKey bound{};
    bound.hasSerialHash = true;
    bound.serialHash = 0xABCDEF01u;
    bound.hasPathFingerprint = true;
    bound.pathFingerprint = 0x1111u;
    bound.guid = "OLD-GUID";

    DeviceRequirements need{};
    need.axes = 2;
    need.buttons = 6;
    need.hats = 1;

    auto pad = [](uint16_t axes, uint16_t buttons, uint16_t hats) {
        MigrationCandidate c{};
        c.selectable = true;
        c.axes = axes;
        c.buttons = buttons;
        c.hats = hats;
        c.guid = "OLD-GUID";
        return c;
    };

    {
        MigrationCandidate same = pad(4, 12, 1);
        same.hasSerialHash = true;
        same.serialHash = 0xABCDEF01u;
        same.hasPathFingerprint = true;
        same.pathFingerprint = 0x9999u;
        const MigrationPlan plan = planDeviceMigration(bound, need, &same, 1);
        QNP_CHECK(plan.verdict == MigrationVerdict::BoundDevicePresent);
    }

    {
        MigrationCandidate other = pad(4, 12, 1);
        other.hasSerialHash = true;
        other.serialHash = 0x22222222u;
        other.hasPathFingerprint = true;
        other.pathFingerprint = 0x1111u;
        const MigrationPlan plan = planDeviceMigration(bound, need, &other, 1);
        QNP_CHECK(plan.verdict == MigrationVerdict::Migrate);
        QNP_CHECK(plan.candidateIndex == 0u);
    }

    {
        MigrationCandidate replacement = pad(4, 12, 1);
        const MigrationPlan plan =
            planDeviceMigration(bound, need, &replacement, 1);
        QNP_CHECK(plan.verdict == MigrationVerdict::Migrate);
        QNP_CHECK(plan.compatibleCount == 1u);
    }

    {
        MigrationCandidate different = pad(4, 12, 1);
        different.guid = "DIFFERENT-GUID";
        const MigrationPlan plan =
            planDeviceMigration(bound, need, &different, 1);
        QNP_CHECK(plan.verdict == MigrationVerdict::NoCompatibleCandidate);
        QNP_CHECK(plan.compatibleCount == 0u);
    }

    {
        MigrationCandidate pair[2] = {pad(4, 12, 1), pad(4, 12, 1)};
        const MigrationPlan plan = planDeviceMigration(bound, need, pair, 2);
        QNP_CHECK(plan.verdict == MigrationVerdict::Ambiguous);
        QNP_CHECK(plan.compatibleCount == 2u);
    }

    {
        MigrationCandidate small = pad(2, 4, 1);
        const MigrationPlan plan =
            planDeviceMigration(bound, need, &small, 1);
        QNP_CHECK(plan.verdict == MigrationVerdict::NoCompatibleCandidate);
        QNP_CHECK(plan.compatibleCount == 0u);
    }

    {
        MigrationCandidate dead = pad(4, 12, 1);
        dead.selectable = false;
        const MigrationPlan plan = planDeviceMigration(bound, need, &dead, 1);
        QNP_CHECK(plan.verdict == MigrationVerdict::NoCandidate);
    }

    {
        DeviceRequirements standardized{};
        standardized.needsStandardized = true;
        MigrationCandidate rawStick = pad(4, 12, 1);
        rawStick.guid = "NEW-GUID";
        rawStick.isGamepad = false;
        MigrationPlan plan =
            planDeviceMigration(bound, standardized, &rawStick, 1);
        QNP_CHECK(plan.verdict == MigrationVerdict::NoCompatibleCandidate);
        MigrationCandidate mapped = pad(4, 12, 1);
        mapped.guid = "NEW-GUID";
        mapped.isGamepad = true;
        plan = planDeviceMigration(bound, standardized, &mapped, 1);
        QNP_CHECK(plan.verdict == MigrationVerdict::Migrate);
    }

    {
        const MigrationPlan plan =
            planDeviceMigration(bound, need, nullptr, 0);
        QNP_CHECK(plan.verdict == MigrationVerdict::NoCandidate);
    }

    {
        BoundDeviceKey unbound{};
        MigrationCandidate any = pad(4, 12, 1);
        const MigrationPlan plan =
            planDeviceMigration(unbound, need, &any, 1);
        QNP_CHECK(plan.verdict == MigrationVerdict::BoundDevicePresent);
    }
}

QNP_TEST(DeviceRequirementsAreDerivedFromTheBoundProbes) {
    using namespace qinput;
    CompiledProbe probes[4] = {};
    probes[0].kind = ProbeKind::RawButton;  probes[0].token = 5;
    probes[1].kind = ProbeKind::RawAxis;    probes[1].token = 1;
    probes[2].kind = ProbeKind::RawHat;     probes[2].token = 0;
    probes[3].kind = ProbeKind::RawButton;  probes[3].token = 2;
    const DeviceRequirements need = deviceRequirementsFromProbes(probes, 4);
    QNP_CHECK(!need.needsStandardized);
    QNP_CHECK(need.buttons == 6);
    QNP_CHECK(need.axes == 2);
    QNP_CHECK(need.hats == 1);

    CompiledProbe mixed[2] = {};
    mixed[0].kind = ProbeKind::RawButton;   mixed[0].token = 1;
    mixed[1].kind = ProbeKind::GamepadAxis; mixed[1].token = 3;
    QNP_CHECK(deviceRequirementsFromProbes(mixed, 2).needsStandardized);

    QNP_CHECK(deviceRequirementsFromProbes(probes, 0).buttons == 0);
    QNP_CHECK(deviceRequirementsFromProbes(nullptr, 4).buttons == 0);
}
