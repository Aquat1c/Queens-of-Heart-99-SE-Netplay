#include "TestHarness.h"

#include "../../game/DirectBoot.h"
#include "../../game/NetplayLifecyclePolicy.h"
#include "../../game/NetplayPacingDeadline.h"
#include "../../game/RollbackBalancePolicy.h"
#include "../../game/RuntimeDelayControl.h"
#include "../../game/RollbackHealthTelemetry.h"
#include "../../game/NetplayPacingTelemetry.h"
#include "../../game/NetplayPresentationPolicy.h"
#include "../../game/SessionPresentation.h"
#include "../../ipc/IpcConfig.h"
#include "../ConfigurationProvenance.h"

#include <array>
#include <cstring>
#include <string>

QNP_TEST(InputProductionStopsAtEveryExactBarrierAndAtThePrompt) {
    QNP_CHECK(qgame::inputProductionPermitted(true, false, false, false));

    QNP_CHECK(!qgame::inputProductionPermitted(true, true, false, false));
    QNP_CHECK(!qgame::inputProductionPermitted(true, false, true, false));
    QNP_CHECK(!qgame::inputProductionPermitted(false, false, false, false));
    QNP_CHECK(!qgame::inputProductionPermitted(true, false, false, true));

    for (int mask = 1; mask < 8; ++mask) {
        QNP_CHECK(!qgame::inputProductionPermitted(
            true, (mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0));
    }

    QNP_CHECK(!qgame::inputProductionPermitted(
        true, qgame::lifecycleTransitionRequiresExactInput(0, 5, 60),
        false, false));
    QNP_CHECK(!qgame::inputProductionPermitted(
        true, qgame::lifecycleTransitionRequiresExactInput(8, 0, 1),
        false, false));
    QNP_CHECK(qgame::inputProductionPermitted(
        true, qgame::lifecycleTransitionRequiresExactInput(8, 0, 60),
        false, false));
}

QNP_TEST(LifecycleTransitionUsesExactInputOnlyForTheMatchEndLatch) {
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(0, 0, 60));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(0, 4, 60));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(1, 0, 60));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(2, 0, 60));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(3, 0, 60));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(7, 0, 60));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(8, 4, 60));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(20, 0, 60));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(101, 0, 60));
    QNP_CHECK(qgame::lifecycleTransitionRequiresExactInput(0, 5, 60));
    QNP_CHECK(qgame::lifecycleTransitionRequiresExactInput(8, 5, 60));

    QNP_CHECK(qgame::lifecycleTransitionRequiresExactInput(8, 0, 1));
    QNP_CHECK(qgame::lifecycleTransitionRequiresExactInput(8, 0, 0));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(8, 0, 2));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(8, 0, 59));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(3, 0, 0));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(7, 0, 1));
    QNP_CHECK(!qgame::lifecycleTransitionRequiresExactInput(0, 0, 0));
    QNP_CHECK(!qgame::rollbackGameplayPolicyActive(true, 8, 0, 1));
    QNP_CHECK(qgame::rollbackGameplayPolicyActive(true, 8, 0, 2));

    QNP_CHECK(qgame::rollbackGameplayPolicyActive(true, 0, 4, 60));
    QNP_CHECK(qgame::rollbackGameplayPolicyActive(true, 1, 4, 60));
    QNP_CHECK(qgame::rollbackGameplayPolicyActive(true, 8, 4, 60));
    QNP_CHECK(!qgame::rollbackGameplayPolicyActive(true, 0, 5, 60));
    QNP_CHECK(!qgame::rollbackGameplayPolicyActive(false, 0, 0, 60));
}

QNP_TEST(ConfigurationProvenanceLocalTraceRecordsEveryAppliedBoundary) {
    using qnetplay::config_provenance::Stage;
    using qnetplay::config_provenance::Trace;
    using qnetplay::config_provenance::TraceLane;

    Trace local(TraceLane::Local);
    QNP_CHECK(local.current() == Stage::Unknown);
    QNP_CHECK(local.advance(Stage::Default));
    QNP_CHECK(local.advance(Stage::ProvisionalRecommendation));
    QNP_CHECK(local.advance(Stage::LocallySelected));
    QNP_CHECK(local.advance(Stage::SessionCommitted));
    QNP_CHECK(local.advance(Stage::IpcDelivered));
    QNP_CHECK(local.advance(Stage::RuntimeApplied));
    QNP_CHECK(local.current() == Stage::RuntimeApplied);
    QNP_CHECK(!local.advance(Stage::PeerRuntimeAttested));

    Trace provisionalOnly(TraceLane::Local);
    QNP_CHECK(provisionalOnly.advance(Stage::Default));
    QNP_CHECK(provisionalOnly.advance(Stage::ProvisionalRecommendation));
    QNP_CHECK(!provisionalOnly.advance(Stage::SessionCommitted));
}

QNP_TEST(ConfigurationProvenancePeerAdvertisementsCannotBecomeRuntimeApplied) {
    using qnetplay::config_provenance::Stage;
    using qnetplay::config_provenance::Trace;
    using qnetplay::config_provenance::TraceLane;

    Trace caster(TraceLane::Peer);
    QNP_CHECK(caster.advance(Stage::CasterAdvertised));
    QNP_CHECK(caster.advance(Stage::SessionCommitted));
    QNP_CHECK(caster.advance(Stage::IpcDelivered));
    QNP_CHECK(!caster.advance(Stage::RuntimeApplied));
    QNP_CHECK(caster.current() == Stage::IpcDelivered);

    Trace peer(TraceLane::Peer);
    QNP_CHECK(peer.advance(Stage::PeerAdvertised));
    QNP_CHECK(peer.advance(Stage::SessionCommitted));
    QNP_CHECK(peer.advance(Stage::IpcDelivered));
    QNP_CHECK(peer.advance(Stage::PeerRuntimeAttested));
    QNP_CHECK(peer.current() == Stage::PeerRuntimeAttested);
}

QNP_TEST(ConfigurationProvenanceIpcWireRejectsUncommittedOrUnattestedClaims) {
    namespace provenance = qnetplay::config_provenance;
    using provenance::Stage;

    qipc::IpcConfig config{};
    config.configurationProvenance = provenance::makeCommittedWire(
        Stage::LocallySelected, Stage::Default,
        Stage::PeerAdvertised, Stage::CasterAdvertised);
    QNP_CHECK(qipc::validateIpcConfigurationProvenance(config) == nullptr);

    config.configurationProvenance.localDelayOrigin =
        static_cast<uint8_t>(Stage::ProvisionalRecommendation);
    QNP_CHECK(qipc::validateIpcConfigurationProvenance(config) != nullptr);
    config.configurationProvenance.localDelayOrigin =
        static_cast<uint8_t>(Stage::LocallySelected);

    config.configurationProvenance.peerRollbackOrigin =
        static_cast<uint8_t>(Stage::RuntimeApplied);
    QNP_CHECK(qipc::validateIpcConfigurationProvenance(config) != nullptr);
    config.configurationProvenance.peerRollbackOrigin =
        static_cast<uint8_t>(Stage::CasterAdvertised);

    config.configurationProvenance.peerRuntimeAttestation =
        static_cast<uint8_t>(Stage::PeerRuntimeAttested);
    QNP_CHECK(qipc::validateIpcConfigurationProvenance(config) != nullptr);
    config.configurationProvenance.peerRuntimeAttestation =
        static_cast<uint8_t>(Stage::Unknown);
    config.configurationProvenance.reserved[1] = 1u;
    QNP_CHECK(qipc::validateIpcConfigurationProvenance(config) != nullptr);
}

QNP_TEST(NetworkDirectBootProductionInputRequiresNeutralRelease) {
    qgame::NetworkDirectBootInputGate gate;

    QNP_CHECK(gate.filter(false, true, 0x00000001u) == 0x00000001u);
    QNP_CHECK(!gate.awaitingNeutral());

    QNP_CHECK(gate.filter(true, true, 0x00000001u) == 0u);
    QNP_CHECK(gate.awaitingNeutral());
    QNP_CHECK(gate.filter(true, true, 0u) == 0u);
    QNP_CHECK(gate.awaitingNeutral());

    QNP_CHECK(gate.filter(false, true, 0x00000001u) == 0u);
    QNP_CHECK(gate.awaitingNeutral());
    QNP_CHECK(gate.filter(false, true, 0u) == 0u);
    QNP_CHECK(!gate.awaitingNeutral());
    QNP_CHECK(gate.filter(false, true, 0x00000001u) == 0x00000001u);
}

