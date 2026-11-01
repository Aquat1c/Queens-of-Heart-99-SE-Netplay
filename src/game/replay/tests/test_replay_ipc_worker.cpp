#include "netplay/tests/TestHarness.h"
#include "ReplayIpcTestFixture.h"
#include "game/PaletteBridge.h"

namespace {
using namespace replay_ipc_test;
using qgame::replay::SourceFault;

struct Fixture {
    Game first;
    ReplayIpcWorker worker;
    MemoryChannel* channel = nullptr;
    std::unique_ptr<qgame::replay::PlaybackPrebufferV3> firstPrebuffer;
    uint32_t sequence = 0u;
    bool start(bool result = true, const qreplay::ReplayPaletteV2* palette = nullptr) {
        auto owned = std::make_unique<MemoryChannel>();
        channel = owned.get();
        if (!feedGame(*channel, sequence, first, palette)
            || !worker.startForTest(first.options(), std::move(owned), nullptr, nullptr)
            || !worker.pumpOnceForTest() || !worker.playbackPrebufferReadyV3()) return false;
        firstPrebuffer = worker.takePlaybackPrebufferV3();
        return !result || (worker.submitPlaybackResultV3(first.result()) && worker.pumpOnceForTest());
    }
    bool request(const wire::PlaybackNextGamePayload& next,
                 const qreplay::GameIdentityV2& envelope) {
        std::vector<uint8_t> p;
        return wire::encodePlaybackNextGame(next, p)
            && feed(*channel, sequence, envelope, wire::MessageType::PlaybackNextGame, p)
            && worker.pumpOnceForTest();
    }
};
}

QNP_TEST(ReplayWorkerNextGameAckIsOldBoundAndLaterQueueEntriesAreNewBound) {
    Fixture f;
    QNP_REQUIRE(f.start());
    Game second(2u);
    const auto request = second.next(1u);
    QNP_REQUIRE(f.request(request, f.first.start.manifest.identity));
    wire::PlaybackNextGamePayload pending{};
    QNP_REQUIRE(f.worker.popPlaybackNextGameV3(pending));
    QNP_CHECK(pending.switchId == 1u && pending.game == request.game);
    QNP_CHECK(!f.worker.popPlaybackNextGameV3(pending));
    wire::PlaybackStatusPayload oldStatus{}, newStatus{};
    oldStatus.presentationSerial = 10u;
    newStatus.presentationSerial = 20u;
    QNP_REQUIRE(f.worker.submitPlaybackStatusV3(oldStatus));
    QNP_REQUIRE(f.worker.submitPlaybackNextGameAckV3(request));
    QNP_REQUIRE(f.worker.submitPlaybackStatusV3(newStatus));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    auto messages = readOutput(*f.channel);
    QNP_REQUIRE(messages.size() == 5u);
    QNP_CHECK(messages[2].type == wire::MessageType::PlaybackStatus);
    QNP_CHECK(messages[2].game == f.first.start.manifest.identity);
    QNP_CHECK(messages[3].type == wire::MessageType::PlaybackNextGameAck);
    QNP_CHECK(messages[3].game == f.first.start.manifest.identity);
    QNP_CHECK(messages[4].game == second.start.manifest.identity);
    for (size_t i = 0u; i < messages.size(); ++i)
        QNP_CHECK(messages[i].sequence == i + 1u);
    QNP_REQUIRE(feedGame(*f.channel, f.sequence, second));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    auto prebuffer = f.worker.takePlaybackPrebufferV3();
    QNP_REQUIRE(prebuffer != nullptr);
    QNP_CHECK(prebuffer->game == second.start.manifest.identity);
    QNP_REQUIRE(f.worker.submitPlaybackResultV3(second.result()));
    wire::PlaybackByePayload bye{};
    bye.reason = wire::PlaybackByeReason::NormalAfterResult;
    bye.resultSent = true;
    QNP_REQUIRE(f.worker.submitPlaybackByeV3(bye));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    messages = readOutput(*f.channel);
    QNP_REQUIRE(messages.size() == 7u);
    QNP_CHECK(messages[5].type == wire::MessageType::PlaybackResult);
    QNP_CHECK(messages[6].type == wire::MessageType::PlaybackBye);
    QNP_CHECK(messages[5].game == second.start.manifest.identity);
    f.channel->failRead = true;
    f.worker.pumpOnceForTest();
    QNP_CHECK(f.worker.playbackFaultV3() == SourceFault::None);
    f.worker.pumpOnceForTest();
    QNP_CHECK(!f.channel->connected());
    QNP_CHECK(readOutput(*f.channel).size() == 7u);
}

