#include "ReplayBrowserModel.h"

#include "ReplayWriter.h"
#include "netplay/MatchConfig.h"
#include "platform/WidePath.h"
#include "replay/CharacterCatalog.h"
#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace qclient::replay {

using namespace qreplay;

namespace {

constexpr double kNominalHz = 60.6;

const BrowserRow kEmptyRow{};

std::string joinLines(const std::vector<std::string>& parts,
                      const char* separator) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) out += separator;
        out += part;
    }
    return out;
}

std::string hexOf(const std::array<uint8_t, 32>& digest, size_t chars) {
    const std::string full = toHex(digest);
    return full.size() <= chars ? full : full.substr(0, chars);
}

const char* sourceKindText(ReplaySourceKind kind) {
    switch (kind) {
        case ReplaySourceKind::OfflineVsPlayer: return "Offline";
        case ReplaySourceKind::NetplayPlayer: return "Netplay";
        case ReplaySourceKind::SpectatorSave: return "Spectator";
        case ReplaySourceKind::RecoveredPrefix: return "Recovered";
    }
    return "Unknown";
}

const char* sourceKindTextV2(ReplaySourceKindV2 kind) {
    switch (kind) {
        case ReplaySourceKindV2::OfflineVsPlayer: return "Offline";
        case ReplaySourceKindV2::NetplayPlayer: return "Netplay";
        case ReplaySourceKindV2::SpectatorSave: return "Spectator";
        case ReplaySourceKindV2::RecoveredPrefix: return "Recovered";
    }
    return "Unknown";
}

const char* friendlySourceKindText(ReplaySourceKind kind) {
    switch (kind) {
        case ReplaySourceKind::OfflineVsPlayer: return "Local match";
        case ReplaySourceKind::NetplayPlayer: return "Online match";
        case ReplaySourceKind::SpectatorSave: return "Watched as spectator";
        case ReplaySourceKind::RecoveredPrefix: return "Recovered recording";
    }
    return "Unknown source";
}

const char* friendlySourceKindTextV2(ReplaySourceKindV2 kind) {
    switch (kind) {
        case ReplaySourceKindV2::OfflineVsPlayer: return "Local match";
        case ReplaySourceKindV2::NetplayPlayer: return "Online match";
        case ReplaySourceKindV2::SpectatorSave: return "Watched as spectator";
        case ReplaySourceKindV2::RecoveredPrefix: return "Recovered recording";
    }
    return "Unknown source";
}

struct PlainStatus {
    const char* shortText;
    BrowserStatusTone tone;
    const char* watchability;
};

PlainStatus plainStatusFor(CompatibilityState state, bool watchable,
                           ReplayVerificationModeV2 mode,
                           bool identityPending) {
    if (state == CompatibilityState::Recording) {
        return { "Recording now", BrowserStatusTone::Recording,
                 "Still being recorded. It becomes watchable when the "
                 "session ends." };
    }
    if (watchable) {
        if (state == CompatibilityState::ContentMismatch) {
            return { "Ready (older files)", BrowserStatusTone::Ready,
                     "Recorded with different game files. It should play; "
                     "the frame-by-frame check will say if it truly "
                     "differs." };
        }
        (void)mode;
        return { "Ready", BrowserStatusTone::Ready, "Ready to watch." };
    }
    switch (state) {
        case CompatibilityState::FileIntact:
            return identityPending
                ? PlainStatus{ "Checking...", BrowserStatusTone::Pending,
                               "Checking this replay against your installed "
                               "game..." }
                : PlainStatus{ "Game not set up",
                               BrowserStatusTone::Attention,
                               "Can't watch yet: the caster doesn't know "
                               "which game to check this against. Set your "
                               "QOH99 folder in Settings > Player and "
                               "files." };
        case CompatibilityState::ContentMismatch:
            return { "Different game files", BrowserStatusTone::Attention,
                     "Can't watch: your game files are different from the "
                     "ones this was recorded with." };
        case CompatibilityState::Unsupported:
            return { "Different version", BrowserStatusTone::Attention,
                     "Can't watch: this was recorded by a different version "
                     "of the game." };
        case CompatibilityState::Incomplete:
        case CompatibilityState::Recoverable:
            return { "Unfinished", BrowserStatusTone::Attention,
                     "Can't watch: the recording stops partway through - the "
                     "session ended before it was saved." };
        case CompatibilityState::Corrupt:
            return { "Damaged file", BrowserStatusTone::Broken,
                     "Can't watch: this file is damaged." };
        default:
            break;
    }
    return { "Not playable", BrowserStatusTone::Attention,
             "Can't watch this replay." };
}

const char* plainVerificationText(ReplayVerificationModeV2 mode,
                                  bool simulationVerified) {
    switch (mode) {
        case ReplayVerificationModeV2::BehaviourVerified:
            return "Rematch recording: checked by how the match plays out, "
                   "not byte for byte. The fight is the same; the check is "
                   "slightly looser.";
        case ReplayVerificationModeV2::ByteExact:
            return simulationVerified
                ? "Checked frame by frame."
                : "Will be checked frame by frame while it plays.";
    }
    return "The check this recording will get is not known.";
}

std::string fixedTextV2(const ReplayTextV2& text) {
    const size_t bytes = std::min<size_t>(
        text.bytes, text.data.size());
    return std::string(text.data.data(), bytes);
}

CompatibilityResult browserCompatibilityV2(
    const ReplayEntry& entry,
    const ReplayRuntimeIdentityV2& runtime) {
    CompatibilityResult result{};
    if (!entry.parsed || !entry.structurallyVerifiedV2
        || !entry.hasManifest || !entry.hasSessionStart) {
        result.state = CompatibilityState::Corrupt;
        result.reason = CompatibilityReason::StructuralCorruption;
        result.detail = entry.parseDetail.empty()
            ? "the Q99R-v2 parser did not prove one complete game boundary"
            : entry.parseDetail;
        return result;
    }
    const ReplayCompatibilityDecisionV2 decision =
        evaluateReplayCompatibilityV2(entry.startV2, runtime,
                                      &entry.compatibilityIdentityV3);
    result.detail = decision.detail;
    result.watchable = decision.watchable;
    switch (decision.state) {
        case ReplayCompatibilityStateV2::Exact:
            if (entry.classificationV2
                == ReplayClassificationV2::
                    UnsealedValidatedPrefix) {
                result.state = CompatibilityState::Incomplete;
                result.reason = CompatibilityReason::MissingFooter;
                result.detail =
                    "incomplete recording; the final footer is missing";
                result.watchable = false;
            } else {
                result.state = CompatibilityState::Exact;
                result.reason = CompatibilityReason::None;
                result.watchable = true;
                if (entry.classificationV2
                    == ReplayClassificationV2::
                        FooterCompleteRenameFailure) {
                    result.detail =
                        "exact runtime match; completed footer verified "
                        "under a legacy publication state";
                }
            }
            break;
        case ReplayCompatibilityStateV2::RuntimeUnknown:
            result.state = CompatibilityState::FileIntact;
            result.reason = CompatibilityReason::None;
            result.watchable = false;
            break;
        case ReplayCompatibilityStateV2::ContentMismatch:
            result.state = CompatibilityState::ContentMismatch;
            result.reason =
                CompatibilityReason::ContentManifestMismatch;
            result.watchable = true;
            break;
        case ReplayCompatibilityStateV2::Unsupported:
            result.state = CompatibilityState::Unsupported;
            result.reason =
                CompatibilityReason::RuntimeSimulationMismatch;
            result.watchable = false;
            break;
        case ReplayCompatibilityStateV2::Corrupt:
            result.state = CompatibilityState::Corrupt;
            result.reason = CompatibilityReason::StructuralCorruption;
            result.watchable = false;
            break;
    }
    if (result.watchable
        && decision.verificationMode
            == qreplay::ReplayVerificationModeV2::BehaviourVerified
        && result.detail.find(kBehaviourVerifiedDetailSuffixV2)
            == std::string::npos) {
        result.detail += kBehaviourVerifiedDetailSuffixV2;
    }
    return result;
}

