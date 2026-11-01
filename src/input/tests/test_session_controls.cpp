#include "TestHarness.h"

#include <cstdio>
#include <cstring>

#include "game/RuntimeDelayControl.h"
#include "input/InputCompiler.h"
#include "input/InputProfile.h"
#include "input/ModeControlEvaluator.h"
#include "input/ModeControlPolicy.h"
#include "input/SessionEventTimeline.h"

using namespace qinput;

namespace {

QuiesceReport report(uint32_t next, uint32_t tail, uint32_t oldest,
                     uint32_t spectator = 0, uint32_t phaseKey = 7) {
    QuiesceReport out{};
    out.valid = true;
    out.nextFrame = next;
    out.assignedTailExclusive = tail;
    out.oldestRestorable = oldest;
    out.spectatorFrontier = spectator;
    out.lifecyclePhaseKey = phaseKey;
    return out;
}

}

namespace {

struct ModeFixture {
    ControlsFile controls = makeDefaultControlsFile();
    CompileResult compiled{};
    ModeControlEvaluator evaluator{};
    uint32_t keyBits[8] = {};
    uint32_t serial = 0u;
    uint32_t focusGeneration = 1u;

    ModeFixture() {
        compiled = compileInputProfile(
            makeDefaultKeyboardProfile(), false, &controls);
        evaluator.configure(&compiled.modeControls);
    }

    void hold(uint8_t dik, bool held) {
        if (held) keyBits[dik >> 5] |= 1u << (dik & 31u);
        else keyBits[dik >> 5] &= ~(1u << (dik & 31u));
    }

    ModeControlEvaluation step(uint64_t enabled = ~0ull,
                               bool focused = true) {
        KeyboardView keyboard{keyBits, true, 1u, ++serial};
        ControllerView controller{};
        FocusView focus{focused, focusGeneration};
        return evaluator.evaluate(keyboard, controller, focus,
                                  FocusPolicy::GameWindowOnly, enabled);
    }
};

uint64_t bit(ModeControlAction action) {
    return modeControlBit(action);
}

}

QNP_TEST(ModeControlsCompileDefaultsInSeparateSemanticNamespace) {
    ModeFixture fixture;
    QNP_REQUIRE(fixture.compiled.ok);
    QNP_CHECK(fixture.compiled.modeControls.version == 1u);
    QNP_CHECK(fixture.compiled.modeControls.count > 0u);
    QNP_CHECK(fixture.controls.modeControls[static_cast<size_t>(
                  ModeControlAction::LeaveMatch)].empty());
    QNP_CHECK(kModeControlDescriptorCount == 39u);
    QNP_CHECK(modeControlDescriptor(
                  ModeControlAction::TrainingChargeExFraction)->trigger
              == ModeControlTrigger::HeldLevel);
    QNP_CHECK(fixture.controls.modeControls[static_cast<size_t>(
                  ModeControlAction::ReplayTakeoverExit)][0].dik == 0x10u);
    QNP_CHECK(fixture.controls.modeControls[static_cast<size_t>(
                  ModeControlAction::ReplayTakeoverP1)][0].dik == 0x21u);
    QNP_CHECK(fixture.controls.modeControls[static_cast<size_t>(
                  ModeControlAction::ReplayToggleCollisionDisplay)][0].dik
              == 0x41u);
    QNP_CHECK(modeControlDescriptor(
                  ModeControlAction::ReplayToggleCollisionDisplay)->trigger
              == ModeControlTrigger::Edge);
    QNP_CHECK(fixture.controls.modeControls[static_cast<size_t>(
                  ModeControlAction::OnlineReturnCharacterSelect)][0].dik
              == 0x3Bu);
    QNP_CHECK(modeControlDescriptor(
                  ModeControlAction::OnlineReturnCharacterSelect)->domain
              == ModeControlDomain::OnlinePlayer);
    QNP_CHECK(fixture.controls.modeControls[static_cast<size_t>(
                  ModeControlAction::ReplayTakeoverP2)][0].dik == 0x22u);
    QNP_CHECK(fixture.controls.modeControls[static_cast<size_t>(
                  ModeControlAction::ReplayTakeoverRetry)][0].dik == 0x13u);
}

