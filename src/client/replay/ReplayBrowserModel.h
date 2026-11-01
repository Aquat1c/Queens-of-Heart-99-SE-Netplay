#pragma once

#include "ReplayAnnotations.h"
#include "ReplayCatalog.h"
#include "ReplayCompatibility.h"
#include "ReplayCompatibilityV2.h"
#include "ReplayParser.h"

#include "replay/ReplayTypes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qclient::replay {

enum class BrowserAction : uint8_t {
    WatchSet = 0,
    WatchMatch,
    StartAtRound,
    VerifyFile,
    VerifySimulation,
    Rename,
    ToggleFavorite,
    EditTags,
    EditNote,
    ExportMatch,
    ImportExternal,
    RevealFolder,
    MoveToTrash,
    UndoTrash,
    PermanentPurge,
    ClearFilters,
    Refresh,
    Count,
};

const char* browserActionText(BrowserAction action);

enum class ActionDisableReason : uint16_t {
    None = 0,
    NoSelection,
    ScanInProgress,
    ActivelyRecording,
    NotWatchable,
    NoMatchSelected,
    NoRoundIndex,
    NoIndependentBootstrap,
    NothingToUndo,
    ReadOnlyLocation,
    ExternalReadOnly,
    FileMissing,
    FileChangedSinceScan,
    Corrupt,
    Unsupported,
    ContentMismatch,
    Incomplete,
    NoAnnotationStore,
    NoFilterActive,
    SimulationNotImplemented,
    SeekNotImplemented,
    NoGameConfigured,
    InTrash,
    NotInTrash,
};

const char* actionDisableReasonText(ActionDisableReason reason);

const char* actionDisableReasonPlainText(ActionDisableReason reason);

struct ActionState {
    bool enabled = false;
    ActionDisableReason reason = ActionDisableReason::NoSelection;
    bool destructive = false;
    bool requiresConfirmation = false;
};

enum class BrowserStatusTone : uint8_t {
    Ready = 0,
    Pending,
    Attention,
    Broken,
    Recording,
};

struct RosterParticipant {
    uint8_t slot = 0u;
    uint8_t side = 0u;
    uint8_t sourceLane = 0u;
    uint8_t characterId = qreplay::kInactiveCharacterId;
    uint8_t palette = 0u;
    qreplay::SlotOwnership ownership = qreplay::SlotOwnership::Inactive;
    std::string displayName;
    std::string laneName;
    bool hiddenColumn = false;
    bool standAlternate = false;
    bool unknownId = false;
    bool selectorProofPresent = false;
    bool selectorProofVerified = false;
};

struct BrowserMatchRow {
    uint32_t matchId = 0u;
    uint32_t ordinal = 0u;
    uint32_t firstTimelineFrame = 0u;
    uint32_t lastTimelineFrame = 0u;
    uint32_t frameCount = 0u;
    uint32_t roundCount = 0u;
    uint8_t winningSideMask = 0u;
    bool independentBootstrap = false;
    bool complete = false;
    std::string resultText;
    std::string outcomeText;
    std::string durationText;
    std::string matchupText;
    std::vector<RosterParticipant> participants;
};

struct BrowserRow {
    uint64_t stableId = 0u;
    size_t entryIndex = 0u;

    std::string dateText;
    std::string titleText;
    std::string matchupText;
    std::string matchTitleText;
    std::string scoreText;
    std::string durationText;
    std::string sourceText;
    std::string statusBadge;
    std::string statusDetail;
    std::string fileNameText;

    std::string matchLabelText;
    std::string statusShort;
    BrowserStatusTone statusTone = BrowserStatusTone::Attention;
    qreplay::ReplayVerificationModeV2 verificationMode =
        qreplay::ReplayVerificationModeV2::ByteExact;

    bool favorite = false;
    bool hasLocalTitle = false;
    bool isPart = false;
    bool inTrash = false;
    uint8_t fighterCount = 0u;
    uint8_t humanCount = 0u;
    uint8_t cpuCount = 0u;

    qreplay::CompatibilityState state = qreplay::CompatibilityState::Unsupported;
    qreplay::CompatibilityReason reason = qreplay::CompatibilityReason::None;
    bool watchable = false;
};

struct BrowserDetails {
    bool valid = false;
    uint64_t stableId = 0u;

