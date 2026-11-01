#include "TestHarness.h"

#include "audio/AudioTimeline.h"

#include <cstring>
#include <memory>

using namespace qaudio;

namespace {

AudioCommand stop(uint16_t slot) {
    AudioCommand command{};
    command.kind = AudioKind::Stop;
    command.endpoint = {AudioEndpoint::Type::Duplicate, slot};
    return command;
}

AudioCommand playDuplicate(int16_t sample, uint16_t slot,
                           LoadOutcome outcome = LoadOutcome::Present,
                           uint32_t assetGeneration = 1) {
    AudioCommand command{};
    command.kind = AudioKind::PlayDuplicate;
    command.endpoint = {AudioEndpoint::Type::Duplicate, slot};
    command.sampleId = sample;
    command.loadOutcome = outcome;
    command.asset.generation = assetGeneration;
    command.asset.pathHash = 0x1234u + static_cast<uint64_t>(sample);
    return command;
}

}

QNP_TEST(TimelineStopPlayPairNormalizesToReplace) {
    auto timelineOwner = std::make_unique<AudioTimeline>();
    AudioTimeline& timeline = *timelineOwner;
    AudioTelemetry telemetry{};
    timeline.reset();
    QNP_REQUIRE(timeline.beginCapture(1, 1, 100));
    QNP_CHECK(timeline.append(stop(33), telemetry));
    QNP_CHECK(timeline.append(playDuplicate(134, 33), telemetry));
    QNP_REQUIRE(timeline.commitCapture());

    const FrameJournal* journal = timeline.journalFor(100);
    QNP_REQUIRE(journal != nullptr);
    QNP_REQUIRE(journal->commandCount == 1);
    QNP_CHECK(journal->commands[0].kind == AudioKind::Replace);
    QNP_CHECK(journal->commands[0].sampleId == 134);
    QNP_CHECK(journal->commands[0].endpoint.index == 33);
    QNP_CHECK(telemetry.intentGenerated == 2);
}

QNP_TEST(TimelineAbsentAssetPairPreservesStopAsReplaceNone) {
    auto timelineOwner = std::make_unique<AudioTimeline>();
    AudioTimeline& timeline = *timelineOwner;
    AudioTelemetry telemetry{};
    timeline.reset();
    QNP_REQUIRE(timeline.beginCapture(1, 1, 5));
    QNP_CHECK(timeline.append(stop(17), telemetry));
    QNP_CHECK(timeline.append(
        playDuplicate(140, 17, LoadOutcome::Absent), telemetry));
    QNP_REQUIRE(timeline.commitCapture());

    const FrameJournal* journal = timeline.journalFor(5);
    QNP_REQUIRE(journal != nullptr);
    QNP_REQUIRE(journal->commandCount == 1);
    QNP_CHECK(journal->commands[0].kind == AudioKind::Replace);
    QNP_CHECK(journal->commands[0].sampleId == -1);
}

QNP_TEST(TimelineTwoLegitimatePlaysAreNeverCollapsed) {
    auto timelineOwner = std::make_unique<AudioTimeline>();
    AudioTimeline& timeline = *timelineOwner;
    AudioTelemetry telemetry{};
    timeline.reset();
    QNP_REQUIRE(timeline.beginCapture(1, 1, 7));
    QNP_CHECK(timeline.append(playDuplicate(134, 33), telemetry));
    QNP_CHECK(timeline.append(playDuplicate(134, 33), telemetry));
    QNP_REQUIRE(timeline.commitCapture());
    const FrameJournal* journal = timeline.journalFor(7);
    QNP_REQUIRE(journal != nullptr);
    QNP_CHECK(journal->commandCount == 2);
    QNP_CHECK(journal->commands[0].ordinal == 0);
    QNP_CHECK(journal->commands[1].ordinal == 1);
}

QNP_TEST(TimelineAbortLeavesNoTrace) {
    auto timelineOwner = std::make_unique<AudioTimeline>();
    AudioTimeline& timeline = *timelineOwner;
    AudioTelemetry telemetry{};
    timeline.reset();
    QNP_REQUIRE(timeline.beginCapture(1, 1, 9));
    QNP_CHECK(timeline.append(playDuplicate(134, 33), telemetry));
    timeline.abortCapture();
    QNP_CHECK(timeline.journalFor(9) == nullptr);
    QNP_CHECK(!timeline.capturing());
}

