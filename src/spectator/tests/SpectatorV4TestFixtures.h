#pragma once

#include "../SpectatorConnectedArchiveV4.h"
#include "../SpectatorConnectedSessionV4.h"

#include "../../compat/CompatibilityIdentity.h"
#include "../../replay/ReplayCodec.h"
#include "../../replay/ReplayIntegrity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qnetplay::spectator::testv4 {

inline compat::CompatibilityIdentityWire compatibilityIdentity() {
    compat::CompatibilityIdentityWire out{};
    out.schemaVersion = compat::kCompatibilityIdentitySchemaVersion;
    out.protocolVersion = 10u;
    (void)compat::setProfileId(out, "qoh99-se1126");
    for (size_t i = 0u; i < compat::kCompatibilitySha256Bytes; ++i) {
        out.runtimeBuild.executableSha256[i] =
            static_cast<uint8_t>(0x10u + i);
        out.simulationContent.simulationRootSha256[i] =
            static_cast<uint8_t>(0x40u + i);
        out.simulationContent.validationRootSha256[i] =
            static_cast<uint8_t>(0x80u + i);
    }
    out.runtimeBuild.executableSize = 0x123400u;
    out.runtimeBuild.peTimestamp = 0x5E1126u;
    out.simulationContent.manifestSchemaVersion = 1u;
    out.simulationContent.manifestFlags = 1u;
    out.simulationContent.entryCount = 68u;
    out.simulationContent.requiredEntryCount = 68u;
    out.stateSchemaId = 0x1001u;
    out.modSimulationId = 0x1002u;
    out.timingProfileId = 0x1003u;
    return out;
}

inline ConnectedSetDescriptorV4 descriptor(uint64_t stream = 0x4401u) {
    ConnectedSetDescriptorV4 out{};
    out.setStreamId = stream;
    out.sourceIdentity.sessionId = 0x123456789ABCDEF0ull;
    out.sourceIdentity.epoch = 7u;
    out.sourceIdentity.cookie.low = 0x1111222233334444ull;
    out.sourceIdentity.cookie.high = 0x5555666677778888ull;
    out.simulationContract = 0x514F483939563400ull;
    out.initialDescriptorDigest = 0xD35C120000000001ull;
    out.compatibilityIdentity = compatibilityIdentity();
    out.contentIdentityAttested = true;
    return out;
}

inline qreplay::ConfirmedGameStartV2 start(uint32_t generation) {
    qreplay::ConfirmedGameStartV2 out{};
    out.manifest.identity.gameUuidLow =
        0xA000000000000000ull + generation;
    out.manifest.identity.gameUuidHigh =
        0xB000000000000000ull + generation;
    out.manifest.identity.gameGeneration = generation;
    qreplay::SimulationContractV2& contract = out.manifest.contract;
    for (size_t i = 0; i < contract.executableSha256.size(); ++i) {
        contract.executableSha256[i] =
            static_cast<uint8_t>(i + generation + 1u);
        contract.contentManifestRoot[i] =
            static_cast<uint8_t>(0x80u + i + generation);
    }
    contract.nativeBuildIdentity = 0x1001u;
    contract.runtimeSimulationId = 0x1002u;
    contract.stateRegionSchemaFingerprint = 0x1003u;
    contract.stateHashProjectionId = 0x1004u;
    contract.stateHashAlgorithmId = 0x1005u;
    contract.bootstrapSchemaId = 0x1006u;
    contract.eventSchemaId = 0x1007u;
    contract.characterCatalogId = 0x1008u;
    contract.terminalResultSchemaId = 0x1009u;
    contract.compactActionDomainVersion = 1u;

    qreplay::GameManifestV2& manifest = out.manifest;
    manifest.vsRow = 1u;
    manifest.activeLaneMask = 0x03u;
    manifest.activeFighterMask = 0x03u;
    manifest.slotToSourceRoute = {{0u, 1u, 2u, 3u}};
    manifest.lanes[0].active = true;
    manifest.lanes[1].active = true;
    (void)qreplay::setReplayTextV2(
        manifest.lanes[0].name, "Viewer P1");
    (void)qreplay::setReplayTextV2(
        manifest.lanes[1].name, "Viewer P2");
    manifest.slots[0].ownership =
        qreplay::SlotOwnership::RecordedHuman;
    manifest.slots[0].side = 0u;
    manifest.slots[0].identity.nativeCharacterId = 0u;
    manifest.slots[0].identity.paletteIndex = 1u;
    manifest.slots[1].ownership =
        qreplay::SlotOwnership::RecordedHuman;
    manifest.slots[1].side = 1u;
    manifest.slots[1].identity.nativeCharacterId = 8u;
    manifest.slots[1].identity.paletteIndex = 2u;
    manifest.slots[2].side = 0u;
    manifest.slots[3].side = 1u;
    manifest.routeEpoch = generation;
    manifest.rosterEpoch = generation;
    manifest.layoutGeneration = generation;
    manifest.stage.nativeStageId = 4u;
    manifest.stage.verified = true;
    manifest.stage.catalogEntryId = 0x5354414745000004ull;
    manifest.display.recordedUnixSeconds = 1785456000 + generation;
    manifest.display.sourceKind =
        qreplay::ReplaySourceKindV2::NetplayPlayer;
    (void)qreplay::setReplayTextV2(
        manifest.display.title, "QSP4 fixture");

    out.start.configuration.timerMode = 2u;
    out.start.configuration.aiDifficulty = 4u;
    out.start.configuration.lifeMultiplier = 1u;
    out.start.configuration.roundsToWinMinusOne = 1u;
    out.start.rng.gameplay = 0x10000000u + generation;
    out.start.rng.cosmetic = 0x20000000u + generation;
    out.start.rng.selector = 0x30000000u + generation;
    out.start.initialPhase = 0u;
    out.start.bootstrapFingerprint =
        0xF000000000000000ull + generation;
    out.start.frameZeroPreStateHash =
        0x1000000000000000ull
        + static_cast<uint64_t>(generation) * 0x1000ull;
    out.start.initialRollingDigest =
        qreplay::initialReplayDigestV2(out);
    return out;
}