QNP_TEST(ReplayWorkerNextGameRejectsEarlyDuplicateAndStaleRequests) {
    for (int scenario = 0; scenario < 4; ++scenario) {
        Fixture f;
        QNP_REQUIRE(f.start(scenario != 0));
        auto request = Game(2u).next(1u);
        auto envelope = f.first.start.manifest.identity;
        if (scenario == 1) {
            QNP_REQUIRE(f.request(request, envelope));
        } else if (scenario == 2) {
            ++envelope.gameUuidLow;
        } else if (scenario == 3) {
            wire::PlaybackByePayload bye{};
            bye.reason = wire::PlaybackByeReason::NormalAfterResult;
            bye.resultSent = true;
            QNP_REQUIRE(f.worker.submitPlaybackByeV3(bye));
            QNP_REQUIRE(f.worker.pumpOnceForTest());
        }
        QNP_REQUIRE(f.request(request, envelope));
        QNP_CHECK(f.worker.playbackFaultV3() != SourceFault::None);
    }
}

QNP_TEST(ReplayWorkerNextGameRepeatedIdentityNeedsFreshSwitchId) {
    Fixture f;
    QNP_REQUIRE(f.start());
    auto request = f.first.next(1u);
    QNP_REQUIRE(f.request(request, request.game));
    wire::PlaybackNextGamePayload pending{};
    QNP_REQUIRE(f.worker.popPlaybackNextGameV3(pending));
    QNP_REQUIRE(f.worker.submitPlaybackNextGameAckV3(pending));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    QNP_REQUIRE(feedGame(*f.channel, f.sequence, f.first));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    QNP_REQUIRE(f.worker.takePlaybackPrebufferV3() != nullptr);
    QNP_REQUIRE(f.worker.submitPlaybackResultV3(f.first.result()));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    request.switchId = 2u;
    QNP_REQUIRE(f.request(request, request.game));
    QNP_REQUIRE(f.worker.popPlaybackNextGameV3(pending));
    QNP_REQUIRE(f.worker.submitPlaybackNextGameAckV3(pending));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    QNP_REQUIRE(feedGame(*f.channel, f.sequence, f.first));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    QNP_REQUIRE(f.worker.takePlaybackPrebufferV3() != nullptr);
    QNP_REQUIRE(f.worker.submitPlaybackResultV3(f.first.result()));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    request.switchId = 1u;
    QNP_REQUIRE(f.request(request, request.game));
    QNP_CHECK(f.worker.playbackFaultV3() == SourceFault::SequenceViolation);
}

QNP_TEST(ReplayWorkerNextGameAckMustEchoEntirePendingRequest) {
    Fixture f;
    QNP_REQUIRE(f.start());
    auto request = Game(2u).next(1u);
    QNP_REQUIRE(f.request(request, f.first.start.manifest.identity));
    wire::PlaybackNextGamePayload pending{};
    QNP_REQUIRE(f.worker.popPlaybackNextGameV3(pending));
    pending.presentationFlags ^= 4u;
    QNP_REQUIRE(f.worker.submitPlaybackNextGameAckV3(pending));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    QNP_CHECK(f.worker.playbackFaultV3() != SourceFault::None);
    QNP_CHECK(readOutput(*f.channel).size() == 2u);
}