QNP_TEST(WinscreenConfirmDoesNotCarryIntoCharacterSelect) {
    qgame::WinscreenCarryGate gate;
    constexpr uint32_t kA = 0x00000001u;

    QNP_CHECK(gate.filter(false, false, kA) == kA);
    QNP_CHECK(!gate.armed());

    QNP_CHECK(gate.filter(true, false, kA) == kA);
    QNP_CHECK(gate.armed());

    QNP_CHECK(gate.filter(true, false, kA) == 0u);
    QNP_CHECK(gate.filter(false, true, kA) == 0u);
    QNP_CHECK(gate.armed());

    QNP_CHECK(gate.filter(false, true, 0u) == 0u);
    QNP_CHECK(!gate.armed());

    QNP_CHECK(gate.filter(false, true, kA) == kA);
    QNP_CHECK(!gate.armed());
    QNP_CHECK(gate.filter(false, true, kA) == kA);
}

QNP_TEST(WinscreenCarryGateSurvivesTheRematchPromptMaskingInputsToZero) {
    qgame::WinscreenCarryGate gate;
    constexpr uint32_t kA = 0x00000001u;

    QNP_CHECK(gate.filter(true, false, kA) == kA);
    QNP_CHECK(gate.armed());
    QNP_CHECK(gate.filter(true, false, kA) == 0u);
    QNP_CHECK(gate.armed());
    QNP_CHECK(gate.filter(false, true, kA) == 0u);
    QNP_CHECK(gate.armed());
    QNP_CHECK(gate.filter(false, true, 0u) == 0u);
    QNP_CHECK(gate.filter(false, true, kA) == kA);
}

QNP_TEST(WinscreenCarryGateDoesNotArmOnAFrontendOnlyPress) {
    qgame::WinscreenCarryGate gate;
    constexpr uint32_t kA = 0x00000001u;
    QNP_CHECK(gate.filter(false, true, kA) == kA);
    QNP_CHECK(!gate.armed());
    QNP_CHECK(gate.filter(false, true, kA) == kA);

    gate.filter(true, false, kA);
    QNP_CHECK(gate.armed());
    QNP_CHECK(gate.filter(false, false, kA) == kA);
    QNP_CHECK(!gate.armed());
}

QNP_TEST(UnselectedCharacterSelectBIsIgnoredWithoutMaskingOtherActions) {
    constexpr uint32_t kB = qinput::kLogicalB;
    constexpr uint32_t kLeft = qinput::kLogicalLeft;
    constexpr uint32_t kA = qinput::kLogicalA;

    QNP_CHECK(qgame::filterUnselectedCharacterSelectBack(
                  true, 1u, kB) == 0u);
    QNP_CHECK(qgame::filterUnselectedCharacterSelectBack(
                  true, 1u, kLeft | kB | kA)
              == (kLeft | kA));
    QNP_CHECK(qgame::filterUnselectedCharacterSelectBack(
                  true, 0u, kB) == 0u);

    QNP_CHECK(qgame::filterUnselectedCharacterSelectBack(
                  true, 2u, kB) == kB);
    QNP_CHECK(qgame::filterUnselectedCharacterSelectBack(
                  true, 4u, kB) == kB);
    QNP_CHECK(qgame::filterUnselectedCharacterSelectBack(
                  false, 1u, kB) == kB);
}

QNP_TEST(OnlineF1ReturnIsMatchDispatcherOnly) {
    QNP_CHECK(qgame::onlineReturnToCharacterSelectEligible(2u));
    QNP_CHECK(!qgame::onlineReturnToCharacterSelectEligible(1u));
    QNP_CHECK(!qgame::onlineReturnToCharacterSelectEligible(3u));
}

QNP_TEST(NetworkDirectBootExactAutomationResumesWithoutHardwareGate) {
    qgame::NetworkDirectBootInputGate gate;

    QNP_CHECK(gate.filter(true, false, 0x00000001u) == 0u);
    QNP_CHECK(gate.awaitingNeutral());
    QNP_CHECK(gate.filter(false, false, 0x00000001u) == 0x00000001u);
    QNP_CHECK(!gate.awaitingNeutral());
}

QNP_TEST(NovemberNetworkCharacterSelectWaitsForBothHumanConfirmations) {
    auto observation =
        qgame::directboot::expectedNovember1126TwoHumanCharacterSelect();
    observation.p1Cursor = 3u;
    observation.p2Cursor = 11u;
    observation.p1PaletteOrForm = 1u;
    observation.p2PaletteOrForm = 2u;

    observation.p1ConfirmationState = 4u;
    QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                  observation)
              == qgame::directboot::NetworkTwoHumanCharacterSelectStatus::
                  AwaitingBothHumanConfirmations);
    QNP_CHECK(qgame::directboot::decideNetworkTwoHumanCharacterSelectHandoff(
                  observation)
              == qgame::directboot::NetworkTwoHumanCharacterSelectHandoffAction::
                  PreserveNativeSelector);
    for (uint32_t frame = 0u; frame < 180u; ++frame) {
        observation.p2Cursor = (11u + frame) % 24u;
        QNP_CHECK(observation.scene == 3u);
        QNP_CHECK(observation.sceneSubstate == 2u);
        QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                      observation)
                  == qgame::directboot::
                      NetworkTwoHumanCharacterSelectStatus::
                          AwaitingBothHumanConfirmations);
    }
    QNP_CHECK(observation.p2Cursor != 11u);

    observation.p2ConfirmationState = 4u;
    QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                  observation)
              == qgame::directboot::NetworkTwoHumanCharacterSelectStatus::
                  AwaitingNativeConfirmationLatch);
    for (uint32_t latch = 0u; latch < 30u; ++latch) {
        observation.nativeConfirmationLatch = latch;
        QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                      observation)
                  == qgame::directboot::
                      NetworkTwoHumanCharacterSelectStatus::
                          AwaitingNativeConfirmationLatch);
    }
    observation.nativeConfirmationLatch = 30u;
    QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                  observation)
              == qgame::directboot::NetworkTwoHumanCharacterSelectStatus::
                  NativeLaunchEligible);
}

QNP_TEST(NovemberNetworkCharacterSelectAcceptsOppositeConfirmationOrder) {
    auto observation =
        qgame::directboot::expectedNovember1126TwoHumanCharacterSelect();
    observation.p1Cursor = 7u;
    observation.p2Cursor = 15u;
    observation.p1PaletteOrForm = 3u;
    observation.p2PaletteOrForm = 1u;
    const auto cursorAndPaletteBefore = observation;

    observation.p2ConfirmationState = 4u;
    QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                  observation)
              == qgame::directboot::NetworkTwoHumanCharacterSelectStatus::
                  AwaitingBothHumanConfirmations);
    QNP_CHECK(observation.cpuSlotMask == 0u);
    QNP_CHECK(observation.p1InputSource == 0u);
    QNP_CHECK(observation.p2InputSource == 1u);
    QNP_CHECK(observation.p1InputRoute == 0u);
    QNP_CHECK(observation.p2InputRoute == 1u);
    QNP_CHECK(observation.p1Cursor == cursorAndPaletteBefore.p1Cursor);
    QNP_CHECK(observation.p1PaletteOrForm
              == cursorAndPaletteBefore.p1PaletteOrForm);
    QNP_CHECK(observation.p2Cursor == cursorAndPaletteBefore.p2Cursor);
    QNP_CHECK(observation.p2PaletteOrForm
              == cursorAndPaletteBefore.p2PaletteOrForm);

    observation.p1ConfirmationState = 4u;
    QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                  observation)
              == qgame::directboot::NetworkTwoHumanCharacterSelectStatus::
                  AwaitingNativeConfirmationLatch);
    observation.nativeConfirmationLatch = 30u;
    QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                  observation)
              == qgame::directboot::NetworkTwoHumanCharacterSelectStatus::
                  NativeLaunchEligible);
}