inline qreplay::ConfirmedGameEventV2 event(
    qreplay::GameEventKindV2 kind,
    qreplay::GameEventPhaseV2 phase) {
    qreplay::ConfirmedGameEventV2 out{};
    out.kind = kind;
    out.phase = phase;
    out.winnerSlot = -1;
    if (kind == qreplay::GameEventKindV2::RoundStarted) {
        out.transitionSiteId = 0x101u;
        out.roundOrdinal = 1u;
    } else if (kind == qreplay::GameEventKindV2::RoundEnded
               || kind == qreplay::GameEventKindV2::GameResult) {
        out.transitionSiteId =
            kind == qreplay::GameEventKindV2::GameResult
            ? 0x303u : 0x202u;
        out.roundOrdinal = 2u;
        out.winningSideMask = 0x01u;
        out.roundTallies = {{2u, 0u}};
        out.winnerSlot = 0;
    }
    return out;
}

inline ConnectedConfirmedRecordV4 record(
    const qreplay::GameIdentityV2& game, uint32_t generation,
    uint32_t index, bool first, bool final) {
    ConnectedConfirmedRecordV4 out{};
    out.hasReplayGameIdentity = true;
    out.game = game;
    out.sourceTimelineFrame = generation * 1000u + index;
    out.frame.inputDomain = qreplay::InputDomain::LogicalSourceV1;
    out.frame.activeLaneMask = 0x03u;
    out.frame.actions[0] =
        static_cast<uint16_t>(0x0101u + (index & 2u));
    out.frame.actions[1] =
        static_cast<uint16_t>(0x0202u + (index & 1u));
    out.frame.before.scene = 2u;
    out.frame.before.requestedScene = 2u;
    out.frame.before.matchSub = static_cast<int32_t>(index);
    out.frame.before.winnerSlot = -1;
    out.frame.before.routeEpoch = generation;
    out.frame.before.rosterEpoch = generation;
    out.frame.before.layoutGeneration = generation;
    out.frame.after = out.frame.before;
    out.frame.after.matchSub = static_cast<int32_t>(index + 1u);
    out.frame.preStateHash =
        0x1000000000000000ull
        + static_cast<uint64_t>(generation) * 0x1000ull + index;
    out.frame.postStateHash = out.frame.preStateHash + 1u;
    if (first) {
        out.preEvents.count = 2u;
        out.preEvents.events[0] = event(
            qreplay::GameEventKindV2::GameStarted,
            qreplay::GameEventPhaseV2::PreFrame);
        out.preEvents.events[1] = event(
            qreplay::GameEventKindV2::RoundStarted,
            qreplay::GameEventPhaseV2::PreFrame);
    }
    if (final) {
        out.frame.after.resultCounter = 1u;
        out.frame.after.roundTallies = {{2u, 0u}};
        out.frame.after.winnerSlot = 0;
        out.postEvents.count = 2u;
        out.postEvents.events[0] = event(
            qreplay::GameEventKindV2::RoundEnded,
            qreplay::GameEventPhaseV2::PostFrame);
        out.postEvents.events[1] = event(
            qreplay::GameEventKindV2::GameResult,
            qreplay::GameEventPhaseV2::PostFrame);
    }
    return out;
}