QNP_TEST(ReplayWorkerNextGameRequiresAckBeforeDataAndRejectsPreviousBindingAfterAck) {
    for (bool acknowledge : {false, true}) {
        Fixture f;
        QNP_REQUIRE(f.start());
        Game second(2u);
        const auto request = second.next(1u);
        QNP_REQUIRE(f.request(request, f.first.start.manifest.identity));
        wire::PlaybackNextGamePayload pending{};
        QNP_REQUIRE(f.worker.popPlaybackNextGameV3(pending));
        if (acknowledge) {
            QNP_REQUIRE(f.worker.submitPlaybackNextGameAckV3(pending));
            QNP_REQUIRE(f.worker.pumpOnceForTest());
        }
        QNP_REQUIRE(feedGame(*f.channel, f.sequence,
                            acknowledge ? f.first : second));
        QNP_REQUIRE(f.worker.pumpOnceForTest());
        QNP_CHECK(f.worker.playbackFaultV3() != SourceFault::None);
        QNP_CHECK(!f.worker.playbackPrebufferReadyV3());
    }
}

QNP_TEST(ReplayWorkerNextGameRejectsAckUntilOldCommandsAreDrained) {
    Fixture f;
    QNP_REQUIRE(f.start());
    wire::PlaybackCommandPayload command{};
    command.commandId = 1u;
    command.kind = wire::PlaybackCommandKind::Pause;
    std::vector<uint8_t> payload;
    QNP_REQUIRE(wire::encodePlaybackCommand(command, payload));
    QNP_REQUIRE(feed(*f.channel, f.sequence, f.first.start.manifest.identity,
                     wire::MessageType::PlaybackCommand, payload));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    QNP_REQUIRE(f.request(Game(2u).next(1u), f.first.start.manifest.identity));
    wire::PlaybackNextGamePayload pending{};
    QNP_REQUIRE(f.worker.popPlaybackNextGameV3(pending));
    QNP_REQUIRE(f.worker.submitPlaybackNextGameAckV3(pending));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    QNP_CHECK(f.worker.playbackFaultV3() != SourceFault::None);
    QNP_CHECK(readOutput(*f.channel).size() == 2u);
}

QNP_TEST(ReplayWorkerNextGameAckWriteFailureCannotPublishNewPrebuffer) {
    Fixture f;
    QNP_REQUIRE(f.start());
    QNP_REQUIRE(f.request(Game(2u).next(1u), f.first.start.manifest.identity));
    wire::PlaybackNextGamePayload pending{};
    QNP_REQUIRE(f.worker.popPlaybackNextGameV3(pending));
    QNP_REQUIRE(f.worker.submitPlaybackNextGameAckV3(pending));
    f.channel->failWrite = true;
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    QNP_CHECK(f.worker.playbackFaultV3() == SourceFault::ChannelLost);
    QNP_CHECK(!f.worker.playbackPrebufferReadyV3());
    QNP_CHECK(readOutput(*f.channel).size() == 2u);
}

QNP_TEST(ReplayWorkerNextGameCannotReplaceLocalRuntimePrimitives) {
    for (int field = 0; field != 4; ++field) {
        Fixture f;
        QNP_REQUIRE(f.start());
        auto request = Game(2u).next(1u);
        auto& contract = request.runtimeContract;
        switch (field) {
            case 0: contract.stateHashAlgorithmId = qreplay::kReplayStateHashAlgorithmIdV2; break;
            case 1: ++contract.bootstrapSchemaId; break;
            case 2: ++contract.stateRegionSchemaFingerprint; break;
            case 3: contract.executableSha256[0] ^= 1u; break;
        }
        QNP_REQUIRE(f.request(request, f.first.start.manifest.identity));
        QNP_CHECK(f.worker.playbackFaultV3() == SourceFault::ManifestMismatch);
        wire::PlaybackNextGamePayload pending{};
        QNP_CHECK(!f.worker.popPlaybackNextGameV3(pending));
    }
}