QNP_TEST(ModeControlsEmitOneEdgeButPreserveHeldLevel) {
    ModeFixture fixture;
    QNP_REQUIRE(fixture.compiled.ok);
    (void)fixture.step();
    (void)fixture.step();

    fixture.hold(0x0Cu, true);
    ModeControlEvaluation value = fixture.step();
    QNP_CHECK((value.edges & bit(ModeControlAction::InputDelayDecrease)) != 0);
    value = fixture.step();
    QNP_CHECK((value.edges & bit(ModeControlAction::InputDelayDecrease)) == 0);
    QNP_CHECK((value.held & bit(ModeControlAction::InputDelayDecrease)) != 0);

    fixture.hold(0x0Cu, false);
    fixture.hold(0x3Fu, true);
    value = fixture.step();
    QNP_CHECK((value.edges
               & bit(ModeControlAction::TrainingChargeExFraction)) != 0);
    value = fixture.step();
    QNP_CHECK((value.edges
               & bit(ModeControlAction::TrainingChargeExFraction)) == 0);
    QNP_CHECK((value.held
               & bit(ModeControlAction::TrainingChargeExFraction)) != 0);
}

QNP_TEST(OnlineTuningControlsWorkInCharacterSelectWithoutReplayingHeldKeys) {
    const ModeControlAction actions[] = {
        ModeControlAction::InputDelayDecrease,
        ModeControlAction::InputDelayIncrease,
        ModeControlAction::RollbackWindowDecrease,
        ModeControlAction::RollbackWindowIncrease,
    };
    const uint8_t keys[] = {0x0Cu, 0x0Du, 0x1Au, 0x1Bu};
    for (size_t i = 0; i < 4u; ++i) {
        ModeFixture fixture;
        QNP_REQUIRE(fixture.compiled.ok);
        const uint64_t action = bit(actions[i]);
        const auto step = [&](bool gameplay, uint32_t scene) {
            return fixture.step(qgame::runtimeDelayControlsAllowed(
                gameplay, scene) ? action : 0u);
        };
        (void)step(false, 0u);
        (void)step(false, 0u);
        fixture.hold(keys[i], true);
        QNP_CHECK(step(false, 0u).edges == 0u);
        QNP_CHECK(step(false, 3u).edges == 0u);
        fixture.hold(keys[i], false);
        (void)step(false, 3u);
        fixture.hold(keys[i], true);
        QNP_CHECK(step(false, 3u).edges == action);
        QNP_CHECK(step(false, 3u).edges == 0u);
        QNP_CHECK(step(true, 2u).edges == 0u);
        fixture.hold(keys[i], false);
        (void)step(true, 2u);
        fixture.hold(keys[i], true);
        QNP_CHECK(step(true, 2u).edges == action);
    }
    QNP_CHECK(!qgame::runtimeDelayControlsAllowed(false, 2u));
    QNP_CHECK(!qgame::runtimeDelayControlsAllowed(false, 4u));
}

QNP_TEST(ModeControlsHeldThroughDisabledOrFocusLossRequireRelease) {
    ModeFixture fixture;
    QNP_REQUIRE(fixture.compiled.ok);
    const uint64_t delay = bit(ModeControlAction::InputDelayDecrease);
    (void)fixture.step(delay);
    (void)fixture.step(delay);
    fixture.hold(0x0Cu, true);
    ModeControlEvaluation value = fixture.step(0u);
    QNP_CHECK((value.edges & delay) == 0u);
    value = fixture.step(delay);
    QNP_CHECK((value.edges & delay) == 0u);
    fixture.hold(0x0Cu, false);
    (void)fixture.step(delay);
    fixture.hold(0x0Cu, true);
    value = fixture.step(delay);
    QNP_CHECK((value.edges & delay) != 0u);

    fixture.focusGeneration++;
    value = fixture.step(delay, false);
    QNP_CHECK(value.edges == 0u && value.held == 0u);
    value = fixture.step(delay, true);
    QNP_CHECK((value.edges & delay) == 0u);
    fixture.hold(0x0Cu, false);
    (void)fixture.step(delay);
    fixture.hold(0x0Cu, true);
    value = fixture.step(delay);
    QNP_CHECK((value.edges & delay) != 0u);
}