inline bool fold(
    const ConnectedConfirmedRecordV4& record, uint32_t startCursor,
    uint32_t& eventSequence, uint64_t& rolling,
    qreplay::CanonicalGameDigestV2& canonical) {
    qreplay::CanonicalTickBundleV2 tick{};
    if (!rebaseConnectedRecordV4(
            record, startCursor, eventSequence, rolling, tick)) {
        return false;
    }
    for (size_t i = 0; i < tick.preEvents.count; ++i)
        if (!canonical.append(tick.preEvents.events[i])) return false;
    if (!canonical.append(tick.frame)) return false;
    for (size_t i = 0; i < tick.postEvents.count; ++i)
        if (!canonical.append(tick.postEvents.events[i])) return false;
    return true;
}

inline bool appendCompletedGame(
    ConnectedChainBuilderV4& builder, uint32_t generation,
    std::vector<ConnectedPublicationV4>& out) {
    const qreplay::ConfirmedGameStartV2 gameStart = start(generation);
    const uint32_t startCursor = builder.nextRecordCursor();
    ConnectedPublicationV4 begin{};
    begin.kind = ConnectedPublicationKindV4::BeginGameBundle;
    begin.begin.start.game = gameStart.manifest.identity;
    begin.begin.start.start = gameStart;
    begin.begin.frameZero = record(
        gameStart.manifest.identity, generation, 0u, true, false);
    if (!builder.stamp(begin)) return false;

    qreplay::CanonicalGameDigestV2 canonical;
    if (!canonical.begin(gameStart)) return false;
    uint32_t eventSequence = 0u;
    uint64_t rolling = gameStart.start.initialRollingDigest;
    if (!fold(
            begin.begin.frameZero, startCursor, eventSequence, rolling,
            canonical)) {
        return false;
    }
    out.push_back(begin);

    ConnectedPublicationV4 middle{};
    middle.kind = ConnectedPublicationKindV4::ConfirmedRecord;
    middle.record = record(
        gameStart.manifest.identity, generation, 1u, false, false);
    if (!builder.stamp(middle)
        || !fold(
            middle.record, startCursor, eventSequence, rolling,
            canonical)) {
        return false;
    }
    out.push_back(middle);

    ConnectedPublicationV4 finish{};
    finish.kind = ConnectedPublicationKindV4::EndGameBundle;
    finish.end.containsFinalRecord = true;
    finish.end.finalRecord = record(
        gameStart.manifest.identity, generation, 2u, false, true);
    finish.end.finalRecord.chain.recordCursorBefore =
        builder.nextRecordCursor();
    if (!fold(
            finish.end.finalRecord, startCursor, eventSequence, rolling,
            canonical)) {
        return false;
    }
    finish.end.end.game = gameStart.manifest.identity;
    finish.end.end.setNextRecordCursor =
        builder.nextRecordCursor() + 1u;
    finish.end.end.end.reason = qreplay::TerminalReason::Completed;
    finish.end.end.end.nextRecord = 3u;
    finish.end.end.end.winningSideMask = 0x01u;
    finish.end.end.end.roundTallies = {{2u, 0u}};
    finish.end.end.end.winnerSlot = 0;
    finish.end.end.end.postTailStateHash =
        finish.end.finalRecord.frame.postStateHash;
    finish.end.end.end.finalRollingDigest = rolling;
    if (!canonical.finish(
            finish.end.end.end,
            finish.end.end.end.canonicalGameDigest)
        || !builder.stamp(finish)) {
        return false;
    }
    out.push_back(finish);
    return true;
}

inline ConnectedPublicationV4 setEvent(
    ConnectedChainBuilderV4& builder, uint32_t matchId) {
    ConnectedPublicationV4 publication{};
    publication.kind = ConnectedPublicationKindV4::SetEvent;
    publication.setEvent.event.kind = SetEventKind::MatchStarted;
    publication.setEvent.event.matchId = matchId;
    publication.setEvent.event.valueC = matchId;
    (void)builder.stamp(publication);
    return publication;
}

inline ConnectedPublicationV4 seal(
    ConnectedChainBuilderV4& builder, uint64_t postTailHash) {
    ConnectedPublicationV4 publication{};
    publication.kind = ConnectedPublicationKindV4::SetSeal;
    publication.seal.reason =
        ConnectedSetTerminalReasonV4::SourceStopped;
    publication.seal.postTailStateHash = postTailHash;
    (void)builder.stamp(publication);
    return publication;
}

}