QNP_TEST(ReplayWorkerNextGameUsesLocalPrimitivesForOlderRecordedHashAndSchema) {
    Fixture f;
    QNP_REQUIRE(f.start());
    Game second(2u);
    auto& recorded = second.start.manifest.contract;
    recorded.stateHashAlgorithmId = qreplay::kReplayStateHashAlgorithmIdV2;
    recorded.contentManifestRoot.fill(9u);
    second.info.recordedContract = recorded;
    second.start.start.initialRollingDigest = qreplay::initialReplayDigestV2(second.start);
    auto digest = second.start.start.initialRollingDigest;
    for (auto& event : second.events) {
        event.rollingDigest = qreplay::advanceReplayDigestV2(digest, event);
        digest = event.rollingDigest;
    }
    second.frame.rollingDigest = qreplay::advanceReplayDigestV2(digest, second.frame);
    second.info.endpoint.finalRollingDigest = second.frame.rollingDigest;
    auto request = second.next(1u);
    request.runtimeContract = f.first.options().playbackRuntimeContract;
    request.runtimeContract.contentManifestRoot = recorded.contentManifestRoot;
    request.supersededStateSchema = true;
    QNP_REQUIRE(f.request(request, f.first.start.manifest.identity));
    wire::PlaybackNextGamePayload pending{};
    QNP_REQUIRE(f.worker.popPlaybackNextGameV3(pending));
    QNP_REQUIRE(f.worker.submitPlaybackNextGameAckV3(pending));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    QNP_REQUIRE(feedGame(*f.channel, f.sequence, second));
    QNP_REQUIRE(f.worker.pumpOnceForTest());
    auto prebuffer = f.worker.takePlaybackPrebufferV3();
    QNP_REQUIRE(prebuffer != nullptr);
    QNP_CHECK(prebuffer->binding.runtimeContract == request.runtimeContract);
    QNP_CHECK(prebuffer->binding.runtimeContract.stateHashAlgorithmId != recorded.stateHashAlgorithmId);
    QNP_CHECK(prebuffer->binding.supersededStateSchema);
}

QNP_TEST(ReplayWorkerNextGameKeepsCommandIdsMonotonic) {
    for (uint64_t nextCommand : {1u, 2u}) {
        Fixture f;
        QNP_REQUIRE(f.start());
        wire::PlaybackCommandPayload command{};
        command.commandId = 1u;
        command.kind = wire::PlaybackCommandKind::Pause;
        std::vector<uint8_t> payload;
        QNP_REQUIRE(wire::encodePlaybackCommand(command, payload));
        QNP_REQUIRE(feed(*f.channel, f.sequence, f.first.start.manifest.identity,
                         wire::MessageType::PlaybackCommand, payload));
        QNP_REQUIRE(f.worker.pumpOnceForTest());
        wire::PlaybackCommandPayload received{};
        QNP_REQUIRE(f.worker.popPlaybackCommandV3(received));
        Game second(2u);
        QNP_REQUIRE(f.request(second.next(1u), f.first.start.manifest.identity));
        wire::PlaybackNextGamePayload pending{};
        QNP_REQUIRE(f.worker.popPlaybackNextGameV3(pending));
        QNP_REQUIRE(f.worker.submitPlaybackNextGameAckV3(pending));
        QNP_REQUIRE(f.worker.pumpOnceForTest());
        QNP_REQUIRE(feedGame(*f.channel, f.sequence, second));
        QNP_REQUIRE(f.worker.pumpOnceForTest());
        QNP_REQUIRE(f.worker.takePlaybackPrebufferV3() != nullptr);
        command.commandId = nextCommand;
        payload.clear();
        QNP_REQUIRE(wire::encodePlaybackCommand(command, payload));
        QNP_REQUIRE(feed(*f.channel, f.sequence, second.start.manifest.identity,
                         wire::MessageType::PlaybackCommand, payload));
        QNP_REQUIRE(f.worker.pumpOnceForTest());
        QNP_CHECK((f.worker.playbackFaultV3() == SourceFault::None) == (nextCommand == 2u));
        QNP_CHECK(f.worker.popPlaybackCommandV3(received) == (nextCommand == 2u));
    }
}