    std::string titleText;
    std::string recordedTitleText;
    std::string dateText;
    std::string sourceText;
    std::string bootstrapText;
    std::string statusBadge;
    std::string statusDetail;
    std::string integrityText;
    std::string simulationVerificationText;
    std::string storageText;
    std::string sizeText;
    std::string durationText;
    std::string routeText;
    std::string networkText;
    std::string cpuContractText;
    std::string inputDomainText;
    std::string stageText;
    std::string firstPlayableText;
    std::string incompatibilityText;
    std::string noteText;

    std::string sourceFriendlyText;
    std::string statusShort;
    BrowserStatusTone statusTone = BrowserStatusTone::Attention;
    std::string watchabilityText;
    std::string verificationText;
    std::string fileCheckText;
    std::string provenanceText;
    std::string scoreText;
    std::string seriesText;
    qreplay::ReplayVerificationModeV2 verificationMode =
        qreplay::ReplayVerificationModeV2::ByteExact;

    std::vector<std::string> tags;
    std::vector<std::string> identityLines;
    std::vector<std::string> warnings;

    std::vector<RosterParticipant> participants;
    std::vector<BrowserMatchRow> matches;
    bool matchesTruncated = false;
    uint32_t matchCount = 0u;
    uint32_t totalFrames = 0u;
    bool recordedSummaryOnly = true;
};

struct WatchRequest {
    bool valid = false;
    uint64_t stableId = 0u;
    uint64_t replayUuidLow = 0u;
    uint64_t replayUuidHigh = 0u;
    std::wstring path;
    FileIdentity identity{};
    uint32_t matchId = 0u;
    uint32_t matchOrdinal = 0u;
    uint32_t startRound = 0u;
    uint32_t startFrame = 0u;
    uint32_t dependencyMatchId = 0u;
    qreplay::CompatibilityResult compatibility{};
};

struct ActionRequest {
    BrowserAction action = BrowserAction::Refresh;
    std::string text;
    std::vector<std::string> tags;
    uint32_t matchOrdinal = 0u;
    uint32_t round = 0u;
    std::wstring path;
};

struct ActionOutcome {
    bool ok = false;
    BrowserAction action = BrowserAction::Refresh;
    ActionDisableReason reason = ActionDisableReason::None;
    std::string message;
    bool needsConfirmation = false;
    uint64_t confirmationToken = 0u;
    std::string confirmationPrompt;
    std::string confirmationTargetName;
    WatchRequest watch{};
    std::wstring producedPath;
    qreplay::ParseResult parse{};
    bool digestVerified = false;
};

struct BrowserViewState {
    std::string query;
    ReplayFilter filter{};
    SortKey sort = SortKey::GameOrder;
    uint64_t selectedStableId = 0u;
    size_t scrollTop = 0u;
    uint32_t expandedMatchOrdinal = 0u;
};

class ReplayBrowserModel {
public:
    ReplayBrowserModel();
    ~ReplayBrowserModel();

    void setAnnotations(ReplayAnnotationStore* annotations);
    void setRuntimeIdentity(const RuntimeIdentity& identity);
    void setRuntimeIdentityV2(
        const ReplayRuntimeIdentityV2& identity);
    void setReplayRoot(const std::wstring& root);
    void setScanning(bool scanning);
    void setIdentityPending(bool pending);
    bool identityPending() const { return identityPending_; }
    void setSnapshot(std::shared_ptr<const ReplaySnapshot> snapshot);
    const std::shared_ptr<const ReplaySnapshot>& snapshot() const {
        return snapshot_;
    }

    void setQuery(const std::string& query);
    const std::string& query() const { return query_; }
    ReplayFilter& filter() { return filter_; }
    const ReplayFilter& filter() const { return filter_; }
    void setSort(SortKey sort);
    SortKey sort() const { return sort_; }
    void refreshView();

    size_t rowCount() const { return rows_.size(); }
    const BrowserRow& row(size_t index) const;
    const ReplayEntry* entryForRow(size_t index) const;
    const std::vector<BrowserRow>& rows() const { return rows_; }
    uint32_t totalFileCount() const;
    bool empty() const { return rows_.empty(); }
    std::string emptyStateText() const;

    bool select(size_t index);
    bool selectByStableId(uint64_t stableId);
    bool moveSelection(int delta);
    size_t selectedIndex() const { return selectedIndex_; }
    uint64_t selectedStableId() const;
    const BrowserDetails& details() const { return details_; }

