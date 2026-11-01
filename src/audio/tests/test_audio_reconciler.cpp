#include "TestHarness.h"

#include "audio/AudioReconciler.h"

#include <cstring>
#include <memory>

using namespace qaudio;

namespace {

class CatalogStub : public IAssetCatalog {
public:
    WaveFacts factsFor(int16_t sample, const AssetKey&) const override {
        WaveFacts facts{};
        if (sample == kUnknownSample) return facts;
        facts.valid = true;
        facts.dataBytes = 44100;
        facts.byteRate = 44100;
        facts.blockAlign = 2;
        facts.bitsPerSample = 16;
        facts.durationFrames = 61;
        return facts;
    }
    static constexpr int16_t kUnknownSample = 250;
};

struct Fixture {
    AudioTimeline timeline;
    AudioTelemetry telemetry{};
    CatalogStub catalog;
    OwnerSnapshot owners;
    ResourceMap baseline{};
    const FrameJournal* staged[16] = {};
    size_t stagedCount = 0;

    Fixture() { timeline.reset(); }

    AudioCommand play(int16_t sample, uint16_t slot,
                      uint32_t assetGeneration = 1,
                      LoadOutcome outcome = LoadOutcome::Present) {
        AudioCommand command{};
        command.kind = AudioKind::PlayDuplicate;
        command.endpoint = {AudioEndpoint::Type::Duplicate, slot};
        command.sampleId = sample;
        command.loadOutcome = outcome;
        command.asset.generation = assetGeneration;
        command.asset.pathHash = 0x9000u + static_cast<uint64_t>(sample);
        return command;
    }
    AudioCommand stopCmd(uint16_t slot) {
        AudioCommand command{};
        command.kind = AudioKind::Stop;
        command.endpoint = {AudioEndpoint::Type::Duplicate, slot};
        return command;
    }
    AudioCommand loadCmd(int16_t sample, const char* path,
                         LoadOutcome outcome, uint32_t generation) {
        AudioCommand command{};
        command.kind = AudioKind::LoadAttempt;
        command.sampleId = sample;
        command.loadOutcome = outcome;
        command.asset.generation = generation;
        command.asset.pathHash = normalizedPathHash(path);
        command.setPath(path);
        return command;
    }

    void oldFrame(uint32_t frame, const AudioCommand* commands, size_t count,
                  bool emitFirst, uint32_t endpointGeneration) {
        timeline.beginCapture(1, 1, frame);
        for (size_t i = 0; i < count; ++i)
            timeline.append(commands[i], telemetry);
        timeline.commitCapture();
        if (emitFirst) {
            const FrameJournal* journal = timeline.journalFor(frame);
            for (uint16_t i = 0; i < journal->commandCount; ++i) {
                const AudioCommand& command = journal->commands[i];
                if (command.kind != AudioKind::PlayDuplicate
                    && command.kind != AudioKind::PlayBase
                    && command.kind != AudioKind::Replace)
                    continue;
                if (command.sampleId < 0
                    || command.loadOutcome == LoadOutcome::Absent)
                    continue;
                EmittedOccurrence occurrence{};
                occurrence.key = semanticKeyOf(command);
                occurrence.frame = frame;
                occurrence.ordinal = command.ordinal;
                occurrence.endpoint = command.endpoint;
                occurrence.sampleId = command.sampleId;
                occurrence.token = {1, endpointGeneration};
                occurrence.startFrame = frame;
                occurrence.durationFrames = 61;
                timeline.recordEmission(occurrence, telemetry);
                owners.set(command.endpoint, occurrence.token);
            }
        }
    }

    void correctedFrame(uint32_t frame, const AudioCommand* commands,
                        size_t count) {
        timeline.beginCorrection();
        timeline.beginCapture(1, 1, frame);
        for (size_t i = 0; i < count; ++i)
            timeline.append(commands[i], telemetry);
        timeline.commitCapture();
    }

    ReconcileResult resultStorage{};

