#include "TestFramework.hpp"
#include "game/DeterministicAudioPolicy.h"
#include "game/DeterministicConfigPolicy.h"
#include "game/FCommandNetplayPolicy.h"
#include "game/NativeDeveloperKeyPolicy.h"
#include "game/NetplayPresentationPolicy.h"
#include "game/SpectatorPlaybackPolicy.h"
#include "spectator/SpectatorSession.h"
#include "ipc/IpcConfig.h"
#include "input/InputActions.h"
#include "qoh99/SceneSystem.hpp"
#include "qoh99/MatchSystem.hpp"
#include <array>
#include <fstream>
#include <iterator>
#include <string>

using namespace qoh99;

static std::string readNetplayFile(const char* rel);

TEST(Stage8ExactBarrierReadsTheNativeStage8Countdown) {
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    CHECK(!hooks.empty());

    CHECK(hooks.find(
              "constexpr uintptr_t kFinishStageTimerAddr = 0x4AA3D0u;")
          != std::string::npos);
    CHECK(hooks.find(
              "constexpr uintptr_t kFinishStageTimerAddr = 0x4AA408u;")
          == std::string::npos);
    CHECK(hooks.find(
              "v.finishTimerA = rd32(kFinishStageTimerAddr);")
          != std::string::npos);
    CHECK(hooks.find("v.finishTimerB = rd32(0x4AA408u);")
          != std::string::npos);
}

TEST(CasterSettingsWaitIsVisibleAndHeartbeatBounded) {
    const std::string caster = readNetplayFile("src/client/Main.cpp");
    CHECK(!caster.empty());

    CHECK(caster.find(
              "constexpr uint64_t kSettingsHeartbeatEveryUs = 1000000ull;")
          != std::string::npos);
    CHECK(caster.find(
              "constexpr uint64_t kSettingsHeartbeatTimeoutUs = 10000000ull;")
          != std::string::npos);
    CHECK(caster.find("\"selection\"") != std::string::npos);
    CHECK(caster.find("\"peer-confirm\"") != std::string::npos);
    CHECK(caster.find("\"route-commit\"") != std::string::npos);
    CHECK(caster.find(
              "Your opponent stopped responding during match settings.")
          != std::string::npos);

    const std::size_t showWait = caster.find(
        "showSettingsWait(con, \"Waiting for opponent to confirm settings...");
    const std::size_t confirmationDeadline = caster.find(
        "const uint64_t confirmationDeadlineMs", showWait);
    CHECK(showWait != std::string::npos);
    CHECK(confirmationDeadline != std::string::npos);
    CHECK(showWait < confirmationDeadline);
    CHECK(caster.find(
              "if (con) con->clear();\n    info(con, line);")
          != std::string::npos);
    CHECK(caster.find(
              "if (con) con->clear();\n        info(con, why + \" - waiting for a new opponent...\");")
          != std::string::npos);
}

static qipc::IpcConfig validClientPrivatePublisherConfig() {
    qipc::IpcConfig cfg{};
    cfg.mode = qipc::MODE_NETPLAY;
    cfg.isHost = 0u;
    cfg.localPlayer = 2u;
    cfg.delay = 3u;
    cfg.rollback = 6u;
    cfg.remoteDelay = 2u;
    cfg.remoteRollback = 7u;
    cfg.sessionId = 0x13572468u;
    cfg.compatibilityId = qipc::kRuntimeCompatibilityId;
    cfg.cookieLow = 0xABCD1234u;
    cfg.spectatorCookieLow = 0x13579BDFu;
    cfg.hasSocket = 1u;
    cfg.peerAddrBE = 0x0100007Fu;
    cfg.peerPortBE = 0x4C1Du;
    cfg.serveSpectators = 1u;
    cfg.spectatorServiceMode = qipc::SPECTATOR_SERVICE_SIDECAR;
    cfg.hasSpectatorSocket = 0u;
    cfg.spectatorClientPublisherEnabled = 1u;
    cfg.spectatorClientPublisherGeneration = 17u;
    cfg.spectatorClientPublisherNonce = 0x8A4C2E17061B3D55ull;
    cfg.configurationProvenance =
        qnetplay::config_provenance::makeCommittedWire(
            qnetplay::config_provenance::Stage::LocallySelected,
            qnetplay::config_provenance::Stage::LocallySelected,
            qnetplay::config_provenance::Stage::CasterAdvertised,
            qnetplay::config_provenance::Stage::CasterAdvertised);
    return cfg;
}

static bool productionRollbackEligible(int scene, int s3sub, int matchSub) {
    GamePhase p = SceneSystem::classify(scene, s3sub, matchSub);
    return SceneSystem::policyFor(p).rollbackAllowed;
}

TEST(ProductionPhasePolicyMatchesReModel) {
    CHECK(productionRollbackEligible(2, 2, 0));
    CHECK(!productionRollbackEligible(1, 0, 0));
    CHECK(!productionRollbackEligible(3, 1, 0));
    CHECK(!productionRollbackEligible(3, 2, 0));
    CHECK(!productionRollbackEligible(2, 2, -1));
    CHECK(!productionRollbackEligible(4, 0, 0));
}

TEST(NoRollbackOutsideGameplay) {
    int eligible = 0;
    for (int matchSub = -1; matchSub <= 5; ++matchSub)
        if (productionRollbackEligible(2, 2, matchSub))
            ++eligible;
    CHECK_EQ(eligible, 1);
    for (int m = 1; m <= 5; ++m)
        CHECK(!productionRollbackEligible(2, 2, m));
}

TEST(NoInputDuringNoControlIntro) {
    PhasePolicy p = SceneSystem::policyFor(GamePhase::MatchIntro);
    CHECK(p.input == InputPolicy::NoInput);
    CHECK(!p.rollbackAllowed);
    CHECK(p.saveAllowed);
}

TEST(RoundIntroRotationGuardOnlyClampsPlayerCatchupHeadroom) {
    const std::string source = readNetplayFile("src/GameHooks.cpp");
    CHECK(!source.empty());
    const std::size_t opportunity = source.find(
        "qgame::OpportunityInputs opportunity{};");
    const std::size_t guard = source.find(
        "qgame::roundIntroRotationPresentationCritical(", opportunity);
    const std::size_t clamp = source.find(
        "opportunity.safeForwardHeadroom = 0u;", guard);
    const std::size_t plan = source.find(
        "qgame::planSchedule(domains, opportunity, kNetSchedulePolicy);",
        opportunity);
    CHECK(opportunity != std::string::npos);
    CHECK(guard != std::string::npos);
    CHECK(clamp != std::string::npos);
    CHECK(plan != std::string::npos);
    CHECK(opportunity < guard);
    CHECK(guard < clamp);
    CHECK(clamp < plan);
    CHECK(source.find(
              "GameplayRegionCatalog::find(\"round_intro_state\")")
          != std::string::npos);
    CHECK(source.find(
              "GameplayRegionCatalog::find(\"round_intro_control\")")
          != std::string::npos);
    if (guard != std::string::npos && plan != std::string::npos)
        CHECK(source.substr(guard, plan - guard).find(
                  "lifecycleExactBoundary") == std::string::npos);
}

TEST(SkipResimOnGameplayExitFrame) {
    CHECK(MatchSystem::exitsGameplay(MatchSub::Gameplay, MatchSub::RoundWin));
    CHECK(MatchSystem::exitsGameplay(MatchSub::Gameplay, MatchSub::Unknown1));
    CHECK(!MatchSystem::exitsGameplay(MatchSub::Gameplay, MatchSub::Gameplay));
}

TEST(RollbackStopsAtCorrectedGameplayExitFrame) {
    CHECK(MatchSystem::exitsGameplay(MatchSub::Gameplay, MatchSub::RoundWin));
    CHECK(MatchSystem::exitsGameplay(MatchSub::Gameplay, MatchSub::Unknown1));
    CHECK(MatchSystem::exitsGameplay(MatchSub::Gameplay, MatchSub::Unknown4));
    CHECK(MatchSystem::exitsGameplay(MatchSub::Gameplay, MatchSub::RoundIntro));
    CHECK(!MatchSystem::exitsGameplay(MatchSub::Gameplay, MatchSub::Gameplay));
    for (int m = 1; m <= 5; ++m)
        CHECK(!SceneSystem::policyFor(SceneSystem::classify(2, 0, m)).rollbackAllowed);
}

TEST(CorrectedGameplayExitCommitsAndReturnsNativeTransition) {
    const std::string source = readNetplayFile("src/GameHooks.cpp");
    const std::string session =
        readNetplayFile("src/netplay/RollbackSession.cpp");
    const std::string header =
        readNetplayFile("src/netplay/RollbackSession.h");
    CHECK(!source.empty());
    CHECK(!session.empty());
    CHECK(!header.empty());

    CHECK(header.find("bool finishRollbackAtBoundary();")
          != std::string::npos);
    CHECK(header.find("bool finishRollbackBeforeBoundary();")
          != std::string::npos);
    CHECK(session.find("discardSpeculativeSuffix")
          != std::string::npos);
    CHECK(session.find("consumed = ConsumedSlot{}")
          != std::string::npos);
    CHECK(source.find(
              "g_netSession->finishRollbackAtBoundary()")
          != std::string::npos);
    CHECK(source.find(
              "g_netSession->finishRollbackBeforeBoundary()")
          != std::string::npos);
    CHECK(source.find(
              "netRestorePreFrame(frame, phaseBefore)")
          != std::string::npos);
    CHECK(source.find(
              "held corrected gameplay boundary frame=%u")
          != std::string::npos);
    CHECK(source.find(
              "const size_t stateSlot = frame % kPoolSlots")
          != std::string::npos);
    CHECK(source.find(
              "invalidateTaggedSlot(")
          != std::string::npos);
    CHECK(source.find(
              "corrected gameplay boundary frame=%u")
          != std::string::npos);
    CHECK(source.find(
              "return correction.dispatchResult;")
          != std::string::npos);
    CHECK(source.find(
              "corrected replay attempted to leave gameplay")
          == std::string::npos);
}

TEST(SpectatorGameplayEntryRendezvousIsViewerLocal) {
    using qgame::SpectatorPreStateDecision;
    constexpr uint64_t expected = 0x123456789ABCDEF0ull;
    constexpr uint64_t other = 0x0FEDCBA987654321ull;

    CHECK(qgame::spectatorPreStateDecision(
              false, expected, expected, 0)
          == SpectatorPreStateDecision::Accept);
    CHECK(qgame::spectatorPreStateDecision(
              false, expected, other,
              qgame::kSpectatorGameplayEntryTimeoutMs - 1u)
          == SpectatorPreStateDecision::RetryEntry);
    CHECK(qgame::spectatorPreStateDecision(
              true, expected, other, 0)
          == SpectatorPreStateDecision::Reject);
    CHECK(qgame::spectatorPreStateDecision(
              false, expected, other,
              qgame::kSpectatorGameplayEntryTimeoutMs)
          == SpectatorPreStateDecision::Reject);
    CHECK(qgame::spectatorPreStateDecision(false, 0, 0, 0)
          == SpectatorPreStateDecision::Reject);
}

TEST(SpectatorArchiveStopsOnlyAtTrueFrontendQuit) {
    CHECK(qgame::spectatorSourceStopsOnDispatchReturn(1, 0));
    CHECK(qgame::spectatorSourceStopsOnDispatchReturn(3, 0));
    CHECK(!qgame::spectatorSourceStopsOnDispatchReturn(2, 0));
    CHECK(!qgame::spectatorSourceStopsOnDispatchReturn(1, 1));
    CHECK(!qgame::spectatorSourceStopsOnDispatchReturn(3, -1));
}

TEST(SidecarSpectatorSocketOwnershipIsAcceptedAndGuarded) {
    qipc::IpcConfig cfg{};
    cfg.mode = qipc::MODE_NETPLAY;
    cfg.isHost = 1;
    cfg.serveSpectators = 1;
    cfg.spectatorServiceMode = qipc::SPECTATOR_SERVICE_SIDECAR;
    cfg.cookieLow = 1;
    cfg.spectatorCookieLow = 2;

    CHECK(qipc::validateSpectatorSocketHandoff(cfg) == nullptr);

    cfg.hasSpectatorSocket = 1;
    CHECK(qipc::validateSpectatorSocketHandoff(cfg) != nullptr);

    cfg.spectatorServiceMode = qipc::SPECTATOR_SERVICE_INPROCESS;
    CHECK(qipc::validateSpectatorSocketHandoff(cfg) == nullptr);
    cfg.hasSpectatorSocket = 0;
    CHECK(qipc::validateSpectatorSocketHandoff(cfg) != nullptr);

    const std::string dllMain = readNetplayFile("src/DllMain.cpp");
    const std::string transport =
        readNetplayFile("src/game/DPlayTransport.cpp");
    CHECK(dllMain.find("validateSpectatorService(cfg)")
          != std::string::npos);
    CHECK(dllMain.find("validateSpectatorSocketHandoff(cfg)")
          != std::string::npos);
    CHECK(transport.find("validateSpectatorService(cfg)")
          != std::string::npos);
    CHECK(transport.find("validateSpectatorSocketHandoff(cfg)")
          != std::string::npos);
}