QNP_TEST(NovemberNetworkCharacterSelectHasNoCrossEntryStateOrPrediction) {
    for (uint32_t entry = 0u; entry < 3u; ++entry) {
        auto observation =
            qgame::directboot::expectedNovember1126TwoHumanCharacterSelect();
        observation.p1Cursor = entry + 1u;
        observation.p2Cursor = entry + 9u;
        observation.p1PaletteOrForm = entry % 4u;
        observation.p2PaletteOrForm = (entry + 1u) % 4u;
        QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                      observation)
                  == qgame::directboot::
                      NetworkTwoHumanCharacterSelectStatus::
                          AwaitingBothHumanConfirmations);
        observation.p1ConfirmationState = 4u;
        observation.p2ConfirmationState = 4u;
        QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                      observation)
                  == qgame::directboot::
                      NetworkTwoHumanCharacterSelectStatus::
                          AwaitingNativeConfirmationLatch);
        observation.nativeConfirmationLatch = 30u;
        QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                      observation)
                  == qgame::directboot::
                      NetworkTwoHumanCharacterSelectStatus::
                          NativeLaunchEligible);
    }
}

QNP_TEST(NovemberNetworkCharacterSelectRejectsChangedNativeOwnershipPolicy) {
    auto observation =
        qgame::directboot::expectedNovember1126TwoHumanCharacterSelect();

    observation.cpuSlotMask = 2u;
    QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                  observation)
              == qgame::directboot::NetworkTwoHumanCharacterSelectStatus::
                  NativeInvariantViolation);
    QNP_CHECK(qgame::directboot::decideNetworkTwoHumanCharacterSelectHandoff(
                  observation)
              == qgame::directboot::NetworkTwoHumanCharacterSelectHandoffAction::
                  FailClosed);
    observation.cpuSlotMask = 0u;
    observation.vsFormationApplied = 0u;
    QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                  observation)
              == qgame::directboot::NetworkTwoHumanCharacterSelectStatus::
                  NativeInvariantViolation);
    observation.vsFormationApplied = 1u;
    observation.p2ConfirmationState = 0u;
    QNP_CHECK(qgame::directboot::observeNetworkTwoHumanCharacterSelect(
                  observation)
              == qgame::directboot::NetworkTwoHumanCharacterSelectStatus::
                  NativeInvariantViolation);
}

QNP_TEST(NetworkCharacterSelectHandoffIsTheNetworkDrivenVsRow) {
    QNP_CHECK(qgame::directboot::isNetworkTwoHumanCharacterSelectHandoff(
                  true, 1));
    QNP_CHECK(!qgame::directboot::isNetworkTwoHumanCharacterSelectHandoff(
                  false, 1));
    QNP_CHECK(!qgame::directboot::isNetworkTwoHumanCharacterSelectHandoff(
                  true, 0));
}

QNP_TEST(OfflineTwoFighterCharacterSelectPreservesTheNativeFormation) {
    for (uint32_t row = 0u; row <= 1u; ++row) {
        auto observation =
            qgame::directboot::expectedTwoFighterCharacterSelect(row);
        QNP_CHECK(observation.matchFormationRow == 1u);
        QNP_CHECK(observation.vsFormationApplied == 1u);
        QNP_CHECK(observation.cpuSlotMask == 0u);

        QNP_CHECK(qgame::directboot::observeTwoFighterCharacterSelect(
                      observation)
                  == qgame::directboot::TwoFighterCharacterSelectStatus::
                      AwaitingHumanConfirmations);
        QNP_CHECK(qgame::directboot::decideTwoFighterCharacterSelectHandoff(
                      observation)
                  == qgame::directboot::
                      TwoFighterCharacterSelectHandoffAction::
                          PreserveNativeSelector);

        observation.p1ConfirmationState = 4u;
        QNP_CHECK(qgame::directboot::observeTwoFighterCharacterSelect(
                      observation)
                  == qgame::directboot::TwoFighterCharacterSelectStatus::
                      AwaitingHumanConfirmations);
        observation.p1ConfirmationState = 1u;
        observation.p2ConfirmationState = 4u;
        QNP_CHECK(qgame::directboot::observeTwoFighterCharacterSelect(
                      observation)
                  == qgame::directboot::TwoFighterCharacterSelectStatus::
                      AwaitingHumanConfirmations);

        observation.p1ConfirmationState = 4u;
        observation.p2ConfirmationState = 4u;
        QNP_CHECK(qgame::directboot::observeTwoFighterCharacterSelect(
                      observation)
                  == qgame::directboot::TwoFighterCharacterSelectStatus::
                      NativeLaunchEligible);
    }
}

QNP_TEST(OfflineTwoFighterCharacterSelectFailsClosedOnTheArcadeHandoff) {
    auto observation =
        qgame::directboot::expectedTwoFighterCharacterSelect(1u);

    observation.matchFormationRow = 0u;
    observation.vsFormationApplied = 0u;
    QNP_CHECK(qgame::directboot::observeTwoFighterCharacterSelect(observation)
              == qgame::directboot::TwoFighterCharacterSelectStatus::
                  NativeFormationViolation);
    QNP_CHECK(qgame::directboot::decideTwoFighterCharacterSelectHandoff(
                  observation)
              == qgame::directboot::
                  TwoFighterCharacterSelectHandoffAction::FailClosed);

    observation.vsFormationApplied = 1u;
    QNP_CHECK(qgame::directboot::observeTwoFighterCharacterSelect(observation)
              == qgame::directboot::TwoFighterCharacterSelectStatus::
                  NativeFormationViolation);

    observation.matchFormationRow = 1u;
    observation.cpuSlotMask = 2u;
    QNP_CHECK(qgame::directboot::observeTwoFighterCharacterSelect(observation)
              == qgame::directboot::TwoFighterCharacterSelectStatus::
                  NativeFormationViolation);

    observation.cpuSlotMask = 0u;
    for (uint32_t row = 2u; row <= 5u; ++row) {
        observation.vsRow = row;
        QNP_CHECK(!qgame::directboot::isTwoFighterDirectBootRow(
                      static_cast<int>(row)));
        QNP_CHECK(qgame::directboot::observeTwoFighterCharacterSelect(
                      observation)
                  == qgame::directboot::TwoFighterCharacterSelectStatus::
                      NotAtTwoFighterCharacterSelect);
    }
}

QNP_TEST(TwoFighterMatchEntryRequiresFormationOneAndTheRowCpuMask) {
    qgame::directboot::MatchEntryObservation entry{};
    entry.vsRow = 1u;
    entry.sceneNext = 2u;
    entry.matchFormationRow = 1u;
    entry.vsFormationApplied = 1u;
    entry.cpuSlotMask = 0u;
    QNP_CHECK(qgame::directboot::observeTwoFighterMatchEntry(entry)
              == qgame::directboot::MatchEntryStatus::
                  TwoFighterMatchEntryVerified);

    entry.matchFormationRow = 0u;
    entry.vsFormationApplied = 0u;
    QNP_CHECK(qgame::directboot::observeTwoFighterMatchEntry(entry)
              == qgame::directboot::MatchEntryStatus::FormationViolation);

    entry.matchFormationRow = 1u;
    entry.vsFormationApplied = 1u;
    entry.cpuSlotMask = 2u;
    QNP_CHECK(qgame::directboot::observeTwoFighterMatchEntry(entry)
              == qgame::directboot::MatchEntryStatus::FormationViolation);

    entry.vsRow = 0u;
    QNP_CHECK(qgame::directboot::observeTwoFighterMatchEntry(entry)
              == qgame::directboot::MatchEntryStatus::
                  TwoFighterMatchEntryVerified);
    QNP_CHECK(qgame::directboot::vsRowCpuSlotMaskAtMatchEntry(0u) == 2u);
    QNP_CHECK(qgame::directboot::vsRowCpuSlotMaskAtMatchEntry(1u) == 0u);
    QNP_CHECK(qgame::directboot::vsRowCpuSlotMaskAtMatchEntry(2u) == 14u);
    QNP_CHECK(qgame::directboot::vsRowCpuSlotMaskAtMatchEntry(3u) == 10u);
    QNP_CHECK(qgame::directboot::vsRowCpuSlotMaskAtMatchEntry(4u) == 12u);
    QNP_CHECK(qgame::directboot::vsRowCpuSlotMaskAtMatchEntry(5u) == 0u);
    QNP_CHECK(qgame::directboot::vsRowCpuSlotMaskAtMatchEntry(6u)
              == 0xFFFFFFFFu);

    entry.sceneNext = 3u;
    QNP_CHECK(qgame::directboot::observeTwoFighterMatchEntry(entry)
              == qgame::directboot::MatchEntryStatus::NotAtMatchEntry);
}