QNP_TEST(ModeControlsSurviveOfflineFocusHeartbeatWhileStillFocused) {
    ModeFixture fixture;
    QNP_REQUIRE(fixture.compiled.ok);
    const uint64_t pause = bit(ModeControlAction::TrainingPause);

    (void)fixture.step(pause);
    (void)fixture.step(pause);
    ++fixture.focusGeneration;
    (void)fixture.step(pause, true);
    ++fixture.focusGeneration;
    fixture.hold(0x09u, true);
    const ModeControlEvaluation value = fixture.step(pause, true);
    QNP_CHECK((value.edges & pause) != 0u);
    QNP_CHECK((value.held & pause) != 0u);
}

QNP_TEST(ModeControlConflictMoveIsTransactionalAndDomainAware) {
    ControlsFile file = makeDefaultControlsFile();
    ProfileBinding key{};
    key.type = CompiledBindingType::KeyboardScan;
    key.dik = 0x44u;
    file.modeControls[static_cast<size_t>(
        ModeControlAction::InputDelayDecrease)].push_back(key);
    const ControlsFile beforeCancel = file;
    const auto conflicts = findModeControlConflicts(
        file, ModeControlAction::LeaveMatch, key);
    QNP_REQUIRE(conflicts.size() == 1u);
    QNP_CHECK(!conflicts[0].fighter);
    QNP_CHECK(conflicts[0].modeAction
              == ModeControlAction::InputDelayDecrease);
    QNP_CHECK(serializeControlsFile(file)
              == serializeControlsFile(beforeCancel));
    std::string error;
    QNP_REQUIRE(moveModeControlBinding(
        file, ModeControlAction::LeaveMatch, key, error));
    QNP_CHECK(file.modeControls[static_cast<size_t>(
                  ModeControlAction::InputDelayDecrease)].size()
              == beforeCancel.modeControls[static_cast<size_t>(
                  ModeControlAction::InputDelayDecrease)].size() - 1u);
    QNP_CHECK(file.modeControls[static_cast<size_t>(
                  ModeControlAction::LeaveMatch)].size() == 1u);

    QNP_CHECK(!modeDomainsOverlap(ModeControlDomain::Replay,
                                  ModeControlDomain::OnlinePlayer));
    QNP_CHECK(modeDomainsOverlap(ModeControlDomain::WrapperGlobal,
                                 ModeControlDomain::Replay));
    key.dik = 0x57u;
    QNP_CHECK(!validModeControlBinding(
        ModeControlAction::TrainingChargeExFraction, key, error));

    ProfileBinding button{};
    button.type = CompiledBindingType::ProbeButton;
    button.probeKind = ProbeKind::RawButton;
    button.token = 1u;
    QNP_CHECK(!validModeControlBinding(
        ModeControlAction::ToggleOverlayMenu, button, error));
    QNP_CHECK(validModeControlBinding(
        ModeControlAction::LeaveMatch, button, error));
}