TEST(ClientPrivateSpectatorPublisherConfigurationIsNarrowAndFailClosed) {
    const qipc::IpcConfig valid = validClientPrivatePublisherConfig();
    CHECK(qipc::validateSpectatorClientPrivatePublisher(valid) == nullptr);

    CHECK(qipc::validateSpectatorService(valid) == nullptr);
    CHECK(qipc::validateSpectatorSocketHandoff(valid) == nullptr);

    qipc::IpcConfig invalid = valid;
    invalid.isHost = 1u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.mode = qipc::MODE_SPECTATOR;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.serveSpectators = 0u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.spectatorServiceMode = qipc::SPECTATOR_SERVICE_INPROCESS;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.hasSpectatorSocket = 1u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.spectatorClientPublisherGeneration = 0u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.spectatorClientPublisherNonce = 0u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.cookieLow = 0u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.compatibilityId = 0u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.spectatorCookieLow = invalid.cookieLow;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.spectatorMaxViewers = 1u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.spectatorGrantId = 1u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.spectatorRelayActive = 1u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.spectatorTopologyEnabled = 1u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.spectatorViewerId = 1u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    invalid = valid;
    invalid.spectatorClientPublisherEnabled = 2u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(invalid) != nullptr);
    CHECK(qipc::validateSpectatorService(invalid) != nullptr);

    qipc::IpcConfig disabled{};
    CHECK(qipc::validateSpectatorClientPrivatePublisher(disabled) == nullptr);
    disabled.spectatorClientPublisherGeneration = 1u;
    CHECK(qipc::validateSpectatorClientPrivatePublisher(disabled) != nullptr);
}

TEST(ClientPrivateSpectatorPublisherRuntimeAppliedReceiptRoundTripsAndBinds) {
    const qipc::IpcConfig cfg = validClientPrivatePublisherConfig();
    constexpr uint32_t kProcessId = 4242u;

    qipc::SpectatorClientPublisherRuntimeAppliedReceipt receipt{};
    CHECK(qipc::buildSpectatorClientPublisherRuntimeAppliedReceipt(
        cfg, kProcessId, receipt));
    CHECK(qipc::validSpectatorClientPublisherRuntimeAppliedReceiptShape(
        receipt));
    CHECK_EQ(receipt.runtimeStage, static_cast<uint8_t>(
        qnetplay::config_provenance::Stage::RuntimeApplied));

    std::array<uint8_t,
               qipc::kSpectatorClientPublisherRuntimeAppliedReceiptBytes> wire{};
    size_t written = 0u;
    CHECK(qipc::encodeSpectatorClientPublisherRuntimeAppliedReceipt(
        receipt, wire.data(), wire.size(), written));
    CHECK_EQ(written,
             qipc::kSpectatorClientPublisherRuntimeAppliedReceiptBytes);

    qipc::SpectatorClientPublisherRuntimeAppliedReceipt decoded{};
    CHECK(qipc::decodeSpectatorClientPublisherRuntimeAppliedReceipt(
        wire.data(), written, decoded));
    CHECK(qipc::spectatorClientPublisherRuntimeAppliedReceiptMatches(
        decoded, cfg, kProcessId));

    decoded.delay ^= 1u;
    CHECK(!qipc::spectatorClientPublisherRuntimeAppliedReceiptMatches(
        decoded, cfg, kProcessId));
    decoded = receipt;
    CHECK(!qipc::spectatorClientPublisherRuntimeAppliedReceiptMatches(
        decoded, cfg, kProcessId + 1u));
    decoded = receipt;
    decoded.configurationProvenance.peerRuntimeAttestation =
        static_cast<uint8_t>(
            qnetplay::config_provenance::Stage::PeerRuntimeAttested);
    CHECK(!qipc::validSpectatorClientPublisherRuntimeAppliedReceiptShape(
        decoded));

    wire[0] ^= 0xFFu;
    CHECK(!qipc::decodeSpectatorClientPublisherRuntimeAppliedReceipt(
        wire.data(), wire.size(), decoded));
    CHECK(!qipc::decodeSpectatorClientPublisherRuntimeAppliedReceipt(
        wire.data(), wire.size() - 1u, decoded));

    qipc::IpcConfig host = cfg;
    host.isHost = 1u;
    CHECK(!qipc::buildSpectatorClientPublisherRuntimeAppliedReceipt(
        host, kProcessId, decoded));
    qipc::IpcConfig off = cfg;
    off.mode = qipc::MODE_OFF;
    CHECK(!qipc::buildSpectatorClientPublisherRuntimeAppliedReceipt(
        off, kProcessId, decoded));
}

TEST(SpectatorTopologyBootstrapIsExplicitAndFailClosed) {
    qipc::IpcConfig disabled{};
    CHECK(qipc::validateSpectatorTopologyClient(disabled) == nullptr);
    disabled.spectatorTopologyHostControlPort = 7500;
    CHECK(qipc::validateSpectatorTopologyClient(disabled) != nullptr);

    qipc::IpcConfig cfg{};
    cfg.mode = qipc::MODE_SPECTATOR;
    cfg.isHost = 0;
    cfg.localPlayer = 0;
    cfg.hasSocket = 1;
    cfg.peerAddrBE = 0x0100007Fu;
    cfg.peerPortBE = 0x4C1Du;
    cfg.sessionId = 0x11223344u;
    cfg.spectatorCookieLow = 1;
    cfg.spectatorViewerId = 0x1020304050607080ull;
    cfg.spectatorGeneration = 7u;
    cfg.spectatorGrantId = 0x8877665544332211ull;
    cfg.spectatorGrantExpiryUnix = 0x0102030405060708ull;
    cfg.spectatorGrantMacLow = 0x0123456789ABCDEFull;
    cfg.spectatorGrantMacHigh = 0xFEDCBA9876543210ull;
    cfg.spectatorTopologyEnabled = 1;
    cfg.spectatorTopologyRouteKind =
        qipc::kSpectatorTopologyRouteDirect;
    cfg.spectatorTopologyHostControlIpv4BE = 0x0100007Fu;
    cfg.spectatorTopologyHostControlPort = 7500;
    cfg.spectatorTopologyQspRouteGeneration = 7u;
    cfg.spectatorTopologyGrantSourceGeneration = 1u;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) == nullptr);

    cfg.spectatorTopologyRouteKind =
        qipc::kSpectatorTopologyRoutePunched;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) == nullptr);
    cfg.spectatorTopologyRouteKind = qipc::kSpectatorTopologyRouteRelay;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) != nullptr);
    cfg.spectatorRelayActive = 1u;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) != nullptr);
    cfg.spectatorRelayRoomLow = 0xA1A2A3A4A5A6A7A8ull;
    cfg.spectatorRelayTokenHigh = 0xB1B2B3B4B5B6B7B8ull;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) != nullptr);
    cfg.spectatorRelaySourceGeneration = 9u;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) == nullptr);
    cfg.reservedV15[1] = 1u;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) != nullptr);
    cfg.reservedV15[1] = 0u;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) == nullptr);
    cfg.spectatorTopologyRouteKind = 4u;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) != nullptr);
    cfg.spectatorTopologyRouteKind = qipc::kSpectatorTopologyRouteDirect;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) != nullptr);
    cfg.spectatorRelayActive = 0u;
    cfg.spectatorRelayRoomLow = 0u;
    cfg.spectatorRelayTokenHigh = 0u;
    cfg.spectatorRelaySourceGeneration = 0u;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) == nullptr);

    cfg.spectatorTopologyQspRouteGeneration = 8u;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) != nullptr);
    cfg.spectatorTopologyQspRouteGeneration = cfg.spectatorGeneration;
    cfg.spectatorTopologyGrantSourceGeneration = 0u;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) != nullptr);
    cfg.spectatorTopologyGrantSourceGeneration = 1u;
    cfg.spectatorRelayActive = 1u;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) != nullptr);
    cfg.spectatorRelayActive = 0u;
    cfg.spectatorSequenceBase = 7u;
    CHECK(qipc::validateSpectatorTopologyClient(cfg) == nullptr);
    cfg.spectatorSequenceBase = 0u;

    const std::string dllMain = readNetplayFile("src/DllMain.cpp");
    CHECK(dllMain.find("validateSpectatorTopologyClient(cfg)")
          != std::string::npos);
}