    const ReconcileResult& run(uint32_t restore, uint32_t frontier) {
        stagedCount = timeline.stagedCount();
        for (size_t i = 0; i < stagedCount; ++i)
            staged[i] = timeline.staged(i);
        ReconcileInput input{};
        input.timeline = &timeline;
        input.restoreFrame = restore;
        input.frontierFrame = frontier;
        input.corrected = staged;
        input.correctedCount = stagedCount;
        input.catalog = &catalog;
        input.owners = &owners;
        input.baselineResources = &baseline;
        reconcileCorrection(input, resultStorage);
        return resultStorage;
    }
};

size_t countActions(const ReconcileResult& result, SinkAction::Type type) {
    size_t count = 0;
    for (size_t i = 0; i < result.actionCount; ++i)
        if (result.actions[i].type == type) ++count;
    return count;
}

}

QNP_TEST(ReconcileIdenticalReplayIsSilent) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    AudioCommand pair[] = {fixture.stopCmd(33), fixture.play(134, 33)};
    fixture.oldFrame(100, pair, 2, true, 5);
    AudioCommand correctedPair[] = {fixture.stopCmd(33),
                                    fixture.play(134, 33)};
    fixture.correctedFrame(100, correctedPair, 2);
    const ReconcileResult& result = fixture.run(100, 101);
    QNP_REQUIRE(result.valid);
    QNP_CHECK(result.actionCount == 0);
    QNP_CHECK(result.counters.replayCreditConsumed == 1);
    QNP_CHECK(result.counters.correctedNewPlayed == 0);
    QNP_CHECK(result.counters.oldOnlyCanceled == 0);
    QNP_REQUIRE(result.emissionCount == 1);
    QNP_CHECK(result.emissions[0].occurrence.token.endpointGeneration == 5);
    QNP_CHECK(result.emissions[0].tokenFromAction == -1);
}

QNP_TEST(ReconcileCorrectedNewPlaysOnce) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    fixture.oldFrame(100, nullptr, 0, false, 0);
    AudioCommand corrected[] = {fixture.play(136, 33)};
    fixture.correctedFrame(100, corrected, 1);
    const ReconcileResult& result = fixture.run(100, 101);
    QNP_REQUIRE(result.valid);
    QNP_CHECK(result.actionCount == 1);
    QNP_CHECK(countActions(result, SinkAction::Type::PlayDuplicate) == 1);
    QNP_CHECK(result.counters.correctedNewPlayed == 1);
    QNP_REQUIRE(result.emissionCount == 1);
    QNP_CHECK(result.emissions[0].tokenFromAction == 0);
}

QNP_TEST(ReconcileOldOnlyStopsExactGeneration) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    AudioCommand old[] = {fixture.play(134, 33)};
    fixture.oldFrame(100, old, 1, true, 5);
    fixture.correctedFrame(100, nullptr, 0);
    const ReconcileResult& result = fixture.run(100, 101);
    QNP_REQUIRE(result.valid);
    QNP_REQUIRE(result.actionCount == 1);
    QNP_CHECK(result.actions[0].type == SinkAction::Type::StopIfOwner);
    QNP_CHECK(result.actions[0].owner.endpointGeneration == 5);
    QNP_CHECK(result.counters.oldOnlyCanceled == 1);
}

QNP_TEST(ReconcileChangedEndpointReplacesOnce) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    AudioCommand old[] = {fixture.play(134, 33)};
    fixture.oldFrame(100, old, 1, true, 5);
    AudioCommand corrected[] = {fixture.play(136, 33)};
    fixture.correctedFrame(100, corrected, 1);
    const ReconcileResult& result = fixture.run(100, 101);
    QNP_REQUIRE(result.valid);
    QNP_REQUIRE(result.actionCount == 2);
    QNP_CHECK(result.actions[0].type == SinkAction::Type::StopIfOwner);
    QNP_CHECK(result.actions[0].owner.endpointGeneration == 5);
    QNP_CHECK(result.actions[1].type == SinkAction::Type::PlayDuplicate);
    QNP_CHECK(result.actions[1].sampleId == 136);
}

QNP_TEST(ReconcileSameFrameMultiplicityPreserved) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    AudioCommand old[] = {fixture.play(134, 33), fixture.play(134, 33)};
    fixture.oldFrame(100, old, 2, true, 5);
    AudioCommand corrected[] = {fixture.play(134, 33),
                                fixture.play(134, 33)};
    fixture.correctedFrame(100, corrected, 2);
    const ReconcileResult& result = fixture.run(100, 101);
    QNP_REQUIRE(result.valid);
    QNP_CHECK(result.counters.matchedTotal == 2);
    QNP_CHECK(result.actionCount == 0);
}