    void setScrollTop(size_t top) { scrollTop_ = top; }
    size_t scrollTop() const { return scrollTop_; }
    void expandMatch(uint32_t ordinal);
    uint32_t expandedMatchOrdinal() const { return expandedMatch_; }

    BrowserViewState captureViewState() const;
    void restoreViewState(const BrowserViewState& state);

    ActionState actionState(BrowserAction action) const;
    ActionOutcome requestAction(const ActionRequest& request);
    ActionOutcome confirmAction(uint64_t token);
    void cancelPendingAction();
    bool hasPendingAction() const { return pending_.active; }
    const std::string& pendingPrompt() const { return pending_.prompt; }

    bool openExternal(const std::wstring& path, ReplayEntry& out,
                      qreplay::CompatibilityResult& compatibility,
                      std::string& message) const;

    static std::string formatFrames(uint32_t frames);
    static std::string formatUnixDate(int64_t unixSeconds);
    static std::string formatBytes(uint64_t bytes);
    static std::string badgeTextFor(qreplay::CompatibilityState state);
    static std::vector<RosterParticipant> buildRoster(
        const std::array<qreplay::FighterSlotDescriptor,
                         qreplay::kFighterSlots>& slots,
        const std::array<uint8_t, qreplay::kFighterSlots>& route,
        const std::array<qreplay::SourceLaneDescriptor,
                         qreplay::kSourceLanes>& lanes);
    static std::vector<RosterParticipant> buildRosterV2(
        const std::array<qreplay::FighterSlotDescriptorV2,
                         qreplay::kFighterSlots>& slots,
        const std::array<uint8_t, qreplay::kFighterSlots>& route,
        const std::array<qreplay::SourceLaneDescriptorV2,
                         qreplay::kSourceLanes>& lanes);
    static std::string matchupTextFor(
        const std::vector<RosterParticipant>& participants);
    static std::string matchTitleTextFor(
        const std::vector<RosterParticipant>& participants);

private:
    struct Pending {
        bool active = false;
        uint64_t token = 0u;
        BrowserAction action = BrowserAction::Refresh;
        uint64_t stableId = 0u;
        std::wstring targetPath;
        FileIdentity targetIdentity{};
        ActionRequest request{};
        std::string prompt;
        std::string targetName;
    };

    struct TrashUndo {
        bool available = false;
        std::wstring trashedPath;
        std::wstring originalPath;
        uint64_t stableId = 0u;
    };

    void rebuildStates();
    void rebuildRows();
    void rebuildDetails();
    const ReplayEntry* selectedEntry() const;
    const ReplayAnnotation* annotationFor(const ReplayEntry& entry) const;
    ReplayIdentitySnapshot snapshotFor(const ReplayEntry& entry) const;

    bool revalidate(const ReplayEntry& entry, ActionOutcome& outcome) const;

    ActionOutcome doVerifyFile(const ReplayEntry& entry);
    ActionOutcome doRename(const ReplayEntry& entry, const std::string& title);
    ActionOutcome doExportMatch(const ReplayEntry& entry, uint32_t ordinal);
    ActionOutcome doMoveToTrash(const ReplayEntry& entry);
    ActionOutcome doUndoTrash();
    ActionOutcome doPurge(const ReplayEntry& entry);
    ActionOutcome doWatch(const ReplayEntry& entry, uint32_t ordinal,
                          uint32_t round, bool wholeSet);

    std::shared_ptr<const ReplaySnapshot> snapshot_;
    ReplayAnnotationStore* annotations_ = nullptr;
    RuntimeIdentity runtime_{};
    ReplayRuntimeIdentityV2 runtimeV2_{};
    std::wstring replayRoot_;
    bool scanning_ = false;
    bool identityPending_ = false;

    std::string query_;
    ReplayFilter filter_{};
    SortKey sort_ = SortKey::GameOrder;

    std::vector<qreplay::CompatibilityState> states_;
    std::vector<qreplay::CompatibilityResult> results_;
    std::vector<size_t> visible_;
    std::vector<BrowserRow> rows_;
    BrowserDetails details_{};

    size_t selectedIndex_ = 0u;
    uint64_t selectedStableId_ = 0u;
    size_t scrollTop_ = 0u;
    uint32_t expandedMatch_ = 0u;

    Pending pending_{};
    TrashUndo undo_{};
    uint64_t tokenCounter_ = 0u;
};

}