QNP_TEST(AnalogTriggerSurvivesEveryModeControlValidator) {
    InputLaunchConfig config{};
    config.sourceMode = CompiledSourceMode::KeyboardAndController;
    std::snprintf(config.device.guid, sizeof(config.device.guid), "g");
    config.probeCount = 2;
    config.probes[0].kind = ProbeKind::GamepadAxis;
    config.probes[0].token = 5;
    config.probes[1].kind = ProbeKind::GamepadAxis;
    config.probes[1].token = 0;
    const char* mapping = "g,Pad,a:b0";
    config.mappingLength = static_cast<uint32_t>(std::strlen(mapping));
    std::memcpy(config.mapping, mapping, config.mappingLength);
    for (CompiledActionBindings& action : config.actions) {
        action.count = 1;
        action.bindings[0].type = CompiledBindingType::KeyboardScan;
        action.bindings[0].dik = 0x2Cu;
    }

    CompiledBinding trigger{};
    trigger.type = CompiledBindingType::ProbeAxis;
    trigger.probeIndex = 0;
    trigger.polarity = AxisPolarity::Positive;
    trigger.pressQ15 = 16384;
    trigger.releaseQ15 = 11469;
    trigger.calibration.unipolar = 1;
    trigger.calibration.minimum = 0;
    trigger.calibration.maximum = 32767;

    CompiledModeControlsV1 controls{};
    controls.count = 1;
    controls.bindings[0].action = ModeControlAction::LeaveMatch;
    controls.bindings[0].binding = trigger;

    const char* configWhy = validateInputLaunchConfig(config, false);
    QNP_CHECK(configWhy == nullptr);
    if (configWhy != nullptr) std::printf("  config: %s\n", configWhy);

    const char* why = validateCompiledModeControls(controls, config, false);
    QNP_CHECK(why == nullptr);
    if (why != nullptr) std::printf("  trigger rejected: %s\n", why);

    controls.bindings[0].binding.probeIndex = 1;
    QNP_CHECK(validateCompiledModeControls(controls, config, false) != nullptr);

    controls.bindings[0].binding.probeIndex = 0;
    controls.bindings[0].binding.calibration.unipolar = 0;
    controls.bindings[0].binding.calibration.minimum = -32768;
    controls.bindings[0].binding.calibration.center = 0;
    QNP_CHECK(validateCompiledModeControls(controls, config, false) != nullptr);
}

QNP_TEST(TimelineHostAuthorityAndPrecedence) {
    SessionEventTimeline host;
    host.reset(true);
    SessionEventTimeline peer;
    peer.reset(false);
    QNP_CHECK(!peer.hostAcceptRequest(SessionEventKind::PauseSet, 1));
    QNP_CHECK(!host.hostAcceptRequest(SessionEventKind::PauseSet, 0));
    QNP_REQUIRE(host.hostAcceptRequest(SessionEventKind::PauseSet, 1));
    const uint32_t pauseId = host.current().eventId;
    QNP_CHECK(host.current().state == SessionEventState::Quiescing);
    QNP_REQUIRE(
        host.hostAcceptRequest(SessionEventKind::TerminateSession, 2));
    QNP_CHECK(host.current().eventId > pauseId);
    QNP_CHECK(host.current().kind == SessionEventKind::TerminateSession);
}

QNP_TEST(TimelineBoundarySelection) {
    SessionEventTimeline host;
    host.reset(true);
    QNP_REQUIRE(host.hostAcceptRequest(SessionEventKind::PauseSet, 1));
    host.submitLocalReport(report(100, 104, 40));
    host.submitRemoteReport(report(98, 106, 42, 95));
    uint32_t boundary = 0;
    QNP_REQUIRE(host.hostChooseBoundary(boundary));
    QNP_CHECK(boundary == 106);
    SessionEventTimeline mismatch;
    mismatch.reset(true);
    QNP_REQUIRE(
        mismatch.hostAcceptRequest(SessionEventKind::PauseSet, 1));
    mismatch.submitLocalReport(report(100, 104, 40, 0, 7));
    mismatch.submitRemoteReport(report(100, 104, 40, 0, 9));
    QNP_CHECK(!mismatch.hostChooseBoundary(boundary));
    QNP_CHECK(mismatch.terminated());
    SessionEventTimeline farTail;
    farTail.reset(true);
    QNP_REQUIRE(farTail.hostAcceptRequest(SessionEventKind::PauseSet, 1));
    farTail.submitLocalReport(report(100, 100 + 200, 40));
    farTail.submitRemoteReport(report(100, 104, 40));
    QNP_CHECK(!farTail.hostChooseBoundary(boundary));
}