std::vector<std::string> playerTokensFor(
    const std::vector<RosterParticipant>& participants) {
    std::vector<std::string> who(participants.size());
    for (size_t i = 0; i < participants.size(); ++i) {
        const RosterParticipant& participant = participants[i];
        if (participant.ownership == SlotOwnership::NativeCpu) {
            who[i] = "CPU";
        } else if (!participant.laneName.empty()) {
            who[i] = participant.laneName;
        } else {
            who[i] = "P" + std::to_string(participant.slot + 1);
        }
    }
    std::vector<bool> collides(who.size(), false);
    for (size_t i = 0; i < who.size(); ++i) {
        for (size_t j = 0; j < who.size(); ++j) {
            if (i != j && who[i] == who[j]) {
                collides[i] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < who.size(); ++i) {
        if (collides[i])
            who[i] += " P" + std::to_string(participants[i].slot + 1);
    }
    return who;
}

const char* noWinnerTextFor(TerminalReason reason) {
    switch (reason) {
        case TerminalReason::Completed:        return "Draw";
        case TerminalReason::Disconnected:     return "No result: disconnect";
        case TerminalReason::ProcessExit:      return "No result: game quit";
        case TerminalReason::RecordingFailure: return "No result: rec. failed";
        case TerminalReason::RecoveredPrefix:  return "No result: recovered";
        case TerminalReason::RecorderOverflow: return "No result: overflow";
    }
    return "No result";
}

std::string matchOutcomeTextFor(
    const std::vector<RosterParticipant>& participants,
    uint8_t winningSideMask, bool complete, TerminalReason endReason) {
    if (winningSideMask == 0u)
        return complete ? noWinnerTextFor(endReason) : "Unfinished";
    const uint8_t winner = (winningSideMask & 0x05u) != 0u ? 0u : 1u;
    const std::vector<std::string> who = playerTokensFor(participants);
    std::vector<std::string> names;
    for (size_t i = 0; i < participants.size(); ++i) {
        if (participants[i].side == winner) names.push_back(who[i]);
    }
    if (names.empty()) return winner == 0u ? "Side 1 wins" : "Side 2 wins";
    return joinLines(names, " + ") + (names.size() > 1u ? " win" : " wins");
}

bool recordedRoundTallies(const ReplayEntry& entry,
                          unsigned& sideZero, unsigned& sideOne) {
    sideZero = 0u;
    sideOne = 0u;
    if (!entry.isV2 || !entry.hasEndV2 || !entry.hasFooter) return false;
    sideZero = static_cast<unsigned>(entry.endV2.roundTallies[0]);
    sideOne = static_cast<unsigned>(entry.endV2.roundTallies[1]);
    if (sideZero == 0u && sideOne == 0u
        && entry.endV2.winningSideMask != 0u) {
        return false;
    }
    return true;
}

const char* bootstrapKindText(BootstrapKind kind) {
    switch (kind) {
        case BootstrapKind::NetworkVsDirectBootV1: return "Network VS direct boot";
        case BootstrapKind::OfflineVsDirectBootV1: return "Offline VS direct boot";
        case BootstrapKind::NativeOfflineMenuS0V1: return "Native offline menu";
    }
    return "Unknown bootstrap";
}

const char* cpuContractText(CpuContract contract) {
    switch (contract) {
        case CpuContract::None: return "All human";
        case CpuContract::NativeAiDeterministicV1: return "Native AI (deterministic)";
        case CpuContract::EffectiveFighterV1: return "Captured effective actions";
    }
    return "Unknown";
}

const char* inputDomainText(InputDomain domain) {
    switch (domain) {
        case InputDomain::LogicalSourceV1: return "Logical source lanes";
        case InputDomain::EffectiveFighterV1: return "Effective fighter actions";
    }
    return "Unknown";
}

const char* ownershipText(SlotOwnership ownership) {
    switch (ownership) {
        case SlotOwnership::Inactive: return "inactive";
        case SlotOwnership::RecordedHuman: return "human";
        case SlotOwnership::NativeCpu: return "CPU";
    }
    return "unknown";
}

std::wstring directoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring baseNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1u);
}

bool hasFinalReplayExtension(const std::wstring& path) {
    static constexpr wchar_t kExtension[] = L".q99r";
    constexpr size_t kExtensionChars = 5u;
    if (path.size() < kExtensionChars) return false;
    const size_t start = path.size() - kExtensionChars;
    for (size_t i = 0u; i < kExtensionChars; ++i) {
        wchar_t value = path[start + i];
        if (value >= L'A' && value <= L'Z')
            value = static_cast<wchar_t>(value - L'A' + L'a');
        if (value != kExtension[i]) return false;
    }
    return true;
}

}

const char* browserActionText(BrowserAction action) {
    switch (action) {
        case BrowserAction::WatchSet: return "Watch set";
        case BrowserAction::WatchMatch: return "Watch match";
        case BrowserAction::StartAtRound: return "Start at round";
        case BrowserAction::VerifyFile: return "Verify file";
        case BrowserAction::VerifySimulation: return "Verify simulation";
        case BrowserAction::Rename: return "Rename";
        case BrowserAction::ToggleFavorite: return "Favorite";
        case BrowserAction::EditTags: return "Edit tags";
        case BrowserAction::EditNote: return "Edit note";
        case BrowserAction::ExportMatch: return "Export match";
        case BrowserAction::ImportExternal: return "Open external";
        case BrowserAction::RevealFolder: return "Reveal folder";
        case BrowserAction::MoveToTrash: return "Move to Trash";
        case BrowserAction::UndoTrash: return "Undo Trash";
        case BrowserAction::PermanentPurge: return "Delete permanently";
        case BrowserAction::ClearFilters: return "Clear filters";
        case BrowserAction::Refresh: return "Refresh";
        case BrowserAction::Count: break;
    }
    return "";
}

const char* actionDisableReasonText(ActionDisableReason reason) {
    switch (reason) {
        case ActionDisableReason::None: return "";
        case ActionDisableReason::NoSelection: return "no replay selected";
        case ActionDisableReason::ScanInProgress: return "the library is still being scanned";
        case ActionDisableReason::ActivelyRecording: return "a recorder is still writing this file";
        case ActionDisableReason::NotWatchable: return "this replay cannot be played by this build";
        case ActionDisableReason::NoMatchSelected: return "no match selected";
        case ActionDisableReason::NoRoundIndex: return "this match has no indexed rounds";
        case ActionDisableReason::NoIndependentBootstrap: return "this match has no independent bootstrap";
        case ActionDisableReason::NothingToUndo: return "nothing was moved to Trash";
        case ActionDisableReason::ReadOnlyLocation: return "the replay folder is not writable";
        case ActionDisableReason::ExternalReadOnly: return "external replays are read-only";
        case ActionDisableReason::FileMissing: return "the file is no longer there";
        case ActionDisableReason::FileChangedSinceScan: return "the file changed since it was scanned";
        case ActionDisableReason::Corrupt: return "the file is corrupt";
        case ActionDisableReason::Unsupported: return "this format or contract is not supported";
        case ActionDisableReason::ContentMismatch: return "installed game content differs";
        case ActionDisableReason::Incomplete: return "the recording is incomplete and cannot be played";
        case ActionDisableReason::NoAnnotationStore: return "annotations are unavailable";
        case ActionDisableReason::NoFilterActive: return "no filters are active";
        case ActionDisableReason::SimulationNotImplemented: return "simulation verification is not available yet";
        case ActionDisableReason::SeekNotImplemented: return "start-at-round requires replay seeking, which is not available in this release";
        case ActionDisableReason::NoGameConfigured: return "no installed game is configured";
        case ActionDisableReason::InTrash: return "this replay is in the Trash";
        case ActionDisableReason::NotInTrash: return "only a replay already in the Trash can be permanently deleted";
    }
    return "";
}

const char* actionDisableReasonPlainText(ActionDisableReason reason) {
    switch (reason) {
        case ActionDisableReason::None: return "";
        case ActionDisableReason::NoSelection: return "pick a replay first";
        case ActionDisableReason::ScanInProgress: return "the replay folder is still being read";
        case ActionDisableReason::ActivelyRecording: return "this match is still being recorded";
        case ActionDisableReason::NotWatchable: return "this replay cannot be played here";
        case ActionDisableReason::NoMatchSelected: return "this recording has no separate matches";
        case ActionDisableReason::NoRoundIndex: return "this match has no round markers";
        case ActionDisableReason::NoIndependentBootstrap: return "this match can only be saved together with the ones before it";
        case ActionDisableReason::NothingToUndo: return "nothing was moved to the Trash";
        case ActionDisableReason::ReadOnlyLocation: return "the replay folder cannot be written to";
        case ActionDisableReason::ExternalReadOnly: return "replays opened from outside the library are read-only";
        case ActionDisableReason::FileMissing: return "the file is no longer there";
        case ActionDisableReason::FileChangedSinceScan: return "the file changed on disk; press R to refresh";
        case ActionDisableReason::Corrupt: return "this file is damaged";
        case ActionDisableReason::Unsupported: return "this was recorded by a different version of the game";
        case ActionDisableReason::ContentMismatch: return "your game files are different from the recorded ones";
        case ActionDisableReason::Incomplete: return "the recording stops partway through";
        case ActionDisableReason::NoAnnotationStore: return "titles, notes and favourites are unavailable";
        case ActionDisableReason::NoFilterActive: return "no search or filter is active";
        case ActionDisableReason::SimulationNotImplemented: return "this check is not available yet";
        case ActionDisableReason::SeekNotImplemented: return "starting inside a match is not available yet";
        case ActionDisableReason::NoGameConfigured: return "set your QOH99 folder in Settings > Player and files";
        case ActionDisableReason::InTrash: return "this replay is in the Trash";
        case ActionDisableReason::NotInTrash: return "move it to the Trash first; only then can it be deleted for good";
    }
    return "";
}

std::string ReplayBrowserModel::badgeTextFor(CompatibilityState state) {
    return compatibilityStateText(state);
}

std::string ReplayBrowserModel::formatFrames(uint32_t frames) {
    const double seconds = static_cast<double>(frames) / kNominalHz;
    const unsigned total = static_cast<unsigned>(seconds + 0.5);
    char buffer[32];
    if (total >= 3600u) {
        std::snprintf(buffer, sizeof(buffer), "%u:%02u:%02u", total / 3600u,
                      (total / 60u) % 60u, total % 60u);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%u:%02u", total / 60u,
                      total % 60u);
    }
    return buffer;
}

std::string ReplayBrowserModel::formatUnixDate(int64_t unixSeconds) {
    if (unixSeconds <= 0) return "unknown";
    std::tm local{};
    const __time64_t stamp = static_cast<__time64_t>(unixSeconds);
    if (_localtime64_s(&local, &stamp) != 0) return "unknown";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min);
    return buffer;
}