QNP_TEST(NetworkRowOneHandoffStillHoldsTheUnchangedContract) {
    const auto network =
        qgame::directboot::expectedNovember1126TwoHumanCharacterSelect();
    const auto offline =
        qgame::directboot::expectedTwoFighterCharacterSelect(1u);
    QNP_CHECK(network.matchFormationRow == offline.matchFormationRow);
    QNP_CHECK(network.vsFormationApplied == offline.vsFormationApplied);
    QNP_CHECK(network.cpuSlotMask == offline.cpuSlotMask);
    QNP_CHECK(network.p1ConfirmationState == offline.p1ConfirmationState);
    QNP_CHECK(network.p2ConfirmationState == offline.p2ConfirmationState);
}

QNP_TEST(PlayerNameCanonicalizationIsBoundedAndSafe) {
    QNP_CHECK(qgame::canonicalPlayerName("") == "Player");
    QNP_CHECK(qgame::canonicalPlayerName(" \t\r\n ") == "Player");
    QNP_CHECK(qgame::canonicalPlayerName("  Alice   Bob  ")
              == "Alice Bob");
    QNP_CHECK(qgame::canonicalPlayerName("Alice|\n\tBob")
              == "Alice Bob");
    QNP_CHECK(qgame::canonicalPlayerName(
                  "12345678901234567890EXTRA")
              == "12345678901234567890");
    QNP_CHECK(qgame::canonicalPlayerName(
                  "1234567890123456789 trailing")
              == "1234567890123456789");

    char stored[qgame::kPlayerNameStorageBytes];
    std::memset(stored, 0x7F, sizeof(stored));
    qgame::storeCanonicalPlayerName(stored, "Alice");
    QNP_CHECK(std::strcmp(stored, "Alice") == 0);
    for (size_t i = 6; i < sizeof(stored); ++i)
        QNP_CHECK(stored[i] == '\0');
}

QNP_TEST(PlayerNameCanonicalizationKeepsMultibyteCharactersAtomic) {
    const std::string cp932Character("\x82\xA0", 2);
    const std::string cp932PipeTrail("\x81\x7C", 2);
    const std::string cp932Fits =
        std::string(18, 'A') + cp932Character;
    const std::string cp932WouldSplit =
        std::string(19, 'A') + cp932Character;
    QNP_CHECK(qgame::canonicalPlayerNameForCodePage(cp932Fits, 932)
              == cp932Fits);
    QNP_CHECK(qgame::canonicalPlayerNameForCodePage(
                  cp932PipeTrail, 932)
              == cp932PipeTrail);
    QNP_CHECK(qgame::isCanonicalPlayerNameForCodePage(
        cp932PipeTrail, 932));
    QNP_CHECK(qgame::canonicalPlayerNameForCodePage("A|B", 932)
              == "A B");
    QNP_CHECK(qgame::canonicalPlayerNameForCodePage(
                  cp932WouldSplit, 932)
              == std::string(19, 'A'));

    const std::string utf8Character("\xE3\x81\x82", 3);
    const std::string utf8Fits =
        std::string(17, 'B') + utf8Character;
    const std::string utf8WouldSplit =
        std::string(18, 'B') + utf8Character;
    QNP_CHECK(qgame::canonicalPlayerNameForCodePage(
                  utf8Fits, CP_UTF8)
              == utf8Fits);
    QNP_CHECK(qgame::canonicalPlayerNameForCodePage(
                  utf8WouldSplit, CP_UTF8)
              == std::string(18, 'B'));

    const std::string malformed("\x82\nAlice", 7);
    QNP_CHECK(qgame::canonicalPlayerNameForCodePage(malformed, 932)
              == "Alice");

    QNP_CHECK(qgame::playerNameForLocalDisplay("Alice", 932) == "Alice");
    const std::string displayed =
        qgame::playerNameForLocalDisplay(utf8Character, CP_UTF8);
    QNP_CHECK(displayed != "Player");
    QNP_CHECK(qgame::isCanonicalPlayerNameForCodePage(
        displayed, GetACP()));

    QNP_CHECK(qgame::playerNameForUtf8Display(cp932Character, 932)
              == utf8Character);
    const std::string cp1251Zhe("\xC6", 1);
    const std::string utf8Zhe("\xD0\x96", 2);
    QNP_CHECK(qgame::playerNameForUtf8Display(cp1251Zhe, 1251)
              == utf8Zhe);
    QNP_CHECK(qgame::playerNameForUtf8Display(utf8Character, CP_UTF8)
              == utf8Character);
    QNP_CHECK(qgame::playerNameForUtf8Display(malformed, 932)
              == "Player");
}

QNP_TEST(NicknameCommandLineValueMustExistAndNotBeAnotherOption) {
    QNP_CHECK(qgame::isNicknameOptionValue("Alice"));
    QNP_CHECK(qgame::isNicknameOptionValue("Alice-Bob"));
    QNP_CHECK(!qgame::isNicknameOptionValue(""));
    QNP_CHECK(!qgame::isNicknameOptionValue("--delay"));
    QNP_CHECK(!qgame::isNicknameOptionValue("-"));
}

QNP_TEST(PlayerNameLookupUsesRequestedDirectPlayId) {
    QNP_CHECK(qgame::directPlayLocalPlayerId(1) == 1);
    QNP_CHECK(qgame::directPlayRemotePlayerId(1) == 2);
    QNP_CHECK(qgame::directPlayLocalPlayerId(2) == 2);
    QNP_CHECK(qgame::directPlayRemotePlayerId(2) == 1);
    QNP_CHECK(std::strcmp(
                  qgame::playerNameForId(1, 1, 2, "Alice", "Bob"),
                  "Alice") == 0);
    QNP_CHECK(std::strcmp(
                  qgame::playerNameForId(2, 1, 2, "Alice", "Bob"),
                  "Bob") == 0);
    QNP_CHECK(std::strcmp(
                  qgame::playerNameForId(2, 2, 1, "Bob", "Alice"),
                  "Bob") == 0);
    QNP_CHECK(std::strcmp(
                  qgame::playerNameForId(1, 2, 1, "Bob", "Alice"),
                  "Alice") == 0);
    QNP_CHECK(std::strcmp(
                  qgame::playerNameForId(99, 1, 2, "Alice", "Bob"),
                  "Player") == 0);
}

QNP_TEST(RuntimeDelayHotkeysAreFocusedGameplayEdgesOnly) {
    qgame::RuntimeDelayHotkeys hotkeys;
    qgame::RuntimeDelayKeySample sample{};
    sample.playerNetplay = true;
    sample.rollbackGameplay = true;
    sample.minusDown = true;

    QNP_CHECK(hotkeys.poll(sample) == 0);
    sample.focused = true;
    QNP_CHECK(hotkeys.poll(sample) == 0);
    sample.minusDown = false;
    QNP_CHECK(hotkeys.poll(sample) == 0);
    sample.minusDown = true;
    QNP_CHECK(hotkeys.poll(sample) == -1);
    QNP_CHECK(hotkeys.poll(sample) == 0);

    sample.minusDown = false;
    sample.equalsDown = true;
    QNP_CHECK(hotkeys.poll(sample) == 1);
    QNP_CHECK(hotkeys.poll(sample) == 0);
}

QNP_TEST(RuntimeDelayHotkeysRejectModifiersFrontendAndSpectator) {
    qgame::RuntimeDelayHotkeys hotkeys;
    qgame::RuntimeDelayKeySample sample{};
    sample.focused = true;
    sample.rollbackGameplay = true;
    sample.minusDown = true;

    sample.playerNetplay = false;
    QNP_CHECK(hotkeys.poll(sample) == 0);
    sample.minusDown = false;
    hotkeys.poll(sample);

    sample.playerNetplay = true;
    sample.rollbackGameplay = false;
    sample.minusDown = true;
    QNP_CHECK(hotkeys.poll(sample) == 0);
    sample.minusDown = false;
    hotkeys.poll(sample);

    sample.rollbackGameplay = true;
    sample.equalsDown = true;
    sample.shiftDown = true;
    QNP_CHECK(hotkeys.poll(sample) == 0);
    sample.equalsDown = false;
    sample.shiftDown = false;
    hotkeys.poll(sample);

    sample.equalsDown = true;
    sample.controlDown = true;
    QNP_CHECK(hotkeys.poll(sample) == 0);
    sample.equalsDown = false;
    sample.controlDown = false;
    hotkeys.poll(sample);

    sample.equalsDown = true;
    sample.altDown = true;
    QNP_CHECK(hotkeys.poll(sample) == 0);
}