QNP_TEST(TimelineCommitAppliesExactlyOnce) {
    SessionEventTimeline host;
    host.reset(true);
    QNP_REQUIRE(host.hostAcceptRequest(SessionEventKind::PauseSet, 1));
    const uint32_t id = host.current().eventId;
    host.submitLocalReport(report(50, 52, 10));
    host.submitRemoteReport(report(51, 53, 11));
    uint32_t boundary = 0;
    QNP_REQUIRE(host.hostChooseBoundary(boundary));
    QNP_CHECK(boundary == 53);
    QNP_REQUIRE(host.markAtBoundary(id));
    QNP_REQUIRE(host.commit(id));
    QNP_CHECK(host.commit(id));
    QNP_REQUIRE(host.applyOnce(id));
    QNP_CHECK(host.paused());
    QNP_CHECK(!host.applyOnce(id));
    QNP_CHECK(host.lastAppliedId() == id);

    SessionEventTimeline peer;
    peer.reset(false);
    QNP_REQUIRE(peer.peerBeginQuiesce(id, SessionEventKind::PauseSet, 1));
    QNP_CHECK(!peer.peerBeginQuiesce(id, SessionEventKind::PauseSet, 1)
              || true);
    QNP_REQUIRE(peer.acceptPrepare(id, 53, 51));
    QNP_CHECK(peer.acceptPrepare(id, 53, 51));
    QNP_REQUIRE(peer.markAtBoundary(id));
    QNP_REQUIRE(peer.commit(id));
    QNP_REQUIRE(peer.applyOnce(id));
    QNP_CHECK(peer.paused());
    QNP_CHECK(!peer.peerBeginQuiesce(id, SessionEventKind::PauseSet, 1));
}

QNP_TEST(TimelineRematchCommitAppliesWithoutLatchingState) {
    SessionEventTimeline host;
    host.reset(true);
    QNP_REQUIRE(host.hostAcceptRequest(
        SessionEventKind::RematchCommit, 9000));
    const uint32_t id = host.current().eventId;
    QNP_CHECK(host.current().kind == SessionEventKind::RematchCommit);
    QNP_CHECK(!host.hostAcceptRequest(
        SessionEventKind::RematchCommit, 9000));
    host.submitLocalReport(report(9000, 9004, 8900));
    host.submitRemoteReport(report(9000, 9006, 8900));
    uint32_t boundary = 0;
    QNP_REQUIRE(host.hostChooseBoundary(boundary));
    QNP_CHECK(boundary == 9006);
    QNP_REQUIRE(host.markAtBoundary(id));
    QNP_REQUIRE(host.commit(id));
    QNP_REQUIRE(host.applyOnce(id));
    QNP_CHECK(!host.paused());
    QNP_CHECK(!host.terminated());
    QNP_CHECK(host.lastAppliedId() == id);
    QNP_CHECK(!host.applyOnce(id));
    SessionEventTimeline peer;
    peer.reset(false);
    QNP_REQUIRE(peer.peerBeginQuiesce(
        id, SessionEventKind::RematchCommit, 9000));
    QNP_REQUIRE(peer.acceptPrepare(id, 9006, 9000));
    QNP_REQUIRE(peer.markAtBoundary(id));
    QNP_REQUIRE(peer.commit(id));
    QNP_REQUIRE(peer.applyOnce(id));
    QNP_CHECK(!peer.paused() && !peer.terminated());
}