QNP_TEST(ReconcileEndpointsAreIndependent) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    AudioCommand old[] = {fixture.play(134, 33), fixture.play(134, 21)};
    fixture.oldFrame(100, old, 2, true, 5);
    AudioCommand corrected[] = {fixture.play(134, 33)};
    fixture.correctedFrame(100, corrected, 1);
    const ReconcileResult& result = fixture.run(100, 101);
    QNP_REQUIRE(result.valid);
    QNP_REQUIRE(result.actionCount == 1);
    QNP_CHECK(result.actions[0].type == SinkAction::Type::StopIfOwner);
    QNP_CHECK(result.actions[0].endpoint.index == 21);
}

QNP_TEST(ReconcileLatestToLatestPreference) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    AudioCommand oldFirst[] = {fixture.play(134, 33)};
    fixture.oldFrame(100, oldFirst, 1, true, 5);
    AudioCommand oldSecond[] = {fixture.stopCmd(33), fixture.play(134, 33)};
    fixture.oldFrame(101, oldSecond, 2, true, 6);
    AudioCommand corrected[] = {fixture.stopCmd(33), fixture.play(134, 33)};
    fixture.correctedFrame(101, corrected, 2);
    const ReconcileResult& result = fixture.run(100, 102);
    QNP_REQUIRE(result.valid);
    for (size_t i = 0; i < result.actionCount; ++i) {
        QNP_REQUIRE(result.actions[i].type == SinkAction::Type::StopIfOwner);
        QNP_CHECK(result.actions[i].owner.endpointGeneration == 5);
    }
    bool correctedKeepsLive = false;
    for (size_t i = 0; i < result.emissionCount; ++i) {
        if (result.emissions[i].occurrence.token.endpointGeneration == 6)
            correctedKeepsLive = true;
    }
    QNP_CHECK(correctedKeepsLive);
}

QNP_TEST(ReconcileDisplacedOwnerResumesAtCursor) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    AudioCommand oldA[] = {fixture.play(134, 33)};
    fixture.oldFrame(100, oldA, 1, true, 5);
    AudioCommand oldB[] = {fixture.stopCmd(33), fixture.play(136, 33)};
    fixture.oldFrame(104, oldB, 2, true, 6);
    AudioCommand correctedA[] = {fixture.play(134, 33)};
    fixture.correctedFrame(100, correctedA, 1);
    fixture.correctedFrame(104, nullptr, 0);
    const ReconcileResult& result = fixture.run(100, 110);
    QNP_REQUIRE(result.valid);
    QNP_REQUIRE(result.actionCount == 2);
    QNP_CHECK(result.actions[0].type == SinkAction::Type::StopIfOwner);
    QNP_CHECK(result.actions[0].owner.endpointGeneration == 6);
    QNP_CHECK(result.actions[1].type == SinkAction::Type::PlayAtCursor);
    QNP_CHECK(result.actions[1].sampleId == 134);
    QNP_CHECK(result.actions[1].cursorBytes == 10);
    QNP_CHECK(result.counters.cursorResumed == 1);
}

QNP_TEST(ReconcileNestedCorrectionDoesNotDuplicate) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    AudioCommand old[] = {fixture.play(134, 33)};
    fixture.oldFrame(100, old, 1, true, 5);
    AudioCommand corrected[] = {fixture.play(134, 33)};
    fixture.correctedFrame(100, corrected, 1);
    const ReconcileResult& first = fixture.run(100, 101);
    QNP_REQUIRE(first.valid && first.actionCount == 0);
    fixture.timeline.commitCorrection();
    EmittedOccurrence installed[4];
    size_t installedCount = 0;
    for (size_t i = 0; i < first.emissionCount; ++i) {
        installed[installedCount] = first.emissions[i].occurrence;
        ++installedCount;
    }
    fixture.timeline.replaceEmissions(100, installed, installedCount);

    AudioCommand correctedAgain[] = {fixture.play(134, 33)};
    fixture.correctedFrame(100, correctedAgain, 1);
    const ReconcileResult& second = fixture.run(100, 101);
    QNP_REQUIRE(second.valid);
    QNP_CHECK(second.actionCount == 0);
    QNP_CHECK(second.counters.replayCreditConsumed == 1);
}