QNP_TEST(RuntimeDelayAdjustmentClampsToProductRange) {
    QNP_CHECK(qgame::adjustedGameplayDelay(0, -1) == 0);
    QNP_CHECK(qgame::adjustedGameplayDelay(1, -1) == 0);
    QNP_CHECK(qgame::adjustedGameplayDelay(7, 1) == 8);
    QNP_CHECK(qgame::adjustedGameplayDelay(15, 1) == 15);
}

QNP_TEST(CharacterSelectDelaySelectionNeverHoldsTheFrontendForADrain) {
    qgame::RuntimeDelayDrain drain;
    QNP_CHECK(!drain.request(6, 1, 1000, false));
    QNP_CHECK(!drain.active());

    QNP_CHECK(drain.request(6, 1, 2000, true));
    QNP_CHECK(!drain.request(7, 1, 3000, false));
    QNP_CHECK(!drain.active());
    QNP_CHECK(!drain.timedOut(10000));

    QNP_CHECK(drain.request(8, 7, 11000, true));
    QNP_CHECK(drain.active());
    QNP_CHECK(drain.target() == 8u);
}

QNP_TEST(RuntimeDelayDrainIsBoundedAndRetargetDoesNotExtendDeadline) {
    qgame::RuntimeDelayDrain drain;
    QNP_CHECK(!drain.active());
    QNP_CHECK(drain.request(6, 1, 1000));
    QNP_CHECK(drain.active());
    QNP_CHECK(drain.target() == 6);
    QNP_CHECK(!drain.timedOut(
        1000 + qgame::kRuntimeDelayDrainTimeoutMs - 1));

    QNP_CHECK(drain.request(7, 1, 4000));
    QNP_CHECK(drain.target() == 7);
    QNP_CHECK(drain.timedOut(
        1000 + qgame::kRuntimeDelayDrainTimeoutMs));

    QNP_CHECK(!drain.request(1, 1, 5000));
    QNP_CHECK(!drain.active());
    QNP_CHECK(drain.elapsedMs(9000) == 0);
}

QNP_TEST(NetplayPacingTelemetryExposesNativeAndCadenceSlowdown) {
    QNP_CHECK(qgame::nativeSimulationAdvanced(
        true, 41u, 42u, 0u, 1u));
    QNP_CHECK(qgame::nativeSimulationAdvanced(
        true, 0u, 0u, 0u, 1u));
    QNP_CHECK(!qgame::nativeSimulationAdvanced(
        false, 0u, 1u, 0u, 1u));
    QNP_CHECK(!qgame::nativeSimulationAdvanced(
        true, 7u, 7u, 1u, 1u));

    qgame::NetplayPacingWindow pacing;
    pacing.observe(1000000u, true, true);
    pacing.observe(1016500u, true, true);
    pacing.observe(1033000u, true, false);
    pacing.observe(1037000u, false, false);
    pacing.observe(1067000u, true, true);

    QNP_CHECK(!pacing.reportDue(1999999u));
    QNP_CHECK(pacing.reportDue(2000000u));
    const qgame::NetplayPacingReport report =
        pacing.finish(2000000u);
    QNP_CHECK(report.visibleTicks == 5u);
    QNP_CHECK(report.gameplayTicks == 4u);
    QNP_CHECK(report.nativeAdvanceTicks == 3u);
    QNP_CHECK(report.nativeAdvanceMisses == 1u);
    QNP_CHECK(report.intervalSamples == 4u);
    QNP_CHECK(report.minimumIntervalUs == 4000u);
    QNP_CHECK(report.p95IntervalUs == 30000u);
    QNP_CHECK(report.maximumIntervalUs == 30000u);
    QNP_CHECK(report.burstIntervals == 1u);
    QNP_CHECK(report.lateIntervals == 1u);
    QNP_CHECK(report.visibleFps() > 4.99);
    QNP_CHECK(report.visibleFps() < 5.01);

    pacing.observe(2016500u, true, true);
    pacing.observe(2033000u, true, true);
    const qgame::NetplayPacingReport second =
        pacing.finish(3000000u);
    QNP_CHECK(second.intervalSamples == 2u);
    QNP_CHECK(second.minimumIntervalUs == 16500u);
    QNP_CHECK(second.maximumIntervalUs == 949500u);
}

QNP_TEST(NetplayPacingDeadlineRetimesWithoutLosingTheRunningSchedule) {
    qgame::NetplayPacingDeadline pacing;
    pacing.configure(1000);
    (void)pacing.poll(0);
    (void)pacing.poll(1000);
    const int64_t beforeRetime = pacing.frameTicks();
    QNP_CHECK(beforeRetime == 1000);

    pacing.setFrameTicks(975);
    QNP_CHECK(pacing.frameTicks() == 975);
    QNP_CHECK(pacing.frameTicks() < beforeRetime);

    pacing.configure(1000);
    QNP_CHECK(pacing.frameTicks() == 1000);
}

QNP_TEST(NetplayPaceCorrectionNeverSlowsTheAheadPeer) {
    const qgame::NetplayPaceCorrection ahead =
        qgame::planTrailingPaceCorrection(
            6, 2,
            0, 0);
    QNP_CHECK(ahead.periodPpm == 0);
    QNP_CHECK(ahead.integralPpm == 0);

    const qgame::NetplayPaceCorrection trailing =
        qgame::planTrailingPaceCorrection(
            -6, 2,
            0, 0);
    QNP_CHECK(trailing.periodPpm < 0);
    QNP_CHECK(trailing.periodPpm >= -qgame::kNetPaceCorrectionMaxPpm);
    QNP_CHECK(trailing.integralPpm > 0);
}

QNP_TEST(NetplayPaceCorrectionReleasesAtNativeCadenceAfterCatchup) {
    qgame::NetplayPaceCorrection correction =
        qgame::planTrailingPaceCorrection(-12, 2, 0, 0);
    for (int i = 0; i < 8; ++i) {
        correction = qgame::planTrailingPaceCorrection(
            -12, 2, correction.periodPpm, correction.integralPpm);
    }
    QNP_CHECK(correction.periodPpm
              == -qgame::kNetPaceCorrectionMaxPpm);

    correction = qgame::planTrailingPaceCorrection(
        -1, 2, correction.periodPpm, correction.integralPpm);
    QNP_CHECK(correction.periodPpm == 0);
    QNP_CHECK(correction.integralPpm == 0);
}

QNP_TEST(RollbackBalanceSpeedsOnlyTheLowerPredictionDepthPeer) {
    qgame::RollbackBalanceInput in{};
    in.enabled = true;
    in.rollbackGameplay = true;
    in.hasPeerReport = true;
    in.gameplayFramesObserved = qgame::kRollbackBalanceWarmupFrames;
    in.peerReportAgeMs = 10;
    in.localPredictionLimit = 13;
    in.peerPredictionLimit = 13;
    in.peerRunState = static_cast<uint8_t>(
        qgame::NetplayRunState::PredictionPressure);

    in.localPredictionDepth = 0;
    in.peerPredictionDepth = 13;
    qgame::RollbackBalanceSample sample =
        qgame::classifyRollbackBalance(in);
    QNP_REQUIRE(sample.usable);
    QNP_CHECK(sample.predictionDepthDelta == -13);
    QNP_CHECK(qgame::planTrailingPaceCorrection(
                  sample.predictionDepthDelta, 2, 0, 0).periodPpm < 0);

    in.localPredictionDepth = 13;
    in.peerPredictionDepth = 0;
    sample = qgame::classifyRollbackBalance(in);
    QNP_REQUIRE(sample.usable);
    QNP_CHECK(sample.predictionDepthDelta == 13);
    QNP_CHECK(qgame::planTrailingPaceCorrection(
                  sample.predictionDepthDelta, 2, 0, 0).periodPpm == 0);
}