std::string ReplayBrowserModel::formatBytes(uint64_t bytes) {
    char buffer[32];
    if (bytes >= 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MiB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KiB",
                      static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

std::vector<RosterParticipant> ReplayBrowserModel::buildRoster(
    const std::array<FighterSlotDescriptor, kFighterSlots>& slots,
    const std::array<uint8_t, kFighterSlots>& route,
    const std::array<SourceLaneDescriptor, kSourceLanes>& lanes) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();
    std::vector<RosterParticipant> participants;
    for (size_t i = 0; i < kFighterSlots; ++i) {
        const FighterSlotDescriptor& slot = slots[i];
        if (slot.ownership == SlotOwnership::Inactive) continue;
        const uint8_t id = slot.identity.nativeCharacterId;
        // Internal partner/form actors are never participants: Edifel 34, Kanako 35, C-Serika 36.
        if (catalog.isInternalFormId(id)) continue;
        if (id == kInactiveCharacterId) continue;

        RosterParticipant participant{};
        participant.slot = static_cast<uint8_t>(i);
        participant.side = static_cast<uint8_t>(i % 2u);
        participant.sourceLane = route[i] < kSourceLanes ? route[i] : 0u;
        participant.characterId = id;
        participant.palette = slot.identity.paletteIndex;
        participant.ownership = slot.ownership;
        participant.displayName = sanitizeForDisplay(catalog.displayName(id), 64u);
        if (participant.sourceLane < kSourceLanes) {
            participant.laneName = sanitizeForDisplay(
                lanes[participant.sourceLane].name, kMaxLabelBytes);
        }
        const CharacterEntry* entry = catalog.find(id);
        participant.unknownId = entry == nullptr;
        if (entry != nullptr) {
            participant.hiddenColumn = entry->hiddenColumn;
            participant.standAlternate = entry->standAlternate;
        }
        participant.selectorProofPresent = slot.selectorProofPresent;
        if (slot.selectorProofPresent) {
            participant.selectorProofVerified =
                catalog.verifySelectorProof(slot.selectorProof, id);
        }
        participants.push_back(std::move(participant));
    }
    return participants;
}

std::vector<RosterParticipant> ReplayBrowserModel::buildRosterV2(
    const std::array<FighterSlotDescriptorV2, kFighterSlots>& slots,
    const std::array<uint8_t, kFighterSlots>& route,
    const std::array<SourceLaneDescriptorV2, kSourceLanes>& lanes) {
    const CharacterCatalog& catalog = CharacterCatalog::forSeBuild();
    std::vector<RosterParticipant> participants;
    for (size_t i = 0; i < kFighterSlots; ++i) {
        const FighterSlotDescriptorV2& slot = slots[i];
        if (slot.ownership == SlotOwnership::Inactive) continue;
        const uint8_t id = slot.identity.nativeCharacterId;
        if (catalog.isInternalFormId(id)
            || id == kInactiveCharacterId) {
            continue;
        }
        RosterParticipant participant{};
        participant.slot = static_cast<uint8_t>(i);
        participant.side = slot.side;
        participant.sourceLane =
            route[i] < kSourceLanes ? route[i] : 0u;
        participant.characterId = id;
        participant.palette = slot.identity.paletteIndex;
        participant.ownership = slot.ownership;
        participant.displayName =
            sanitizeForDisplay(catalog.displayName(id), 64u);
        if (participant.sourceLane < kSourceLanes) {
            participant.laneName = sanitizeForDisplay(
                fixedTextV2(lanes[participant.sourceLane].name),
                kMaxLabelBytes);
        }
        const CharacterEntry* character = catalog.find(id);
        participant.unknownId = character == nullptr;
        if (character != nullptr) {
            participant.hiddenColumn = character->hiddenColumn;
            participant.standAlternate = character->standAlternate;
        }
        participant.selectorProofPresent =
            slot.selectorProofPresent;
        if (slot.selectorProofPresent) {
            participant.selectorProofVerified =
                catalog.verifySelectorProof(slot.selectorProof, id);
        }
        participants.push_back(std::move(participant));
    }
    return participants;
}

std::string ReplayBrowserModel::matchupTextFor(
    const std::vector<RosterParticipant>& participants) {
    std::vector<std::string> sideZero;
    std::vector<std::string> sideOne;
    for (const RosterParticipant& participant : participants) {
        std::string label = participant.displayName;
        if (participant.ownership == SlotOwnership::NativeCpu) label += " (CPU)";
        if (participant.side == 0u) sideZero.push_back(label);
        else sideOne.push_back(label);
    }
    if (sideZero.empty() && sideOne.empty()) return "no roster";
    return joinLines(sideZero, " + ") + " vs " + joinLines(sideOne, " + ");
}

std::string ReplayBrowserModel::matchTitleTextFor(
    const std::vector<RosterParticipant>& participants) {
    const std::vector<std::string> who = playerTokensFor(participants);
    std::vector<std::string> sideZero;
    std::vector<std::string> sideOne;
    for (size_t i = 0; i < participants.size(); ++i) {
        const RosterParticipant& participant = participants[i];
        std::string token = who[i];
        token += " (" + participant.displayName + ")";
        if (participant.side == 0u) sideZero.push_back(std::move(token));
        else sideOne.push_back(std::move(token));
    }
    if (sideZero.empty() && sideOne.empty()) return "no roster";
    if (sideOne.empty())
        return joinLines(sideZero, " + ") + " vs (no opponent recorded)";
    if (sideZero.empty())
        return "(no opponent recorded) vs " + joinLines(sideOne, " + ");
    return joinLines(sideZero, " + ") + " vs " + joinLines(sideOne, " + ");
}

ReplayBrowserModel::ReplayBrowserModel() = default;
ReplayBrowserModel::~ReplayBrowserModel() = default;

void ReplayBrowserModel::setAnnotations(ReplayAnnotationStore* annotations) {
    annotations_ = annotations;
    refreshView();
}

void ReplayBrowserModel::setRuntimeIdentity(const RuntimeIdentity& identity) {
    runtime_ = identity;
    rebuildStates();
    rebuildRows();
    rebuildDetails();
}

void ReplayBrowserModel::setRuntimeIdentityV2(
    const ReplayRuntimeIdentityV2& identity) {
    runtimeV2_ = identity;
    rebuildStates();
    rebuildRows();
    rebuildDetails();
}

void ReplayBrowserModel::setReplayRoot(const std::wstring& root) {
    replayRoot_ = root;
}

void ReplayBrowserModel::setScanning(bool scanning) { scanning_ = scanning; }

void ReplayBrowserModel::setIdentityPending(bool pending) {
    if (identityPending_ == pending) return;
    identityPending_ = pending;
    rebuildRows();
    rebuildDetails();
}

void ReplayBrowserModel::setSnapshot(
    std::shared_ptr<const ReplaySnapshot> snapshot) {
    snapshot_ = std::move(snapshot);
    cancelPendingAction();
    rebuildStates();
    rebuildRows();
    rebuildDetails();
}

void ReplayBrowserModel::setQuery(const std::string& query) {
    query_ = query;
    refreshView();
}

void ReplayBrowserModel::setSort(SortKey sort) {
    sort_ = sort;
    refreshView();
}

void ReplayBrowserModel::refreshView() {
    rebuildRows();
    rebuildDetails();
}

ReplayIdentitySnapshot ReplayBrowserModel::snapshotFor(
    const ReplayEntry& entry) const {
    ReplayIdentitySnapshot file{};
    file.prefix = entry.prefix;
    file.contract = entry.manifest.contract;
    file.structurallyValid = entry.parsed && entry.hasManifest;
    file.hasFooter = entry.hasFooter;
    file.hasSessionStart = entry.hasSessionStart;
    file.activelyRecording = false;
    file.recoverable = false;
    file.activeFighterCount = entry.activeFighterCount();
    file.cpuContract = entry.manifest.cpuContract;
    file.usesNonIdentityRoute = entry.usesNonIdentityRoute();
    file.inputDomain = entry.manifest.cpuContract == CpuContract::EffectiveFighterV1
        ? InputDomain::EffectiveFighterV1
        : InputDomain::LogicalSourceV1;

    const ReplayAnnotation* annotation = annotationFor(entry);
    if (annotation != nullptr && annotation->simulationVerified) {
        file.simulationVerified =
            annotation->verifiedContentRoot == runtime_.contentManifestRoot
            && annotation->verifiedRuntimeSimulationId
                == runtime_.runtimeSimulationId;
    }
    return file;
}

void ReplayBrowserModel::rebuildStates() {
    states_.clear();
    results_.clear();
    if (!snapshot_) return;
    states_.reserve(snapshot_->entries.size());
    results_.reserve(snapshot_->entries.size());
    for (const ReplayEntry& entry : snapshot_->entries) {
        const CompatibilityResult result = entry.isV2
            ? browserCompatibilityV2(entry, runtimeV2_)
            : evaluate(snapshotFor(entry), runtime_);
        results_.push_back(result);
        states_.push_back(result.state);
    }
}

const ReplayAnnotation* ReplayBrowserModel::annotationFor(
    const ReplayEntry& entry) const {
    if (annotations_ == nullptr) return nullptr;
    return annotations_->find(entry.prefix.sessionUuidLow,
                              entry.prefix.sessionUuidHigh);
}

void ReplayBrowserModel::rebuildRows() {
    rows_.clear();
    visible_.clear();
    if (!snapshot_) {
        selectedIndex_ = 0u;
        return;
    }

    QueryInputs inputs{};
    inputs.snapshot = snapshot_.get();
    inputs.annotations = annotations_;
    inputs.states = &states_;
    inputs.query = query_;
    inputs.filter = filter_;
    inputs.sort = sort_;
    visible_ = applyQuery(inputs);

    rows_.reserve(visible_.size());
    for (size_t index : visible_) {
        const ReplayEntry& entry = snapshot_->entries[index];
        const CompatibilityResult& result = index < results_.size()
            ? results_[index]
            : CompatibilityResult{};
        const ReplayAnnotation* annotation = annotationFor(entry);

        BrowserRow row{};
        row.stableId = entry.stableId;
        row.entryIndex = index;
        row.dateText = formatUnixDate(entry.recordedUnixSeconds());
        row.fileNameText = sanitizeForDisplay(entry.fileName, 96u);
        row.favorite = annotation != nullptr && annotation->favorite;
        row.hasLocalTitle = annotation != nullptr && !annotation->title.empty();
        row.isPart = entry.isPart;
        row.inTrash = entry.inTrash;
        row.state = result.state;
        row.reason = result.reason;
        row.watchable = result.watchable;
        row.statusBadge = badgeTextFor(result.state);
        row.statusDetail = sanitizeForDisplay(result.detail, 160u);
        row.verificationMode = entry.isV2
            ? replayVerificationModeForV2(entry.startV2.manifest.identity)
            : ReplayVerificationModeV2::ByteExact;
        {
            const PlainStatus plain = plainStatusFor(
                result.state, result.watchable, row.verificationMode,
                identityPending_);
            row.statusShort = plain.shortText;
            row.statusTone = plain.tone;
        }

        if (row.hasLocalTitle) {
            row.titleText = sanitizeForDisplay(annotation->title, kMaxLabelBytes);
        } else if (entry.isV2
                   && entry.startV2.manifest.display.title.bytes != 0u) {
            row.titleText = sanitizeForDisplay(
                fixedTextV2(entry.startV2.manifest.display.title),
                kMaxLabelBytes);
        } else if (!entry.manifest.display.title.empty()) {
            row.titleText =
                sanitizeForDisplay(entry.manifest.display.title, kMaxLabelBytes);
        } else {
            row.titleText = row.fileNameText;
        }

        const std::vector<RosterParticipant> participants = entry.isV2
            ? buildRosterV2(
                entry.startV2.manifest.slots,
                entry.startV2.manifest.slotToSourceRoute,
                entry.startV2.manifest.lanes)
            : buildRoster(
                entry.manifest.slots,
                entry.manifest.slotToSourceRoute,
                entry.manifest.lanes);
        row.matchupText = matchupTextFor(participants);
        row.matchTitleText = matchTitleTextFor(participants);
        row.matchLabelText = row.hasLocalTitle
            ? row.titleText
            : (participants.empty() ? row.fileNameText : row.matchTitleText);
        row.fighterCount = static_cast<uint8_t>(participants.size());
        row.humanCount = entry.humanFighterCount();
        row.cpuCount = entry.cpuFighterCount();
        row.sourceText = !entry.hasManifest
            ? "Unknown"
            : entry.isV2
                ? sourceKindTextV2(
                    entry.startV2.manifest.display.sourceKind)
                : sourceKindText(entry.manifest.display.sourceKind);
        row.durationText = formatFrames(entry.durationFrames());

        unsigned sideZeroRounds = 0u;
        unsigned sideOneRounds = 0u;
        if (recordedRoundTallies(entry, sideZeroRounds, sideOneRounds)) {
            char score[32];
            std::snprintf(score, sizeof(score), "%u-%u",
                          sideZeroRounds, sideOneRounds);
            row.scoreText = score;
        } else {
            row.scoreText = "...";
        }
        rows_.push_back(std::move(row));
    }

    if (selectedStableId_ != 0u) {
        bool found = false;
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].stableId == selectedStableId_) {
                selectedIndex_ = i;
                found = true;
                break;
            }
        }
        if (!found)
            selectedIndex_ = rows_.empty() ? 0u : std::min(selectedIndex_, rows_.size() - 1u);
    }
    if (selectedIndex_ >= rows_.size())
        selectedIndex_ = rows_.empty() ? 0u : rows_.size() - 1u;
    if (!rows_.empty()) selectedStableId_ = rows_[selectedIndex_].stableId;
    else selectedStableId_ = 0u;
}