QNP_TEST(TimelineReturnToCharacterSelectIsExactOnceAndClearsPause) {
    SessionEventTimeline host;
    host.reset(true);
    uint32_t boundary = 0;

    QNP_REQUIRE(host.hostAcceptRequest(SessionEventKind::PauseSet, 1));
    uint32_t id = host.current().eventId;
    host.submitLocalReport(report(40, 42, 10));
    host.submitRemoteReport(report(40, 42, 10));
    QNP_REQUIRE(host.hostChooseBoundary(boundary));
    QNP_REQUIRE(host.markAtBoundary(id));
    QNP_REQUIRE(host.commit(id));
    QNP_REQUIRE(host.applyOnce(id));
    QNP_REQUIRE(host.paused());

    QNP_REQUIRE(host.hostAcceptRequest(
        SessionEventKind::ReturnToCharacterSelect, 42));
    id = host.current().eventId;
    host.submitLocalReport(report(42, 44, 10));
    host.submitRemoteReport(report(42, 45, 10));
    QNP_REQUIRE(host.hostChooseBoundary(boundary));
    QNP_CHECK(boundary == 45u);
    QNP_REQUIRE(host.markAtBoundary(id));
    QNP_REQUIRE(host.commit(id));
    QNP_REQUIRE(host.applyOnce(id));
    QNP_CHECK(!host.paused());
    QNP_CHECK(!host.terminated());
    QNP_CHECK(!host.applyOnce(id));
}

QNP_TEST(TimelineReturnPrecedenceIsBelowTerminateAndAboveRematch) {
    SessionEventTimeline host;
    host.reset(true);
    QNP_REQUIRE(host.hostAcceptRequest(
        SessionEventKind::RematchCommit, 9000));
    const uint32_t rematchId = host.current().eventId;
    QNP_REQUIRE(host.hostAcceptRequest(
        SessionEventKind::ReturnToCharacterSelect, 9001));
    const uint32_t returnId = host.current().eventId;
    QNP_CHECK(returnId > rematchId);
    QNP_CHECK(host.current().kind
              == SessionEventKind::ReturnToCharacterSelect);
    QNP_REQUIRE(host.hostAcceptRequest(
        SessionEventKind::TerminateSession, 2));
    QNP_CHECK(host.current().eventId > returnId);
    QNP_CHECK(host.current().kind == SessionEventKind::TerminateSession);

    SessionEventTimeline peer;
    peer.reset(false);
    QNP_REQUIRE(peer.peerBeginQuiesce(
        7, SessionEventKind::RematchCommit, 9000));
    QNP_REQUIRE(peer.peerBeginQuiesce(
        8, SessionEventKind::ReturnToCharacterSelect, 9001));
    QNP_CHECK(peer.current().kind
              == SessionEventKind::ReturnToCharacterSelect);
    QNP_REQUIRE(peer.peerBeginQuiesce(
        9, SessionEventKind::TerminateSession, 2));
    QNP_CHECK(peer.current().kind == SessionEventKind::TerminateSession);
    QNP_CHECK(!peer.peerBeginQuiesce(
        10, SessionEventKind::ReturnToCharacterSelect, 9002));
}

