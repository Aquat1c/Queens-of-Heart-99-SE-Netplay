#include "TestHarness.h"
#include "replay/ReplaySimulationContractV2.h"

#include "ReplayTestFixtures.h"

#include "client/replay/ReplayBrowserModel.h"

#include "netplay/MatchConfig.h"
#include "platform/WidePath.h"
#include "replay/CharacterCatalog.h"
#include "replay/ReplayIntegrity.h"
#include "replay/StageCatalog.h"

#include <memory>
#include <string>
#include <vector>

using namespace qreplay;
using namespace qclient::replay;
using namespace qclient::replay::test;

namespace {

RuntimeIdentity matchingRuntime(const FixtureOptions& options) {
    const SimulationContract contract = makeContract(options);
    RuntimeIdentity runtime{};
    runtime.known = true;
    runtime.executableSha256 = contract.executableSha256;
    runtime.runtimeSimulationId = contract.runtimeSimulationId;
    runtime.stateRegionFingerprint = contract.stateRegionFingerprint;
    runtime.stateHashProjectionId = contract.stateHashProjectionId;
    runtime.stateHashAlgorithmId = contract.stateHashAlgorithmId;
    runtime.bootstrapCodecVersion = contract.bootstrapCodecVersion;
    runtime.eventSchemaVersion = contract.eventSchemaVersion;
    runtime.contentManifestRoot = contract.contentManifestRoot;
    runtime.characterCatalogId = contract.characterCatalogId;
    runtime.fpuControlWord = contract.fpuControlWord;
    runtime.mxcsr = contract.mxcsr;
    runtime.supportsFourPlayer = true;
    runtime.supportsCpuFighters = true;
    runtime.supportsPortableCheckpoint = true;
    runtime.supportsEffectiveFighterDomain = true;
    return runtime;
}

std::shared_ptr<const ReplaySnapshot> scan(const std::wstring& root) {
    ReplayScanner::Config config{};
    config.roots.push_back(root);
    config.useCache = false;
    return std::make_shared<const ReplaySnapshot>(
        ReplayScanner::scanOnce(config, nullptr, nullptr, 1u));
}

bool writeFixture(const std::wstring& path, const FixtureOptions& options) {
    const Fixture fixture = makeFixture(options);
    return fixture.ok && writeBytes(path, fixture.bytes);
}

}

QNP_TEST(replay_browser_roster_shows_resolved_identities) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();
    std::array<FighterSlotDescriptor, kFighterSlots> slots{};
    const uint8_t ids[kFighterSlots] = { 8u, 24u, 30u, 21u };
    for (size_t i = 0; i < kFighterSlots; ++i) {
        slots[i].ownership = SlotOwnership::RecordedHuman;
        slots[i].side = static_cast<uint8_t>(i % 2u);
        slots[i].identity.nativeCharacterId = ids[i];
        slots[i].identity.paletteIndex = static_cast<uint8_t>(i);
    }
    const std::array<uint8_t, kFighterSlots> route = { 0u, 1u, 2u, 3u };
    std::array<SourceLaneDescriptor, kSourceLanes> lanes{};
    for (size_t i = 0; i < kSourceLanes; ++i) {
        lanes[i].active = true;
        lanes[i].name = "P" + std::to_string(i + 1u);
    }
    const std::vector<RosterParticipant> roster =
        ReplayBrowserModel::buildRoster(slots, route, lanes);
    QNP_REQUIRE(roster.size() == 4u);

    QNP_CHECK(roster[0].displayName == catalog.displayName(8u));
    QNP_CHECK(roster[1].displayName == catalog.displayName(24u));
    QNP_CHECK(roster[1].displayName != catalog.displayName(1u));
    QNP_CHECK(roster[2].hiddenColumn);
    QNP_CHECK(!roster[2].unknownId);
    QNP_CHECK(roster[2].displayName == catalog.displayName(30u));
    QNP_CHECK(roster[3].standAlternate);
    QNP_CHECK(roster[3].displayName != catalog.displayName(8u));

    QNP_CHECK(roster[0].side == 0u && roster[2].side == 0u);
    QNP_CHECK(roster[1].side == 1u && roster[3].side == 1u);
}

QNP_TEST(replay_browser_roster_never_lists_internal_form_ids) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();
    std::array<FighterSlotDescriptor, kFighterSlots> slots{};
    const uint8_t ids[kFighterSlots] = { 8u, 34u, 35u, 36u };
    for (size_t i = 0; i < kFighterSlots; ++i) {
        slots[i].ownership = SlotOwnership::RecordedHuman;
        slots[i].identity.nativeCharacterId = ids[i];
    }
    const std::array<uint8_t, kFighterSlots> route = { 0u, 1u, 2u, 3u };
    std::array<SourceLaneDescriptor, kSourceLanes> lanes{};
    const std::vector<RosterParticipant> roster =
        ReplayBrowserModel::buildRoster(slots, route, lanes);
    QNP_CHECK(roster.size() == 1u);
    QNP_CHECK(roster[0].characterId == 8u);
    QNP_CHECK(catalog.isInternalFormId(34u));
    QNP_CHECK(catalog.isInternalFormId(35u));
    QNP_CHECK(catalog.isInternalFormId(36u));
}

QNP_TEST(replay_browser_roster_renders_an_uncatalogued_id_honestly) {
    std::array<FighterSlotDescriptor, kFighterSlots> slots{};
    slots[0].ownership = SlotOwnership::RecordedHuman;
    slots[0].identity.nativeCharacterId = 200u;
    const std::array<uint8_t, kFighterSlots> route = { 0u, 1u, 2u, 3u };
    std::array<SourceLaneDescriptor, kSourceLanes> lanes{};
    const std::vector<RosterParticipant> roster =
        ReplayBrowserModel::buildRoster(slots, route, lanes);
    QNP_REQUIRE(roster.size() == 1u);
    QNP_CHECK(roster[0].unknownId);
    QNP_CHECK(roster[0].displayName == "Unknown character #200");
}

QNP_TEST(replay_browser_roster_flags_a_selector_proof_that_does_not_resolve) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();
    std::array<FighterSlotDescriptor, kFighterSlots> slots{};
    slots[0].ownership = SlotOwnership::RecordedHuman;
    slots[0].identity.nativeCharacterId = 8u;
    slots[0].selectorProofPresent = true;
    slots[0].selectorProof.cursorCell = catalog.cellForBaseId(8u);
    slots[0].selectorProof.alternateFlag = 0u;
    slots[0].selectorProof.confirmState = 4u;

    slots[1].ownership = SlotOwnership::RecordedHuman;
    slots[1].identity.nativeCharacterId = 8u;
    slots[1].selectorProofPresent = true;
    slots[1].selectorProof.cursorCell = catalog.cellForBaseId(1u);
    slots[1].selectorProof.alternateFlag = 0u;
    slots[1].selectorProof.confirmState = 4u;

    const std::array<uint8_t, kFighterSlots> route = { 0u, 1u, 2u, 3u };
    std::array<SourceLaneDescriptor, kSourceLanes> lanes{};
    const std::vector<RosterParticipant> roster =
        ReplayBrowserModel::buildRoster(slots, route, lanes);
    QNP_REQUIRE(roster.size() == 2u);
    QNP_CHECK(roster[0].selectorProofVerified);
    QNP_CHECK(!roster[1].selectorProofVerified);
}

QNP_TEST(replay_browser_builds_rows_with_honest_status) {
    TempDir dir(L"br_rows");
    QNP_REQUIRE(dir.valid());

    FixtureOptions good{};
    good.uuidLow = 1u;
    good.title = "Complete set";
    QNP_REQUIRE(writeFixture(dir.file(L"good.q99r"), good));

    FixtureOptions incomplete{};
    incomplete.uuidLow = 2u;
    incomplete.finalize = false;
    incomplete.title = "Interrupted set";
    QNP_REQUIRE(writeFixture(dir.file(L"partial.q99r.part"), incomplete));

    FixtureOptions corruptOptions{};
    corruptOptions.uuidLow = 3u;
    Fixture corrupt = makeFixture(corruptOptions);
    QNP_REQUIRE(corrupt.ok);
    for (size_t i = kPrefixBytes + 60u; i < corrupt.bytes.size(); i += 5u)
        corrupt.bytes[i] ^= 0x33u;
    QNP_REQUIRE(writeBytes(dir.file(L"corrupt.q99r"), corrupt.bytes));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(good));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 2u);

    bool sawExact = false;
    bool sawRecoverable = false;
    bool sawCorrupt = false;
    for (size_t i = 0; i < model.rowCount(); ++i) {
        const BrowserRow& row = model.row(i);
        QNP_CHECK(!row.statusBadge.empty());
        QNP_CHECK(!row.dateText.empty());
        if (row.state == CompatibilityState::Exact) {
            sawExact = true;
            QNP_CHECK(row.watchable);
            QNP_CHECK(row.scoreText == "...");
        }
        if (row.state == CompatibilityState::Recoverable) sawRecoverable = true;
        if (row.state == CompatibilityState::Corrupt) {
            sawCorrupt = true;
            QNP_CHECK(!row.watchable);
        }
        for (char ch : row.titleText + row.matchupText + row.statusDetail) {
            const unsigned char byte = static_cast<unsigned char>(ch);
            QNP_CHECK(byte >= 0x20u && byte != 0x7Fu);
        }
    }
    QNP_CHECK(sawExact);
    QNP_CHECK(!sawRecoverable);
    QNP_CHECK(sawCorrupt);

    const CompatibilityState states[] = {
        CompatibilityState::Exact, CompatibilityState::Compatible,
        CompatibilityState::FileIntact, CompatibilityState::SimulationVerified,
        CompatibilityState::Recording, CompatibilityState::Incomplete,
        CompatibilityState::Recoverable, CompatibilityState::ContentMismatch,
        CompatibilityState::Unsupported, CompatibilityState::Corrupt,
    };
    for (size_t i = 0; i < 10u; ++i) {
        const std::string badge = ReplayBrowserModel::badgeTextFor(states[i]);
        QNP_CHECK(!badge.empty());
        for (size_t j = 0; j < i; ++j)
            QNP_CHECK(badge != ReplayBrowserModel::badgeTextFor(states[j]));
    }
}