QNP_TEST(RollbackBalanceFailsSafeToNativeCadence) {
    qgame::RollbackBalanceInput in{};
    in.enabled = true;
    in.rollbackGameplay = true;
    in.hasPeerReport = true;
    in.gameplayFramesObserved = qgame::kRollbackBalanceWarmupFrames;
    in.localPredictionDepth = 0;
    in.localPredictionLimit = 13;
    in.peerPredictionDepth = 13;
    in.peerPredictionLimit = 13;
    in.peerRunState = static_cast<uint8_t>(qgame::NetplayRunState::Running);

    in.peerReportAgeMs = qgame::kRollbackBalancePressureFreshMs + 1;
    QNP_CHECK(!qgame::classifyRollbackBalance(in).usable);
    QNP_CHECK(qgame::classifyRollbackBalance(in).reason
              == qgame::RollbackBalanceReason::StalePeerReport);

    in.peerReportAgeMs = 0;
    in.gameplayFramesObserved = qgame::kRollbackBalanceWarmupFrames - 1;
    QNP_CHECK(qgame::classifyRollbackBalance(in).reason
              == qgame::RollbackBalanceReason::WarmingUp);

    in.gameplayFramesObserved = qgame::kRollbackBalanceWarmupFrames;
    in.peerRunState = static_cast<uint8_t>(
        qgame::NetplayRunState::ExactLifecycleBarrier);
    QNP_CHECK(qgame::classifyRollbackBalance(in).reason
              == qgame::RollbackBalanceReason::PeerStateNotPaceable);

    in.peerRunState = static_cast<uint8_t>(qgame::NetplayRunState::Running);
    in.peerPredictionDepth = 14;
    QNP_CHECK(qgame::classifyRollbackBalance(in).reason
              == qgame::RollbackBalanceReason::InvalidPeerDepth);

    in.peerPredictionDepth = 13;
    in.enabled = false;
    QNP_CHECK(qgame::classifyRollbackBalance(in).reason
              == qgame::RollbackBalanceReason::Disabled);
}

QNP_TEST(NetplayPacingDeadlineBoundsVisibleDebtRecovery) {
    qgame::NetplayPacingDeadline pacing;
    pacing.configure(16000);

    QNP_CHECK(pacing.frameTicks() == 16000);
    QNP_CHECK(pacing.minimumSpacingTicks() == 14000);

    auto poll = pacing.poll(0);
    QNP_CHECK(poll.ready());
    QNP_CHECK(!poll.rebased());
    QNP_CHECK(pacing.idealDeadline() == 16000);

    poll = pacing.poll(28000);
    QNP_CHECK(poll.ready());
    QNP_CHECK(!poll.rebased());
    QNP_CHECK(pacing.idealDeadline() == 32000);
    QNP_CHECK(pacing.earliestVisibleDeadline() == 42000);

    poll = pacing.poll(41999);
    QNP_CHECK(!poll.ready());
    QNP_CHECK(poll.spacingBlocked());
    poll = pacing.poll(42000);
    QNP_CHECK(poll.ready());
    QNP_CHECK(pacing.idealDeadline() == 48000);

    poll = pacing.poll(56000);
    QNP_CHECK(poll.ready());
    QNP_CHECK(pacing.idealDeadline() == 64000);
    QNP_CHECK(pacing.poll(70000).ready());
    QNP_CHECK(pacing.poll(84000).ready());
    QNP_CHECK(pacing.poll(98000).ready());
    QNP_CHECK(pacing.poll(112000).ready());
    poll = pacing.poll(127999);
    QNP_CHECK(!poll.ready());
    QNP_CHECK(!poll.spacingBlocked());
    poll = pacing.poll(128000);
    QNP_CHECK(poll.ready());
    QNP_CHECK(pacing.idealDeadline() == 144000);
}

QNP_TEST(NetplayPacingDeadlineRebasesOnlyTrueStallsAndClockRegression) {
    qgame::NetplayPacingDeadline pacing;
    pacing.configure(16000);
    QNP_CHECK(pacing.poll(1000).ready());

    auto poll = pacing.poll(33000);
    QNP_CHECK(poll.ready());
    QNP_CHECK(!poll.rebased());

    pacing.configure(16000);
    QNP_CHECK(pacing.poll(1000).ready());
    poll = pacing.poll(33001);
    QNP_CHECK(poll.ready());
    QNP_CHECK(poll.rebased());
    QNP_CHECK(pacing.idealDeadline() == 49001);

    poll = pacing.poll(32000);
    QNP_CHECK(!poll.ready());
    QNP_CHECK(!poll.rebased());
    QNP_CHECK(pacing.idealDeadline() == 48000);
    QNP_CHECK(!pacing.poll(47999).ready());
    QNP_CHECK(pacing.poll(48000).ready());
}

QNP_TEST(NetplayPacingDeadlineSpacesActualPublishCompletion) {
    qgame::NetplayPacingDeadline pacing;
    pacing.configure(16000);
    QNP_CHECK(pacing.poll(0).ready());

    pacing.observePresentationCompleted(10000);
    QNP_CHECK(pacing.idealDeadline() == 16000);
    QNP_CHECK(pacing.earliestVisibleDeadline() == 18000);
    auto poll = pacing.poll(16000);
    QNP_CHECK(!poll.ready());
    QNP_CHECK(poll.spacingBlocked());
    QNP_CHECK(pacing.poll(17999).spacingBlocked());
    QNP_CHECK(pacing.poll(18000).ready());
    QNP_CHECK(pacing.idealDeadline() == 32000);
}

QNP_TEST(NetplayPacingDeadlineAnchorsPreOwnershipPresentation) {
    qgame::NetplayPacingDeadline pacing;
    pacing.configure(16000);

    pacing.anchorPresentationCompleted(10000);
    QNP_CHECK(pacing.idealDeadline() == 26000);
    QNP_CHECK(pacing.earliestVisibleDeadline() == 24000);
    QNP_CHECK(!pacing.poll(25999).ready());
    QNP_CHECK(pacing.poll(26000).ready());
    QNP_CHECK(pacing.idealDeadline() == 42000);

    pacing.anchorPresentationCompleted(30000);
    QNP_CHECK(pacing.idealDeadline() == 46000);
    QNP_CHECK(!pacing.poll(45999).ready());
    QNP_CHECK(pacing.poll(46000).ready());
}

QNP_TEST(NetplayPacingDeadlineSemanticHoldDiscardsWallClockDebt) {
    qgame::NetplayPacingDeadline pacing;
    pacing.configure(16000);
    QNP_CHECK(pacing.poll(1000).ready());

    pacing.semanticHold(2000);
    QNP_CHECK(!pacing.poll(14999).ready());
    QNP_CHECK(!pacing.poll(16999).ready());
    QNP_CHECK(pacing.poll(17000).ready());

    pacing.semanticHold(34000);
    QNP_CHECK(pacing.poll(34000).ready());
    QNP_CHECK(pacing.idealDeadline() == 49000);
    QNP_CHECK(!pacing.poll(48999).ready());
    QNP_CHECK(pacing.poll(49000).ready());

    pacing.configure(16000);
    QNP_CHECK(pacing.poll(1000).ready());
    pacing.semanticHold(2000);
    const auto rebased = pacing.poll(33002);
    QNP_CHECK(rebased.ready());
    QNP_CHECK(rebased.rebased());
    QNP_CHECK(pacing.idealDeadline() == 49002);
}

QNP_TEST(RollbackCorrectionNeverOwnsAnUnpacedPresentation) {
    for (uint32_t replayIndex = 0; replayIndex < 16u; ++replayIndex) {
        (void)replayIndex;
        QNP_CHECK(qgame::rollbackCorrectionTickVisibility()
                  == qgame::NativeTickVisibility::Historical);
    }

    QNP_CHECK(qgame::spectatorCatchupTickVisibility(0u, 1u)
              == qgame::NativeTickVisibility::VisibleCanonical);
    QNP_CHECK(qgame::spectatorCatchupTickVisibility(0u, 3u)
              == qgame::NativeTickVisibility::Historical);
    QNP_CHECK(qgame::spectatorCatchupTickVisibility(1u, 3u)
              == qgame::NativeTickVisibility::Historical);
    QNP_CHECK(qgame::spectatorCatchupTickVisibility(2u, 3u)
              == qgame::NativeTickVisibility::VisibleCanonical);
}

QNP_TEST(PresentationClockWaitsForPlayerGoAndSpectatorPlayback) {
    QNP_CHECK(!qgame::deterministicPresentationClockStarted(
        false, false, false));
    QNP_CHECK(qgame::deterministicPresentationClockStarted(
        true, false, false));
    QNP_CHECK(!qgame::deterministicPresentationClockStarted(
        false, true, false));
    QNP_CHECK(!qgame::deterministicPresentationClockStarted(
        true, true, false));
    QNP_CHECK(qgame::deterministicPresentationClockStarted(
        true, true, true));
}