const BrowserRow& ReplayBrowserModel::row(size_t index) const {
    if (index >= rows_.size()) return kEmptyRow;
    return rows_[index];
}

uint32_t ReplayBrowserModel::totalFileCount() const {
    return snapshot_ ? static_cast<uint32_t>(snapshot_->entries.size()) : 0u;
}

std::string ReplayBrowserModel::emptyStateText() const {
    if (!snapshot_) return "Replay library has not been scanned yet.";
    if ((scanning_ || !snapshot_->complete) && snapshot_->entries.empty())
        return "Scanning the replay folder...";
    if (snapshot_->entries.empty()) {
        std::string text = "No replays yet. Recordings appear here after a "
                           "netplay or offline VS session.";
        if (!snapshot_->rootWritable) {
            text += " The configured replay folder is not writable, so a "
                    "per-user folder is being used instead.";
        }
        return text;
    }
    if (filter_.active() || !query_.empty())
        return "No replays match the current search and filters.";
    return "No replays to show.";
}

bool ReplayBrowserModel::select(size_t index) {
    if (index >= rows_.size()) return false;
    selectedIndex_ = index;
    selectedStableId_ = rows_[index].stableId;
    expandedMatch_ = 0u;
    rebuildDetails();
    return true;
}

bool ReplayBrowserModel::selectByStableId(uint64_t stableId) {
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].stableId == stableId) return select(i);
    }
    return false;
}

bool ReplayBrowserModel::moveSelection(int delta) {
    if (rows_.empty()) return false;
    long long index = static_cast<long long>(selectedIndex_) + delta;
    if (index < 0) index = 0;
    if (index >= static_cast<long long>(rows_.size()))
        index = static_cast<long long>(rows_.size()) - 1;
    return select(static_cast<size_t>(index));
}

uint64_t ReplayBrowserModel::selectedStableId() const {
    return selectedStableId_;
}

void ReplayBrowserModel::expandMatch(uint32_t ordinal) {
    expandedMatch_ = ordinal;
}

const ReplayEntry* ReplayBrowserModel::entryForRow(size_t index) const {
    if (!snapshot_ || index >= rows_.size()) return nullptr;
    const size_t entry = rows_[index].entryIndex;
    if (entry >= snapshot_->entries.size()) return nullptr;
    return &snapshot_->entries[entry];
}

const ReplayEntry* ReplayBrowserModel::selectedEntry() const {
    if (!snapshot_ || selectedIndex_ >= rows_.size()) return nullptr;
    const size_t index = rows_[selectedIndex_].entryIndex;
    if (index >= snapshot_->entries.size()) return nullptr;
    return &snapshot_->entries[index];
}