QNP_TEST(ReplayWorkerPalettesTravelWithPrebufferWithoutChangingLiveGame) {
    struct ClearHold { ~ClearHold() { qgame::paletteClearReplayHold(); } } clearHold;
    for (bool secondHasPalette : {false, true}) {
        qgame::paletteClearReplayHold();
        qreplay::ReplayPaletteV2 live{};
        live.present = 1u;
        std::memset(live.table, 0x52, sizeof(live.table));
        qgame::paletteHoldFromReplay(live);
        qgame::paletteReplayHold().applied = 1u;
        qreplay::ReplayPaletteV2 recorded = live;
        recorded.side = 3u;
        recorded.variant = 2u;
        std::memset(recorded.table, 0x93, sizeof(recorded.table));
        for (size_t variant = 0u; variant != 4u; ++variant)
            std::memset(recorded.table + variant * 64u, 0, 4u);
        Fixture f;
        QNP_REQUIRE(f.start(true, &recorded));
        QNP_REQUIRE(f.firstPrebuffer != nullptr);
        QNP_CHECK(f.firstPrebuffer->palettes[3].present == 1u);
        QNP_CHECK(f.firstPrebuffer->palettes[3].variant == 2u);
        QNP_CHECK(f.firstPrebuffer->palettes[3].table[100] == 0x93u);
        QNP_CHECK(qgame::paletteReplayHold().held == 1u);
        QNP_CHECK(qgame::paletteReplayHold().applied == 1u);

        recorded.table[100] = 0xAAu;
        std::vector<uint8_t> payload;
        QNP_REQUIRE(wire::encodePlaybackPalette(recorded, payload));
        QNP_REQUIRE(feed(*f.channel, f.sequence, f.first.start.manifest.identity,
                         wire::MessageType::PlaybackPalette, payload));
        QNP_REQUIRE(f.worker.pumpOnceForTest());
        QNP_CHECK(f.worker.playbackFaultV3() == SourceFault::None);
        QNP_CHECK(f.firstPrebuffer->palettes[3].table[100] == 0x93u);
        QNP_CHECK(qgame::paletteReplayHold().held == 1u);

        Game second(2u);
        QNP_REQUIRE(f.request(second.next(1u), f.first.start.manifest.identity));
        QNP_REQUIRE(feed(*f.channel, f.sequence, f.first.start.manifest.identity,
                         wire::MessageType::PlaybackPalette, payload));
        QNP_REQUIRE(f.worker.pumpOnceForTest());
        QNP_CHECK(f.worker.playbackFaultV3() == SourceFault::None);
        wire::PlaybackNextGamePayload pending{};
        QNP_REQUIRE(f.worker.popPlaybackNextGameV3(pending));
        QNP_REQUIRE(f.worker.submitPlaybackNextGameAckV3(pending));
        QNP_REQUIRE(f.worker.pumpOnceForTest());
        recorded.side = 1u;
        QNP_REQUIRE(feedGame(*f.channel, f.sequence, second,
                            secondHasPalette ? &recorded : nullptr));
        QNP_REQUIRE(f.worker.pumpOnceForTest());
        auto prebuffer = f.worker.takePlaybackPrebufferV3();
        QNP_REQUIRE(prebuffer != nullptr);
        for (size_t slot = 0u; slot != prebuffer->palettes.size(); ++slot)
            QNP_CHECK((prebuffer->palettes[slot].present != 0u)
                      == (secondHasPalette && slot == 1u));
        QNP_CHECK(qgame::paletteReplayHold().held == 1u);
        QNP_CHECK(qgame::paletteReplayHold().applied == 1u);
        QNP_CHECK(qgame::paletteReplayHold().slots[0].table[100] == 0x52u);
    }
}