QNP_TEST(HiddenTicksPreserveQohNativeFpsAccounting) {
    std::array<uint32_t, 3> values{{1000u, 61u, 42u}};
    const auto indexForAddress = [](const uintptr_t address) -> size_t {
        if (address == qgame::kQohFpsLastSampleMs) return 0u;
        if (address == qgame::kQohPublishedFps) return 1u;
        return 2u;
    };
    const qgame::QohNativeFpsAccounting before =
        qgame::captureQohNativeFpsAccounting(
            [&](const uintptr_t address) {
                return values[indexForAddress(address)];
            });

    values = {{2000u, 161u, 0u}};
    qgame::restoreQohNativeFpsAccounting(
        before,
        [&](const uintptr_t address, const uint32_t value) {
            values[indexForAddress(address)] = value;
        });

    QNP_CHECK(values[0] == 1000u);
    QNP_CHECK(values[1] == 61u);
    QNP_CHECK(values[2] == 42u);
}

QNP_TEST(RollbackHealthReportsCanonicalSpeedAndStallWallTime) {
    qgame::RollbackHealthTelemetry health;
    qnetplay::RollbackDiagnostics diagnostics{};
    qnetplay::RollbackStats stats{};
    diagnostics.valid = true;
    diagnostics.frontier = 100u;
    diagnostics.nextConfirmedFrame = 100u;

    auto observation = health.observe(100u, diagnostics, stats);
    QNP_CHECK(!observation.stall.occurred());
    QNP_CHECK(!observation.reportDue);

    stats.visibleAdvances = 30u;
    stats.confirmedFrames = 29u;
    stats.replayedFrames = 5u;
    stats.rollbackCount = 2u;
    stats.rollbackFrames = 7u;
    diagnostics.frontier = 130u;
    diagnostics.nextConfirmedFrame = 129u;
    observation = health.observe(500100u, diagnostics, stats);
    QNP_CHECK(!observation.reportDue);

    diagnostics.activeStallReason =
        qnetplay::StallReason::PredictionLimit;
    observation = health.observe(600100u, diagnostics, stats);
    QNP_CHECK(observation.stall.kind
              == qgame::RollbackStallTransitionKind::Began);
    QNP_CHECK(observation.stall.from
              == qnetplay::StallReason::None);
    QNP_CHECK(observation.stall.to
              == qnetplay::StallReason::PredictionLimit);
    QNP_CHECK(observation.stall.episodeDurationUs == 0u);

    observation = health.observe(1100100u, diagnostics, stats);
    QNP_CHECK(observation.reportDue);
    QNP_CHECK(health.reportDue());

    const qgame::RollbackHealthReport report = health.finishReport();
    QNP_CHECK(report.elapsedUs == 1100000u);
    QNP_CHECK(report.newForwardFrames == 30u);
    QNP_CHECK(report.newForwardFps() > 27.27);
    QNP_CHECK(report.newForwardFps() < 27.28);
    QNP_CHECK(report.newForwardSpeedPermille == 450u);
    QNP_CHECK(report.confirmedFrames == 29u);
    QNP_CHECK(report.confirmedFps() > 26.36);
    QNP_CHECK(report.confirmedFps() < 26.37);
    QNP_CHECK(report.confirmedLagFrames == 1u);
    QNP_CHECK(report.maximumConfirmedLagFrames == 1u);
    QNP_CHECK(report.replayedFrames == 5u);
    QNP_CHECK(report.rollbackCount == 2u);
    QNP_CHECK(report.rollbackFrames == 7u);
    QNP_CHECK(report.activeWallUs == 600000u);
    QNP_CHECK(report.stalledWallUs == 500000u);
    QNP_CHECK(report.activeWallUs + report.stalledWallUs
              == report.elapsedUs);
    QNP_CHECK(report.stalledWallUsByReason[
                  static_cast<size_t>(
                      qnetplay::StallReason::PredictionLimit)]
              == 500000u);
    QNP_CHECK(report.noProgressAgeUs == 600000u);
    QNP_CHECK(health.totalObservedWallUs() == report.elapsedUs);
    QNP_CHECK(health.totalActiveWallUs() == report.activeWallUs);
    QNP_CHECK(health.totalStalledWallUs() == report.stalledWallUs);
    QNP_CHECK(!health.reportDue());
}

QNP_TEST(RollbackHealthReturnsTypedStallEpisodeTransitions) {
    qgame::RollbackHealthTelemetry health;
    qnetplay::RollbackDiagnostics diagnostics{};
    qnetplay::RollbackStats stats{};
    diagnostics.valid = true;

    health.observe(0u, diagnostics, stats);
    diagnostics.activeStallReason =
        qnetplay::StallReason::LocalInputMissing;
    auto observation = health.observe(100u, diagnostics, stats);
    QNP_CHECK(observation.stall.kind
              == qgame::RollbackStallTransitionKind::Began);
    QNP_CHECK(observation.stall.episodeDurationUs == 0u);

    observation = health.observe(250u, diagnostics, stats);
    QNP_CHECK(!observation.stall.occurred());

    diagnostics.activeStallReason =
        qnetplay::StallReason::PredictionLimit;
    observation = health.observe(400u, diagnostics, stats);
    QNP_CHECK(observation.stall.kind
              == qgame::RollbackStallTransitionKind::ReasonChanged);
    QNP_CHECK(observation.stall.from
              == qnetplay::StallReason::LocalInputMissing);
    QNP_CHECK(observation.stall.to
              == qnetplay::StallReason::PredictionLimit);
    QNP_CHECK(observation.stall.episodeDurationUs == 300u);
    QNP_CHECK(observation.stall.previousReasonDurationUs == 300u);

    diagnostics.activeStallReason = qnetplay::StallReason::None;
    observation = health.observe(600u, diagnostics, stats);
    QNP_CHECK(observation.stall.kind
              == qgame::RollbackStallTransitionKind::Ended);
    QNP_CHECK(observation.stall.from
              == qnetplay::StallReason::PredictionLimit);
    QNP_CHECK(observation.stall.to
              == qnetplay::StallReason::None);
    QNP_CHECK(observation.stall.episodeDurationUs == 500u);
    QNP_CHECK(observation.stall.previousReasonDurationUs == 200u);

    const qgame::RollbackHealthReport report = health.finishReport();
    QNP_CHECK(report.activeWallUs == 100u);
    QNP_CHECK(report.stalledWallUs == 500u);
    QNP_CHECK(report.stalledWallUsByReason[
                  static_cast<size_t>(
                      qnetplay::StallReason::LocalInputMissing)]
              == 300u);
    QNP_CHECK(report.stalledWallUsByReason[
                  static_cast<size_t>(
                      qnetplay::StallReason::PredictionLimit)]
              == 200u);
}

QNP_TEST(RollbackHealthClockRegressionIsBoundedAndResetClears) {
    qgame::RollbackHealthTelemetry health;
    qnetplay::RollbackDiagnostics diagnostics{};
    qnetplay::RollbackStats stats{};
    diagnostics.valid = true;

    health.observe(1000u, diagnostics, stats);
    stats.visibleAdvances = 1u;
    health.observe(2000u, diagnostics, stats);

    const auto regression =
        health.observe(1500u, diagnostics, stats);
    QNP_CHECK(regression.clockRegressed);
    QNP_CHECK(!regression.reportDue);
    QNP_CHECK(health.totalObservedWallUs() == 1000u);
    QNP_CHECK(health.totalClockRegressionFaults() == 1u);

    const auto recovered =
        health.observe(2500u, diagnostics, stats);
    QNP_CHECK(!recovered.clockRegressed);
    QNP_CHECK(health.totalObservedWallUs() == 2000u);
    const qgame::RollbackHealthReport report = health.finishReport();
    QNP_CHECK(report.elapsedUs == 2000u);
    QNP_CHECK(report.clockRegressionFaults == 1u);
    QNP_CHECK(report.totalClockRegressionFaults == 1u);
    QNP_CHECK(report.noProgressAgeUs == 1000u);

    health.reset();
    QNP_CHECK(health.totalObservedWallUs() == 0u);
    QNP_CHECK(health.totalActiveWallUs() == 0u);
    QNP_CHECK(health.totalStalledWallUs() == 0u);
    QNP_CHECK(health.totalClockRegressionFaults() == 0u);
    QNP_CHECK(health.totalStatsRegressionFaults() == 0u);
    QNP_CHECK(!health.reportDue());

    diagnostics.activeStallReason =
        qnetplay::StallReason::ReplayInProgress;
    const auto afterReset = health.observe(777u, diagnostics, stats);
    QNP_CHECK(afterReset.stall.kind
              == qgame::RollbackStallTransitionKind::Began);
    QNP_CHECK(afterReset.stall.to
              == qnetplay::StallReason::ReplayInProgress);
    QNP_CHECK(afterReset.stall.episodeDurationUs == 0u);
}