void ReplayBrowserModel::rebuildDetails() {
    details_ = BrowserDetails{};
    const ReplayEntry* entry = selectedEntry();
    if (entry == nullptr) return;
    const size_t index = rows_[selectedIndex_].entryIndex;
    const CompatibilityResult& result =
        index < results_.size() ? results_[index] : CompatibilityResult{};
    const ReplayAnnotation* annotation = annotationFor(*entry);

    details_.valid = true;
    details_.stableId = entry->stableId;
    details_.recordedTitleText = entry->isV2
        ? sanitizeForDisplay(
            fixedTextV2(entry->startV2.manifest.display.title),
            kMaxLabelBytes)
        : sanitizeForDisplay(
            entry->manifest.display.title, kMaxLabelBytes);
    details_.titleText = annotation != nullptr && !annotation->title.empty()
        ? sanitizeForDisplay(annotation->title, kMaxLabelBytes)
        : details_.recordedTitleText;
    if (details_.titleText.empty())
        details_.titleText = sanitizeForDisplay(entry->fileName, 96u);
    details_.dateText = formatUnixDate(entry->recordedUnixSeconds());
    details_.sourceText = !entry->hasManifest
        ? "Unknown"
        : entry->isV2
            ? sourceKindTextV2(
                entry->startV2.manifest.display.sourceKind)
            : sourceKindText(entry->manifest.display.sourceKind);
    details_.sourceFriendlyText = !entry->hasManifest
        ? "Unknown source"
        : entry->isV2
            ? friendlySourceKindTextV2(
                entry->startV2.manifest.display.sourceKind)
            : friendlySourceKindText(entry->manifest.display.sourceKind);
    details_.bootstrapText = entry->isV2
        ? "Match-active direct boot"
        : bootstrapKindText(entry->manifest.bootstrapKind);
    details_.statusBadge = badgeTextFor(result.state);
    details_.statusDetail = sanitizeForDisplay(result.detail, 240u);
    details_.verificationMode = entry->isV2
        ? replayVerificationModeForV2(entry->startV2.manifest.identity)
        : ReplayVerificationModeV2::ByteExact;
    {
        const PlainStatus plain = plainStatusFor(
            result.state, result.watchable, details_.verificationMode,
            identityPending_);
        details_.statusShort = plain.shortText;
        details_.statusTone = plain.tone;
        details_.watchabilityText = plain.watchability;
    }
    const CpuContract cpuContract = entry->isV2
        ? entry->startV2.manifest.cpuContract
        : entry->manifest.cpuContract;
    details_.cpuContractText = cpuContractText(cpuContract);
    details_.inputDomainText = inputDomainText(
        cpuContract == CpuContract::EffectiveFighterV1
            ? InputDomain::EffectiveFighterV1
            : InputDomain::LogicalSourceV1);
    details_.durationText = formatFrames(entry->durationFrames());
    details_.matchCount = entry->matchCount;
    details_.totalFrames = entry->totalFrames;
    details_.matchesTruncated = entry->matchesTruncated;

    {
        unsigned sideZeroRounds = 0u;
        unsigned sideOneRounds = 0u;
        if (recordedRoundTallies(*entry, sideZeroRounds, sideOneRounds)) {
            char buffer[80];
            // qoh99.exe's config table at 0x4A8A7C has three entries; 2 is the largest roundsToWinMinusOne.
            const uint32_t roundsToWinMinusOne =
                entry->startV2.start.configuration.roundsToWinMinusOne;
            if (roundsToWinMinusOne <= qnetplay::kMaxRoundsToWinMinusOne) {
                std::snprintf(buffer, sizeof(buffer),
                              "Rounds %u-%u (first to %u)",
                              sideZeroRounds, sideOneRounds,
                              static_cast<unsigned>(roundsToWinMinusOne) + 1u);
            } else {
                std::snprintf(buffer, sizeof(buffer), "Rounds %u-%u",
                              sideZeroRounds, sideOneRounds);
            }
            details_.scoreText = buffer;
        } else {
            details_.scoreText = "Round score not recorded";
        }
    }
    if (entry->isV2) {
        const uint32_t generation =
            entry->startV2.manifest.identity.gameGeneration;
        details_.seriesText = generation > 1u
            ? "Game " + std::to_string(generation)
                  + " of this session. The set score is not recorded in this "
                    "file."
            : std::string("First game of this session.");
    }

    if (annotation != nullptr && annotation->structuralVerified) {
        details_.integrityText = "File intact - verified "
            + formatUnixDate(annotation->structuralVerifiedUnix);
    } else if (entry->hasFooter) {
        details_.integrityText =
            "Structure indexed; run Verify file to check the digest";
    } else if (entry->isV2) {
        details_.integrityText =
            "Incomplete recording: the completed-game footer is missing; "
            "this file cannot be played";
    } else {
        details_.integrityText =
            "Incomplete recording: no finalized footer, digest, match index, "
            "or final-state proof; this file cannot be played";
    }
    if (annotation != nullptr && annotation->simulationVerified) {
        details_.simulationVerificationText = "Simulation verified "
            + formatUnixDate(annotation->simulationVerifiedUnix);
        details_.recordedSummaryOnly = false;
    } else {
        details_.simulationVerificationText =
            "Simulation not verified (recorded summary only)";
    }
    details_.verificationText = plainVerificationText(
        details_.verificationMode, !details_.recordedSummaryOnly);
    if (annotation != nullptr && annotation->structuralVerified) {
        details_.fileCheckText = "This file's contents were checked on "
            + formatUnixDate(annotation->structuralVerifiedUnix) + ".";
    } else if (entry->hasFooter) {
        details_.fileCheckText =
            "The file's contents haven't been checked yet.";
    }

    details_.storageText = sanitizeForDisplay(entry->fileName, 128u);
    details_.sizeText = formatBytes(entry->identity.sizeBytes);
    details_.participants = entry->isV2
        ? buildRosterV2(
            entry->startV2.manifest.slots,
            entry->startV2.manifest.slotToSourceRoute,
            entry->startV2.manifest.lanes)
        : buildRoster(
            entry->manifest.slots,
            entry->manifest.slotToSourceRoute,
            entry->manifest.lanes);

    {
        std::vector<std::string> parts;
        for (size_t i = 0; i < kFighterSlots; ++i) {
            const uint8_t route = entry->isV2
                ? entry->startV2.manifest.slotToSourceRoute[i]
                : entry->manifest.slotToSourceRoute[i];
            const SlotOwnership ownership = entry->isV2
                ? entry->startV2.manifest.slots[i].ownership
                : entry->manifest.slots[i].ownership;
            char buffer[48];
            std::snprintf(buffer, sizeof(buffer), "slot %u -> lane %u (%s)",
                          static_cast<unsigned>(i),
                          static_cast<unsigned>(route),
                          ownershipText(ownership));
            parts.push_back(buffer);
        }
        details_.routeText = joinLines(parts, ", ");
    }

    const bool networkMetadata = entry->isV2
        ? (entry->startV2.manifest.display.flags
            & DisplayMetadataNetworkV2) != 0u
        : entry->manifest.display.networkMetadataPresent;
    if (networkMetadata) {
        const uint16_t delayOne = entry->isV2
            ? entry->startV2.manifest.display.playerOneDelay
            : entry->manifest.display.playerOneDelay;
        const uint16_t delayTwo = entry->isV2
            ? entry->startV2.manifest.display.playerTwoDelay
            : entry->manifest.display.playerTwoDelay;
        const uint16_t rollback = entry->isV2
            ? entry->startV2.manifest.display.rollbackWindow
            : entry->manifest.display.rollbackWindow;
        const uint32_t rtt = entry->isV2
            ? entry->startV2.manifest.display.averageRttMicros
            : entry->manifest.display.averageRttMicros;
        const uint32_t loss = entry->isV2
            ? entry->startV2.manifest.display.lossPermille
            : entry->manifest.display.lossPermille;
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer),
                      "delay %u/%u, rollback %u, RTT %.1f ms, loss %.1f%%",
                      static_cast<unsigned>(delayOne),
                      static_cast<unsigned>(delayTwo),
                      static_cast<unsigned>(rollback),
                      static_cast<double>(rtt) / 1000.0,
                      static_cast<double>(loss) / 10.0);
        details_.networkText = buffer;
    } else {
        details_.networkText = "network diagnostics not recorded";
    }

    if (entry->isV2 && entry->startV2.manifest.stage.verified) {
        details_.stageText =
            "stage native ID "
            + std::to_string(
                entry->startV2.manifest.stage.nativeStageId)
            + " (catalog verified)";
    } else {
        details_.stageText = "stage: not verified metadata";
    }

    if (!entry->matches.empty()) {
        const MatchSpan& first = entry->matches.front();
        if (entry->isV2) {
            details_.firstPlayableText =
                "this file is one independently bootable game; playback "
                "starts at confirmed replay frame 0";
        } else if (first.independentBootstrap) {
            details_.firstPlayableText = "every indexed match has its own "
                                         "bootstrap";
        } else {
            char buffer[96];
            std::snprintf(buffer, sizeof(buffer),
                          "playback starts from match %u",
                          static_cast<unsigned>(first.dependsOnMatchId));
            details_.firstPlayableText = buffer;
        }
    } else if (!entry->hasFooter) {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer),
                      "last valid frame %u at offset %llu",
                      static_cast<unsigned>(entry->lastTimelineFrame),
                      static_cast<unsigned long long>(entry->parseOffset));
        details_.firstPlayableText = buffer;
    }

    if (result.reason != CompatibilityReason::None) {
        details_.incompatibilityText = std::string(
            compatibilityReasonText(result.reason));
        if (!details_.statusDetail.empty())
            details_.incompatibilityText += ": " + details_.statusDetail;
    }
    if (!entry->parsed && !entry->parseDetail.empty()) {
        details_.warnings.push_back(
            std::string(parseStatusText(entry->parseStatus)) + " - "
            + sanitizeForDisplay(entry->parseDetail, 160u));
    }
    for (const RosterParticipant& participant : details_.participants) {
        if (participant.unknownId) {
            details_.warnings.push_back(
                participant.displayName
                + " is not in this build's roster catalog");
        }
        if (participant.selectorProofPresent
            && !participant.selectorProofVerified) {
            details_.warnings.push_back(
                participant.displayName
                + " has a selector proof that does not resolve to its "
                  "recorded identity");
        }
    }
    if (entry->matchesTruncated) {
        details_.warnings.push_back(
            "only the first indexed matches are shown; select Verify file to "
            "read the whole index");
    }

    details_.identityLines.push_back(
        "executable " + hexOf(entry->manifest.contract.executableSha256, 16u));
    details_.identityLines.push_back(
        "content " + hexOf(entry->manifest.contract.contentManifestRoot, 16u));
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer),
                      "runtime %016llx  state %016llx  catalog %016llx",
                      static_cast<unsigned long long>(
                          entry->manifest.contract.runtimeSimulationId),
                      static_cast<unsigned long long>(
                          entry->manifest.contract.stateRegionFingerprint),
                      static_cast<unsigned long long>(
                          entry->manifest.contract.characterCatalogId));
        details_.identityLines.push_back(buffer);
    }
    if (entry->hasFooter) {
        details_.identityLines.push_back(
            "file digest " + hexOf(entry->fileDigest, 16u));
    }

    if (annotation != nullptr) {
        details_.noteText = sanitizeForDisplay(annotation->note, kMaxNoteBytes);
        for (const std::string& tag : annotation->tags)
            details_.tags.push_back(sanitizeForDisplay(tag, kMaxTagBytes));
        if (annotation->hasProvenance) {
            details_.warnings.push_back(
                "recovered from " + annotation->sourceFileName);
            details_.provenanceText =
                "Made from " + annotation->sourceFileName + ".";
        }
    }

    for (const MatchSpan& span : entry->matches) {
        BrowserMatchRow match{};
        match.matchId = span.matchId;
        match.ordinal = span.ordinal;
        match.firstTimelineFrame = span.firstTimelineFrame;
        match.lastTimelineFrame = span.lastTimelineFrame;
        match.frameCount = span.frameCount;
        match.roundCount = span.roundCount;
        match.winningSideMask = span.winningSideMask;
        match.independentBootstrap = span.independentBootstrap;
        match.complete = span.hasEnd;
        match.durationText = formatFrames(span.frameCount);
        if (span.winningSideMask == 0u) {
            match.resultText = span.hasEnd ? "draw / aborted" : "unfinished";
        } else {
            match.resultText = (span.winningSideMask & 0x05u) != 0u
                ? "side 1 (P1+P3) win"
                : "side 2 (P2+P4) win";
        }
        if (entry->isV2) {
            std::array<FighterSlotDescriptorV2, kFighterSlots> slots{};
            for (size_t i = 0; i < kFighterSlots; ++i) {
                slots[i].ownership = span.ownership[i];
                slots[i].side = entry->startV2.manifest.slots[i].side;
                slots[i].identity.nativeCharacterId =
                    span.roster[i].nativeCharacterId;
                slots[i].identity.paletteIndex = span.roster[i].paletteIndex;
            }
            match.participants = buildRosterV2(
                slots, entry->startV2.manifest.slotToSourceRoute,
                entry->startV2.manifest.lanes);
        } else {
            std::array<FighterSlotDescriptor, kFighterSlots> slots{};
            for (size_t i = 0; i < kFighterSlots; ++i) {
                slots[i].ownership = span.ownership[i];
                slots[i].side = static_cast<uint8_t>(i % 2u);
                slots[i].identity = span.roster[i];
            }
            match.participants = buildRoster(
                slots, entry->manifest.slotToSourceRoute,
                entry->manifest.lanes);
        }
        match.matchupText = matchupTextFor(match.participants);
        match.outcomeText = matchOutcomeTextFor(
            match.participants, span.winningSideMask, span.hasEnd,
            span.endReason);
        details_.matches.push_back(std::move(match));
    }
}

BrowserViewState ReplayBrowserModel::captureViewState() const {
    BrowserViewState state{};
    state.query = query_;
    state.filter = filter_;
    state.sort = sort_;
    state.selectedStableId = selectedStableId_;
    state.scrollTop = scrollTop_;
    state.expandedMatchOrdinal = expandedMatch_;
    return state;
}

void ReplayBrowserModel::restoreViewState(const BrowserViewState& state) {
    query_ = state.query;
    filter_ = state.filter;
    sort_ = state.sort;
    selectedStableId_ = state.selectedStableId;
    rebuildRows();
    if (state.selectedStableId != 0u) selectByStableId(state.selectedStableId);
    scrollTop_ = state.scrollTop;
    expandedMatch_ = state.expandedMatchOrdinal;
    rebuildDetails();
}