QNP_TEST(replay_browser_details_expose_every_participant) {
    TempDir dir(L"br_details");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    options.fourPlayer = true;
    options.matchCount = 2u;
    options.title = "Team set";
    QNP_REQUIRE(writeFixture(dir.file(L"team.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_REQUIRE(model.select(0u));

    const BrowserDetails& details = model.details();
    QNP_CHECK(details.valid);
    QNP_CHECK(details.participants.size() == 4u);
    QNP_CHECK(details.matches.size() == 2u);
    for (const BrowserMatchRow& match : details.matches)
        QNP_CHECK(match.participants.size() == 4u);
    QNP_CHECK(!details.routeText.empty());
    QNP_CHECK(details.identityLines.size() >= 3u);
    QNP_CHECK(!details.integrityText.empty());
    QNP_CHECK(details.simulationVerificationText.find("not verified")
              != std::string::npos);
    QNP_CHECK(details.recordedSummaryOnly);
    QNP_CHECK(details.stageText.find("not verified") != std::string::npos);
}

QNP_TEST(replay_browser_defaults_to_game_order) {
    ReplayBrowserModel model;
    QNP_CHECK(model.sort() == SortKey::GameOrder);
    QNP_CHECK(model.captureViewState().sort == SortKey::GameOrder);
    QNP_CHECK(BrowserViewState{}.sort == SortKey::GameOrder);
    QNP_CHECK(QueryInputs{}.sort == SortKey::GameOrder);
}

QNP_TEST(replay_browser_preserves_view_state_across_playback) {
    TempDir dir(L"br_state");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    for (int i = 0; i < 5; ++i) {
        wchar_t name[32];
        _snwprintf_s(name, 32, _TRUNCATE, L"s%02d.q99r", i);
        options.uuidLow = static_cast<uint64_t>(10 + i);
        options.recordedUnixSeconds = 1767225600 + i * 86400;
        QNP_REQUIRE(writeFixture(dir.file(name), options));
    }

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 5u);

    model.setQuery("fixture");
    model.setSort(SortKey::Oldest);
    model.filter().fighterCount = 2u;
    model.refreshView();
    QNP_REQUIRE(model.select(3u));
    model.setScrollTop(2u);
    model.expandMatch(1u);
    const BrowserViewState saved = model.captureViewState();
    const uint64_t selectedId = model.selectedStableId();

    ReplayBrowserModel other;
    other.setAnnotations(&annotations);
    other.setReplayRoot(dir.path);
    other.setRuntimeIdentity(matchingRuntime(options));
    other.setSnapshot(scan(dir.path));
    other.restoreViewState(saved);

    QNP_CHECK(other.query() == "fixture");
    QNP_CHECK(other.sort() == SortKey::Oldest);
    QNP_CHECK(other.filter().fighterCount == 2u);
    QNP_CHECK(other.scrollTop() == 2u);
    QNP_CHECK(other.expandedMatchOrdinal() == 1u);
    QNP_CHECK(other.selectedStableId() == selectedId);
    QNP_CHECK(other.details().valid);
}

QNP_TEST(replay_browser_reports_empty_and_filtered_states) {
    TempDir dir(L"br_empty");
    QNP_REQUIRE(dir.valid());

    ReplayBrowserModel model;
    QNP_CHECK(!model.emptyStateText().empty());
    model.setReplayRoot(dir.path);
    model.setSnapshot(scan(dir.path));
    QNP_CHECK(model.empty());
    QNP_CHECK(model.emptyStateText().find("No replays yet") != std::string::npos);

    FixtureOptions options{};
    QNP_REQUIRE(writeFixture(dir.file(L"one.q99r"), options));
    model.setSnapshot(scan(dir.path));
    QNP_CHECK(model.rowCount() == 1u);
    model.setQuery("definitely-not-present");
    QNP_CHECK(model.empty());
    QNP_CHECK(model.emptyStateText().find("match the current search")
              != std::string::npos);

    QNP_CHECK(model.actionState(BrowserAction::ClearFilters).enabled);
    ActionRequest request{};
    request.action = BrowserAction::ClearFilters;
    QNP_CHECK(model.requestAction(request).ok);
    QNP_CHECK(model.rowCount() == 1u);
    QNP_CHECK(!model.actionState(BrowserAction::ClearFilters).enabled);
    QNP_CHECK(model.actionState(BrowserAction::ClearFilters).reason
              == ActionDisableReason::NoFilterActive);
}

QNP_TEST(replay_browser_action_states_have_typed_reasons) {
    TempDir dir(L"br_actions");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    QNP_REQUIRE(writeFixture(dir.file(L"good.q99r"), options));

    ReplayBrowserModel model;
    for (uint8_t i = 0; i < static_cast<uint8_t>(BrowserAction::Count); ++i) {
        const BrowserAction action = static_cast<BrowserAction>(i);
        if (action == BrowserAction::Refresh
            || action == BrowserAction::ImportExternal) {
            continue;
        }
        const ActionState state = model.actionState(action);
        if (!state.enabled) QNP_CHECK(!std::string(
            actionDisableReasonText(state.reason)).empty()
            || state.reason == ActionDisableReason::None);
    }

    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setSnapshot(scan(dir.path));

    RuntimeIdentity unknown{};
    model.setRuntimeIdentity(unknown);
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_REQUIRE(model.select(0u));
    QNP_CHECK(model.row(0u).state == CompatibilityState::FileIntact);
    QNP_CHECK(!model.actionState(BrowserAction::WatchSet).enabled);
    QNP_CHECK(model.actionState(BrowserAction::WatchSet).reason
              == ActionDisableReason::NoGameConfigured);
    QNP_CHECK(model.actionState(BrowserAction::VerifyFile).enabled);

    model.setRuntimeIdentity(matchingRuntime(options));
    QNP_CHECK(model.actionState(BrowserAction::WatchSet).enabled);
    QNP_CHECK(model.actionState(BrowserAction::WatchMatch).enabled);

    const BrowserAction destructive[] = { BrowserAction::MoveToTrash,
                                          BrowserAction::PermanentPurge,
                                          BrowserAction::Rename };
    for (BrowserAction action : destructive) {
        const ActionState state = model.actionState(action);
        QNP_CHECK(state.destructive);
        QNP_CHECK(state.requiresConfirmation);
    }
    QNP_CHECK(!model.actionState(BrowserAction::ToggleFavorite).destructive);
    QNP_CHECK(!model.actionState(BrowserAction::VerifyFile).destructive);
}

QNP_TEST(replay_browser_watch_request_carries_no_metadata_path) {
    TempDir dir(L"br_watch");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    options.matchCount = 3u;
    options.title = "set";
    QNP_REQUIRE(writeFixture(dir.file(L"watch.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_REQUIRE(model.select(0u));

    ActionRequest request{};
    request.action = BrowserAction::WatchMatch;
    request.matchOrdinal = 2u;
    const ActionOutcome outcome = model.requestAction(request);
    QNP_CHECK(outcome.ok);
    QNP_CHECK(outcome.watch.valid);
    QNP_CHECK(outcome.watch.matchOrdinal == 2u);
    QNP_CHECK(outcome.watch.matchId == 2u);
    QNP_CHECK(outcome.watch.compatibility.watchable);
    QNP_CHECK(outcome.watch.path.compare(0, dir.path.size(), dir.path) == 0);
    QNP_CHECK(outcome.watch.identity.known);

    request.action = BrowserAction::WatchSet;
    const ActionOutcome whole = model.requestAction(request);
    QNP_CHECK(whole.ok);
    QNP_CHECK(whole.watch.matchId == 0u);
}

QNP_TEST(replay_browser_never_destroys_without_an_explicit_confirmation) {
    TempDir dir(L"br_trash");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    QNP_REQUIRE(writeFixture(dir.file(L"doomed.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_REQUIRE(model.select(0u));

    ActionRequest request{};
    request.action = BrowserAction::MoveToTrash;
    const ActionOutcome armed = model.requestAction(request);
    QNP_CHECK(!armed.ok);
    QNP_CHECK(armed.needsConfirmation);
    QNP_CHECK(armed.confirmationToken != 0u);
    QNP_CHECK(armed.confirmationTargetName == "doomed.q99r");
    QNP_CHECK(armed.confirmationPrompt.find("doomed.q99r") != std::string::npos);
    QNP_CHECK(qplat::fileExistsW(dir.file(L"doomed.q99r")));

    QNP_CHECK(!model.confirmAction(armed.confirmationToken + 1u).ok);
    QNP_CHECK(qplat::fileExistsW(dir.file(L"doomed.q99r")));
    QNP_CHECK(!model.confirmAction(0u).ok);

    const ActionOutcome done = model.confirmAction(armed.confirmationToken);
    QNP_CHECK(done.ok);
    QNP_CHECK(!qplat::fileExistsW(dir.file(L"doomed.q99r")));
    QNP_CHECK(qplat::fileExistsW(done.producedPath));
    QNP_CHECK(done.producedPath.find(L".trash") != std::wstring::npos);

    QNP_CHECK(!model.confirmAction(armed.confirmationToken).ok);

    QNP_CHECK(model.actionState(BrowserAction::UndoTrash).enabled);
    ActionRequest undo{};
    undo.action = BrowserAction::UndoTrash;
    const ActionOutcome restored = model.requestAction(undo);
    QNP_CHECK(restored.ok);
    QNP_CHECK(qplat::fileExistsW(dir.file(L"doomed.q99r")));
    QNP_CHECK(!model.actionState(BrowserAction::UndoTrash).enabled);
}

QNP_TEST(replay_browser_refuses_an_action_when_the_file_changed) {
    TempDir dir(L"br_changed");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    QNP_REQUIRE(writeFixture(dir.file(L"moving.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_REQUIRE(model.select(0u));

    FixtureOptions other{};
    other.uuidLow = 4242u;
    other.matchCount = 2u;
    QNP_REQUIRE(writeFixture(dir.file(L"moving.q99r"), other));

    ActionRequest request{};
    request.action = BrowserAction::VerifyFile;
    const ActionOutcome outcome = model.requestAction(request);
    QNP_CHECK(!outcome.ok);
    QNP_CHECK(outcome.reason == ActionDisableReason::FileChangedSinceScan);

    QNP_REQUIRE(::DeleteFileW(dir.file(L"moving.q99r").c_str()) != 0);
    const ActionOutcome gone = model.requestAction(request);
    QNP_CHECK(!gone.ok);
    QNP_CHECK(gone.reason == ActionDisableReason::FileMissing);
}

QNP_TEST(replay_browser_rename_refuses_a_collision_and_keeps_the_recorded_title) {
    TempDir dir(L"br_rename");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    options.title = "Recorded original";
    QNP_REQUIRE(writeFixture(dir.file(L"first.q99r"), options));
    FixtureOptions second{};
    second.uuidLow = 77u;
    QNP_REQUIRE(writeFixture(dir.file(L"taken.q99r"), second));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 2u);
    size_t index = 0u;
    for (size_t i = 0; i < model.rowCount(); ++i) {
        if (model.row(i).fileNameText == "first.q99r") index = i;
    }
    QNP_REQUIRE(model.select(index));

    ActionRequest request{};
    request.action = BrowserAction::Rename;
    request.text = "taken";
    ActionOutcome armed = model.requestAction(request);
    QNP_REQUIRE(armed.needsConfirmation);
    const ActionOutcome collision = model.confirmAction(armed.confirmationToken);
    QNP_CHECK(!collision.ok);
    QNP_CHECK(collision.message.find("already exists") != std::string::npos);
    QNP_CHECK(qplat::fileExistsW(dir.file(L"taken.q99r")));
    QNP_CHECK(qplat::fileExistsW(dir.file(L"first.q99r")));

    request.text = "Grand Final!! <script>";
    armed = model.requestAction(request);
    QNP_REQUIRE(armed.needsConfirmation);
    const ActionOutcome renamed = model.confirmAction(armed.confirmationToken);
    QNP_CHECK(renamed.ok);
    QNP_CHECK(!qplat::fileExistsW(dir.file(L"first.q99r")));
    QNP_CHECK(renamed.producedPath.find(L"<") == std::wstring::npos);
    QNP_CHECK(renamed.producedPath.find(L"Grand-Final") != std::wstring::npos);

    model.setSnapshot(scan(dir.path));
    for (size_t i = 0; i < model.rowCount(); ++i) {
        const BrowserRow& row = model.row(i);
        if (!row.hasLocalTitle) continue;
        QNP_CHECK(row.titleText.find("Grand Final") != std::string::npos);
        model.select(i);
        QNP_CHECK(model.details().recordedTitleText == "Recorded original");
    }
}

QNP_TEST(replay_browser_trashed_replay_stays_visible_and_restorable) {
    TempDir dir(L"br_trash_roundtrip");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    QNP_REQUIRE(writeFixture(dir.file(L"keeper.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.select(0u));

    ActionRequest trash{};
    trash.action = BrowserAction::MoveToTrash;
    ActionOutcome trashed = model.requestAction(trash);
    if (trashed.needsConfirmation)
        trashed = model.confirmAction(trashed.confirmationToken);
    QNP_REQUIRE(trashed.ok);
    QNP_CHECK(!qplat::fileExistsW(dir.file(L"keeper.q99r")));
    QNP_REQUIRE(qplat::fileExistsW(trashed.producedPath));

    auto scanWithTrash = [&dir](uint64_t generation) {
        ReplayScanner::Config config{};
        config.roots.push_back(dir.path);
        config.useCache = false;
        config.includeTrash = true;
        return std::make_shared<const ReplaySnapshot>(
            ReplayScanner::scanOnce(config, nullptr, nullptr, generation));
    };

    ReplayBrowserModel later;
    ReplayAnnotationStore laterAnnotations;
    later.setAnnotations(&laterAnnotations);
    later.setReplayRoot(dir.path);
    later.setRuntimeIdentity(matchingRuntime(options));
    later.setSnapshot(scanWithTrash(2u));

    size_t trashedRow = later.rowCount();
    for (size_t i = 0; i < later.rowCount(); ++i) {
        if (later.row(i).inTrash) { trashedRow = i; break; }
    }
    QNP_REQUIRE(trashedRow < later.rowCount());
    QNP_CHECK(later.row(trashedRow).fileNameText == "keeper.q99r");
    QNP_REQUIRE(later.select(trashedRow));

    const ActionState watchState =
        later.actionState(BrowserAction::WatchSet);
    QNP_CHECK(!watchState.enabled);
    QNP_CHECK(watchState.reason == ActionDisableReason::InTrash);

    QNP_CHECK(later.actionState(BrowserAction::UndoTrash).enabled);
    ActionRequest undo{};
    undo.action = BrowserAction::UndoTrash;
    ActionOutcome restored = later.requestAction(undo);
    if (restored.needsConfirmation)
        restored = later.confirmAction(restored.confirmationToken);
    QNP_CHECK(restored.ok);
    QNP_CHECK(qplat::fileExistsW(dir.file(L"keeper.q99r")));
    QNP_CHECK(!qplat::fileExistsW(trashed.producedPath));
}

QNP_TEST(replay_browser_purge_is_a_separate_confirmed_action) {
    TempDir dir(L"br_purge");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    QNP_REQUIRE(writeFixture(dir.file(L"gone.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.select(0u));

    ActionRequest request{};
    request.action = BrowserAction::PermanentPurge;

    const ActionState libraryState =
        model.actionState(BrowserAction::PermanentPurge);
    QNP_CHECK(!libraryState.enabled);
    QNP_CHECK(libraryState.reason == ActionDisableReason::NotInTrash);
    const ActionOutcome refused = model.requestAction(request);
    QNP_CHECK(!refused.ok);
    QNP_CHECK(!refused.needsConfirmation);
    QNP_CHECK(qplat::fileExistsW(dir.file(L"gone.q99r")));

    const std::wstring trashDir = dir.path + L"\\.trash";
    QNP_REQUIRE(qplat::ensureDirW(trashDir));
    const std::wstring trashedPath = trashDir + L"\\gone.q99r";
    QNP_REQUIRE(writeFixture(trashedPath, options));

    ReplayScanner::Config withTrash{};
    withTrash.roots.push_back(dir.path);
    withTrash.useCache = false;
    withTrash.includeTrash = true;
    model.setSnapshot(std::make_shared<const ReplaySnapshot>(
        ReplayScanner::scanOnce(withTrash, nullptr, nullptr, 2u)));
    size_t trashedRow = model.rowCount();
    for (size_t i = 0; i < model.rowCount(); ++i) {
        if (model.row(i).inTrash) { trashedRow = i; break; }
    }
    QNP_REQUIRE(trashedRow < model.rowCount());
    QNP_REQUIRE(model.select(trashedRow));

    const ActionOutcome armed = model.requestAction(request);
    QNP_CHECK(armed.needsConfirmation);
    QNP_CHECK(armed.confirmationPrompt.find("cannot be undone")
              != std::string::npos);
    QNP_CHECK(qplat::fileExistsW(trashedPath));

    model.cancelPendingAction();
    QNP_CHECK(!model.confirmAction(armed.confirmationToken).ok);
    QNP_CHECK(qplat::fileExistsW(trashedPath));

    const ActionOutcome again = model.requestAction(request);
    QNP_CHECK(model.confirmAction(again.confirmationToken).ok);
    QNP_CHECK(!qplat::fileExistsW(trashedPath));
}

QNP_TEST(replay_browser_verify_records_integrity_separately) {
    TempDir dir(L"br_verify");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    QNP_REQUIRE(writeFixture(dir.file(L"v.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.select(0u));

    ActionRequest request{};
    request.action = BrowserAction::VerifyFile;
    const ActionOutcome outcome = model.requestAction(request);
    QNP_CHECK(outcome.ok);
    QNP_CHECK(outcome.digestVerified);
    QNP_CHECK(outcome.message.find("File intact") != std::string::npos);

    const ReplayAnnotation* annotation = annotations.find(
        options.uuidLow, options.uuidHigh);
    QNP_REQUIRE(annotation != nullptr);
    QNP_CHECK(annotation->structuralVerified);
    QNP_CHECK(!annotation->simulationVerified);
    QNP_CHECK(model.details().recordedSummaryOnly);
}

QNP_TEST(replay_browser_ignores_legacy_part_files) {
    TempDir dir(L"br_recover");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    options.finalize = false;
    options.matchCount = 2u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);
    const std::wstring partPath = dir.file(L"broken.q99r.part");
    QNP_REQUIRE(writeBytes(partPath, fixture.bytes));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_CHECK(model.rowCount() == 0u);
    QNP_CHECK(qplat::fileExistsW(partPath));
}

QNP_TEST(replay_browser_exports_one_match_with_provenance) {
    TempDir dir(L"br_export");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    options.matchCount = 3u;
    options.framesPerMatch = 6u;
    QNP_REQUIRE(writeFixture(dir.file(L"set.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.select(0u));
    QNP_CHECK(model.actionState(BrowserAction::ExportMatch).enabled);

    ActionRequest request{};
    request.action = BrowserAction::ExportMatch;
    request.matchOrdinal = 2u;
    const ActionOutcome outcome = model.requestAction(request);
    QNP_CHECK(outcome.ok);
    QNP_REQUIRE(!outcome.producedPath.empty());

    ParseOptions parseOptions{};
    parseOptions.mode = ParseMode::Verify;
    ParsedReplay parsed;
    QNP_CHECK(ReplayParser::parseFile(outcome.producedPath, parseOptions,
                                      parsed).ok());
    QNP_CHECK(parsed.hasFooter);
    QNP_CHECK(parsed.matches.size() == 1u);
    QNP_CHECK(parsed.matches[0].independentBootstrap);
    QNP_CHECK(parsed.totalFrames == 6u);
    QNP_CHECK(parsed.file.prefix.sessionUuidLow != options.uuidLow);
    const ReplayAnnotation* annotation = annotations.find(
        parsed.file.prefix.sessionUuidLow, parsed.file.prefix.sessionUuidHigh);
    QNP_REQUIRE(annotation != nullptr);
    QNP_CHECK(annotation->hasProvenance);
    QNP_CHECK(annotation->sourceSessionUuidLow == options.uuidLow);
}

QNP_TEST(replay_browser_opens_an_external_file_read_only) {
    TempDir library(L"br_lib");
    TempDir outside(L"br_outside");
    QNP_REQUIRE(library.valid() && outside.valid());
    FixtureOptions options{};
    QNP_REQUIRE(writeFixture(outside.file(L"external.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(library.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(library.path));
    QNP_CHECK(model.rowCount() == 0u);

    ReplayEntry entry{};
    CompatibilityResult compatibility{};
    std::string message;
    QNP_CHECK(model.openExternal(outside.file(L"external.q99r"), entry,
                                 compatibility, message));
    QNP_CHECK(entry.parsed);
    QNP_CHECK(compatibility.state == CompatibilityState::Exact);
    QNP_CHECK(model.rowCount() == 0u);

    QNP_CHECK(!model.openExternal(library.path + L"\\..\\x.q99r", entry,
                                  compatibility, message));
    QNP_CHECK(message.find("not acceptable") != std::string::npos);

    QNP_REQUIRE(writeFixture(outside.file(L"legacy.q99r.part"), options));
    QNP_CHECK(!model.openExternal(outside.file(L"legacy.q99r.part"), entry,
                                  compatibility, message));
    QNP_CHECK(message.find("finalized .q99r") != std::string::npos);
}

QNP_TEST(replay_browser_reveal_returns_a_folder_not_a_command) {
    TempDir dir(L"br_reveal");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    options.title = "& del /q *";
    QNP_REQUIRE(writeFixture(dir.file(L"r.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.select(0u));

    ActionRequest request{};
    request.action = BrowserAction::RevealFolder;
    const ActionOutcome outcome = model.requestAction(request);
    QNP_CHECK(outcome.ok);
    QNP_CHECK(outcome.producedPath == dir.path);
    QNP_CHECK(outcome.producedPath.find(L"del") == std::wstring::npos);
}

QNP_TEST(replay_browser_favorites_and_notes_never_touch_the_container) {
    TempDir dir(L"br_annot");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    QNP_REQUIRE(writeFixture(dir.file(L"n.q99r"), options));
    FileIdentity before{};
    QNP_REQUIRE(queryFileIdentity(dir.file(L"n.q99r"), before));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.select(0u));

    ActionRequest favorite{};
    favorite.action = BrowserAction::ToggleFavorite;
    QNP_CHECK(model.requestAction(favorite).ok);
    QNP_CHECK(model.row(0u).favorite);
    QNP_CHECK(model.requestAction(favorite).ok);
    QNP_CHECK(!model.row(0u).favorite);

    ActionRequest note{};
    note.action = BrowserAction::EditNote;
    note.text = "good set";
    QNP_CHECK(model.requestAction(note).ok);
    note.text = std::string("\x1b[31mred");
    const ActionOutcome rejected = model.requestAction(note);
    QNP_CHECK(!rejected.ok);
    QNP_CHECK(rejected.message.find("rejected") != std::string::npos);

    ActionRequest tags{};
    tags.action = BrowserAction::EditTags;
    tags.tags = { "tourney", "final" };
    QNP_CHECK(model.requestAction(tags).ok);
    tags.tags = std::vector<std::string>(kMaxTags + 1u, "x");
    QNP_CHECK(!model.requestAction(tags).ok);

    FileIdentity after{};
    QNP_REQUIRE(queryFileIdentity(dir.file(L"n.q99r"), after));
    QNP_CHECK(after.sizeBytes == before.sizeBytes);
    QNP_CHECK(after.modifiedUnixSeconds == before.modifiedUnixSeconds);
}

QNP_TEST(replay_browser_rejects_an_incomplete_recording) {
    TempDir dir(L"br_interrupted");
    QNP_REQUIRE(dir.valid());

    FixtureOptions options{};
    options.uuidLow = 9u;
    options.matchCount = 2u;
    options.finalize = false;
    options.title = "Interrupted set";
    QNP_REQUIRE(writeFixture(dir.file(L"interrupted.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_REQUIRE(model.select(0u));

    const ActionState watch = model.actionState(BrowserAction::WatchSet);
    QNP_CHECK(!watch.enabled);
    QNP_CHECK(watch.reason == ActionDisableReason::Incomplete);

    const ActionState exportMatch =
        model.actionState(BrowserAction::ExportMatch);
    QNP_CHECK(!exportMatch.enabled);
    QNP_CHECK(exportMatch.reason == ActionDisableReason::Incomplete);

    const ActionState startAtRound =
        model.actionState(BrowserAction::StartAtRound);
    QNP_CHECK(!startAtRound.enabled);
    QNP_CHECK(startAtRound.reason == ActionDisableReason::Incomplete);

    const auto& details = model.details();
    QNP_CHECK(details.integrityText.find("Incomplete") != std::string::npos);
}

namespace {

std::array<uint8_t, 32> v2Digest(uint8_t seed) {
    std::array<uint8_t, 32> out{};
    for (size_t i = 0u; i < out.size(); ++i)
        out[i] = static_cast<uint8_t>(seed + i);
    return out;
}

qnetplay::compat::CompatibilityIdentity v2CompatibilityIdentity() {
    qnetplay::compat::CompatibilityIdentity identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = 10u;
    (void)qnetplay::compat::setProfileId(identity, "qoh99-se1126");
    const std::array<uint8_t, 32> executable = v2Digest(0x11u);
    const std::array<uint8_t, 32> simulation = v2Digest(0x61u);
    const std::array<uint8_t, 32> validation = v2Digest(0x91u);
    std::copy(executable.begin(), executable.end(),
              identity.runtimeBuild.executableSha256);
    identity.runtimeBuild.executableSize = 704512u;
    identity.runtimeBuild.peTimestamp = 0x3A19A8A9u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    std::copy(simulation.begin(), simulation.end(),
              identity.simulationContent.simulationRootSha256);
    std::copy(validation.begin(), validation.end(),
              identity.simulationContent.validationRootSha256);
    identity.simulationContent.entryCount = 68u;
    identity.simulationContent.requiredEntryCount = 68u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.stateSchemaId = 0x1010101010101010ull;
    identity.modSimulationId = 0x2020202020202020ull;
    identity.timingProfileId = 0x3030303030303030ull;
    return identity;
}

ReplayEntry sealedV2Entry() {
    ReplayEntry entry{};
    entry.stableId = 0x51993921u;
    entry.path = L"synthetic-v2.q99r";
    entry.fileName = L"synthetic-v2.q99r";
    entry.parsed = true;
    entry.parseStatus = qreplay::ParseStatus::Ok;
    entry.prefix.sessionUuidLow = 0x1020304050607080ull;
    entry.prefix.sessionUuidHigh = 0x8070605040302010ull;
    entry.hasManifest = true;
    entry.hasSessionStart = true;
    entry.hasFooter = true;
    entry.matchCount = 1u;
    entry.totalFrames = 3u;
    entry.isV2 = true;
    entry.classificationV2 = ReplayClassificationV2::Sealed;
    entry.structurallyVerifiedV2 = true;
    entry.fileDigestVerifiedV2 = true;
    entry.eventCountV2 = 4u;

    ConfirmedGameStartV2& game = entry.startV2;
    game.manifest.identity.gameUuidLow = entry.prefix.sessionUuidLow;
    game.manifest.identity.gameUuidHigh = entry.prefix.sessionUuidHigh;
    game.manifest.identity.gameGeneration = 1u;

    SimulationContractV2& contract = game.manifest.contract;
    contract.executableSha256 = v2Digest(1u);
    contract.nativeBuildIdentity = 0x1111111111111111ull;
    contract.runtimeSimulationId = 0x2222222222222222ull;
    contract.stateRegionSchemaFingerprint = 0x3333333333333333ull;
    contract.stateHashProjectionId = 0x4444444444444444ull;
    contract.stateHashAlgorithmId =
        qreplay::kReplayStateHashAlgorithmIdCurrent;
    contract.contentManifestRoot = v2Digest(0x80u);
    contract.bootstrapSchemaId = 0x6666666666666666ull;
    contract.eventSchemaId = 0x7777777777777777ull;
    contract.characterCatalogId =
        CharacterCatalog::forSeBuild().catalogId();
    contract.terminalResultSchemaId = 0x8888888888888888ull;
    contract.compactActionDomainVersion = 1u;

    GameManifestV2& manifest = game.manifest;
    manifest.vsRow = 1u;
    manifest.activeLaneMask = 0x03u;
    manifest.activeFighterMask = 0x03u;
    manifest.slotToSourceRoute = {{0u, 1u, 2u, 3u}};
    manifest.lanes[0].active = true;
    manifest.lanes[1].active = true;
    (void)setReplayTextV2(manifest.lanes[0].name, "P1");
    (void)setReplayTextV2(manifest.lanes[1].name, "P2");
    const CharacterCatalog& characters = CharacterCatalog::forSeBuild();
    for (size_t slot = 0u; slot < kFighterSlots; ++slot)
        manifest.slots[slot].side = static_cast<uint8_t>(slot & 1u);
    for (size_t slot = 0u; slot < 2u; ++slot) {
        const uint8_t cell = static_cast<uint8_t>(slot);
        manifest.slots[slot].ownership = SlotOwnership::RecordedHuman;
        manifest.slots[slot].identity.nativeCharacterId =
            characters.baseIdForCell(cell);
        manifest.slots[slot].identity.paletteIndex =
            static_cast<uint8_t>(slot);
        manifest.slots[slot].selectorProofPresent = true;
        manifest.slots[slot].selectorProof.cursorCell = cell;
        manifest.slots[slot].selectorProof.alternateFlag = 0u;
        manifest.slots[slot].selectorProof.confirmState = 4u;
    }
    manifest.routeEpoch = 3u;
    manifest.rosterEpoch = 4u;
    manifest.layoutGeneration = 5u;
    if (!StageCatalogV2::forSeBuild().makeIdentity(3u, manifest.stage))
        return entry;
    manifest.display.recordedUnixSeconds = 1785456000;
    manifest.display.sourceKind = ReplaySourceKindV2::NetplayPlayer;
    (void)setReplayTextV2(manifest.display.title, "V2 identity wiring");

    game.start.configuration.timerMode = 2u;
    game.start.configuration.roundsToWinMinusOne = 1u;
    game.start.rng.gameplay = 1u;
    game.start.rng.cosmetic = 2u;
    game.start.rng.selector = 3u;
    game.start.fpu.x87ControlWord = 0x027Fu;
    game.start.fpu.mxcsr = 0x1F80u;
    game.start.bootstrapFingerprint = 0xABCDEF1234567890ull;
    game.start.frameZeroPreStateHash = 0x1000u;
    game.start.initialRollingDigest = initialReplayDigestV2(game);
    return entry;
}

}

QNP_TEST(replay_browser_v2_entries_need_the_v2_runtime_identity_wired) {
    auto snapshot = std::make_shared<ReplaySnapshot>();
    snapshot->complete = true;
    snapshot->entries.push_back(sealedV2Entry());

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setRuntimeIdentity(matchingRuntime(FixtureOptions{}));
    model.setSnapshot(
        std::shared_ptr<const ReplaySnapshot>(std::move(snapshot)));
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_REQUIRE(model.select(0u));

    QNP_CHECK(!model.row(0u).watchable);
    QNP_CHECK(model.row(0u).state == CompatibilityState::FileIntact);
    QNP_CHECK(model.row(0u).statusDetail.find(
                  "no installed runtime identity is available")
              != std::string::npos);
    const ActionState blocked = model.actionState(BrowserAction::WatchSet);
    QNP_CHECK(!blocked.enabled);
    QNP_CHECK(blocked.reason == ActionDisableReason::NoGameConfigured);

    ReplayRuntimeIdentityV2 runtime{};
    runtime.known = true;
    runtime.contract = sealedV2Entry().startV2.manifest.contract;
    model.setRuntimeIdentityV2(runtime);
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_CHECK(model.row(0u).watchable);
    QNP_CHECK(model.row(0u).state == CompatibilityState::Exact);
    const ActionState allowed = model.actionState(BrowserAction::WatchSet);
    QNP_CHECK(allowed.enabled);

    model.setRuntimeIdentityV2(ReplayRuntimeIdentityV2{});
    QNP_CHECK(!model.row(0u).watchable);
    QNP_CHECK(!model.actionState(BrowserAction::WatchSet).enabled);
}

QNP_TEST(replay_browser_v2_content_drift_watchable_attested_and_unattested) {
    ReplayEntry attested = sealedV2Entry();
    attested.compatibilityIdentityV3.status =
        qreplay::ReplayCompatibilityIdentityStatusV3::Attested;
    attested.compatibilityIdentityV3.identity = v2CompatibilityIdentity();
    ReplayEntry unattested = sealedV2Entry();
    unattested.stableId = attested.stableId + 1u;
    unattested.prefix.sessionUuidLow ^= 1u;
    unattested.fileName = L"synthetic-v2-unattested.q99r";
    unattested.path = unattested.fileName;

    auto snapshot = std::make_shared<ReplaySnapshot>();
    snapshot->complete = true;
    snapshot->entries.push_back(attested);
    snapshot->entries.push_back(unattested);

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setRuntimeIdentity(matchingRuntime(FixtureOptions{}));

    ReplayRuntimeIdentityV2 runtime{};
    runtime.known = true;
    runtime.contract = sealedV2Entry().startV2.manifest.contract;
    runtime.contract.contentManifestRoot[0] ^= 1u;
    runtime.compatibilityIdentity = v2CompatibilityIdentity();
    model.setRuntimeIdentityV2(runtime);
    model.setSnapshot(
        std::shared_ptr<const ReplaySnapshot>(std::move(snapshot)));
    QNP_REQUIRE(model.rowCount() == 2u);

    for (size_t index = 0u; index < 2u; ++index) {
        const BrowserRow& row = model.row(index);
        const bool isAttested = row.fileNameText == "synthetic-v2.q99r";
        if (isAttested) {
            QNP_CHECK(row.watchable);
            QNP_CHECK(row.state == CompatibilityState::Exact);
            QNP_CHECK(row.statusDetail.find("unrelated local files")
                      != std::string::npos);
        } else {
            QNP_CHECK(row.watchable);
            QNP_CHECK(row.state == CompatibilityState::ContentMismatch);
            QNP_CHECK(row.statusShort == "Ready (older files)");
        }
    }
}

QNP_TEST(replay_browser_v2_labels_a_rematch_as_behaviour_verified) {
    ReplayEntry firstGame = sealedV2Entry();
    QNP_CHECK(firstGame.startV2.manifest.identity.gameGeneration == 1u);
    ReplayEntry rematch = sealedV2Entry();
    rematch.startV2.manifest.identity.gameGeneration = 6u;
    rematch.stableId = firstGame.stableId + 1u;
    rematch.prefix.sessionUuidLow ^= 1u;
    rematch.fileName = L"synthetic-v2-rematch.q99r";
    rematch.path = rematch.fileName;

    auto snapshot = std::make_shared<ReplaySnapshot>();
    snapshot->complete = true;
    snapshot->entries.push_back(firstGame);
    snapshot->entries.push_back(rematch);

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setRuntimeIdentity(matchingRuntime(FixtureOptions{}));
    ReplayRuntimeIdentityV2 runtime{};
    runtime.known = true;
    runtime.contract = sealedV2Entry().startV2.manifest.contract;
    model.setRuntimeIdentityV2(runtime);
    model.setSnapshot(
        std::shared_ptr<const ReplaySnapshot>(std::move(snapshot)));
    QNP_REQUIRE(model.rowCount() == 2u);

    for (size_t index = 0u; index < 2u; ++index) {
        const BrowserRow& row = model.row(index);
        const bool isRematch =
            row.fileNameText == "synthetic-v2-rematch.q99r";
        QNP_CHECK(row.watchable);
        QNP_CHECK(row.state == CompatibilityState::Exact);
        QNP_CHECK((row.statusDetail.find("behaviour-verified")
                   != std::string::npos) == isRematch);
        QNP_REQUIRE(model.select(index));
        QNP_CHECK((model.details().statusDetail.find("behaviour-verified")
                   != std::string::npos) == isRematch);

        QNP_CHECK((row.verificationMode
                   == ReplayVerificationModeV2::BehaviourVerified)
                  == isRematch);
        QNP_CHECK(row.verificationMode
                  == model.details().verificationMode);
        QNP_CHECK(row.statusShort == std::string("Ready"));
        QNP_CHECK(row.statusTone == BrowserStatusTone::Ready);
        QNP_CHECK(model.details().watchabilityText == "Ready to watch.");
        const std::string verification = model.details().verificationText;
        QNP_CHECK(!verification.empty());
        if (isRematch) {
            QNP_CHECK(verification.find("not byte for byte")
                      != std::string::npos);
            QNP_CHECK(verification.find("Rematch") != std::string::npos);
        } else {
            QNP_CHECK(verification.find("frame by frame")
                      != std::string::npos);
            QNP_CHECK(verification.find("Rematch") == std::string::npos);
        }
    }
}

namespace {

std::string defaultViewText(const ReplayBrowserModel& model) {
    std::string text = model.emptyStateText();
    text += "\n";
    for (size_t i = 0u; i < model.rowCount(); ++i) {
        const BrowserRow& row = model.row(i);
        text += row.dateText + "\n" + row.matchLabelText + "\n"
            + row.scoreText + "\n" + row.durationText + "\n"
            + row.statusShort + "\n";
    }
    const BrowserDetails& details = model.details();
    text += details.titleText + "\n" + details.recordedTitleText + "\n"
        + details.dateText + "\n" + details.sourceFriendlyText + "\n"
        + details.durationText + "\n" + details.statusShort + "\n"
        + details.watchabilityText + "\n" + details.verificationText + "\n"
        + details.fileCheckText + "\n" + details.provenanceText + "\n"
        + details.storageText + "\n" + details.sizeText + "\n"
        + details.noteText + "\n";
    for (const std::string& tag : details.tags) text += tag + "\n";
    for (const RosterParticipant& participant : details.participants)
        text += participant.displayName + "\n" + participant.laneName + "\n";
    for (const BrowserMatchRow& match : details.matches)
        text += match.outcomeText + "\n" + match.durationText + "\n";
    return text;
}

std::string lowered(const std::string& text) {
    std::string out = text;
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

bool hasHexRun(const std::string& text, size_t run) {
    size_t length = 0u;
    for (const char ch : text) {
        const bool hex = (ch >= '0' && ch <= '9')
            || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        length = hex ? length + 1u : 0u;
        if (length >= run) return true;
    }
    return false;
}

}

QNP_TEST(replay_browser_default_view_carries_no_developer_detail) {
    TempDir dir(L"br_plain");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    options.matchCount = 2u;
    options.networkMetadata = true;
    options.title = "Fixture set";
    QNP_REQUIRE(writeFixture(dir.file(L"plain.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_REQUIRE(model.select(0u));

    const std::string visible = lowered(defaultViewText(model));
    static const char* const kJargon[] = {
        "schema", "digest", "sha256", "fingerprint", "manifest", "contract",
        "bootstrap", "runtime", "simulation", "recorded summary",
        "byte-exact", "behaviour-verified", "compatibility", "uuid", "epoch",
        "catalog", "projection", "topology", "cursor", "selector", "0x",
        "state hash", "container", "chunk", "ordinal", "unattested",
        "descriptor", "generation", "rollback", "rtt", "permille",
        "slot 0", "lane 0", "side 1 (p1", "side 2 (p2", "not verified",
    };
    for (const char* term : kJargon) {
        QNP_CHECK(visible.find(term) == std::string::npos);
    }
    QNP_CHECK(!hasHexRun(visible, 16u));

    const BrowserDetails& details = model.details();
    QNP_CHECK(details.watchabilityText == "Ready to watch.");
    QNP_CHECK(details.verificationText.find("frame by frame")
              != std::string::npos);
    QNP_CHECK(details.fileCheckText.find("haven't been checked")
              != std::string::npos);
    QNP_CHECK(details.sourceFriendlyText.find("match") != std::string::npos);
    QNP_CHECK(model.row(0u).statusShort == "Ready");
    QNP_CHECK(model.row(0u).statusTone == BrowserStatusTone::Ready);

    QNP_REQUIRE(details.matches.size() == 2u);
    for (const BrowserMatchRow& match : details.matches) {
        QNP_CHECK(!match.outcomeText.empty());
        QNP_CHECK(match.outcomeText.find("side") == std::string::npos);
        QNP_CHECK(match.outcomeText.find("P1+P3") == std::string::npos);
        QNP_CHECK(match.resultText.find("side") != std::string::npos);
    }

    QNP_CHECK(!details.statusBadge.empty());
    QNP_CHECK(!details.integrityText.empty());
    QNP_CHECK(details.simulationVerificationText.find("recorded summary")
              != std::string::npos);
    QNP_CHECK(!details.routeText.empty());
    QNP_CHECK(details.identityLines.size() >= 3u);
}

QNP_TEST(replay_browser_row_label_prefers_the_matchup_over_the_file_name) {
    TempDir dir(L"br_label");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    options.title = "session-20260803-2114-netplay-recording";
    QNP_REQUIRE(writeFixture(dir.file(L"session-20260803-2114.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 1u);
    const BrowserRow& row = model.row(0u);

    QNP_CHECK(row.matchLabelText == row.matchTitleText);
    QNP_CHECK(row.matchLabelText != row.matchupText);
    QNP_CHECK(row.matchLabelText.find(" vs ") != std::string::npos);
    QNP_CHECK(row.matchLabelText.find("Player1 (") != std::string::npos);
    QNP_CHECK(row.matchLabelText.find(") vs Player2 (") != std::string::npos);
    QNP_CHECK(row.matchLabelText != row.fileNameText);
    QNP_CHECK(row.matchLabelText != row.titleText);
    QNP_CHECK(row.fileNameText == "session-20260803-2114.q99r");
    QNP_CHECK(!row.titleText.empty());

    QNP_REQUIRE(model.select(0u));
    ActionRequest rename{};
    rename.action = BrowserAction::Rename;
    rename.text = "Grand finals";
    const ActionOutcome armed = model.requestAction(rename);
    QNP_REQUIRE(armed.needsConfirmation);
    QNP_REQUIRE(model.confirmAction(armed.confirmationToken).ok);
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_CHECK(model.row(0u).hasLocalTitle);
    QNP_CHECK(model.row(0u).matchLabelText == "Grand finals");
}

QNP_TEST(replay_browser_row_shows_recorded_rounds_and_who_played) {
    ReplayEntry entry = sealedV2Entry();
    entry.hasEndV2 = true;
    entry.endV2.winningSideMask = 0x05u;
    entry.endV2.roundTallies = {{2u, 1u}};

    auto snapshot = std::make_shared<ReplaySnapshot>();
    snapshot->complete = true;
    snapshot->entries.push_back(entry);

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setRuntimeIdentity(matchingRuntime(FixtureOptions{}));
    ReplayRuntimeIdentityV2 runtime{};
    runtime.known = true;
    runtime.contract = sealedV2Entry().startV2.manifest.contract;
    model.setRuntimeIdentityV2(runtime);
    model.setSnapshot(
        std::shared_ptr<const ReplaySnapshot>(std::move(snapshot)));
    QNP_REQUIRE(model.rowCount() == 1u);
    const BrowserRow& row = model.row(0u);

    QNP_CHECK(row.scoreText == "2-1");

    QNP_CHECK(row.matchTitleText.find("P1 (") != std::string::npos);
    QNP_CHECK(row.matchTitleText.find(") vs P2 (") != std::string::npos);
    QNP_CHECK(row.matchLabelText == row.matchTitleText);
    QNP_CHECK(row.matchupText != row.matchTitleText);
    QNP_CHECK(row.matchupText.find("P1") == std::string::npos);

    QNP_REQUIRE(model.select(0u));
    const BrowserDetails& details = model.details();
    QNP_CHECK(details.scoreText == "Rounds 2-1 (first to 2)");
    QNP_CHECK(details.seriesText == "First game of this session.");
}

QNP_TEST(replay_browser_refuses_to_invent_a_round_score) {
    ReplayEntry missing = sealedV2Entry();
    missing.hasEndV2 = false;
    missing.endV2.roundTallies = {{2u, 1u}};

    ReplayEntry contradictory = sealedV2Entry();
    contradictory.stableId = missing.stableId + 1u;
    contradictory.prefix.sessionUuidLow ^= 1u;
    contradictory.fileName = L"synthetic-v2-contradictory.q99r";
    contradictory.path = contradictory.fileName;
    contradictory.hasEndV2 = true;
    contradictory.endV2.winningSideMask = 0x05u;
    contradictory.endV2.roundTallies = {{0u, 0u}};

    ReplayEntry rematch = sealedV2Entry();
    rematch.stableId = missing.stableId + 2u;
    rematch.prefix.sessionUuidLow ^= 2u;
    rematch.fileName = L"synthetic-v2-rematch.q99r";
    rematch.path = rematch.fileName;
    rematch.startV2.manifest.identity.gameGeneration = 3u;
    rematch.hasEndV2 = true;
    rematch.endV2.winningSideMask = 0x0Au;
    rematch.endV2.roundTallies = {{1u, 2u}};

    auto snapshot = std::make_shared<ReplaySnapshot>();
    snapshot->complete = true;
    snapshot->entries.push_back(missing);
    snapshot->entries.push_back(contradictory);
    snapshot->entries.push_back(rematch);

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setRuntimeIdentity(matchingRuntime(FixtureOptions{}));
    ReplayRuntimeIdentityV2 runtime{};
    runtime.known = true;
    runtime.contract = sealedV2Entry().startV2.manifest.contract;
    model.setRuntimeIdentityV2(runtime);
    model.setSnapshot(
        std::shared_ptr<const ReplaySnapshot>(std::move(snapshot)));
    QNP_REQUIRE(model.rowCount() == 3u);

    QNP_REQUIRE(model.selectByStableId(missing.stableId));
    QNP_CHECK(model.row(model.selectedIndex()).scoreText == "...");
    QNP_CHECK(model.details().scoreText == "Round score not recorded");

    QNP_REQUIRE(model.selectByStableId(contradictory.stableId));
    QNP_CHECK(model.row(model.selectedIndex()).scoreText == "...");
    QNP_CHECK(model.details().scoreText == "Round score not recorded");

    QNP_REQUIRE(model.selectByStableId(rematch.stableId));
    QNP_CHECK(model.row(model.selectedIndex()).scoreText == "1-2");
    QNP_CHECK(model.details().scoreText == "Rounds 1-2 (first to 2)");
    QNP_CHECK(model.details().seriesText
              == "Game 3 of this session. The set score is not recorded in "
                 "this file.");
}

namespace {

MatchSpan v2SpanFor(const ReplayEntry& entry, uint8_t winningSideMask,
                    TerminalReason reason) {
    MatchSpan span{};
    span.matchId = entry.startV2.manifest.identity.gameGeneration;
    span.ordinal = 1u;
    span.firstTimelineFrame = 0u;
    span.lastTimelineFrame = entry.totalFrames - 1u;
    span.frameCount = entry.totalFrames;
    span.roundCount = 3u;
    span.hasEnd = true;
    span.independentBootstrap = true;
    span.winningSideMask = winningSideMask;
    span.endReason = reason;
    for (size_t slot = 0u; slot < kFighterSlots; ++slot) {
        span.roster[slot].nativeCharacterId =
            entry.startV2.manifest.slots[slot].identity.nativeCharacterId;
        span.roster[slot].paletteIndex =
            entry.startV2.manifest.slots[slot].identity.paletteIndex;
        span.ownership[slot] =
            entry.startV2.manifest.slots[slot].ownership;
    }
    return span;
}

ReplayBrowserModel& wireV2(ReplayBrowserModel& model,
                           ReplayAnnotationStore& annotations,
                           std::shared_ptr<ReplaySnapshot> snapshot) {
    snapshot->complete = true;
    model.setAnnotations(&annotations);
    model.setRuntimeIdentity(matchingRuntime(FixtureOptions{}));
    ReplayRuntimeIdentityV2 runtime{};
    runtime.known = true;
    runtime.contract = sealedV2Entry().startV2.manifest.contract;
    model.setRuntimeIdentityV2(runtime);
    model.setSnapshot(
        std::shared_ptr<const ReplaySnapshot>(std::move(snapshot)));
    return model;
}

}

QNP_TEST(replay_browser_never_calls_an_aborted_game_a_draw) {
    ReplayEntry dropped = sealedV2Entry();
    dropped.hasEndV2 = true;
    dropped.endV2.winningSideMask = 0u;
    dropped.endV2.roundTallies = {{1u, 1u}};
    dropped.matches.push_back(
        v2SpanFor(dropped, 0u, TerminalReason::Disconnected));

    ReplayEntry drawn = sealedV2Entry();
    drawn.stableId = dropped.stableId + 1u;
    drawn.prefix.sessionUuidLow ^= 1u;
    drawn.fileName = L"synthetic-v2-draw.q99r";
    drawn.path = drawn.fileName;
    drawn.hasEndV2 = true;
    drawn.endV2.winningSideMask = 0u;
    drawn.endV2.roundTallies = {{1u, 1u}};
    drawn.matches.push_back(v2SpanFor(drawn, 0u, TerminalReason::Completed));

    auto snapshot = std::make_shared<ReplaySnapshot>();
    snapshot->entries.push_back(dropped);
    snapshot->entries.push_back(drawn);

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    wireV2(model, annotations, std::move(snapshot));
    QNP_REQUIRE(model.rowCount() == 2u);

    QNP_REQUIRE(model.selectByStableId(dropped.stableId));
    QNP_REQUIRE(model.details().matches.size() == 1u);
    QNP_CHECK(model.details().matches[0].outcomeText
              == "No result: disconnect");
    QNP_CHECK(model.details().matches[0].resultText == "draw / aborted");

    QNP_REQUIRE(model.selectByStableId(drawn.stableId));
    QNP_REQUIRE(model.details().matches.size() == 1u);
    QNP_CHECK(model.details().matches[0].outcomeText == "Draw");
}

QNP_TEST(replay_browser_names_the_winner_by_person_not_character) {
    ReplayEntry mirror = sealedV2Entry();
    const CharacterCatalog& characters = CharacterCatalog::forSeBuild();
    for (size_t slot = 0u; slot < 2u; ++slot) {
        mirror.startV2.manifest.slots[slot].identity.nativeCharacterId =
            characters.baseIdForCell(0u);
        mirror.startV2.manifest.slots[slot].selectorProof.cursorCell = 0u;
        (void)setReplayTextV2(mirror.startV2.manifest.lanes[slot].name,
                              "Aquatic");
    }
    mirror.startV2.start.initialRollingDigest =
        initialReplayDigestV2(mirror.startV2);
    mirror.hasEndV2 = true;
    mirror.endV2.winningSideMask = 0x05u;
    mirror.endV2.roundTallies = {{2u, 0u}};
    mirror.matches.push_back(
        v2SpanFor(mirror, 0x05u, TerminalReason::Completed));

    auto snapshot = std::make_shared<ReplaySnapshot>();
    snapshot->entries.push_back(mirror);

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    wireV2(model, annotations, std::move(snapshot));
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_REQUIRE(model.selectByStableId(mirror.stableId));

    QNP_CHECK(model.row(0u).matchTitleText.find("Aquatic P1 (")
              != std::string::npos);
    QNP_CHECK(model.row(0u).matchTitleText.find("Aquatic P2 (")
              != std::string::npos);
    QNP_REQUIRE(model.details().matches.size() == 1u);
    QNP_CHECK(model.details().matches[0].outcomeText == "Aquatic P1 wins");
    QNP_CHECK(model.details().matches[0].matchupText.find("Aquatic")
              == std::string::npos);
}

QNP_TEST(replay_browser_refuses_to_quote_a_match_format_the_game_cannot_run) {
    ReplayEntry invented = sealedV2Entry();
    invented.hasEndV2 = true;
    invented.endV2.winningSideMask = 0x05u;
    invented.endV2.roundTallies = {{2u, 1u}};
    invented.startV2.start.configuration.roundsToWinMinusOne = 40u;

    ReplayEntry longest = sealedV2Entry();
    longest.stableId = invented.stableId + 1u;
    longest.prefix.sessionUuidLow ^= 1u;
    longest.fileName = L"synthetic-v2-first-to-three.q99r";
    longest.path = longest.fileName;
    longest.hasEndV2 = true;
    longest.endV2.winningSideMask = 0x05u;
    longest.endV2.roundTallies = {{3u, 2u}};
    longest.startV2.start.configuration.roundsToWinMinusOne =
        qnetplay::kMaxRoundsToWinMinusOne;

    auto snapshot = std::make_shared<ReplaySnapshot>();
    snapshot->entries.push_back(invented);
    snapshot->entries.push_back(longest);

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    wireV2(model, annotations, std::move(snapshot));
    QNP_REQUIRE(model.rowCount() == 2u);

    QNP_REQUIRE(model.selectByStableId(invented.stableId));
    QNP_CHECK(model.details().scoreText == "Rounds 2-1");

    QNP_REQUIRE(model.selectByStableId(longest.stableId));
    QNP_CHECK(model.details().scoreText == "Rounds 3-2 (first to 3)");
}

QNP_TEST(replay_browser_reads_an_unknown_game_as_checking_then_not_set_up) {
    TempDir dir(L"br_pending");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    QNP_REQUIRE(writeFixture(dir.file(L"pending.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(RuntimeIdentity{});
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_REQUIRE(model.select(0u));
    QNP_CHECK(model.row(0u).state == CompatibilityState::FileIntact);

    model.setIdentityPending(true);
    QNP_CHECK(model.identityPending());
    QNP_CHECK(model.row(0u).statusShort == "Checking...");
    QNP_CHECK(model.row(0u).statusTone == BrowserStatusTone::Pending);
    QNP_CHECK(model.details().watchabilityText.find("Checking")
              != std::string::npos);

    model.setIdentityPending(false);
    QNP_CHECK(model.row(0u).statusShort == "Game not set up");
    QNP_CHECK(model.row(0u).statusTone == BrowserStatusTone::Attention);
    QNP_CHECK(model.details().watchabilityText.find("Settings")
              != std::string::npos);
    QNP_CHECK(!model.row(0u).statusBadge.empty());
}

QNP_TEST(replay_browser_plain_disable_reasons_exist_for_every_reason) {
    for (uint16_t i = 0u; i <= static_cast<uint16_t>(
             ActionDisableReason::InTrash); ++i) {
        const ActionDisableReason reason =
            static_cast<ActionDisableReason>(i);
        const std::string plain = actionDisableReasonPlainText(reason);
        if (reason == ActionDisableReason::None) {
            QNP_CHECK(plain.empty());
            continue;
        }
        QNP_CHECK(!plain.empty());
        const std::string low = lowered(plain);
        QNP_CHECK(low.find("bootstrap") == std::string::npos);
        QNP_CHECK(low.find("simulation verification") == std::string::npos);
        QNP_CHECK(low.find("seeking") == std::string::npos);
        QNP_CHECK(!std::string(actionDisableReasonText(reason)).empty());
    }
}

namespace {

RosterParticipant rosterPlayer(
    uint8_t slot, uint8_t side, const char* who, const char* character,
    SlotOwnership ownership = SlotOwnership::RecordedHuman) {
    RosterParticipant participant{};
    participant.slot = slot;
    participant.side = side;
    participant.sourceLane = slot;
    participant.characterId = static_cast<uint8_t>(slot + 1u);
    participant.ownership = ownership;
    participant.displayName = character;
    participant.laneName = who;
    return participant;
}

}

QNP_TEST(replay_browser_match_title_names_every_player_with_their_character) {
    {
        const std::vector<RosterParticipant> roster = {
            rosterPlayer(0u, 0u, "Aquatic", "Reimu"),
            rosterPlayer(1u, 1u, "Nanashi", "Marisa"),
        };
        QNP_CHECK(ReplayBrowserModel::matchTitleTextFor(roster)
                  == "Aquatic (Reimu) vs Nanashi (Marisa)");
        QNP_CHECK(ReplayBrowserModel::matchupTextFor(roster)
                  == "Reimu vs Marisa");
    }

    {
        const std::vector<RosterParticipant> roster = {
            rosterPlayer(0u, 0u, "Aquatic", "Reimu"),
            rosterPlayer(1u, 1u, "", "Marisa"),
        };
        QNP_CHECK(ReplayBrowserModel::matchTitleTextFor(roster)
                  == "Aquatic (Reimu) vs P2 (Marisa)");
    }
    {
        const std::vector<RosterParticipant> roster = {
            rosterPlayer(0u, 0u, "", "Reimu"),
            rosterPlayer(1u, 1u, "", "Marisa"),
        };
        QNP_CHECK(ReplayBrowserModel::matchTitleTextFor(roster)
                  == "P1 (Reimu) vs P2 (Marisa)");
    }

    {
        const std::vector<RosterParticipant> roster = {
            rosterPlayer(0u, 0u, "Aquatic", "Reimu"),
            rosterPlayer(1u, 1u, "Aquatic", "Marisa"),
        };
        QNP_CHECK(ReplayBrowserModel::matchTitleTextFor(roster)
                  == "Aquatic P1 (Reimu) vs Aquatic P2 (Marisa)");
    }
    {
        const std::vector<RosterParticipant> roster = {
            rosterPlayer(0u, 0u, "Aquatic", "Reimu"),
            rosterPlayer(1u, 1u, "Nanashi", "Marisa"),
            rosterPlayer(2u, 0u, "Aquatic", "Sakuya"),
            rosterPlayer(3u, 1u, "Youmu-fan", "Youmu"),
        };
        QNP_CHECK(ReplayBrowserModel::matchTitleTextFor(roster)
                  == "Aquatic P1 (Reimu) + Aquatic P3 (Sakuya)"
                     " vs Nanashi (Marisa) + Youmu-fan (Youmu)");
    }

    {
        const std::vector<RosterParticipant> roster = {
            rosterPlayer(0u, 0u, "Aquatic", "Reimu"),
            rosterPlayer(1u, 1u, "Nanashi", "Marisa"),
            rosterPlayer(2u, 0u, "Kai", "Sakuya"),
            rosterPlayer(3u, 1u, "Rin", "Youmu"),
        };
        QNP_CHECK(ReplayBrowserModel::matchTitleTextFor(roster)
                  == "Aquatic (Reimu) + Kai (Sakuya)"
                     " vs Nanashi (Marisa) + Rin (Youmu)");
    }

    {
        const std::vector<RosterParticipant> roster = {
            rosterPlayer(0u, 0u, "Aquatic", "Reimu"),
            rosterPlayer(1u, 1u, "leftover-label", "Marisa",
                   SlotOwnership::NativeCpu),
        };
        QNP_CHECK(ReplayBrowserModel::matchTitleTextFor(roster)
                  == "Aquatic (Reimu) vs CPU (Marisa)");
        QNP_CHECK(ReplayBrowserModel::matchTitleTextFor(roster)
                      .find("(CPU)") == std::string::npos);
    }

    {
        const std::vector<RosterParticipant> none;
        QNP_CHECK(ReplayBrowserModel::matchTitleTextFor(none) == "no roster");
        QNP_CHECK(ReplayBrowserModel::matchTitleTextFor(none)
                  == ReplayBrowserModel::matchupTextFor(none));
    }
    {
        const std::vector<RosterParticipant> lonely = {
            rosterPlayer(0u, 0u, "Aquatic", "Reimu"),
        };
        QNP_CHECK(ReplayBrowserModel::matchTitleTextFor(lonely)
                  == "Aquatic (Reimu) vs (no opponent recorded)");
        const std::vector<RosterParticipant> other = {
            rosterPlayer(1u, 1u, "Nanashi", "Marisa"),
        };
        QNP_CHECK(ReplayBrowserModel::matchTitleTextFor(other)
                  == "(no opponent recorded) vs Nanashi (Marisa)");
    }
}

QNP_TEST(replay_browser_never_shows_a_round_score_it_did_not_read) {
    TempDir dir(L"br_legacy_score");
    QNP_REQUIRE(dir.valid());
    FixtureOptions options{};
    options.matchCount = 1u;
    options.roundsPerMatch = 3u;
    QNP_REQUIRE(writeFixture(dir.file(L"legacy.q99r"), options));

    ReplayBrowserModel model;
    ReplayAnnotationStore annotations;
    model.setAnnotations(&annotations);
    model.setReplayRoot(dir.path);
    model.setRuntimeIdentity(matchingRuntime(options));
    model.setSnapshot(scan(dir.path));
    QNP_REQUIRE(model.rowCount() == 1u);
    QNP_REQUIRE(model.select(0u));
    QNP_CHECK(!model.row(0u).isPart);
    QNP_CHECK(model.row(0u).scoreText == "...");
    QNP_CHECK(model.details().scoreText == "Round score not recorded");
    QNP_CHECK(model.details().seriesText.empty());
    QNP_CHECK(model.row(0u).matchTitleText.find("Player1 (")
              != std::string::npos);

    ReplayEntry footerless = sealedV2Entry();
    footerless.hasFooter = false;
    footerless.hasEndV2 = true;
    footerless.endV2.winningSideMask = 0x05u;
    footerless.endV2.roundTallies = {{2u, 1u}};

    auto snapshot = std::make_shared<ReplaySnapshot>();
    snapshot->complete = true;
    snapshot->entries.push_back(footerless);

    ReplayBrowserModel v2Model;
    ReplayAnnotationStore v2Annotations;
    v2Model.setAnnotations(&v2Annotations);
    v2Model.setRuntimeIdentity(matchingRuntime(FixtureOptions{}));
    ReplayRuntimeIdentityV2 runtime{};
    runtime.known = true;
    runtime.contract = sealedV2Entry().startV2.manifest.contract;
    v2Model.setRuntimeIdentityV2(runtime);
    v2Model.setSnapshot(
        std::shared_ptr<const ReplaySnapshot>(std::move(snapshot)));
    QNP_REQUIRE(v2Model.rowCount() == 1u);
    QNP_REQUIRE(v2Model.select(0u));
    QNP_CHECK(v2Model.row(0u).scoreText == "...");
    QNP_CHECK(v2Model.details().scoreText == "Round score not recorded");
}