QNP_TEST(RollbackHealthSeparatesAdapterHoldsFromRunnableWallTime) {
    QNP_CHECK(std::strcmp(qgame::adapterHoldReasonName(
                  qgame::AdapterHoldReason::ExactInput),
              "exact-input") == 0);
    QNP_CHECK(std::strcmp(qgame::adapterHoldReasonName(
                  qgame::AdapterHoldReason::DelayDrain),
              "delay-drain") == 0);
    QNP_CHECK(std::strcmp(qgame::adapterHoldReasonName(
                  qgame::AdapterHoldReason::Count),
              "unknown") == 0);

    qgame::RollbackHealthTelemetry health;
    qnetplay::RollbackDiagnostics diagnostics{};
    qnetplay::RollbackStats stats{};
    diagnostics.valid = true;

    health.observe(0u, diagnostics, stats);
    auto observation = health.observe(
        100u, diagnostics, stats,
        qgame::AdapterHoldReason::ExactInput);
    QNP_CHECK(observation.adapterHold.kind
              == qgame::RollbackStallTransitionKind::Began);
    QNP_CHECK(observation.adapterHold.from
              == qgame::AdapterHoldReason::None);
    QNP_CHECK(observation.adapterHold.to
              == qgame::AdapterHoldReason::ExactInput);
    QNP_CHECK(observation.adapterHold.episodeDurationUs == 0u);
    QNP_CHECK(health.currentAdapterHoldReason()
              == qgame::AdapterHoldReason::ExactInput);

    observation = health.observe(
        400u, diagnostics, stats,
        qgame::AdapterHoldReason::DelayDrain);
    QNP_CHECK(observation.adapterHold.kind
              == qgame::RollbackStallTransitionKind::ReasonChanged);
    QNP_CHECK(observation.adapterHold.from
              == qgame::AdapterHoldReason::ExactInput);
    QNP_CHECK(observation.adapterHold.to
              == qgame::AdapterHoldReason::DelayDrain);
    QNP_CHECK(observation.adapterHold.episodeDurationUs == 300u);
    QNP_CHECK(observation.adapterHold.previousReasonDurationUs == 300u);
    QNP_CHECK(health.currentAdapterHoldEpisodeAgeUs() == 300u);
    QNP_CHECK(health.currentAdapterHoldReasonAgeUs() == 0u);

    diagnostics.activeStallReason =
        qnetplay::StallReason::PredictionLimit;
    observation = health.observe(
        650u, diagnostics, stats,
        qgame::AdapterHoldReason::DelayDrain);
    QNP_CHECK(observation.stall.kind
              == qgame::RollbackStallTransitionKind::Began);
    QNP_CHECK(!observation.adapterHold.occurred());

    observation = health.observe(
        800u, diagnostics, stats,
        qgame::AdapterHoldReason::None);
    QNP_CHECK(observation.adapterHold.kind
              == qgame::RollbackStallTransitionKind::Ended);
    QNP_CHECK(observation.adapterHold.from
              == qgame::AdapterHoldReason::DelayDrain);
    QNP_CHECK(observation.adapterHold.to
              == qgame::AdapterHoldReason::None);
    QNP_CHECK(observation.adapterHold.episodeDurationUs == 700u);
    QNP_CHECK(observation.adapterHold.previousReasonDurationUs == 400u);

    diagnostics.activeStallReason = qnetplay::StallReason::None;
    observation = health.observe(900u, diagnostics, stats);
    QNP_CHECK(observation.stall.kind
              == qgame::RollbackStallTransitionKind::Ended);

    const qgame::RollbackHealthReport report = health.finishReport();
    QNP_CHECK(report.elapsedUs == 900u);
    QNP_CHECK(report.activeWallUs == 650u);
    QNP_CHECK(report.stalledWallUs == 250u);
    QNP_CHECK(report.adapterHoldWallUs == 700u);
    QNP_CHECK(report.adapterHoldWallUsByReason[
                  static_cast<size_t>(
                      qgame::AdapterHoldReason::ExactInput)]
              == 300u);
    QNP_CHECK(report.adapterHoldWallUsByReason[
                  static_cast<size_t>(
                      qgame::AdapterHoldReason::DelayDrain)]
              == 400u);
    QNP_CHECK(report.runnableWallUs == 100u);
    QNP_CHECK(report.blockedWallUs == 800u);
    QNP_CHECK(report.runnableWallUs + report.blockedWallUs
              == report.elapsedUs);
    QNP_CHECK(report.adapterHoldReason
              == qgame::AdapterHoldReason::None);
    QNP_CHECK(health.totalAdapterHoldWallUs() == 700u);
    QNP_CHECK(health.totalRunnableWallUs() == 100u);
    QNP_CHECK(health.totalBlockedWallUs() == 800u);
    QNP_CHECK(health.totalAdapterHoldWallUsByReason()[
                  static_cast<size_t>(
                      qgame::AdapterHoldReason::DelayDrain)]
              == 400u);

    health.reset();
    QNP_CHECK(health.currentAdapterHoldReason()
              == qgame::AdapterHoldReason::None);
    QNP_CHECK(health.currentAdapterHoldEpisodeAgeUs() == 0u);
    QNP_CHECK(health.currentAdapterHoldReasonAgeUs() == 0u);
    QNP_CHECK(health.totalAdapterHoldWallUs() == 0u);
    QNP_CHECK(health.totalRunnableWallUs() == 0u);
    QNP_CHECK(health.totalBlockedWallUs() == 0u);
}

QNP_TEST(RollbackWindowHotkeysAreIndependentAndGatedLikeTheDelayKeys) {
    qgame::RuntimeDelayHotkeys keys;
    qgame::RuntimeDelayKeySample s{};
    s.playerNetplay = true;
    s.focused = true;
    s.rollbackGameplay = true;

    s.bracketRightDown = true;
    QNP_CHECK(keys.pollRollback(s) == 1);
    QNP_CHECK(keys.pollRollback(s) == 0);
    s.bracketRightDown = false;
    QNP_CHECK(keys.pollRollback(s) == 0);
    s.bracketLeftDown = true;
    QNP_CHECK(keys.pollRollback(s) == -1);
    s.bracketLeftDown = false;
    (void)keys.pollRollback(s);

    s.bracketLeftDown = true;
    s.bracketRightDown = true;
    QNP_CHECK(keys.pollRollback(s) == 0);
    s.bracketLeftDown = false;
    s.bracketRightDown = false;
    (void)keys.pollRollback(s);

    s.bracketRightDown = true;
    s.controlDown = true;
    QNP_CHECK(keys.pollRollback(s) == 0);
    s.controlDown = false;
    QNP_CHECK(keys.pollRollback(s) == 0);
    s.bracketRightDown = false;
    (void)keys.pollRollback(s);

    s.focused = false;
    s.bracketRightDown = true;
    QNP_CHECK(keys.pollRollback(s) == 0);
    s.focused = true;
    s.bracketRightDown = false;
    (void)keys.pollRollback(s);

    s.equalsDown = true;
    s.bracketRightDown = true;
    QNP_CHECK(keys.poll(s) == 1);
    QNP_CHECK(keys.pollRollback(s) == 1);

    QNP_CHECK(qgame::adjustedGameplayRollback(
                  qgame::kMaximumGameplayRollback, 1)
              == qgame::kMaximumGameplayRollback);
    QNP_CHECK(qgame::adjustedGameplayRollback(
                  qgame::kMinimumGameplayRollback, -1)
              == qgame::kMinimumGameplayRollback);
    QNP_CHECK(qgame::adjustedGameplayRollback(8, 1) == 9);
    QNP_CHECK(qgame::adjustedGameplayRollback(8, -1) == 7);
    QNP_CHECK(qgame::adjustedGameplayRollback(8, 0) == 8);
}