ActionState ReplayBrowserModel::actionState(BrowserAction action) const {
    ActionState state{};
    state.destructive = action == BrowserAction::MoveToTrash
        || action == BrowserAction::PermanentPurge
        || action == BrowserAction::Rename;
    state.requiresConfirmation = state.destructive;

    if (action == BrowserAction::Refresh) {
        state.enabled = true;
        state.reason = ActionDisableReason::None;
        return state;
    }
    if (action == BrowserAction::ClearFilters) {
        state.enabled = filter_.active() || !query_.empty();
        state.reason = state.enabled ? ActionDisableReason::None
                                     : ActionDisableReason::NoFilterActive;
        return state;
    }
    if (action == BrowserAction::ImportExternal) {
        state.enabled = true;
        return state;
    }
    if (action == BrowserAction::UndoTrash) {
        const ReplayEntry* selected = selectedEntry();
        state.enabled = undo_.available
            || (selected != nullptr && selected->inTrash
                && !replayRoot_.empty());
        state.reason = state.enabled ? ActionDisableReason::None
                                     : ActionDisableReason::NothingToUndo;
        return state;
    }

    const ReplayEntry* entry = selectedEntry();
    if (entry == nullptr) {
        state.enabled = false;
        state.reason = ActionDisableReason::NoSelection;
        return state;
    }
    const size_t index = rows_[selectedIndex_].entryIndex;
    const CompatibilityResult& result =
        index < results_.size() ? results_[index] : CompatibilityResult{};
    const bool recording = result.state == CompatibilityState::Recording;

    auto deny = [&state](ActionDisableReason reason) {
        state.enabled = false;
        state.reason = reason;
        return state;
    };

    switch (action) {
        case BrowserAction::WatchSet:
        case BrowserAction::WatchMatch:
        case BrowserAction::StartAtRound: {
            if (recording) return deny(ActionDisableReason::ActivelyRecording);
            if (entry->inTrash) return deny(ActionDisableReason::InTrash);
            if (!result.watchable) {
                switch (result.state) {
                    case CompatibilityState::Corrupt:
                        return deny(ActionDisableReason::Corrupt);
                    case CompatibilityState::Unsupported:
                        return deny(ActionDisableReason::Unsupported);
                    case CompatibilityState::ContentMismatch:
                        return deny(ActionDisableReason::ContentMismatch);
                    case CompatibilityState::Incomplete:
                    case CompatibilityState::Recoverable:
                        return deny(ActionDisableReason::Incomplete);
                    case CompatibilityState::FileIntact:
                        return deny(ActionDisableReason::NoGameConfigured);
                    default:
                        return deny(ActionDisableReason::NotWatchable);
                }
            }
            if (action == BrowserAction::WatchMatch
                && entry->matches.empty()) {
                return deny(ActionDisableReason::NoMatchSelected);
            }
            if (action == BrowserAction::StartAtRound && entry->isV2) {
                return deny(ActionDisableReason::SeekNotImplemented);
            }
            if (action == BrowserAction::StartAtRound) {
                bool anyRounds = false;
                for (const MatchSpan& span : entry->matches) {
                    if (span.roundCount != 0u) { anyRounds = true; break; }
                }
                if (!anyRounds) return deny(ActionDisableReason::NoRoundIndex);
            }
            state.enabled = true;
            return state;
        }
        case BrowserAction::VerifyFile: {
            if (recording) return deny(ActionDisableReason::ActivelyRecording);
            state.enabled = true;
            return state;
        }
        case BrowserAction::VerifySimulation: {
            if (recording) return deny(ActionDisableReason::ActivelyRecording);
            if (entry->inTrash) return deny(ActionDisableReason::InTrash);
            if (!result.watchable) return deny(ActionDisableReason::NotWatchable);
            if (!runtime_.known)
                return deny(ActionDisableReason::NoGameConfigured);
            state.enabled = true;
            return state;
        }
        case BrowserAction::Rename: {
            if (recording) return deny(ActionDisableReason::ActivelyRecording);
            if (entry->inTrash) return deny(ActionDisableReason::InTrash);
            state.enabled = true;
            return state;
        }
        case BrowserAction::ToggleFavorite:
        case BrowserAction::EditTags:
        case BrowserAction::EditNote: {
            if (annotations_ == nullptr)
                return deny(ActionDisableReason::NoAnnotationStore);
            state.enabled = true;
            return state;
        }
        case BrowserAction::ExportMatch: {
            if (recording) return deny(ActionDisableReason::ActivelyRecording);
            if (!entry->hasFooter) return deny(ActionDisableReason::Incomplete);
            if (entry->matches.empty())
                return deny(ActionDisableReason::NoMatchSelected);
            bool independent = false;
            for (const MatchSpan& span : entry->matches) {
                if (span.independentBootstrap) { independent = true; break; }
            }
            if (!independent)
                return deny(ActionDisableReason::NoIndependentBootstrap);
            if (replayRoot_.empty())
                return deny(ActionDisableReason::ReadOnlyLocation);
            state.enabled = true;
            return state;
        }
        case BrowserAction::RevealFolder: {
            state.enabled = true;
            return state;
        }
        case BrowserAction::MoveToTrash: {
            if (recording) return deny(ActionDisableReason::ActivelyRecording);
            if (entry->inTrash) return deny(ActionDisableReason::InTrash);
            if (replayRoot_.empty())
                return deny(ActionDisableReason::ReadOnlyLocation);
            state.enabled = true;
            return state;
        }
        case BrowserAction::PermanentPurge: {
            if (recording) return deny(ActionDisableReason::ActivelyRecording);
            if (!entry->inTrash) return deny(ActionDisableReason::NotInTrash);
            state.enabled = true;
            state.destructive = true;
            state.requiresConfirmation = true;
            return state;
        }
        default:
            break;
    }
    state.enabled = false;
    state.reason = ActionDisableReason::NoSelection;
    return state;
}

bool ReplayBrowserModel::revalidate(const ReplayEntry& entry,
                                    ActionOutcome& outcome) const {
    FileIdentity now{};
    if (!queryFileIdentity(entry.path, now)) {
        outcome.ok = false;
        outcome.reason = ActionDisableReason::FileMissing;
        outcome.message = actionDisableReasonText(outcome.reason);
        return false;
    }
    if (!now.unchanged(entry.identity)) {
        outcome.ok = false;
        outcome.reason = ActionDisableReason::FileChangedSinceScan;
        outcome.message = actionDisableReasonText(outcome.reason);
        return false;
    }
    return true;
}

ActionOutcome ReplayBrowserModel::requestAction(const ActionRequest& request) {
    ActionOutcome outcome{};
    outcome.action = request.action;

    const ActionState state = actionState(request.action);
    if (!state.enabled) {
        outcome.reason = state.reason;
        outcome.message = actionDisableReasonText(state.reason);
        return outcome;
    }

    switch (request.action) {
        case BrowserAction::Refresh:
        case BrowserAction::ClearFilters: {
            if (request.action == BrowserAction::ClearFilters) {
                filter_.clear();
                query_.clear();
                refreshView();
            }
            outcome.ok = true;
            return outcome;
        }
        case BrowserAction::UndoTrash:
            return doUndoTrash();
        case BrowserAction::ImportExternal: {
            ReplayEntry entry{};
            CompatibilityResult compatibility{};
            std::string message;
            outcome.ok = openExternal(request.path, entry, compatibility, message);
            outcome.message = message;
            if (outcome.ok) {
                outcome.watch.valid = compatibility.watchable;
                outcome.watch.path = entry.path;
                outcome.watch.identity = entry.identity;
                outcome.watch.stableId = entry.stableId;
                outcome.watch.compatibility = compatibility;
            } else {
                outcome.reason = ActionDisableReason::ExternalReadOnly;
            }
            return outcome;
        }
        default:
            break;
    }

    const ReplayEntry* entry = selectedEntry();
    if (entry == nullptr) {
        outcome.reason = ActionDisableReason::NoSelection;
        outcome.message = actionDisableReasonText(outcome.reason);
        return outcome;
    }

    if (state.requiresConfirmation) {
        if (!revalidate(*entry, outcome)) return outcome;
        pending_ = Pending{};
        pending_.active = true;
        pending_.token = ++tokenCounter_;
        pending_.action = request.action;
        pending_.stableId = entry->stableId;
        pending_.targetPath = entry->path;
        pending_.targetIdentity = entry->identity;
        pending_.request = request;
        pending_.targetName = sanitizeForDisplay(entry->fileName, 128u);
        switch (request.action) {
            case BrowserAction::MoveToTrash:
                pending_.prompt = "Move \"" + pending_.targetName
                    + "\" to the replay Trash?";
                break;
            case BrowserAction::PermanentPurge:
                pending_.prompt = "Permanently delete \"" + pending_.targetName
                    + "\"? This cannot be undone.";
                break;
            case BrowserAction::Rename:
                pending_.prompt = "Rename \"" + pending_.targetName + "\" to \""
                    + sanitizeForDisplay(request.text, 96u) + "\"?";
                break;
            default:
                pending_.prompt = "Confirm?";
                break;
        }
        outcome.ok = false;
        outcome.needsConfirmation = true;
        outcome.confirmationToken = pending_.token;
        outcome.confirmationPrompt = pending_.prompt;
        outcome.confirmationTargetName = pending_.targetName;
        return outcome;
    }

    if (!revalidate(*entry, outcome)) return outcome;

    switch (request.action) {
        case BrowserAction::WatchSet:
            return doWatch(*entry, 0u, 0u, true);
        case BrowserAction::WatchMatch:
            return doWatch(*entry, request.matchOrdinal, 0u, false);
        case BrowserAction::StartAtRound:
            return doWatch(*entry, request.matchOrdinal, request.round, false);
        case BrowserAction::VerifyFile:
            return doVerifyFile(*entry);
        case BrowserAction::VerifySimulation: {
            ActionOutcome watch = doWatch(*entry, request.matchOrdinal, 0u, true);
            watch.action = BrowserAction::VerifySimulation;
            return watch;
        }
        case BrowserAction::ExportMatch:
            return doExportMatch(*entry, request.matchOrdinal);
        case BrowserAction::ToggleFavorite: {
            const ReplayAnnotation* annotation = annotationFor(*entry);
            const bool favorite = annotation == nullptr || !annotation->favorite;
            outcome.ok = annotations_->setFavorite(entry->prefix.sessionUuidLow,
                                                   entry->prefix.sessionUuidHigh,
                                                   favorite);
            annotations_->saveIfDirty();
            refreshView();
            outcome.message = favorite ? "favorited" : "unfavorited";
            return outcome;
        }
        case BrowserAction::EditTags: {
            outcome.ok = annotations_->setTags(entry->prefix.sessionUuidLow,
                                               entry->prefix.sessionUuidHigh,
                                               request.tags);
            if (!outcome.ok) {
                outcome.message = "tags rejected: at most 16 tags, 32 bytes "
                                  "each, printable text only";
                return outcome;
            }
            annotations_->saveIfDirty();
            refreshView();
            return outcome;
        }
        case BrowserAction::EditNote: {
            outcome.ok = annotations_->setNote(entry->prefix.sessionUuidLow,
                                               entry->prefix.sessionUuidHigh,
                                               request.text);
            if (!outcome.ok) {
                outcome.message = "note rejected: at most 1024 bytes of "
                                  "printable text";
                return outcome;
            }
            annotations_->saveIfDirty();
            refreshView();
            return outcome;
        }
        case BrowserAction::RevealFolder: {
            outcome.ok = true;
            outcome.producedPath = directoryOf(entry->path);
            return outcome;
        }
        default:
            break;
    }
    outcome.reason = ActionDisableReason::NoSelection;
    return outcome;
}