static std::string readNetplayFile(const char* rel) {
    std::string here = __FILE__;
    std::size_t pos = here.find("src");
    if (pos == std::string::npos) return {};
    std::string root = here.substr(0, pos);
    std::ifstream f(root + rel);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
TEST(AutomatedHarnessUsesHiddenDesktopByDefault) {
    const std::string harness =
        readNetplayFile("src/netplay/tests/live_game_e2e.py");
    CHECK(!harness.empty());
    CHECK(harness.find("--visible-games") != std::string::npos);
    std::size_t hiddenFlagUses = 0;
    for (std::size_t at = harness.find("\"--hidden-desktop\"");
         at != std::string::npos;
         at = harness.find("\"--hidden-desktop\"", at + 1)) {
        ++hiddenFlagUses;
    }
    if (hiddenFlagUses < 3)
        testfw::reportFail(__FILE__, __LINE__,
            "live_game_e2e.py must pass --hidden-desktop for host, client, "
            "and spectator commands");
}

TEST(LayoutFingerprintEmitsOrderedComponentDiagnostics) {
    const std::string source = readNetplayFile("src/GameHooks.cpp");
    CHECK(!source.empty());
    CHECK(source.find("struct NetLayoutDigestComponents")
          != std::string::npos);
    CHECK(source.find(
              "g_rb.hashLive(perRegion, RollbackManager::Profile::Gameplay)")
          != std::string::npos);
    CHECK(source.find("[layout-diagnostic]") != std::string::npos);
    CHECK(source.find("[layout-region] index=%zu name=%s")
          != std::string::npos);
    CHECK(source.find("[layout-config] valid=%u profile=%u")
          != std::string::npos);
    CHECK(source.find("shared20=%016llX shared334=%016llX")
          != std::string::npos);
    const std::string rollback =
        readNetplayFile("src/RollbackManager.cpp");
    CHECK(rollback.find("\"qoh_config_common_tail\"")
          == std::string::npos);
    CHECK(rollback.find("kQohConfigSharedBlockTailSize")
          == std::string::npos);
    CHECK(rollback.find("qohNetSchedulerConfigValid")
          != std::string::npos);
    CHECK(source.find("netNormalizeLocalSchedulerConfig")
          != std::string::npos);
    CHECK(source.find("pinned real frame-dispatch thread")
          != std::string::npos);
    CHECK(source.find("netLogLayoutDigest(\"first-submission\")")
          != std::string::npos);
    CHECK(source.find("netLogLayoutDigest(\"peer-mismatch\")")
          != std::string::npos);
}

TEST(NetplayOverridesAreScopedOutOfQohConfigPersistence) {
    const std::string source = readNetplayFile("src/GameHooks.cpp");
    CHECK(!source.empty());

    CHECK(source.find("FunctionId::ConfigSave") != std::string::npos);
    CHECK(source.find("g_configSaveFn") != std::string::npos);
    CHECK(source.find("kConfigSaveFn =") == std::string::npos);
    CHECK(source.find(
              "0x81, 0xEC, 0x04, 0x01, 0x00, 0x00,")
          != std::string::npos);
    CHECK(source.find("unsigned int __fastcall hkConfigSave")
          != std::string::npos);
    CHECK(source.find("netStageOriginalConfigForSave(runtime)")
          != std::string::npos);
    CHECK(source.find("netRestoreRuntimeConfigAfterSave(runtime)")
          != std::string::npos);
    CHECK(source.find("netRestoreOriginalConfigInMemory()")
          != std::string::npos);
    CHECK(source.find("netActiveConfigIdentityValid(")
          != std::string::npos);
    CHECK(source.find(
              "RollbackManager::expectedQohBindingPointer(active)")
          != std::string::npos);
    CHECK(source.find(
              "36u, 40u, 44u, 48u, 64u, 80u, 84u, 88u, 92u, 96u, 112u, 376u")
          != std::string::npos);
    CHECK(source.find("!g_configSaveHooked")
          != std::string::npos);
    CHECK(source.find(
              "preference-preserving save hook enabled")
          != std::string::npos);
    CHECK(source.find(
              "saved QOHcnf.key successfully with local profile")
          != std::string::npos);
    CHECK(source.find("save did not preserve local")
          != std::string::npos);
    const std::size_t originalSave =
        source.find("g_origConfigSave(path, edx)");
    const std::size_t successMarker =
        source.find("saved QOHcnf.key successfully with local profile");
    CHECK(originalSave != std::string::npos);
    CHECK(successMarker != std::string::npos);
    CHECK(originalSave < successMarker);
}

TEST(FCommandNetplayPolicyForcesExactlyTheAuditedRiskyValues) {
    qgame::FCommandRiskyState state{};
    state.qohNetworkMode = 1u;
    state.waitTimeMode = 0u;
    state.waitPeriod = 0x80004E20u;
    state.printDecDamage = 1u;
    state.printMasterDamage = 1u;
    state.comVsAsobiVersion = 1u;
    state.extraFunctionFlags = 3u;
    state.waitCount = 7u;
    state.waitCounter = 6u;
    state.extraSkip = 5u;
    state.extraSkipCounter = 4u;
    state.debugWindow = 1u;
    state.displayFlags = 0x6u;

    const qgame::FCommandBootPacing boot{
        1u, 0x80ABCDEFu
    };
    qgame::applyNetplayFCommandDefaults(state, boot);
    CHECK(qgame::netplayFCommandDefaultsApplied(state, boot));
    CHECK_EQ(state.qohNetworkMode, 0u);
    CHECK_EQ(state.waitTimeMode, 1u);
    CHECK_EQ(state.waitPeriod, 0x80ABCDEFu);
    CHECK_EQ(state.printDecDamage, 0u);
    CHECK_EQ(state.printMasterDamage, 0u);
    CHECK_EQ(state.comVsAsobiVersion, 0u);
    CHECK_EQ(state.extraFunctionFlags, 0u);
    CHECK_EQ(state.waitCount, 0u);
    CHECK_EQ(state.waitCounter, 0u);
    CHECK_EQ(state.extraSkip, 0u);
    CHECK_EQ(state.extraSkipCounter, 0u);
    CHECK_EQ(state.debugWindow, 0u);
    CHECK_EQ(state.displayFlags & qgame::kFullColorDisplayFlag,
             qgame::kFullColorDisplayFlag);
    CHECK_EQ(state.displayFlags, 0x7u);

    qgame::applyNetplayFCommandDefaults(state, boot);
    CHECK(qgame::netplayFCommandDefaultsApplied(state, boot));
    CHECK_EQ(state.displayFlags, 0x7u);
}

TEST(FCommandNetplayPolicyLeavesAnAlreadyFullColorProfileUntouched) {
    qgame::FCommandRiskyState state{};
    state.displayFlags = 0x5u;
    const qgame::FCommandBootPacing boot{ 1u, 0x80ABCDEFu };
    qgame::applyNetplayFCommandDefaults(state, boot);
    CHECK_EQ(state.displayFlags, 0x5u);
    CHECK(qgame::netplayFCommandDefaultsApplied(state, boot));
}

TEST(NativeDeveloperKeyPolicyRespectsOnlyInteractiveOfflinePlay) {
    using qgame::NativeDeveloperKeyPolicy;
    using qgame::NativeDeveloperRuntimeMode;
    constexpr NativeDeveloperRuntimeMode offline =
        NativeDeveloperRuntimeMode::OfflineInteractive;
    constexpr NativeDeveloperRuntimeMode diagnostic =
        NativeDeveloperRuntimeMode::SelfTestOrProbe;

    CHECK(qgame::nativeDeveloperKeyPolicy(
              offline, true, false, false, false, false)
          == NativeDeveloperKeyPolicy::RespectUserFCommand);
    CHECK(qgame::nativeDeveloperKeyPolicy(
              offline, false, false, false, false, false)
          == NativeDeveloperKeyPolicy::ForceDisabled);
    CHECK(qgame::nativeDeveloperKeyPolicy(
              offline, true, true, false, false, false)
          == NativeDeveloperKeyPolicy::ForceDisabled);
    CHECK(qgame::nativeDeveloperKeyPolicy(
              offline, true, false, true, false, false)
          == NativeDeveloperKeyPolicy::ForceDisabled);
    CHECK(qgame::nativeDeveloperKeyPolicy(
              offline, true, false, false, true, false)
          == NativeDeveloperKeyPolicy::ForceDisabled);
    CHECK(qgame::nativeDeveloperKeyPolicy(
              offline, true, false, false, false, true)
          == NativeDeveloperKeyPolicy::ForceDisabled);
    CHECK(qgame::nativeDeveloperKeyPolicy(
              diagnostic, true, false, false, false, false)
          == NativeDeveloperKeyPolicy::ForceDisabled);
}

TEST(InteractiveOfflineDeveloperKeyBridgeFeedsNativeF2ThroughF9F12AndDelete) {
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    CHECK(!hooks.empty());

    const std::size_t begin = hooks.find(
        "static void "
        "mirrorInteractiveOfflineDeveloperKeysFromManagedSnapshot()");
    const std::size_t end = hooks.find(
        "\nstatic LRESULT CALLBACK inputFocusWndProc", begin);
    CHECK(begin != std::string::npos);
    CHECK(end != std::string::npos);
    if (begin == std::string::npos || end == std::string::npos) return;
    const std::string bridge = hooks.substr(begin, end - begin);

    CHECK(bridge.find("NativeDeveloperKeyPolicy::RespectUserFCommand")
          != std::string::npos);
    CHECK(bridge.find("g_inputKeyboardWorker->snapshot().read(snapshot)")
          != std::string::npos);
    CHECK(bridge.find("SnapshotStatus::Connected") != std::string::npos);
    CHECK(bridge.find("kInputStaleThresholdMs") != std::string::npos);
    CHECK(bridge.find("netProcessOwnsForegroundWindow()")
          != std::string::npos);

    CHECK(bridge.find(
              "0x3Cu, 0x3Du, 0x3Eu, 0x3Fu, 0x40u, 0x41u, 0x42u, 0x43u")
          != std::string::npos);
    CHECK(bridge.find("0x3Bu") == std::string::npos);
    CHECK(bridge.find("0x44u") == std::string::npos);
    CHECK(bridge.find("constexpr uint8_t kDikF12 = 0x58u;")
          != std::string::npos);
    CHECK(bridge.find("constexpr uint8_t kDikDelete = 0xD3u;")
          != std::string::npos);
    CHECK(bridge.find("if (mirrorKey(kDikF12)) pressedMask |= 0x100u;")
          != std::string::npos);
    CHECK(bridge.find("if (mirrorKey(kDikDelete)) pressedMask |= 0x200u;")
          != std::string::npos);
    CHECK(bridge.find("kNativeDikToVirtualKey + dik")
          != std::string::npos);
    CHECK(bridge.find("kNativeKeyStateAddrManaged + nativeIndex")
          != std::string::npos);

    const std::size_t poll = hooks.find("int __cdecl hkNativeFullPoll()");
    const std::size_t pollEnd = hooks.find("\n}", poll);
    CHECK(poll != std::string::npos);
    CHECK(pollEnd != std::string::npos);
    if (poll != std::string::npos && pollEnd != std::string::npos) {
        const std::string body = hooks.substr(poll, pollEnd - poll);
        const std::size_t original = body.find("g_origFullPoll()");
        const std::size_t mirror = body.find(
            "mirrorInteractiveOfflineDeveloperKeysFromManagedSnapshot()");
        const std::size_t policy = body.find(
            "applyNativeDeveloperEdgePolicy()", mirror);
        CHECK(original != std::string::npos);
        CHECK(mirror != std::string::npos);
        CHECK(policy != std::string::npos);
        CHECK(original < mirror);
        CHECK(mirror < policy);
    }

    CHECK(hooks.find(
              "deterministicTimelineMode || selfTestOrProbeMode\n"
              "              || interactiveOfflineDeveloperKeys")
          != std::string::npos);
    CHECK(hooks.find(
              "selfTestOrProbeMode || interactiveOfflineDeveloperKeys")
          != std::string::npos);
}

TEST(ForcedDisplayFlagsAreRestoredBeforeConfigSave) {
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    const std::size_t table =
        hooks.find("kQohSessionOverrideOffsets = {");
    CHECK(table != std::string::npos);
    if (table == std::string::npos) return;
    const std::size_t end = hooks.find("};", table);
    CHECK(end != std::string::npos);
    if (end == std::string::npos) return;
    const std::string offsets = hooks.substr(table, end - table);
    CHECK(offsets.find("376u") != std::string::npos);
    CHECK(hooks.find("wr32(active + 376u, state.displayFlags)")
          != std::string::npos);
    CHECK(hooks.find("state.displayFlags = rd32(active + 376u)")
          != std::string::npos);
}

TEST(FCommandParserHookIsLiveNetworkOnlyAndLeavesFilesAndPresentationAlone) {
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    const std::string dllMain = readNetplayFile("src/DllMain.cpp");
    const std::string caster = readNetplayFile("src/client/Main.cpp");
    CHECK(!hooks.empty());
    CHECK(!dllMain.empty());
    CHECK(!caster.empty());

    CHECK(hooks.find("kFCommandParseAddress = 0x46ED90u")
          != std::string::npos);
    const std::size_t signature = hooks.find(
        "\"FCommand parser\", kFCommandParseAddress");
    const std::size_t verifyLoop = hooks.find(
        "for (const MandatorySignature& check : kMandatory)");
    const std::size_t liveBlock = hooks.find(
        "if (deterministicNetworkMode) {");
    const std::size_t liveBlockEnd = hooks.find(
        "\n    void* target =", liveBlock);
    CHECK(signature != std::string::npos);
    CHECK(verifyLoop != std::string::npos);
    CHECK(liveBlock != std::string::npos);
    CHECK(liveBlockEnd != std::string::npos);
    CHECK(signature < verifyLoop);
    CHECK(verifyLoop < liveBlock);
    if (liveBlock != std::string::npos
        && liveBlockEnd != std::string::npos) {
        const std::string block =
            hooks.substr(liveBlock, liveBlockEnd - liveBlock);
        CHECK(block.find("if (deterministicNetworkMode)")
              != std::string::npos);
        CHECK(block.find("MH_CreateHook(") != std::string::npos);
        CHECK(block.find("MH_QueueEnableHook(fCommandTarget)")
              != std::string::npos);
        CHECK(block.find("replayMode") == std::string::npos);
        CHECK(block.find("deterministicTimelineMode")
              == std::string::npos);
        CHECK(block.find("explicitTrainingMode") == std::string::npos);
        CHECK(block.find("needsSceneHooks") == std::string::npos);
    }
    CHECK(hooks.find("!g_fCommandPolicyHooked")
          != std::string::npos);
    CHECK(hooks.find("!g_fCommandPolicyApplied")
          != std::string::npos);
    CHECK(hooks.find("g_origFCommandParse = nullptr")
          != std::string::npos);

    const std::size_t writer = hooks.find(
        "void netWriteFCommandRiskyState(");
    const std::size_t writerEnd = hooks.find(
        "\nvoid netResetFCommandPolicySession()", writer);
    CHECK(writer != std::string::npos);
    CHECK(writerEnd != std::string::npos);
    if (writer != std::string::npos
        && writerEnd != std::string::npos) {
        const std::string writes =
            hooks.substr(writer, writerEnd - writer);
        CHECK(writes.find("active + 80u") != std::string::npos);
        CHECK(writes.find("active + 112u") != std::string::npos);
        CHECK(writes.find("active + 376u") != std::string::npos);
        CHECK(writes.find("active + 108u") == std::string::npos);
        CHECK(writes.find("active + 116u") == std::string::npos);
        CHECK(writes.find("0x4AB89C") == std::string::npos);
    }

    const std::size_t hookBody = hooks.find(
        "unsigned int __fastcall hkFCommandParse(");
    const std::size_t hookBodyEnd = hooks.find(
        "\nunsigned int __fastcall hkConfigSave(", hookBody);
    CHECK(hookBody != std::string::npos);
    CHECK(hookBodyEnd != std::string::npos);
    if (hookBody != std::string::npos
        && hookBodyEnd != std::string::npos) {
        const std::string body =
            hooks.substr(hookBody, hookBodyEnd - hookBody);
        const std::size_t original =
            body.find("g_origFCommandParse(directory)");
        CHECK(original != std::string::npos);
        CHECK(body.find("g_origFCommandParse(directory)",
                        original + 1) == std::string::npos);
        CHECK(body.find("netStoreOriginalConfig(")
              != std::string::npos);
        CHECK(body.find("applyNetplayFCommandDefaults(")
              != std::string::npos);
    }

    CHECK(dllMain.find("cfg.mode == qipc::MODE_NETPLAY")
          != std::string::npos);
    CHECK(dllMain.find("cfg.mode == qipc::MODE_SPECTATOR")
          != std::string::npos);
    CHECK(caster.find("namespace fcmd_netplay")
          == std::string::npos);
    CHECK(caster.find("c.disableExtraFunctions = 1")
          != std::string::npos);
    CHECK(caster.find(
              "allowNativeDeveloperKeys && !trainingEnabled ? 0 : 1")
          != std::string::npos);
    CHECK(caster.find(
              "--allow-extra-functions is no longer supported")
          != std::string::npos);
}

TEST(NetplayConfigIdentityAndSaveResultFailClosedPolicy) {
    CHECK(qgame::netActiveConfigIdentityValid(
        true, 0x615B60u, 0x615B60u, 0x615CDCu, 0x615CDCu));
    CHECK(!qgame::netActiveConfigIdentityValid(
        false, 0x615B60u, 0x615B60u, 0x615CDCu, 0x615CDCu));
    CHECK(!qgame::netActiveConfigIdentityValid(
        true, 0x615E5Cu, 0x615B60u, 0x615FD8u, 0x615FD8u));
    CHECK(!qgame::netActiveConfigIdentityValid(
        true, 0x615B60u, 0x615B60u, 0x615FD8u, 0x615CDCu));

    CHECK(qgame::qohConfigSaveSucceeded(true, 1u));
    CHECK(!qgame::qohConfigSaveSucceeded(false, 1u));
    CHECK(!qgame::qohConfigSaveSucceeded(true, 0u));
    CHECK(!qgame::qohConfigSaveSucceeded(true, 2u));
}

TEST(TestNetworkImpairmentStatsSnapshotIsPumpOwnedAndOneShot) {
    const std::string transport =
        readNetplayFile("src/game/DPlayTransport.cpp");
    const std::string header =
        readNetplayFile("src/game/DPlayTransport.h");
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    CHECK(!transport.empty());
    CHECK(!header.empty());
    CHECK(!hooks.empty());

    CHECK(header.find("void requestTestNetworkStatsSnapshot();")
          != std::string::npos);
    const std::size_t saveHook =
        hooks.find("unsigned int __fastcall hkConfigSave");
    const std::size_t snapshotRequest = hooks.find(
        "qgame::requestTestNetworkStatsSnapshot();", saveHook);
    const std::size_t originalSave =
        hooks.find("g_origConfigSave(path, edx)", saveHook);
    CHECK(saveHook != std::string::npos);
    CHECK(snapshotRequest != std::string::npos);
    CHECK(originalSave != std::string::npos);
    CHECK(snapshotRequest < originalSave);

    CHECK(transport.find(
              "static void serviceTestNetworkStatsSnapshotRequest()")
          != std::string::npos);
    CHECK(transport.find(
              "requestTestNetworkStatsSnapshotNoWait();")
          != std::string::npos);
    CHECK(transport.find(
              "serviceTestNetworkStatsSnapshotRequest();")
          != std::string::npos);
    CHECK(transport.find("kTestNetworkStatsEmitting")
          != std::string::npos);
    CHECK(transport.find("kTestNetworkStatsEmitted")
          != std::string::npos);

    constexpr const char* marker = "[test-net] STATS profile=%s";
    std::size_t markerCount = 0;
    for (std::size_t at = transport.find(marker);
         at != std::string::npos;
         at = transport.find(marker, at + 1)) {
        ++markerCount;
    }
    CHECK_EQ(markerCount, static_cast<std::size_t>(1));
}

TEST(NetplayStatusTitleCannotBlockTheUdpPumpAndCoversLocalFailures) {
    const std::string transport =
        readNetplayFile("src/game/DPlayTransport.cpp");
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    CHECK(!transport.empty());
    CHECK(!hooks.empty());

    CHECK(transport.find("qplat::widen(title)") != std::string::npos);
    CHECK(transport.find("DefWindowProcW(") != std::string::npos);
    CHECK(transport.find("SendMessageTimeoutA(") == std::string::npos);
    CHECK(transport.find("SendMessageTimeoutW(") == std::string::npos);
    CHECK(transport.find("SetWindowTextA(") == std::string::npos);
    CHECK(transport.find("SetWindowTextW(") == std::string::npos);
    CHECK(transport.find("playerNameForUtf8Display(")
          != std::string::npos);
    CHECK(transport.find("playerNameForLocalDisplay(")
          == std::string::npos);
    CHECK(transport.find("void netShowTerminalStatus(")
          != std::string::npos);
    CHECK(hooks.find(
              "qgame::netShowTerminalStatus(code, detail, g_netHaltReason, graceful)")
          != std::string::npos);
}

TEST(SpectatorCannotGateAuthoritativePlayerReturn) {
    const std::string source = readNetplayFile("src/GameHooks.cpp");
    const std::size_t begin = source.find("int doNetplay(");
    const std::size_t end = source.find("bool spectatorSavePreFrame", begin);
    CHECK(!source.empty());
    CHECK(begin != std::string::npos);
    CHECK(end != std::string::npos);
    if (begin == std::string::npos || end == std::string::npos) return;

    const std::string playerDriver = source.substr(begin, end - begin);
    CHECK(playerDriver.find("spectatorSourceViewerCount(")
          == std::string::npos);
    CHECK(playerDriver.find("g_netSpectatorDrain")
          == std::string::npos);
    CHECK(playerDriver.find("spectatorSourceStopsOnDispatchReturn(")
          != std::string::npos);
    CHECK(playerDriver.find("return out.result;") != std::string::npos);
}

TEST(EverySpectatorWaitIsBoundedAndTheLadderIsConsistent) {
    CHECK(qgame::kSpectatorGameStartTimeoutMs == 300000u);
    CHECK(qgame::spectatorStartWaitVerdict(true, 0u, 1000u)
          == qgame::SpectatorStartWait::KeepWaiting);
    CHECK(qgame::spectatorStartWaitVerdict(true, 44999u, 1000u)
          == qgame::SpectatorStartWait::KeepWaiting);
    CHECK(qgame::spectatorStartWaitVerdict(true, 45000u, 1000u)
          == qgame::SpectatorStartWait::SourceGone);
    CHECK(qgame::spectatorStartWaitVerdict(false, 0u, 44999u)
          == qgame::SpectatorStartWait::KeepWaiting);
    CHECK(qgame::spectatorStartWaitVerdict(false, 0u, 45000u)
          == qgame::SpectatorStartWait::NeverAnswered);
    CHECK(qgame::spectatorStartWaitVerdict(true, 0u, 300000u)
          == qgame::SpectatorStartWait::Expired);
    CHECK(qgame::kSpectatorSourceGoneMs
          >= 10u * qgame::kSpectatorSourceIdleDwellMs / 2u);

    CHECK(qgame::kSpectatorStartRendezvousTimeoutMs == 30000u);
    CHECK(qgame::kSpectatorStartRendezvousTimeoutMs
          < qgame::kSpectatorGameStartTimeoutMs);

    CHECK(qgame::spectatorPlaybackPrimed(1u, true, false));
    CHECK(qgame::spectatorPlaybackPrimed(1u, false, true));
    CHECK(qgame::spectatorPlaybackPrimed(1u, false, false, false, 0u, true));
    CHECK(qgame::kSpectatorSourceIdleDwellMs >= 2000u);
    CHECK(qgame::kSpectatorSourceIdleDwellMs < qgame::kSpectatorSourceGoneMs);

    CHECK(qnetplay::spectator::SourceConfig{}.idleTimeoutMs == 15000u);
    CHECK(qnetplay::spectator::SourceConfig{}.handshakeDeadlineMs == 10000u);
    CHECK(qgame::kSpectatorSourceGoneMs
          > qnetplay::spectator::SourceConfig{}.idleTimeoutMs);

    CHECK(qgame::kSpectatorLayoutHoldDeadlineMs == 10000u);

    CHECK(qnetplay::spectator::kCheckpointPushPasses
              * qnetplay::spectator::kCheckpointPushRepassMs
          < qgame::kSpectatorSetSeekWaitMs);
    CHECK(qgame::kSpectatorSetSeekWaitMs == 15000u);

    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    for (const char* bound : {
             "qgame::spectatorStartWaitVerdict(",
             "kSpectatorStartRendezvousTimeoutMs",
             "kSpectatorLayoutHoldDeadlineMs",
             "qgame::spectatorSourceHoldsNothingMore(",
             "qgame::kSpectatorSetSeekWaitMs"}) {
        CHECK(hooks.find(bound) != std::string::npos);
    }
    CHECK(hooks.find("the host stopped answering while we waited")
          != std::string::npos);
    CHECK(hooks.find("the host never answered this spectator")
          != std::string::npos);
    CHECK(hooks.find("spectator GameStart timed out") != std::string::npos);
}

TEST(SpectatorCatchupUsesBoundedHiddenPresentationTicks) {
    CHECK(qgame::kSpectatorStartupBufferRecords == 120u);
    CHECK(qgame::kSpectatorRebufferRecords == 90u);
    CHECK(qgame::kSpectatorLiveTargetRecords == 120u);
    CHECK(qgame::kSpectatorCatchupEnterRecords == 420u);
    CHECK(qgame::spectatorCushionBonus(0u) == 0u);
    CHECK(qgame::spectatorCushionBonus(1u) == 120u);
    CHECK(qgame::spectatorCushionBonus(3u) == 360u);
    CHECK(qgame::spectatorCushionBonus(9u) == 360u);
    CHECK(qgame::spectatorLiveTarget(120u) == 240u);
    CHECK(qgame::spectatorCatchupEnter(120u) == 540u);
    CHECK(!qgame::spectatorPlaybackPrimed(119u, false, false, false, 0u));
    CHECK(qgame::spectatorPlaybackPrimed(120u, false, false, false, 0u));
    CHECK(!qgame::spectatorPlaybackPrimed(239u, false, false, false, 120u));
    CHECK(qgame::spectatorPlaybackPrimed(240u, false, false, false, 120u));
    CHECK(qgame::kSpectatorGameStartTimeoutMs >= 300000u);
    CHECK(qgame::kSpectatorStartRendezvousTimeoutMs == 30000u);

    CHECK(!qgame::spectatorPlaybackPrimed(0u, true, true));
    CHECK(qgame::spectatorPlaybackPrimed(1u, true, false));
    CHECK(qgame::spectatorPlaybackPrimed(29u, false, true));
    CHECK(!qgame::spectatorPlaybackPrimed(119u, false, false));
    CHECK(qgame::spectatorPlaybackPrimed(120u, false, false));
    CHECK(!qgame::spectatorPlaybackPrimed(89u, false, false, true));
    CHECK(qgame::spectatorPlaybackPrimed(90u, false, false, true));
    CHECK(!qgame::spectatorPlaybackPrimed(90u, false, false, false));

    CHECK(!qgame::spectatorSourceHoldsNothingMore(true, 400u, 30u, 60000u));
    CHECK(!qgame::spectatorSourceHoldsNothingMore(true, 30u, 30u, 1999u));
    CHECK(qgame::spectatorSourceHoldsNothingMore(true, 30u, 30u, 2000u));
    CHECK(!qgame::spectatorSourceHoldsNothingMore(false, 0u, 30u, 60000u));
    CHECK(!qgame::spectatorSourceHoldsNothingMore(true, 30u, 31u, 5000u));
    CHECK(qgame::kSpectatorSourceIdleDwellMs == 2000u);
    CHECK(qgame::spectatorPlaybackPrimed(1u, false, false, false, 0u, true));
    CHECK(!qgame::spectatorPlaybackPrimed(0u, false, false, false, 0u, true));
    CHECK(!qgame::spectatorPlaybackPrimed(119u, false, false, false, 0u,
                                          false));

    CHECK(qgame::spectatorEmptyBufferIsUnderrun(true, 400u, 30u));
    CHECK(!qgame::spectatorEmptyBufferIsUnderrun(true, 30u, 30u));
    CHECK(qgame::spectatorEmptyBufferIsUnderrun(true, 30u, 31u));
    CHECK(qgame::spectatorEmptyBufferIsUnderrun(false, 0u, 30u));

    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    CHECK(hooks.find("qgame::spectatorSourceHoldsNothingMore(")
          != std::string::npos);
    CHECK(hooks.find("qgame::spectatorEmptyBufferIsUnderrun(")
          != std::string::npos);
    CHECK(hooks.find("g_spectatorCushionBonus,\n"
                     "                sourceExhausted)")
          != std::string::npos);
    CHECK(hooks.find("qgame::SpectatorWait::Joining") != std::string::npos);
    CHECK(hooks.find("qgame::SpectatorWait::SourceIdle") != std::string::npos);
    CHECK(hooks.find("qgame::SpectatorWait::AwaitingEvent")
          != std::string::npos);
    CHECK(hooks.find("[spectator] joining:") != std::string::npos);

    CHECK(qgame::kSpectatorElasticNearLowRecords == 60u);
    CHECK(qgame::kSpectatorElasticLowRecords == 30u);
    CHECK(qgame::kSpectatorElasticCriticalRecords == 15u);
    CHECK(qgame::spectatorElasticSpeedPermille(120u) == 1000u);
    CHECK(qgame::spectatorElasticSpeedPermille(60u) == 1000u);
    CHECK(qgame::spectatorElasticSpeedPermille(59u) == 970u);
    CHECK(qgame::spectatorElasticSpeedPermille(30u) == 970u);
    CHECK(qgame::spectatorElasticSpeedPermille(29u) == 900u);
    CHECK(qgame::spectatorElasticSpeedPermille(15u) == 900u);
    CHECK(qgame::spectatorElasticSpeedPermille(14u) == 750u);
    CHECK(qgame::spectatorElasticSpeedPermille(1u) == 750u);
    CHECK(qgame::spectatorElasticSpeedPermille(0u) == 0u);
    const auto countElasticAdvances = [](std::size_t buffered,
                                         unsigned periods) {
        uint32_t credit = 0u;
        unsigned advances = 0u;
        for (unsigned period = 0u; period < periods; ++period) {
            const qgame::SpectatorElasticPlan plan =
                qgame::spectatorElasticPlan(buffered, credit);
            credit = plan.nextCredit;
            if (plan.advance) ++advances;
        }
        return advances;
    };
    CHECK(countElasticAdvances(59u, 100u) == 97u);
    CHECK(countElasticAdvances(29u, 100u) == 90u);
    CHECK(countElasticAdvances(14u, 100u) == 75u);
    CHECK(countElasticAdvances(0u, 100u) == 0u);
    const qgame::SpectatorElasticPlan healthyElastic =
        qgame::spectatorElasticPlan(60u, 777u);
    CHECK(healthyElastic.advance);
    CHECK(healthyElastic.nextCredit == 0u);

    CHECK(qgame::kSpectatorLiveTargetRecords
          < qgame::kSpectatorCatchupEnterRecords);
    CHECK(qgame::spectatorCatchupPlan(0u, false).tickBudget == 0u);
    CHECK(!qgame::spectatorCatchupPlan(0u, true).catchingUp);
    CHECK(!qgame::spectatorCatchupPlan(
              qgame::kSpectatorCatchupEnterRecords, false).catchingUp);
    CHECK(qgame::spectatorCatchupPlan(
              qgame::kSpectatorCatchupEnterRecords + 1u, false).catchingUp);
    CHECK(qgame::spectatorCatchupPlan(
              qgame::kSpectatorLiveTargetRecords + 1u, true).catchingUp);
    CHECK(!qgame::spectatorCatchupPlan(
              qgame::kSpectatorLiveTargetRecords, true).catchingUp);
    CHECK(qgame::spectatorCatchupPlan(540u, false, 120u).catchingUp == false);
    CHECK(qgame::spectatorCatchupPlan(541u, false, 120u).catchingUp);
    CHECK(qgame::spectatorCatchupPlan(241u, true, 120u).catchingUp);
    CHECK(!qgame::spectatorCatchupPlan(240u, true, 120u).catchingUp);
    CHECK(qgame::spectatorCatchupPlan(121u, true).speedPermille == 2000u);
    CHECK(qgame::spectatorCatchupPlan(421u, false).speedPermille == 2000u);
    CHECK(qgame::spectatorCatchupPlan(1320u, true).speedPermille == 2000u);
    CHECK(qgame::spectatorCatchupPlan(1321u, true).speedPermille == 4000u);
    CHECK(qgame::spectatorCatchupPlan(2521u, true).speedPermille == 8000u);
    CHECK(qgame::spectatorCatchupPlan(4921u, true).speedPermille == 16000u);
    CHECK(qgame::spectatorCatchupPlan(9721u, true).speedPermille == 24000u);
    CHECK(qgame::spectatorCatchupPlan(121u, true).tickBudget == 2u);
    CHECK(qgame::spectatorCatchupPlan(9721u, true).tickBudget == 24u);
    CHECK(qgame::spectatorCatchupPlan(1u, false).tickBudget == 1u);
    CHECK(qgame::spectatorCatchupPlan(1321u, true, 120u).speedPermille
          == 2000u);
    CHECK(!qgame::spectatorCatchupPlan(120u, false, 0u, false).catchingUp);
    CHECK(qgame::spectatorCatchupPlan(121u, false, 0u, false).catchingUp);
    CHECK(qgame::spectatorCatchupPlan(121u, false, 0u, false).speedPermille
          == qgame::kSpectatorCatchupMaxPermille);
    CHECK(qgame::spectatorCatchupPlan(121u, false, 0u, false).tickBudget
          == 24u);
    CHECK(!qgame::spectatorCatchupPlan(400u, false, 0u, true).catchingUp);
    CHECK(qgame::spectatorCatchupPlan(400u, false, 0u, true, true).catchingUp);
    CHECK(qgame::spectatorCatchupPlan(400u, false, 0u, true, true)
              .speedPermille == 24000u);
    CHECK(!qgame::spectatorCatchupPlan(120u, true, 0u, true, true).catchingUp);
    CHECK(qgame::spectatorCatchupSliceMicros(true)
          == qgame::kSpectatorCatchupSliceMicros);
    CHECK(qgame::spectatorCatchupSliceMicros(false)
          == qgame::kSpectatorCatchupMenuSliceMicros);
    CHECK(qgame::kSpectatorCatchupMenuSliceMicros
          > qgame::kSpectatorCatchupSliceMicros);
    {
        qgame::SpectatorCatchupPlan gentle{};
        gentle.catchingUp = true;
        gentle.speedPermille = 1250u;
        gentle.tickBudget = 1u;
        uint32_t credit = 0u;
        std::size_t ticks = 0u;
        std::size_t maxTicks = 0u;
        for (unsigned slot = 0u; slot < 100u; ++slot) {
            const qgame::SpectatorCatchupSlot s =
                qgame::spectatorCatchupSlot(gentle, credit);
            credit = s.nextCredit;
            ticks += s.ticks;
            if (s.ticks > maxTicks) maxTicks = s.ticks;
        }
        CHECK(ticks == 125u);
        CHECK(maxTicks == 2u);
        const qgame::SpectatorCatchupSlot idle = qgame::spectatorCatchupSlot(
            qgame::spectatorCatchupPlan(100u, false), 999u);
        CHECK(idle.ticks == 1u && idle.nextCredit == 0u);
    }
    CHECK(!qgame::spectatorJumpToLiveNeeded(1u));
    CHECK(!qgame::spectatorJumpToLiveNeeded(90u));
    CHECK(!qgame::spectatorJumpToLiveNeeded(120u));
    CHECK(qgame::spectatorJumpToLiveNeeded(121u));
    CHECK(!qgame::spectatorJumpToLiveNeeded(240u, 120u));
    CHECK(qgame::spectatorJumpToLiveNeeded(241u, 120u));
    CHECK(qgame::spectatorEstimatedSlotsToLive(
              qgame::kSpectatorLiveTargetRecords, true) == 0u);
    CHECK(qgame::spectatorEstimatedSlotsToLive(400u, false) == 0u);
    CHECK(qgame::spectatorEstimatedSlotsToLive(721u, false) > 0u);
    CHECK(qgame::spectatorEstimatedSlotsToLive(721u, false) == 601u);
    CHECK(qgame::kSpectatorCatchupSliceMicros > 0u
          && qgame::kSpectatorCatchupSliceMicros <= 40000u);

    CHECK(!qgame::spectatorCatchupMustYield(
        false, false, false, 3u, 3u));
    CHECK(qgame::spectatorCatchupMustYield(
        true, false, false, 3u, 3u));
    CHECK(qgame::spectatorCatchupMustYield(
        false, true, false, 3u, 3u));
    CHECK(qgame::spectatorCatchupMustYield(
        false, false, true, 3u, 3u));
    CHECK(qgame::spectatorCatchupMustYield(
        false, false, false, 1u, 3u));

    const std::string source = readNetplayFile("src/GameHooks.cpp");
    const std::size_t begin = source.find("int doSpectator(");
    const std::size_t end = source.find("int doReplay(", begin);
    CHECK(!source.empty());
    CHECK(begin != std::string::npos);
    CHECK(end != std::string::npos);
    if (begin == std::string::npos || end == std::string::npos) return;

    const std::string spectatorDriver = source.substr(begin, end - begin);
    CHECK(spectatorDriver.find(
              "qgame::spectatorCatchupPlan(status.bufferedRecords, "
              "wasCatchingUp,")
          != std::string::npos);
    CHECK(spectatorDriver.find("g_spectatorCushionBonus")
          != std::string::npos);
    CHECK(spectatorDriver.find(
              "qgame::spectatorCatchupSlot(catchupPlan, "
              "g_spectatorCatchupCredit)")
          != std::string::npos);
    CHECK(spectatorDriver.find("size_t tickBudget = catchupSlot.ticks;")
          != std::string::npos);
    CHECK(spectatorDriver.find("spectatorLogFreezeDiagnostics(status, nowMs);")
          != std::string::npos);
    CHECK(spectatorDriver.find("spectatorTelemetryTick(status, nowMs);")
          != std::string::npos);
    CHECK(spectatorDriver.find("g_spectatorCatchingUp = catchupPlan.catchingUp")
          != std::string::npos);
    CHECK(spectatorDriver.find("qgame::spectatorPlaybackPrimed(")
          != std::string::npos);
    CHECK(spectatorDriver.find("g_spectatorHasPrimed")
          != std::string::npos);
    CHECK(spectatorDriver.find("qgame::spectatorElasticPlan(")
          != std::string::npos);
    CHECK(spectatorDriver.find(
              "qgame::spectatorPublishPlaybackPresentation(")
          != std::string::npos);
    const std::size_t localCommands = spectatorDriver.find(
        "spectatorApplyPendingLocalCommands();");
    const std::size_t emptyBufferGate = spectatorDriver.find(
        "if (status.bufferedRecords == 0)");
    CHECK(localCommands != std::string::npos);
    CHECK(emptyBufferGate != std::string::npos);
    CHECK(localCommands < emptyBufferGate);
    CHECK(spectatorDriver.find(
              "!qgame::spectatorJumpToLiveNeeded(status.bufferedRecords,")
          != std::string::npos);
    CHECK(spectatorDriver.find(
              "g_spectatorFastForwardLatched = false;")
          != std::string::npos);
    CHECK(spectatorDriver.find("qgame::kSpectatorCatchupSliceMicros")
          != std::string::npos);
    CHECK(spectatorDriver.find("const bool inGameplay = rollbackEligible();")
          != std::string::npos);
    CHECK(spectatorDriver.find("qgame::kSpectatorCatchupMenuSliceMicros")
          != std::string::npos);
    CHECK(spectatorDriver.find("g_spectatorCushionBonus, inGameplay,")
          != std::string::npos);
    CHECK(spectatorDriver.find("qgame::spectatorEstimatedSlotsToLive(")
          != std::string::npos);
    CHECK(spectatorDriver.find(
              "netRunOwnedTick(orig, frame, inputs, hiddenCatchup")
          != std::string::npos);
    CHECK(spectatorDriver.find(
              "qgame::spectatorCatchupExecutedTickVisibility(")
          != std::string::npos);
    CHECK(spectatorDriver.find("netPaceReady()")
          == std::string::npos);
    CHECK(spectatorDriver.find(
              "g_netPresentationSlotGrants")
          != std::string::npos);
    CHECK(spectatorDriver.find(
              "netConsumePresentationSlot(")
          != std::string::npos);
    CHECK(spectatorDriver.find("g_spectatorBufferPrimed = false")
          != std::string::npos);
    CHECK(spectatorDriver.find("spectatorCatchupMustYield(")
          != std::string::npos);
    CHECK(spectatorDriver.find("rd32(0x6234EC), rd32(0x61801C)")
          != std::string::npos);
    CHECK(spectatorDriver.find(
              "!status.linkAlive && !status.hasTerminal")
          != std::string::npos);
    CHECK(spectatorDriver.find(
              "status.linkAlive || status.hasTerminal")
          != std::string::npos);
    CHECK(spectatorDriver.find(
              "spectator DirectBoot start rendezvous timed out")
          != std::string::npos);
}

TEST(SpectatorWindowTitleUsesAuthoritativePublishedPresentationState) {
    const std::string transport =
        readNetplayFile("src/game/DPlayTransport.cpp");
    const std::string transportHeader =
        readNetplayFile("src/game/DPlayTransport.h");
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    CHECK(!transport.empty());
    CHECK(!transportHeader.empty());
    CHECK(!hooks.empty());

    const std::size_t begin = transport.find("void updateWindowTitle()");
    const std::size_t end = transport.find(
        "typedef void (__thiscall* DispatchFn)(void* msg, int idFrom, int idTo);", begin);
    CHECK(begin != std::string::npos);
    CHECK(end != std::string::npos);
    if (begin == std::string::npos || end == std::string::npos) return;
    const std::string title = transport.substr(begin, end - begin);

    const std::size_t presentationRead = title.find(
        "&g_spectatorPresentationForTitle, 0, 0");
    CHECK(presentationRead != std::string::npos);
    CHECK(presentationRead == std::string::npos
          || title.find("&g_spectatorPresentationForTitle, 0, 0",
                        presentationRead + 1u) == std::string::npos);
    CHECK(transport.find("g_spectatorPlaybackPrimedForTitle")
          == std::string::npos);
    CHECK(transport.find("g_spectatorCatchingUpForTitle")
          == std::string::npos);
    CHECK(title.find(
              "(presentation & kSpectatorTitlePrimed) != 0")
          != std::string::npos);
    CHECK(title.find(
              "(presentation & kSpectatorTitleCatchingUp) != 0")
          != std::string::npos);
    CHECK(title.find(
              "(presentation & kSpectatorTitlePaused) != 0")
          != std::string::npos);
    CHECK(title.find("backlog > 16u") == std::string::npos);
    CHECK(title.find("kSpectatorCatchupEnterRecords")
          == std::string::npos);

    const std::size_t sealed = title.find("if (hasTerminal)");
    const std::size_t paused = title.find("else if (paused)", sealed);
    const std::size_t buffering = title.find(
        "else if (!primed || backlog == 0u)");
    const std::size_t catching = title.find("else if (catchingUp)");
    const std::size_t live = title.find(
        "std::snprintf(state, sizeof(state), \"LIVE\")", catching);
    CHECK(sealed != std::string::npos);
    CHECK(paused != std::string::npos);
    CHECK(buffering != std::string::npos);
    CHECK(catching != std::string::npos);
    CHECK(live != std::string::npos);
    CHECK(sealed < paused);
    CHECK(paused < buffering);
    CHECK(buffering < catching);
    CHECK(catching < live);
    CHECK(title.find(
              "std::snprintf(state, sizeof(state), \"PAUSED\")")
          != std::string::npos);
    CHECK(title.find("backlog - qgame::kSpectatorLiveTargetRecords")
          != std::string::npos);
    CHECK(title.find("CATCHING UP (%zu excess, ~%.1fs)")
          != std::string::npos);

    CHECK(transportHeader.find(
              "void spectatorPublishPlaybackPresentation(bool bufferPrimed,\n"
              "                                           bool catchingUp,\n"
              "                                           bool paused);")
          != std::string::npos);
    const std::size_t publisher = transport.find(
        "void spectatorPublishPlaybackPresentation(bool bufferPrimed,");
    const std::size_t publisherEnd = transport.find(
        "\nbool spectatorViewerGetStatus", publisher);
    CHECK(publisher != std::string::npos);
    CHECK(publisherEnd != std::string::npos);
    if (publisher != std::string::npos
        && publisherEnd != std::string::npos) {
        const std::string publishBody = transport.substr(
            publisher, publisherEnd - publisher);
        CHECK(publishBody.find(
                  "bool catchingUp,\n"
                  "                                           bool paused)")
              != std::string::npos);
        CHECK(publishBody.find(
                  "(bufferPrimed ? kSpectatorTitlePrimed : 0)")
              != std::string::npos);
        CHECK(publishBody.find(
                  "(catchingUp ? kSpectatorTitleCatchingUp : 0)")
              != std::string::npos);
        CHECK(publishBody.find(
                  "(paused ? kSpectatorTitlePaused : 0)")
              != std::string::npos);
        const std::string exchange =
            "InterlockedExchange(&g_spectatorPresentationForTitle, "
            "presentation)";
        const std::size_t firstExchange = publishBody.find(exchange);
        CHECK(firstExchange != std::string::npos);
        CHECK(firstExchange == std::string::npos
              || publishBody.find(exchange, firstExchange + 1u)
                     == std::string::npos);
    }
    CHECK(hooks.find(
              "qgame::spectatorPublishPlaybackPresentation("
              "false, false, false)")
          != std::string::npos);
    CHECK(hooks.find(
              "qgame::spectatorPublishPlaybackPresentation(\n"
              "        g_spectatorBufferPrimed, g_spectatorCatchingUp,\n"
              "        g_spectatorLocalPaused)")
          != std::string::npos);
    CHECK(hooks.find(
              "qgame::spectatorPublishPlaybackPresentation("
              "true, false, true)")
          != std::string::npos);
}

TEST(HistoricalReplayPresentationIsSuppressedAtCompletedFrameBoundary) {
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    const std::string wrapper =
        readNetplayFile("ddraw_wrapper/ddraw.c");
    CHECK(!hooks.empty());
    CHECK(!wrapper.empty());
    CHECK(hooks.find(
              "PresentationDisposition::SuppressHistorical")
          != std::string::npos);
    const size_t replayBegin = hooks.find(
        "NetReplayCorrectionResult netReplayCorrection(");
    const size_t replayEnd = hooks.find(
        "\nint doNetplay(", replayBegin);
    CHECK(replayBegin != std::string::npos);
    CHECK(replayEnd != std::string::npos);
    if (replayBegin != std::string::npos
        && replayEnd != std::string::npos) {
        const std::string replay =
            hooks.substr(replayBegin, replayEnd - replayBegin);
        CHECK(replay.find(
                  "qgame::rollbackCorrectionTickVisibility()")
              != std::string::npos);
        CHECK(replay.find(
                  "PresentationDisposition::CorrectedFinal")
              == std::string::npos);
        CHECK(replay.find(
                  "frame + 1u == action.replayUntilExclusive")
              == std::string::npos);
    }
    CHECK(hooks.find("captureQohNativeFpsAccounting")
          != std::string::npos);
    CHECK(hooks.find("restoreQohNativeFpsAccounting")
          != std::string::npos);
    CHECK(wrapper.find(
              "\"qoh99np_presentation_disposition\"")
          != std::string::npos);
    CHECK(wrapper.find(
              "\"qoh99np_presentation_report\"")
          != std::string::npos);
    CHECK(wrapper.find(
              "disposition == QOH99_PRESENT_SUPPRESS_HISTORICAL")
          != std::string::npos);
    CHECK(wrapper.find("++g_qoh99SuppressedPresents")
          != std::string::npos);
    CHECK(wrapper.find("present_frame(dst->owner, TRUE)")
          != std::string::npos);
    CHECK(hooks.find(
              "presentationQueriesAfter == presentationQueriesBefore")
          != std::string::npos);
    CHECK(hooks.find(
              "compatible ddraw presentation gate not active")
          != std::string::npos);
    CHECK(hooks.find(
              "presentationDispositionQueryCount() == 0u")
          != std::string::npos);
    CHECK(hooks.find("verifiedHiddenPresentationTicks")
          != std::string::npos);
    CHECK(hooks.find("normalVisiblePresents")
          != std::string::npos);
    const size_t configSaveBegin = hooks.find(
        "unsigned int __fastcall hkConfigSave(");
    const size_t configSaveEnd = hooks.find(
        "\nbool netReadActiveMatchConfig(", configSaveBegin);
    CHECK(configSaveBegin != std::string::npos);
    CHECK(configSaveEnd != std::string::npos);
    if (configSaveBegin != std::string::npos
        && configSaveEnd != std::string::npos) {
        const std::string configSave = hooks.substr(
            configSaveBegin, configSaveEnd - configSaveBegin);
        CHECK(configSave.find(
                  "requestTestNetworkStatsSnapshot();")
              != std::string::npos);
        CHECK(configSave.find(
                  "netLogPresentationSummary();")
              != std::string::npos);
        const std::size_t sourceSeal = configSave.find(
            "netSealSpectatorArchive(");
        const std::size_t publisherStop = configSave.find(
            "stopSidecarPublisher();");
        CHECK(sourceSeal != std::string::npos);
        CHECK(publisherStop != std::string::npos);
        CHECK(sourceSeal < publisherStop);
        CHECK(configSave.find(
                  "TerminalReason::SourceStopped")
              != std::string::npos);
    }
    CHECK(hooks.find("[presentation-summary] ")
          != std::string::npos);
    CHECK(hooks.find(
              "wrapper normalVisiblePresents=%lu suppressedPresents=%lu")
          != std::string::npos);
}

TEST(NetplayOwnsExactlyOnePresentationPacer) {
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    const std::string hooksHeader = readNetplayFile("src/GameHooks.h");
    const std::string dllMain = readNetplayFile("src/DllMain.cpp");
    const std::string wrapper =
        readNetplayFile("ddraw_wrapper/ddraw.c");
    const std::string exports =
        readNetplayFile("ddraw_wrapper/ddraw.def");
    CHECK(!hooks.empty());
    CHECK(!hooksHeader.empty());
    CHECK(!dllMain.empty());
    CHECK(!wrapper.empty());
    CHECK(!exports.empty());

    CHECK(exports.find("EFZDDRAW_SetFrameLimit @12")
          != std::string::npos);
    CHECK(exports.find("EFZDDRAW_GetNetplayPacingAbi @13")
          != std::string::npos);
    CHECK(wrapper.find(
              "BOOL __stdcall EFZDDRAW_SetFrameLimit(UINT fps)")
          != std::string::npos);
    CHECK(wrapper.find(
              "UINT __stdcall EFZDDRAW_GetNetplayPacingAbi(void)")
          != std::string::npos);
    CHECK(wrapper.find(
              "typedef int (__cdecl "
              "*Qoh99PresentationTryAcquireSlotV2Fn)(void);")
          != std::string::npos);
    CHECK(wrapper.find("static BOOL qoh99_frame_commit(void)")
          != std::string::npos);
    CHECK(wrapper.find("static BOOL present_frame("
                       "MyDDraw *dd, BOOL canonicalFrame)")
          != std::string::npos);
    CHECK(wrapper.find(
              "if (qoh99_netplay_owns_pacing() && !canonicalFrame)")
          != std::string::npos);
    CHECK(wrapper.find(
              "InterlockedExchange(&g_runtimeFpsLimit, (LONG)fps)")
          != std::string::npos);
    CHECK(wrapper.find(
              "generation != g_appliedFpsLimitGeneration")
          != std::string::npos);
    CHECK(wrapper.find("if (qoh99_netplay_owns_pacing())")
          != std::string::npos);
    CHECK(wrapper.find("\"qoh99np_owns_pacing\"")
          != std::string::npos);
    CHECK(wrapper.find("\"qoh99np_owns_network_pacing\"")
          != std::string::npos);
    CHECK(wrapper.find("&& !qoh99_netplay_owns_network_pacing()")
          != std::string::npos);
    CHECK(wrapper.find("&& !qoh99_netplay_owns_pacing()")
          == std::string::npos);
    CHECK(wrapper.find("return qoh99_netplay_owns_pacing();")
          != std::string::npos);
    CHECK(wrapper.find(
              "\"qoh99np_presentation_try_acquire_slot_v2\"")
          != std::string::npos);
    CHECK(wrapper.find(
              "\"qoh99np_presentation_failed_v3\"")
          != std::string::npos);
    CHECK(wrapper.find("\"qoh99np_presentation_wait_slot\"")
          == std::string::npos);
    CHECK(wrapper.find("return 3u;")
          != std::string::npos);
    CHECK(wrapper.find("present_frame(dd, FALSE);")
          != std::string::npos);
    CHECK(wrapper.find("present_current(g_dd);")
          != std::string::npos);
    CHECK(wrapper.find("present_snapshot(dd);")
          != std::string::npos);
    CHECK(wrapper.find("present_frame(dst->owner, FALSE);")
          != std::string::npos);
    CHECK(wrapper.find("present_frame(s->owner, FALSE);")
          != std::string::npos);
    CHECK(wrapper.find("present_frame(dd);")
          == std::string::npos);
    CHECK(wrapper.find("present_frame(g_dd);")
          == std::string::npos);
    CHECK(wrapper.find("present_frame(dst->owner);")
          == std::string::npos);
    CHECK(wrapper.find("present_frame(s->owner);")
          == std::string::npos);

    CHECK(hooks.find("\"EFZDDRAW_SetFrameLimit\"")
          != std::string::npos);
    CHECK(hooks.find("\"EFZDDRAW_GetNetplayPacingAbi\"")
          != std::string::npos);
    CHECK(hooks.find(
              "constexpr UINT kWrapperNetplayPacingAbi = 3u;")
          != std::string::npos);
    CHECK(hooks.find("setFrameLimit(0u)")
          != std::string::npos);
    CHECK(hooks.find("setFrameLimit(60u)")
          != std::string::npos);
    const std::size_t pacingOwnerBegin = hooks.find(
        "bool netAcquireWrapperPacingOwnership() {");
    const std::size_t pacingOwnerEnd = hooks.find(
        "\nvoid netReleaseWrapperPacingOwnership()", pacingOwnerBegin);
    CHECK(pacingOwnerBegin != std::string::npos);
    CHECK(pacingOwnerEnd != std::string::npos);
    if (pacingOwnerBegin != std::string::npos
        && pacingOwnerEnd != std::string::npos) {
        const std::string pacingOwner = hooks.substr(
            pacingOwnerBegin, pacingOwnerEnd - pacingOwnerBegin);
        CHECK(pacingOwner.find("netPrepareOverlayHud()")
              != std::string::npos);
        CHECK(pacingOwner.find("wrapper renderer not ready")
              == std::string::npos);
    }
    const std::size_t firstRetry = hooks.find(
        "netRetryDeferredOverlayHud();");
    const std::size_t secondRetry = hooks.find(
        "netRetryDeferredOverlayHud();", firstRetry + 1u);
    const std::size_t thirdRetry = hooks.find(
        "netRetryDeferredOverlayHud();", secondRetry + 1u);
    const std::size_t fourthRetry = hooks.find(
        "netRetryDeferredOverlayHud();", thirdRetry + 1u);
    CHECK(firstRetry != std::string::npos);
    CHECK(secondRetry != std::string::npos);
    CHECK(thirdRetry != std::string::npos);
    CHECK(fourthRetry == std::string::npos);
    CHECK(hooks.find(
              "mandatory single-owner pacing handoff FAILED")
          != std::string::npos);
    CHECK(dllMain.find("qoh99np_owns_pacing()")
          != std::string::npos);
    CHECK(dllMain.find(
              "qoh99np_presentation_try_acquire_slot_v2()")
          != std::string::npos);
    CHECK(dllMain.find(
              "qoh99np_presentation_failed_v3(unsigned long reason)")
          != std::string::npos);
    CHECK(dllMain.find("qoh99np_presentation_wait_slot()")
          == std::string::npos);
    CHECK(dllMain.find(
              "return qoh99::tryAcquirePresentationSlot() ? 1 : 0;")
          != std::string::npos);
    CHECK(hooksHeader.find("bool tryAcquirePresentationSlot();")
          != std::string::npos);
    CHECK(hooksHeader.find(
              "void observePresentationFailure(unsigned long reason);")
          != std::string::npos);
    CHECK(hooks.find(
              "const int64_t nowTicks = netUnhookedQpcNow();")
          != std::string::npos);
    CHECK(hooks.find(
              "return g_origQPC(&sampled) ? sampled.QuadPart : 0;")
          != std::string::npos);
    CHECK(hooks.find("netPaceReady()") == std::string::npos);
    CHECK(hooks.find(
              "g_paceDeadline.semanticHold(netUnhookedQpcNow())")
          != std::string::npos);
    CHECK(hooks.find(
              "netConsumePresentationSlot(ownedTickSerial)")
          != std::string::npos);
    CHECK(hooks.find(
              "GetTickCount64() - waitStartedMs > 500u")
          != std::string::npos);
    CHECK(hooks.find("g_netPacingConsumersInFlight")
          != std::string::npos);
    CHECK(hooks.find(
              "GetCurrentThreadId() != g_clockThread")
          != std::string::npos);
    CHECK(hooks.find(
              "g_paceDeadline.observePresentationCompleted(nowTicks)")
          != std::string::npos);
    CHECK(hooks.find(
              "g_paceDeadline.anchorPresentationCompleted(nowTicks)")
          != std::string::npos);
    CHECK(hooks.find("[pacing-ledger]")
          != std::string::npos);
    CHECK(hooks.find("commitGrants=%ld")
          != std::string::npos);
    CHECK(hooks.find("noCommitFallbackGrants=%ld")
          != std::string::npos);
    CHECK(hooks.find("spectatorBatchFallbackGrants=%ld")
          != std::string::npos);
    CHECK(hooks.find("duplicateCommitCallbacks=%ld")
          != std::string::npos);
    CHECK(hooks.find("duplicatePublishCallbacks=%ld")
          != std::string::npos);
    CHECK(hooks.find("hiddenCommitBypasses=%ld")
          != std::string::npos);
    CHECK(hooks.find("unownedCommitCallbacks=%ld")
          != std::string::npos);
    CHECK(hooks.find("publishCallbacks=%ld")
          != std::string::npos);
    CHECK(hooks.find("publishFailures=%ld")
          != std::string::npos);
    CHECK(hooks.find("[pacing-publish-failures]")
          != std::string::npos);
    CHECK(hooks.find("clockFaults=%ld")
          != std::string::npos);
    CHECK(hooks.find("g_netPublishedTickSerial")
          != std::string::npos);
    CHECK(hooks.find("g_netPresentationCommitCallbacks")
          != std::string::npos);
    CHECK(hooks.find("g_netPresentationFailureCallbacks")
          != std::string::npos);
    CHECK(hooks.find("g_netDuplicatePublishCallbacks")
          != std::string::npos);
    CHECK(hooks.find("g_netUnownedCommitCallbacks")
          != std::string::npos);
    CHECK(hooks.find(
              "callback rejected outside an owned frame-dispatch tick")
          != std::string::npos);
    CHECK(hooks.find("g_netTestPacingSummaryRequest")
          != std::string::npos);
    CHECK(hooks.find("g_netTestPacingSummaryAcknowledged")
          != std::string::npos);
    CHECK(hooks.find("netLatchPacingClockFault();")
          != std::string::npos);
    CHECK(hooks.find(
              "InterlockedExchange(&g_netPacingClockFault, 1)")
          == std::string::npos);
    CHECK(hooks.find("netPaceWaitBrief();")
          != std::string::npos);
    CHECK(hooks.find("timer-period-1ms active=%u")
          != std::string::npos);

    const std::size_t acquireSlotBegin = hooks.find(
        "bool tryAcquirePresentationSlot()");
    const std::size_t acquireSlotEnd = hooks.find(
        "\nPresentationDisposition queryPresentationDisposition()",
        acquireSlotBegin);
    CHECK(acquireSlotBegin != std::string::npos);
    CHECK(acquireSlotEnd != std::string::npos);
    if (acquireSlotBegin != std::string::npos
        && acquireSlotEnd != std::string::npos) {
        const std::string acquireSlot = hooks.substr(
            acquireSlotBegin, acquireSlotEnd - acquireSlotBegin);
        const std::size_t preClockBypass = acquireSlot.find(
            "if (!presentationClockStarted())");
        const std::size_t callbackAccounting = acquireSlot.find(
            "InterlockedIncrement(&g_netPresentationWaitCallbacks)");
        const std::size_t ownedTickGate = acquireSlot.find(
            "&g_netGateOwnsTick, 0, 0) == 0");
        const std::size_t unownedAccounting = acquireSlot.find(
            "InterlockedIncrement(&g_netUnownedCommitCallbacks)");
        CHECK(preClockBypass != std::string::npos);
        CHECK(callbackAccounting != std::string::npos);
        CHECK(ownedTickGate != std::string::npos);
        CHECK(unownedAccounting != std::string::npos);
        CHECK(preClockBypass < callbackAccounting);
        CHECK(callbackAccounting < ownedTickGate);
        CHECK(ownedTickGate < unownedAccounting);
    }
    CHECK(hooks.find("bool presentationClockStarted()")
          != std::string::npos);
    CHECK(hooks.find(
              "qgame::deterministicPresentationClockStarted(")
          != std::string::npos);
    CHECK(hooks.find(
              "if ((!g_netActive && !g_replayRoleActive)\n"
              "        || !presentationClockStarted())")
          != std::string::npos);
    CHECK(hooks.find("const bool visibleReplayOwner = g_replayRoleActive")
          != std::string::npos);
    CHECK(hooks.find("!qgame::replay::replayHeadlessV3()")
          != std::string::npos);
    CHECK(hooks.find("g_replayBootstrapPhase\n"
                     "                == ReplayBootstrapPhase::Complete")
          != std::string::npos);

    const std::size_t frameCommit = wrapper.find(
        "static BOOL qoh99_frame_commit(void)");
    const std::size_t waitAtCommit = wrapper.find(
        "if (g_qoh99PresentationTryAcquireSlotV2)", frameCommit);
    const std::size_t framePublished = wrapper.find(
        "static void qoh99_frame_published(void)", frameCommit);
    const std::size_t observeAfterPublish = wrapper.find(
        "if (g_qoh99PresentationCommit)", framePublished);
    CHECK(frameCommit != std::string::npos);
    CHECK(waitAtCommit != std::string::npos);
    CHECK(framePublished != std::string::npos);
    CHECK(observeAfterPublish != std::string::npos);

    const std::size_t frameBlt = wrapper.find(
        "if (fb.matched)");
    const std::size_t ownerGate = wrapper.find(
        "if (dst->owner)", frameBlt);
    const std::size_t commitCall = wrapper.find(
        "if (qoh99_frame_commit())", ownerGate);
    const std::size_t presentCall = wrapper.find(
        "if (present_frame(dst->owner, TRUE))", commitCall);
    const std::size_t publishedCall = wrapper.find(
        "qoh99_frame_published();", presentCall);
    CHECK(frameBlt != std::string::npos);
    CHECK(ownerGate != std::string::npos);
    CHECK(commitCall != std::string::npos);
    CHECK(presentCall != std::string::npos);
    CHECK(publishedCall != std::string::npos);
    CHECK(ownerGate < commitCall);
    CHECK(commitCall < presentCall);
    CHECK(presentCall < publishedCall);

    const std::size_t install = hooks.find(
        "bool installHooks(RunMode mode)");
    const std::size_t acquire = hooks.find(
        "netAcquireWrapperPacingOwnership()", install);
    const std::size_t minHook = hooks.find("MH_Initialize()", install);
    CHECK(install != std::string::npos);
    CHECK(acquire != std::string::npos);
    CHECK(minHook != std::string::npos);
    CHECK(acquire < minHook);
}

TEST(ConfirmedCheckpointTelemetryDoesNotBlockTheGameThread) {
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    CHECK(!hooks.empty());

    const std::size_t checkpointBegin = hooks.find(
        "void netWriteConfirmedCheckpoint(const NetFrameMeta& meta)");
    const std::size_t checkpointEnd = hooks.find(
        "\nvoid netWriteConfirmedDesync(", checkpointBegin);
    CHECK(checkpointBegin != std::string::npos);
    CHECK(checkpointEnd != std::string::npos);
    if (checkpointBegin != std::string::npos
        && checkpointEnd != std::string::npos) {
        const std::string checkpoint = hooks.substr(
            checkpointBegin, checkpointEnd - checkpointBegin);
        CHECK(checkpoint.find("[confirmed-components]")
              != std::string::npos);
        CHECK(checkpoint.find("meta.preHash")
              != std::string::npos);
        CHECK(checkpoint.find("g_rb.hashSlot(")
              == std::string::npos);
        CHECK(checkpoint.find("confirmed_checkpoint_latest")
              == std::string::npos);
        CHECK(checkpoint.find("std::fopen(")
              == std::string::npos);
        CHECK(checkpoint.find("std::fwrite(")
              == std::string::npos);
    }
}

TEST(DdrawMemoryDiffInstrumentationIsExactOptIn) {
    const std::string instrument =
        readNetplayFile("ddraw_wrapper/qoh99_instrument.c");
    const std::string policy =
        readNetplayFile("ddraw_wrapper/qoh99_instrument_policy.h");
    CHECK(!instrument.empty());
    CHECK(!policy.empty());
    CHECK(instrument.find("\"QOH99_DDRAW_INSTRUMENT\"")
          != std::string::npos);
    CHECK(instrument.find("if (!instr_enabled()) return;")
          != std::string::npos);
    CHECK(policy.find(
              "value[0] == '1' && value[1] == '\\0'")
          != std::string::npos);
}

TEST(SpectatorTerminalProvesAppliedPostTailState) {
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    const std::string protocol =
        readNetplayFile("src/spectator/SpectatorProtocol.h");
    const std::string session =
        readNetplayFile("src/spectator/SpectatorSession.cpp");
    const std::string archive =
        readNetplayFile("src/spectator/SpectatorArchive.cpp");
    CHECK(!hooks.empty());
    CHECK(!protocol.empty());
    CHECK(!session.empty());
    CHECK(!archive.empty());

    CHECK(protocol.find("kSpectatorVersion = 4")
          != std::string::npos);
    CHECK(protocol.find("postTailStateHash")
          != std::string::npos);
    CHECK(hooks.find("uint64_t spectatorPreHash")
          != std::string::npos);
    CHECK(hooks.find("uint64_t spectatorPostHash")
          != std::string::npos);
    CHECK(hooks.find("netFrontendStateFingerprint(uint64_t audioFrame")
          != std::string::npos);
    CHECK(hooks.find("netFrontendFingerprint(false, g_ovrFrame)")
          != std::string::npos);
    CHECK(hooks.find("0x4A9938u, 0x4A993Cu, 0x4ABB24u")
          != std::string::npos);
    CHECK(hooks.find(
              "descriptor.initialStateHash = netSpectatorLiveStateHash()")
          != std::string::npos);
    CHECK(hooks.find(
              "g_netSpectatorPostTailHash = meta.spectatorPostHash")
          != std::string::npos);
    CHECK(hooks.find(
              "record.record, observedPreHash,\n"
              "                meta.spectatorPostHash")
          != std::string::npos);
    CHECK(session.find(
              "observedPostTail != _terminal.postTailStateHash")
          != std::string::npos);
    CHECK(session.find("_terminalAckDirty = true")
          != std::string::npos);
    CHECK(archive.find(
              "postTailStateHash != game->descriptor.initialStateHash")
          != std::string::npos);
    const std::size_t spectatorCompleteBegin = hooks.find(
        "if (status.bufferedRecords == 0)");
    const std::size_t spectatorCompleteEnd = hooks.find(
        "if (g_spectatorBufferPrimed) {",
        spectatorCompleteBegin);
    CHECK(spectatorCompleteBegin != std::string::npos);
    CHECK(spectatorCompleteEnd != std::string::npos);
    if (spectatorCompleteBegin != std::string::npos
        && spectatorCompleteEnd != std::string::npos) {
        const std::string spectatorComplete = hooks.substr(
            spectatorCompleteBegin,
            spectatorCompleteEnd - spectatorCompleteBegin);
        CHECK(spectatorComplete.find(
                  "g_spectatorPacingSummaryLogged")
              != std::string::npos);
        CHECK(spectatorComplete.find(
                  "netLogPresentationSummary();")
              != std::string::npos);
    }
    CHECK(hooks.find("finalStateHash") == std::string::npos);
}

TEST(ReplayCrossesTheNativeInputDomainWithTheAuditedCodec) {
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    CHECK(!hooks.empty());

    CHECK(hooks.find(
              "return unpackLogical(qgame::replay::replayInjectedAction(player))")
          != std::string::npos);
    CHECK(hooks.find("static_cast<int>(qgame::replay::replayInjectedAction(")
          == std::string::npos);

    CHECK(hooks.find("qinput::compactFromLogical(g_offlinePlayers[0].mask)")
          != std::string::npos);
    CHECK(hooks.find("qinput::compactFromLogical(g_offlinePlayers[1].mask)")
          != std::string::npos);
    CHECK(hooks.find("g_offlinePlayers[0].mask & 0x0F0F")
          == std::string::npos);
    CHECK(hooks.find("g_offlinePlayers[1].mask & 0x0F0F")
          == std::string::npos);

    CHECK(qinput::kCanonicalFightMask == 0x0F0Fu);
    CHECK(qinput::kLogicalFightMask == 0xF000Fu);
    CHECK(qinput::logicalFromCompact(0x0F0Fu) == qinput::kLogicalFightMask);
    CHECK(qinput::compactFromLogical(qinput::kLogicalFightMask) == 0x0F0Fu);
    CHECK(qinput::logicalFromCompact(0x0100u) == 0x10000u);
    CHECK((qinput::logicalFromCompact(0x0100u) & 0x0100u) == 0u);
}

TEST(NetplayPaletteAndAudioUseSeparateDeterministicContracts) {
    CHECK(qgame::kSimulationStatusSoundIds.size() == 11u);
    CHECK(qgame::simulationStatusSoundIndex(2) == 0);
    CHECK(qgame::simulationStatusSoundIndex(8) == 3);
    CHECK(qgame::simulationStatusSoundIndex(162) == 4);
    CHECK(qgame::simulationStatusSoundIndex(166) == 5);
    CHECK(qgame::simulationStatusSoundIndex(171) == 10);
    CHECK(qgame::soundStatusAffectsSimulation(4));
    CHECK(qgame::soundStatusAffectsSimulation(170));
    CHECK(!qgame::soundStatusAffectsSimulation(-1));
    CHECK(!qgame::soundStatusAffectsSimulation(0));
    CHECK(!qgame::soundStatusAffectsSimulation(1));
    CHECK(!qgame::soundStatusAffectsSimulation(163));
    CHECK(!qgame::soundStatusAffectsSimulation(165));
    CHECK(!qgame::soundStatusAffectsSimulation(172));

    CHECK(qgame::waveDurationFrames(44100u, 44100u) == 61u);
    CHECK(qgame::waveDurationFrames(22050u, 44100u) == 31u);
    CHECK(qgame::waveDurationFrames(0u, 44100u)
          == qgame::kFallbackSoundDurationFrames);
    CHECK(qgame::waveDurationFrames(44100u, 0u)
          == qgame::kFallbackSoundDurationFrames);
    CHECK(qgame::waveChunkNextOffset(44u, 40357u, 40401u)
          == 40401u);
    CHECK(qgame::waveChunkNextOffset(44u, 40357u, 40402u)
          == 40402u);
    CHECK(qgame::waveChunkNextOffset(44u, 40357u, 40400u)
          == UINT64_MAX);

    const std::string source = readNetplayFile("src/GameHooks.cpp");
    CHECK(!source.empty());
    CHECK(source.find("hkPaletteApply") != std::string::npos);
    CHECK(source.find("callNativePalettePreservingFpu")
          != std::string::npos);
    CHECK(source.find("hkSoundLoad") != std::string::npos);
    CHECK(source.find("readWaveDurationFrames") != std::string::npos);
    CHECK(source.find("hkSoundDuplicatePlay") != std::string::npos);
    CHECK(source.find("hkSoundDirectPlay") != std::string::npos);
    CHECK(source.find("hkSoundStop") != std::string::npos);
    CHECK(source.find("hkSoundStatus") != std::string::npos);
    CHECK(source.find("return virtualSoundStatus(slot, domain)")
          != std::string::npos);
    const std::size_t audioStateBegin =
        source.find("struct VirtualAudioState");
    const std::size_t audioStateEnd =
        source.find("static VirtualAudioState", audioStateBegin);
    CHECK(audioStateBegin != std::string::npos);
    CHECK(audioStateEnd != std::string::npos);
    if (audioStateBegin != std::string::npos
        && audioStateEnd != std::string::npos) {
        const std::string audioState =
            source.substr(audioStateBegin, audioStateEnd - audioStateBegin);
        CHECK(audioState.find("statusUntil") != std::string::npos);
        CHECK(audioState.find("durationFrames") == std::string::npos);
        CHECK(audioState.find("duplicateUntil") == std::string::npos);
    }
    CHECK(source.find("g_statusSoundDurationFrames")
          != std::string::npos);
    CHECK(source.find(
              "const bool canonical = deterministicAudioRoleActive()")
          != std::string::npos);
    CHECK(source.find("&& g_netCanonicalPoll;") != std::string::npos);
    CHECK(source.find(
              "if (statusIndex >= 0 && deterministicAudioRoleActive()) "
              "return 1;")
          != std::string::npos);
    CHECK(source.find("return canonical ? 1 : nativeResult")
          != std::string::npos);
    CHECK(source.find(
              "return canonical ? (parsed ? 1 : 0) : nativeResult")
          == std::string::npos);
    CHECK(source.find("kStatusSoundIds[]")
          != std::string::npos);
    CHECK(source.find(
              "g_statusSoundDurationFrames[\n"
              "                    static_cast<size_t>(statusIndex)] = frames")
          != std::string::npos);
    CHECK(source.find(
              "hash = netFnv1a(&audioManifest, sizeof(audioManifest), hash)")
          != std::string::npos);
    CHECK(source.find(
              "digest = netFnv1a(&audioManifest, sizeof(audioManifest), digest)")
          == std::string::npos);
    CHECK(source.find("QOH99_POST_LOAD_LAYOUT_V2_NO_AUDIO")
          != std::string::npos);
    CHECK(source.find(
              "inline uint64_t virtualSoundDuration(int sound) {\n"
              "    return qgame::canonicalStatusSoundFrames(sound);\n"
              "}")
          != std::string::npos);
    CHECK(qgame::canonicalStatusSoundFrames(162) == 64u);
    CHECK(qgame::canonicalStatusSoundFrames(166) == 33u);
    CHECK(qgame::canonicalStatusSoundFrames(167) == 35u);
    CHECK(qgame::canonicalStatusSoundFrames(168) == 52u);
    CHECK(qgame::canonicalStatusSoundFrames(169) == 41u);
    CHECK(qgame::canonicalStatusSoundFrames(170) == 42u);
    CHECK(qgame::canonicalStatusSoundFrames(171) == 99u);
    for (const int voiceSlot : {2, 4, 6, 8}) {
        CHECK(qgame::canonicalStatusSoundFrames(voiceSlot)
              == qgame::kFallbackSoundDurationFrames);
    }
    for (const int sound : qgame::kSimulationStatusSoundIds) {
        CHECK(qgame::canonicalStatusSoundFrames(sound) > 0u);
    }
    CHECK(source.find("resetCanonicalAudioSession()")
          != std::string::npos);
    CHECK(source.find("VirtualAudioState audio{}")
          != std::string::npos);
    CHECK(source.find("s.audio = g_virtualAudio")
          != std::string::npos);
    CHECK(source.find("g_virtualAudio = s.audio")
          != std::string::npos);
    CHECK(source.find("canonicalVirtualAudioForHash(")
          != std::string::npos);
    CHECK(source.find("s.audioFrame = frame;")
          != std::string::npos);
    CHECK(source.find("netFnv1a(&audio, sizeof(audio), h)")
          != std::string::npos);
    CHECK(source.find("0x45F760") != std::string::npos);
    CHECK(source.find("0x45F9C0") != std::string::npos);
    CHECK(source.find("0x45FB20") != std::string::npos);
    CHECK(source.find("0x45FB60") != std::string::npos);
    CHECK(source.find("0x45FBB0") != std::string::npos);
}

TEST(JoiningABusyHostOffersToWatchOnCloseAndOnTimeout) {
    const std::string host = readNetplayFile("src/client/Main.cpp");
    CHECK(!host.empty());

    const std::string sendStatement =
        "visitorLink.sendMsg(MSG_SPECTATE_INFO, &answer, sizeof(answer));";
    const size_t served = host.rfind(sendStatement);
    CHECK(served != std::string::npos);
    if (served == std::string::npos) return;
    const size_t closedAfter = host.find(
        "visitorLink.closeGraceful(kSpectateInfoCloseGraceMs);", served);
    CHECK(closedAfter != std::string::npos);
    CHECK(host.find_first_not_of(" \t\r\n", served + sendStatement.size())
          == closedAfter);

    CHECK(host.find("offerLiveSpectate") != std::string::npos);
    const size_t timeoutCase = host.find("case Pump::Timeout: {");
    const size_t closedCase = host.find("case Pump::PeerClosed: {");
    CHECK(timeoutCase != std::string::npos);
    CHECK(closedCase != std::string::npos);

    const size_t offerInTimeout = host.find("offerLiveSpectate(handled)",
                                            timeoutCase);
    const size_t timeoutGivesUp = host.find(
        "\"No initial config from host within 5 s\"", timeoutCase);
    CHECK(offerInTimeout != std::string::npos);
    CHECK(timeoutGivesUp != std::string::npos);
    CHECK(offerInTimeout < timeoutGivesUp);

    const size_t offerInClosed = host.find("offerLiveSpectate(handled)",
                                           closedCase);
    const size_t closedGivesUp = host.find(
        "\"Host disconnected during initial exchange\"", closedCase);
    CHECK(offerInClosed != std::string::npos);
    CHECK(closedGivesUp != std::string::npos);
    CHECK(offerInClosed < closedGivesUp);

    CHECK(host.find("ctx.spectateInfo.state != SPECTATE_LIVE")
          != std::string::npos);
}

TEST(AutopunchIsWiredIntoHostJoinAndSpectate) {
    const std::string main = readNetplayFile("src/client/Main.cpp");
    const std::string route =
        readNetplayFile("src/client/spectator/SpectatorRoute.cpp");
    const std::string routeHeader =
        readNetplayFile("src/client/spectator/SpectatorRoute.h");
    CHECK(!main.empty());
    CHECK(!route.empty());
    CHECK(!routeHeader.empty());

    CHECK(main.find("autopunchFallbackPass") != std::string::npos);
    const size_t raceCall = main.find("if (raceDirectCandidates(");
    const size_t fallbackCall = main.find("autopunchFallbackPass(", raceCall);
    CHECK(raceCall != std::string::npos);
    CHECK(fallbackCall != std::string::npos);

    CHECK(main.find("qnet::autopunch::AutopunchSession hostPunch")
          != std::string::npos);
    CHECK(main.find("&hostPunch") != std::string::npos);

    CHECK(routeHeader.find("punchRelayIpv4BE") != std::string::npos);
    CHECK(main.find("target.punchRelayIpv4BE") != std::string::npos);
    CHECK(route.find("qnet::autopunch::decodeMapping") != std::string::npos);
    CHECK(route.find("encodeRegister") != std::string::npos);
    CHECK(route.find("encodeLookup") != std::string::npos);

    CHECK(main.find("getStr(\"relayAddress\"") == std::string::npos);
    CHECK(main.find("setStr(\"relayAddress\"") == std::string::npos);
    CHECK(main.find("s.relayAddress") == std::string::npos);
    CHECK(main.find("dropRetiredKeys") != std::string::npos);
    CHECK(main.find("qnet::autopunch::kRelayHost") != std::string::npos);

    CHECK(main.find("bool autopunchRelayEndpoint(") != std::string::npos);
    CHECK(main.find("bool configuredRelayServer(") != std::string::npos);
    const size_t punchPass = main.find("autopunchFallbackPass");
    const size_t punchAccessor = main.find("autopunchRelayEndpoint(relay",
                                           punchPass);
    CHECK(punchAccessor != std::string::npos);

    CHECK(route.find("from.ipv4BE == state.target.punchRelayIpv4BE")
          != std::string::npos);
}

TEST(NetDriverDrainsAcceptedTrafficBeforeJudgingPeerLiveness) {
    const std::string source = readNetplayFile("src/GameHooks.cpp");

    const size_t driverCheck = source.find(
        "if (!netCheckLink(false) || !g_netSession\n"
        "        || !netSetSimulationFrame(g_netSession->currentFrame())\n"
        "        || !netDrainRemoteInputs()) {");
    CHECK(driverCheck != std::string::npos);

    const size_t livenessCheck = source.find(
        "if (!qgame::netPeerAlive()) {\n"
        "        netFailClosed(kNetTimeout, g_netFrame,\n"
        "                      \"peer liveness timeout; simulation halted\");\n"
        "        return kNetHoldFrameRet;",
        driverCheck);
    CHECK(livenessCheck != std::string::npos);
    CHECK(livenessCheck > driverCheck);

    const size_t terminalHandling = source.find(
        "if (qgame::netGetLinkError(error) && error.active) {");
    CHECK(terminalHandling != std::string::npos);
}

TEST(ControllerDirectionsBindIndividuallyWhenThereIsNoDpadOrStick) {
    const std::string preview =
        readNetplayFile("src/input/ControllerPreview.cpp");

    CHECK(preview.find("bool ControllerPreview::directionsMustBindIndividually")
          != std::string::npos);
    CHECK(preview.find("if (selection_.hatCount != 0) return false;")
          != std::string::npos);
    CHECK(preview.find("if (selection_.axisCount >= 2) return false;")
          != std::string::npos);

    CHECK(preview.find(
              "if (individualDirections && !semanticMode)\n"
              "        return probe.kind == ProbeKind::RawButton;")
          != std::string::npos);

    CHECK(preview.find("if (!captureMovement_ || individualDirections) {")
          != std::string::npos);

    const std::string ui = readNetplayFile("src/client/Main.cpp");
    CHECK(ui.find("if (directionRow\n"
                  "                        && !preview."
                  "directionsMustBindIndividually()) {")
          != std::string::npos);
}

TEST(RematchHudTextReadsTheSameChoiceStateAsTheMarkers) {
    const std::string source = readNetplayFile("src/GameHooks.cpp");

    const size_t canonText = source.find(
        "if (g_canonRematch.active) {\n"
        "                const size_t localSlot = g_netLocalCtrl == 0 ? 0u : 1u;");
    CHECK(canonText != std::string::npos);

    CHECK(source.find("g_canonRematch.ready[localSlot]") != std::string::npos);
    CHECK(source.find("g_canonRematch.choiceNo[localSlot]")
          != std::string::npos);
    CHECK(source.find("g_canonRematch.ready[remoteSlot]")
          != std::string::npos);
    CHECK(source.find("g_canonRematch.choiceNo[remoteSlot]")
          != std::string::npos);

    const size_t guardedFallback = source.find(
        "if (!g_canonRematch.active) {\n"
        "                const char* opp = \"deciding...\";",
        canonText);
    CHECK(guardedFallback != std::string::npos);

    CHECK(source.find("outState->rematchP1Ready =\n"
                      "                g_canonRematch.ready[0] ? TRUE : FALSE;")
          != std::string::npos);
}

TEST(RollbackBalanceIsAnIsolatedPeerLocalDeadlineController) {
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    const std::string policy =
        readNetplayFile("src/game/RollbackBalancePolicy.h");
    CHECK(!hooks.empty());
    CHECK(!policy.empty());

    const std::size_t begin = hooks.find("void netRegulatePaceSlew()");
    const std::size_t end = hooks.find(
        "bool netAdvanceOneCanonicalFrame", begin);
    CHECK(begin != std::string::npos);
    CHECK(end != std::string::npos);
    const std::string controller = hooks.substr(begin, end - begin);

    CHECK(controller.find("qgame::classifyRollbackBalance(balanceInput)")
          != std::string::npos);
    CHECK(controller.find("peer.predictionDepth") != std::string::npos);
    CHECK(controller.find("g_paceDeadline.setFrameTicks(adjusted)")
          != std::string::npos);
    CHECK(controller.find("QOH99_NP_DISABLE_ROLLBACK_BALANCE")
          == std::string::npos);
    CHECK(hooks.find("QOH99_NP_DISABLE_ROLLBACK_BALANCE")
          != std::string::npos);
    CHECK(hooks.find("netOwnedFrameAdvantage") == std::string::npos);

    CHECK(controller.find("netRequestGameplayDelay") == std::string::npos);
    CHECK(controller.find("capturePreFrame") == std::string::npos);
    CHECK(controller.find("finishRollback") == std::string::npos);
}

TEST(RematchDoorwayObservesTheCommittedCpuMask) {
    const std::string hooks = readNetplayFile("src/GameHooks.cpp");
    CHECK(!hooks.empty());
    CHECK(hooks.find("o.committedCpuMask = rd32(0x6236D4u);")
          != std::string::npos);

    CHECK(hooks.find("o.vacancyMask = rd32(0x6236CCu);") != std::string::npos);

    const std::string theater = readNetplayFile("src/netplay/RematchTheater.h");
    CHECK(!theater.empty());
    CHECK(theater.find("o.formationActive == 1u && o.committedCpuMask == 0u")
          != std::string::npos);
    CHECK(theater.find("&& rematchTheaterHumanVersusHuman(o);")
          != std::string::npos);
}