QNP_TEST(ReconcileOverflowFailsClosed) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    fixture.oldFrame(100, nullptr, 0, false, 0);
    fixture.timeline.beginCorrection();
    fixture.timeline.beginCapture(1, 1, 100);
    for (int i = 0; i < 25; ++i)
        fixture.timeline.append(fixture.play(134, 33), fixture.telemetry);
    fixture.timeline.commitCapture();
    const ReconcileResult& result = fixture.run(100, 101);
    QNP_CHECK(!result.valid);
    QNP_CHECK(result.actionCount == 0);
}

QNP_TEST(ReconcileResourceFoldRestoresBaselineAsset) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    fixture.baseline.samples[34].present = true;
    fixture.baseline.samples[34].asset.pathHash =
        normalizedPathHash(".\\akari\\akari.wav");
    fixture.baseline.samples[34].asset.generation = 1;
    std::strcpy(fixture.baseline.samples[34].ownedPath,
                ".\\akari\\akari.wav");
    AudioCommand old[] = {fixture.loadCmd(
        34, ".\\mai\\mai.wav", LoadOutcome::Present, 2)};
    fixture.oldFrame(100, old, 1, false, 0);
    fixture.correctedFrame(100, nullptr, 0);
    const ReconcileResult& result = fixture.run(100, 101);
    QNP_REQUIRE(result.valid);
    QNP_REQUIRE(countActions(result, SinkAction::Type::Load) == 1);
    bool restored = false;
    for (size_t i = 0; i < result.actionCount; ++i) {
        if (result.actions[i].type == SinkAction::Type::Load
            && std::strcmp(result.actions[i].ownedPath,
                           ".\\akari\\akari.wav") == 0)
            restored = true;
    }
    QNP_CHECK(restored);
    QNP_CHECK(result.counters.assetReconciled == 1);
}

QNP_TEST(ReconcileFailedLoadLeavesAbsent) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    fixture.baseline.samples[34].present = true;
    fixture.baseline.samples[34].asset.generation = 1;
    std::strcpy(fixture.baseline.samples[34].ownedPath, ".\\old.wav");
    fixture.oldFrame(100, nullptr, 0, false, 0);
    AudioCommand corrected[] = {fixture.loadCmd(
        34, ".\\missing.wav", LoadOutcome::Absent, 0)};
    fixture.correctedFrame(100, corrected, 1);
    const ReconcileResult& result = fixture.run(100, 101);
    QNP_REQUIRE(result.valid);
    QNP_CHECK(!result.correctedResources.samples[34].present);
    QNP_CHECK(countActions(result, SinkAction::Type::Unload) == 1);
}

QNP_TEST(ReconcileAbsentPlayHasNoOnset) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    AudioCommand old[] = {fixture.play(134, 33)};
    fixture.oldFrame(100, old, 1, true, 5);
    AudioCommand corrected[] = {
        fixture.play(134, 33),
        fixture.play(136, 21, 1, LoadOutcome::Absent),
    };
    fixture.correctedFrame(100, corrected, 2);
    const ReconcileResult& result = fixture.run(100, 101);
    QNP_REQUIRE(result.valid);
    QNP_CHECK(result.actionCount == 0);
}

QNP_TEST(ReconcileAssetGenerationGatesCredits) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    AudioCommand old[] = {fixture.play(134, 33, 1)};
    fixture.oldFrame(100, old, 1, true, 5);
    AudioCommand corrected[] = {fixture.play(134, 33, 2)};
    fixture.correctedFrame(100, corrected, 1);
    const ReconcileResult& result = fixture.run(100, 101);
    QNP_REQUIRE(result.valid);
    QNP_CHECK(result.counters.replayCreditConsumed == 0);
    QNP_CHECK(result.counters.correctedNewPlayed == 1);
    QNP_CHECK(result.counters.oldOnlyCanceled == 1);
}

QNP_TEST(ReconcileExpiredCorrectedNewIsSkippedWithCount) {
    auto fixtureOwner = std::make_unique<Fixture>();
    Fixture& fixture = *fixtureOwner;
    fixture.oldFrame(100, nullptr, 0, false, 0);
    AudioCommand corrected[] = {fixture.play(134, 33)};
    fixture.correctedFrame(100, corrected, 1);
    const ReconcileResult& result = fixture.run(100, 170);
    QNP_REQUIRE(result.valid);
    QNP_CHECK(result.actionCount == 0);
    QNP_CHECK(result.counters.correctedNewExpiredSkipped == 1);
}