ActionOutcome ReplayBrowserModel::confirmAction(uint64_t token) {
    ActionOutcome outcome{};
    if (!pending_.active || token == 0u || token != pending_.token) {
        outcome.message = "no matching pending action";
        return outcome;
    }
    const Pending pending = pending_;
    pending_ = Pending{};
    outcome.action = pending.action;

    const ReplayEntry* entry = selectedEntry();
    if (entry == nullptr || entry->stableId != pending.stableId
        || entry->path != pending.targetPath) {
        outcome.reason = ActionDisableReason::FileChangedSinceScan;
        outcome.message = "the confirmed target is no longer selected";
        return outcome;
    }
    FileIdentity now{};
    if (!queryFileIdentity(entry->path, now)) {
        outcome.reason = ActionDisableReason::FileMissing;
        outcome.message = actionDisableReasonText(outcome.reason);
        return outcome;
    }
    if (!now.unchanged(pending.targetIdentity)) {
        outcome.reason = ActionDisableReason::FileChangedSinceScan;
        outcome.message = actionDisableReasonText(outcome.reason);
        return outcome;
    }

    switch (pending.action) {
        case BrowserAction::MoveToTrash: return doMoveToTrash(*entry);
        case BrowserAction::PermanentPurge: return doPurge(*entry);
        case BrowserAction::Rename:
            return doRename(*entry, pending.request.text);
        default: break;
    }
    outcome.message = "unsupported confirmed action";
    return outcome;
}

void ReplayBrowserModel::cancelPendingAction() { pending_ = Pending{}; }

ActionOutcome ReplayBrowserModel::doWatch(const ReplayEntry& entry,
                                          uint32_t ordinal, uint32_t round,
                                          bool wholeSet) {
    ActionOutcome outcome{};
    outcome.action = wholeSet ? BrowserAction::WatchSet
                              : BrowserAction::WatchMatch;
    WatchRequest watch{};
    watch.stableId = entry.stableId;
    watch.replayUuidLow = entry.prefix.sessionUuidLow;
    watch.replayUuidHigh = entry.prefix.sessionUuidHigh;
    watch.path = entry.path;
    watch.identity = entry.identity;
    watch.startRound = round;
    const size_t index = rows_[selectedIndex_].entryIndex;
    watch.compatibility =
        index < results_.size() ? results_[index] : CompatibilityResult{};

    if (!wholeSet) {
        const MatchSpan* span = nullptr;
        for (const MatchSpan& candidate : entry.matches) {
            if (candidate.ordinal == ordinal) { span = &candidate; break; }
        }
        if (span == nullptr && !entry.matches.empty()) span = &entry.matches.front();
        if (span == nullptr) {
            outcome.reason = ActionDisableReason::NoMatchSelected;
            outcome.message = actionDisableReasonText(outcome.reason);
            return outcome;
        }
        watch.matchId = span->matchId;
        watch.matchOrdinal = span->ordinal;
        watch.startFrame = span->firstTimelineFrame;
        if (!span->independentBootstrap) {
            watch.dependencyMatchId = span->dependsOnMatchId;
        }
    } else if (!entry.matches.empty()) {
        watch.startFrame = entry.matches.front().firstTimelineFrame;
    }
    watch.valid = true;
    outcome.ok = true;
    outcome.watch = watch;
    return outcome;
}

ActionOutcome ReplayBrowserModel::doVerifyFile(const ReplayEntry& entry) {
    ActionOutcome outcome{};
    outcome.action = BrowserAction::VerifyFile;

    ParseOptions options{};
    options.mode = ParseMode::Verify;
    options.requireFooter = false;
    ParsedReplay parsed;
    const ParseResult result = ReplayParser::parseFile(entry.path, options, parsed);
    outcome.parse = result;
    outcome.ok = result.ok();
    outcome.digestVerified = parsed.hasFooter && result.ok();

    if (!result.ok()) {
        outcome.message = std::string(parseStatusText(result.status)) + " at "
            + std::to_string(result.offset);
        if (!result.detail.empty())
            outcome.message += " (" + sanitizeForDisplay(result.detail, 160u) + ")";
        return outcome;
    }
    if (!parsed.hasFooter) {
        outcome.ok = false;
        outcome.reason = ActionDisableReason::Incomplete;
        outcome.message = "structure valid up to the end of the file, but "
                          "there is no footer index";
        return outcome;
    }
    outcome.message = "File intact: framing, per-chunk CRC and the SHA-256 "
                      "file digest all verified";
    if (annotations_ != nullptr) {
        annotations_->recordStructuralVerification(
            entry.prefix.sessionUuidLow, entry.prefix.sessionUuidHigh,
            parsed.file.footer.fileDigest,
            static_cast<int64_t>(::_time64(nullptr)));
        annotations_->saveIfDirty();
    }
    refreshView();
    return outcome;
}

ActionOutcome ReplayBrowserModel::doRename(const ReplayEntry& entry,
                                           const std::string& title) {
    ActionOutcome outcome{};
    outcome.action = BrowserAction::Rename;

    const std::string token = ReplayWriter::sanitizeNameToken(title, 64u);
    if (token.empty() || token == "session") {
        outcome.message = "that name has no usable characters";
        return outcome;
    }
    const std::wstring directory = directoryOf(entry.path);
    const std::wstring extension = L".q99r";
    const std::wstring target = directory + L"\\" + qplat::widen(token)
        + extension;
    if (!isSafeReplayPath(target)) {
        outcome.message = "that name is not a valid file name";
        return outcome;
    }
    if (qplat::fileExistsW(target)) {
        outcome.message = "a replay with that name already exists";
        return outcome;
    }
    if (::MoveFileExW(qplat::longPathW(entry.path).c_str(),
                      qplat::longPathW(target).c_str(),
                      MOVEFILE_WRITE_THROUGH) == 0) {
        outcome.message = "rename failed";
        return outcome;
    }
    if (annotations_ != nullptr) {
        annotations_->setTitle(entry.prefix.sessionUuidLow,
                               entry.prefix.sessionUuidHigh,
                               sanitizeForDisplay(title, kMaxLabelBytes));
        annotations_->saveIfDirty();
    }
    outcome.ok = true;
    outcome.producedPath = target;
    outcome.message = "renamed";
    return outcome;
}