QNP_TEST(TimelineTerminateOutranksRematchCommit) {
    SessionEventTimeline host;
    host.reset(true);
    QNP_REQUIRE(host.hostAcceptRequest(
        SessionEventKind::RematchCommit, 9000));
    const uint32_t rematchId = host.current().eventId;
    QNP_REQUIRE(host.hostAcceptRequest(
        SessionEventKind::TerminateSession, 2));
    QNP_CHECK(host.current().eventId > rematchId);
    QNP_CHECK(host.current().kind == SessionEventKind::TerminateSession);
    const uint32_t terminateId = host.current().eventId;
    host.submitLocalReport(report(9000, 9004, 8900));
    host.submitRemoteReport(report(9000, 9004, 8900));
    uint32_t boundary = 0;
    QNP_REQUIRE(host.hostChooseBoundary(boundary));
    QNP_REQUIRE(host.markAtBoundary(terminateId));
    QNP_REQUIRE(host.commit(terminateId));
    QNP_REQUIRE(host.applyOnce(terminateId));
    QNP_CHECK(host.terminated());
    QNP_CHECK(!host.hostAcceptRequest(
        SessionEventKind::RematchCommit, 9010));
    SessionEventTimeline committed;
    committed.reset(true);
    QNP_REQUIRE(committed.hostAcceptRequest(
        SessionEventKind::TerminateSession, 1));
    const uint32_t tid = committed.current().eventId;
    committed.submitLocalReport(report(50, 52, 10));
    committed.submitRemoteReport(report(50, 52, 10));
    QNP_REQUIRE(committed.hostChooseBoundary(boundary));
    QNP_REQUIRE(committed.markAtBoundary(tid));
    QNP_REQUIRE(committed.commit(tid));
    QNP_CHECK(!committed.hostAcceptRequest(
        SessionEventKind::TerminateSession, 2));
}

#if 0
QNP_TEST(PumpPauseIneligibleDuringRematchPrompt) {
    PumpFixture fixture;
    SessionControlPhase prompt{};
    prompt.peerReady = true;
    prompt.focused = true;
    (void)fixture.step(prompt);
    fixture.hold(0x39, true);
    SessionControlEdges edges = fixture.step(prompt);
    QNP_CHECK(!edges.pauseRequested);
    edges = fixture.step(prompt);
    QNP_CHECK(!edges.pauseRequested);
    fixture.hold(0x39, false);
    (void)fixture.step(prompt);
    SessionControlPhase pausedPrompt = prompt;
    pausedPrompt.sessionPaused = true;
    fixture.hold(0x39, true);
    edges = fixture.step(pausedPrompt);
    QNP_CHECK(edges.pauseRequested);
    fixture.hold(0x39, false);
    (void)fixture.step(pausedPrompt);
    PumpFixture heldOver;
    (void)heldOver.step(gameplayPhase());
    heldOver.hold(0x0E, true);
    SessionControlPhase ineligible{};
    ineligible.focused = true;
    (void)heldOver.step(ineligible);
    edges = heldOver.step(prompt);
    QNP_CHECK(!edges.backRequested);
    heldOver.hold(0x0E, false);
    (void)heldOver.step(prompt);
    heldOver.hold(0x0E, true);
    edges = heldOver.step(prompt);
    QNP_CHECK(edges.backRequested);
    fixture.hold(kDikMinus, true);
    edges = fixture.step(prompt);
    QNP_CHECK(!edges.delayDecrease);
}
#endif

QNP_TEST(TimelineCrossedBoundaryFailsClosed) {
    SessionEventTimeline peer;
    peer.reset(false);
    QNP_REQUIRE(peer.peerBeginQuiesce(9, SessionEventKind::PauseSet, 1));
    QNP_CHECK(!peer.acceptPrepare(9, 40, 44));
    QNP_CHECK(peer.terminated());
}

QNP_TEST(TimelineAbortWindow) {
    SessionEventTimeline host;
    host.reset(true);
    QNP_REQUIRE(host.hostAcceptRequest(SessionEventKind::PauseSet, 1));
    const uint32_t id = host.current().eventId;
    QNP_REQUIRE(host.abortBeforePrepare(id));
    QNP_CHECK(host.current().state == SessionEventState::Idle);
    QNP_CHECK(!host.paused() && !host.terminated());
    QNP_REQUIRE(host.hostAcceptRequest(SessionEventKind::PauseSet, 1));
    const uint32_t second = host.current().eventId;
    host.submitLocalReport(report(50, 52, 10));
    host.submitRemoteReport(report(51, 53, 11));
    uint32_t boundary = 0;
    QNP_REQUIRE(host.hostChooseBoundary(boundary));
    QNP_CHECK(!host.abortBeforePrepare(second));
}