QNP_TEST(TimelineOverflowFailsClosedAndCounts) {
    auto timelineOwner = std::make_unique<AudioTimeline>();
    AudioTimeline& timeline = *timelineOwner;
    AudioTelemetry telemetry{};
    timeline.reset();
    QNP_REQUIRE(timeline.beginCapture(1, 1, 11));
    for (size_t i = 0; i < kMaxCommandsPerFrame; ++i)
        QNP_CHECK(timeline.append(playDuplicate(134, 33), telemetry));
    QNP_CHECK(!timeline.append(playDuplicate(134, 33), telemetry));
    QNP_CHECK(timeline.overflowFailClosed());
    QNP_CHECK(telemetry.journalOverflow == 1);
}

QNP_TEST(TimelineCorrectionStagingIsAtomic) {
    auto timelineOwner = std::make_unique<AudioTimeline>();
    AudioTimeline& timeline = *timelineOwner;
    AudioTelemetry telemetry{};
    timeline.reset();
    QNP_REQUIRE(timeline.beginCapture(1, 1, 20));
    QNP_CHECK(timeline.append(playDuplicate(134, 33), telemetry));
    QNP_REQUIRE(timeline.commitCapture());
    const uint64_t originalDigest = timeline.intentDigestFor(20);
    QNP_REQUIRE(originalDigest != 0);

    QNP_REQUIRE(timeline.beginCorrection());
    QNP_REQUIRE(timeline.beginCapture(1, 1, 20));
    QNP_CHECK(timeline.append(playDuplicate(136, 33), telemetry));
    QNP_REQUIRE(timeline.commitCapture());
    QNP_CHECK(timeline.stagedCount() == 1);
    timeline.abortCorrection();
    QNP_CHECK(timeline.intentDigestFor(20) == originalDigest);

    QNP_REQUIRE(timeline.beginCorrection());
    QNP_REQUIRE(timeline.beginCapture(1, 1, 20));
    QNP_CHECK(timeline.append(playDuplicate(136, 33), telemetry));
    QNP_REQUIRE(timeline.commitCapture());
    QNP_REQUIRE(timeline.commitCorrection());
    QNP_CHECK(timeline.intentDigestFor(20) != originalDigest);
    const FrameJournal* journal = timeline.journalFor(20);
    QNP_REQUIRE(journal != nullptr);
    QNP_CHECK(journal->commands[0].sampleId == 136);
}

QNP_TEST(TimelineEmissionRecordsAndReplacement) {
    auto timelineOwner = std::make_unique<AudioTimeline>();
    AudioTimeline& timeline = *timelineOwner;
    AudioTelemetry telemetry{};
    timeline.reset();
    EmittedOccurrence occurrence{};
    occurrence.frame = 30;
    occurrence.ordinal = 0;
    occurrence.endpoint = {AudioEndpoint::Type::Duplicate, 21};
    occurrence.sampleId = 141;
    occurrence.token = {1, 7};
    QNP_CHECK(timeline.recordEmission(occurrence, telemetry));
    const FrameEmissions* emissions = timeline.emissionsFor(30);
    QNP_REQUIRE(emissions != nullptr && emissions->count == 1);
    QNP_CHECK(emissions->entries[0].token.endpointGeneration == 7);

    EmittedOccurrence corrected = occurrence;
    corrected.token = {1, 9};
    timeline.replaceEmissions(30, &corrected, 1);
    const FrameEmissions* replaced = timeline.emissionsFor(30);
    QNP_REQUIRE(replaced != nullptr && replaced->count == 1);
    QNP_CHECK(replaced->entries[0].token.endpointGeneration == 9);
}

QNP_TEST(TimelineRingWrapValidatesExactFrame) {
    auto timelineOwner = std::make_unique<AudioTimeline>();
    AudioTimeline& timeline = *timelineOwner;
    AudioTelemetry telemetry{};
    timeline.reset();
    QNP_REQUIRE(timeline.beginCapture(1, 1, 3));
    QNP_CHECK(timeline.append(playDuplicate(134, 33), telemetry));
    QNP_REQUIRE(timeline.commitCapture());
    QNP_CHECK(timeline.journalFor(3) != nullptr);
    QNP_CHECK(timeline.journalFor(3 + kTimelineFrames) == nullptr);
}

QNP_TEST(TimelineOwnedPathIsCopiedNotReferenced) {
    char stackPath[64];
    std::strcpy(stackPath, ".\\akari\\akari.wav");
    AudioCommand command{};
    command.kind = AudioKind::LoadAttempt;
    command.sampleId = 34;
    command.loadOutcome = LoadOutcome::Present;
    command.setPath(stackPath);
    std::strcpy(stackPath, "XXXXXXXX");
    QNP_CHECK(std::strcmp(command.ownedPath, ".\\akari\\akari.wav") == 0);
}