ActionOutcome ReplayBrowserModel::doExportMatch(const ReplayEntry& entry,
                                                uint32_t ordinal) {
    ActionOutcome outcome{};
    outcome.action = BrowserAction::ExportMatch;

    const MatchSpan* span = nullptr;
    for (const MatchSpan& candidate : entry.matches) {
        if (candidate.ordinal == ordinal) { span = &candidate; break; }
    }
    if (span == nullptr && !entry.matches.empty()) span = &entry.matches.front();
    if (span == nullptr) {
        outcome.reason = ActionDisableReason::NoMatchSelected;
        outcome.message = actionDisableReasonText(outcome.reason);
        return outcome;
    }
    if (!span->independentBootstrap) {
        outcome.reason = ActionDisableReason::NoIndependentBootstrap;
        outcome.message = actionDisableReasonText(outcome.reason);
        return outcome;
    }

    ParseOptions options{};
    options.mode = ParseMode::Full;
    options.matchIdFilter = span->matchId;
    ParsedReplay parsed;
    const ParseResult result = ReplayParser::parseFile(entry.path, options, parsed);
    if (!result.ok()) {
        outcome.message = std::string("source rejected: ")
            + parseStatusText(result.status);
        return outcome;
    }
    const MatchStartRecord* start = nullptr;
    for (const MatchStartRecord& candidate : parsed.file.matchStarts) {
        if (candidate.matchId == span->matchId) { start = &candidate; break; }
    }
    if (start == nullptr) {
        outcome.message = "the selected match has no start record";
        return outcome;
    }

    ReplayPrefix prefix = parsed.file.prefix;
    {
        Sha256 sha;
        static const char kDomain[] = "q99r-exported-match-v1";
        sha.update(reinterpret_cast<const uint8_t*>(kDomain), sizeof(kDomain) - 1u);
        sha.update(parsed.file.footer.fileDigest.data(),
                   parsed.file.footer.fileDigest.size());
        const uint32_t matchId = span->matchId;
        uint8_t idBytes[4];
        for (unsigned i = 0; i < 4u; ++i)
            idBytes[i] = static_cast<uint8_t>(matchId >> (i * 8u));
        sha.update(idBytes, 4u);
        std::array<uint8_t, 32> out{};
        sha.finish(out);
        prefix.sessionUuidLow = 0u;
        prefix.sessionUuidHigh = 0u;
        for (unsigned i = 0; i < 8u; ++i) {
            prefix.sessionUuidLow |= static_cast<uint64_t>(out[i]) << (i * 8u);
            prefix.sessionUuidHigh |=
                static_cast<uint64_t>(out[8u + i]) << (i * 8u);
        }
    }

    MatchStartRecord exported = *start;
    exported.ordinal = 1u;
    exported.dependsOnMatchId = 0u;
    exported.independentBootstrap = true;

    WriterConfig config{};
    config.rootDirectory = replayRoot_;
    config.nameHint = "match" + std::to_string(span->ordinal) + "-"
        + ReplayWriter::sanitizeNameToken(
              sanitizeForDisplay(entry.fileName, 64u), 32u);
    config.recordedUnixSeconds = entry.manifest.display.recordedUnixSeconds;

    ReplayWriter writer;
    if (!writer.begin(config, prefix, parsed.file.manifest)) {
        outcome.message = std::string("export failed: ") + writer.errorText();
        return outcome;
    }
    DeterministicStart start0 = exported.start;
    if (!writer.writeSessionStart(start0)
        || !writer.writeMatchStart(exported)) {
        writer.abandon();
        outcome.message = std::string("export failed: ") + writer.errorText();
        return outcome;
    }

    size_t frameIndex = 0u;
    size_t eventIndex = 0u;
    const std::vector<ReplayFrameV1>& frames = parsed.file.frames;
    const std::vector<ReplayEvent>& events = parsed.file.events;
    while (frameIndex < frames.size()) {
        const size_t take = std::min<size_t>(kMaxRecordsPerInputBlock,
                                             frames.size() - frameIndex);
        if (frames[frameIndex].timelineFrame < span->firstTimelineFrame
            || frames[frameIndex].timelineFrame > span->lastTimelineFrame) {
            ++frameIndex;
            continue;
        }
        size_t count = 0u;
        while (count < take
               && frames[frameIndex + count].timelineFrame
                   <= span->lastTimelineFrame) {
            if (count != 0u
                && frames[frameIndex + count].timelineFrame
                    != frames[frameIndex + count - 1u].timelineFrame + 1u) {
                break;
            }
            ++count;
        }
        if (count == 0u) break;
        if (writer.nextTimelineFrame() != frames[frameIndex].timelineFrame) {
            writer.abandon();
            outcome.message = "export failed: the match timeline is not "
                              "contiguous";
            return outcome;
        }
        if (!writer.writeInputBlock(&frames[frameIndex], count)) {
            writer.abandon();
            outcome.message = std::string("export failed: ") + writer.errorText();
            return outcome;
        }
        frameIndex += count;
        std::vector<ReplayEvent> batch;
        while (eventIndex < events.size()
               && events[eventIndex].timelineFrame
                   < writer.nextTimelineFrame()) {
            if (events[eventIndex].matchId == span->matchId
                && events[eventIndex].timelineFrame >= span->firstTimelineFrame) {
                batch.push_back(events[eventIndex]);
            }
            ++eventIndex;
        }
        if (!batch.empty() && !writer.writeEventBlock(batch.data(), batch.size())) {
            writer.abandon();
            outcome.message = std::string("export failed: ") + writer.errorText();
            return outcome;
        }
    }

    MatchEndRecord end{};
    bool foundEnd = false;
    for (const MatchEndRecord& candidate : parsed.file.matchEnds) {
        if (candidate.matchId == span->matchId) {
            end = candidate;
            foundEnd = true;
            break;
        }
    }
    if (!foundEnd) {
        end.matchId = span->matchId;
        end.lastTimelineFrame = span->lastTimelineFrame;
        end.winningSideMask = span->winningSideMask;
        end.reason = TerminalReason::Completed;
    }
    if (!writer.writeMatchEnd(end)) {
        writer.abandon();
        outcome.message = std::string("export failed: ") + writer.errorText();
        return outcome;
    }

    SessionEndRecord sessionEnd{};
    sessionEnd.lastTimelineFrame = end.lastTimelineFrame;
    sessionEnd.reason = TerminalReason::Completed;
    sessionEnd.postTailStateHash = end.postTailStateHash;
    sessionEnd.rollingDigest = end.rollingDigest;
    sessionEnd.matchCount = 1u;
    if (!writer.finalize(sessionEnd)) {
        outcome.message = std::string("export failed: ") + writer.errorText();
        return outcome;
    }
    if (annotations_ != nullptr) {
        annotations_->recordProvenance(
            prefix.sessionUuidLow, prefix.sessionUuidHigh,
            entry.prefix.sessionUuidLow, entry.prefix.sessionUuidHigh,
            entry.fileDigest, sanitizeForDisplay(entry.fileName, 128u));
        annotations_->saveIfDirty();
    }
    outcome.ok = true;
    outcome.producedPath = writer.summary().finalPath;
    outcome.message = "exported one match";
    return outcome;
}

ActionOutcome ReplayBrowserModel::doMoveToTrash(const ReplayEntry& entry) {
    ActionOutcome outcome{};
    outcome.action = BrowserAction::MoveToTrash;
    std::wstring trashDir = replayRoot_;
    while (!trashDir.empty()
           && (trashDir.back() == L'\\' || trashDir.back() == L'/')) {
        trashDir.pop_back();
    }
    trashDir += L"\\.trash";
    if (!entry.relativeDir.empty()) trashDir += L"\\" + entry.relativeDir;
    if (!qplat::ensureDirW(trashDir)) {
        outcome.reason = ActionDisableReason::ReadOnlyLocation;
        outcome.message = "could not create the replay Trash folder";
        return outcome;
    }
    std::wstring finalPath;
    if (!qplat::moveToTrashDirW(entry.path, trashDir, finalPath)) {
        outcome.message = "could not move the replay to Trash";
        return outcome;
    }
    undo_.available = true;
    undo_.trashedPath = finalPath;
    undo_.originalPath = entry.path;
    undo_.stableId = entry.stableId;
    outcome.ok = true;
    outcome.producedPath = finalPath;
    outcome.message = "moved to Trash";
    return outcome;
}

ActionOutcome ReplayBrowserModel::doUndoTrash() {
    ActionOutcome outcome{};
    outcome.action = BrowserAction::UndoTrash;

    const ReplayEntry* selected = selectedEntry();
    if (selected != nullptr && selected->inTrash && !replayRoot_.empty()) {
        std::wstring root = replayRoot_;
        while (!root.empty()
               && (root.back() == L'\\' || root.back() == L'/')) {
            root.pop_back();
        }
        std::wstring destinationDir = root;
        if (!selected->relativeDir.empty())
            destinationDir += L"\\" + selected->relativeDir;
        if (destinationDir != root && !qplat::ensureDirW(destinationDir)) {
            outcome.message =
                "could not recreate the folder this replay came from";
            return outcome;
        }
        const std::wstring target =
            destinationDir + L"\\" + selected->fileName;
        if (qplat::fileExistsW(target)) {
            outcome.message = "a file with the original name exists again";
            return outcome;
        }
        if (::MoveFileExW(qplat::longPathW(selected->path).c_str(),
                          qplat::longPathW(target).c_str(),
                          MOVEFILE_WRITE_THROUGH) == 0) {
            outcome.message = "restore failed";
            return outcome;
        }
        if (undo_.available && undo_.trashedPath == selected->path)
            undo_ = TrashUndo{};
        outcome.ok = true;
        outcome.producedPath = target;
        outcome.message = "restored from Trash";
        return outcome;
    }

    if (!undo_.available) {
        outcome.reason = ActionDisableReason::NothingToUndo;
        outcome.message = actionDisableReasonText(outcome.reason);
        return outcome;
    }
    if (qplat::fileExistsW(undo_.originalPath)) {
        outcome.message = "a file with the original name exists again";
        return outcome;
    }
    {
        const size_t slash = undo_.originalPath.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const std::wstring parent = undo_.originalPath.substr(0, slash);
            if (!parent.empty() && !qplat::ensureDirW(parent)) {
                outcome.message =
                    "could not recreate the folder this replay came from";
                return outcome;
            }
        }
    }
    if (::MoveFileExW(qplat::longPathW(undo_.trashedPath).c_str(),
                      qplat::longPathW(undo_.originalPath).c_str(),
                      MOVEFILE_WRITE_THROUGH) == 0) {
        outcome.message = "restore failed";
        return outcome;
    }
    outcome.ok = true;
    outcome.producedPath = undo_.originalPath;
    outcome.message = "restored from Trash";
    undo_ = TrashUndo{};
    return outcome;
}

ActionOutcome ReplayBrowserModel::doPurge(const ReplayEntry& entry) {
    ActionOutcome outcome{};
    outcome.action = BrowserAction::PermanentPurge;
    if (::DeleteFileW(qplat::longPathW(entry.path).c_str()) == 0) {
        outcome.message = "permanent delete failed";
        return outcome;
    }
    if (undo_.available && undo_.trashedPath == entry.path) undo_ = TrashUndo{};
    outcome.ok = true;
    outcome.message = "permanently deleted";
    return outcome;
}

bool ReplayBrowserModel::openExternal(const std::wstring& path,
                                      ReplayEntry& out,
                                      CompatibilityResult& compatibility,
                                      std::string& message) const {
    out = ReplayEntry{};
    compatibility = CompatibilityResult{};
    if (!isSafeReplayPath(path)) {
        message = "that path is not acceptable for a replay file";
        return false;
    }
    if (!hasFinalReplayExtension(path)) {
        message = "only finalized .q99r replay files can be opened";
        return false;
    }
    if (!ReplayScanner::loadEntry(path, out)) {
        message = std::string("rejected: ") + parseStatusText(out.parseStatus);
        if (!out.parseDetail.empty())
            message += " (" + sanitizeForDisplay(out.parseDetail, 160u) + ")";
    }
    ReplayIdentitySnapshot file = snapshotFor(out);
    compatibility = evaluate(file, runtime_);
    if (message.empty()) message = "opened read-only";
    return out.parsed;
}

}
