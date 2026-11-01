#include "audio/AudioReconciler.h"

#include <cstring>
#include <new>

namespace qaudio {
namespace {

constexpr uint32_t kActiveAtFrontier = 0xFFFFFFFFu;

struct Generation {
    AudioSemanticKey key{};
    int16_t sampleId = -1;
    uint32_t flags = 0;
    uint32_t startFrame = 0;
    uint16_t startOrdinal = 0;
    uint32_t endFrame = kActiveAtFrontier;
    uint32_t durationFrames = 0;
    bool looping = false;
    bool emitted = false;
    PhysicalToken token{};
    int matchedTo = -1;
    bool fromReplace = false;
};

struct EndpointHistory {
    AudioEndpoint endpoint{};
    Generation generations[kMaxGenerationsPerEndpoint];
    size_t count = 0;
    bool overflowed = false;
    bool valid = false;

    Generation* active() {
        return count > 0
                && generations[count - 1].endFrame == kActiveAtFrontier
            ? &generations[count - 1]
            : nullptr;
    }
};

struct EndpointTable {
    EndpointHistory endpoints[kMaxTrackedEndpoints];
    size_t count = 0;
    bool overflowed = false;

    EndpointHistory* find(const AudioEndpoint& endpoint) {
        for (size_t i = 0; i < count; ++i) {
            if (endpoints[i].endpoint == endpoint) return &endpoints[i];
        }
        return nullptr;
    }
    EndpointHistory* findOrAdd(const AudioEndpoint& endpoint) {
        if (EndpointHistory* existing = find(endpoint)) return existing;
        if (count >= kMaxTrackedEndpoints) {
            overflowed = true;
            return nullptr;
        }
        endpoints[count].endpoint = endpoint;
        endpoints[count].valid = true;
        return &endpoints[count++];
    }
};

void closeActive(EndpointHistory& history, uint32_t frame) {
    if (Generation* current = history.active())
        current->endFrame = frame;
}

void openGeneration(EndpointHistory& history, const AudioCommand& command,
                    const IAssetCatalog* catalog) {
    if (history.count >= kMaxGenerationsPerEndpoint) {
        history.overflowed = true;
        return;
    }
    Generation& generation = history.generations[history.count++];
    generation = Generation{};
    generation.key = semanticKeyOf(command);
    generation.key.kind =
        command.endpoint.type == AudioEndpoint::Type::Duplicate
            ? AudioKind::PlayDuplicate
            : AudioKind::PlayBase;
    generation.sampleId = command.sampleId;
    generation.flags = command.flags;
    generation.startFrame = command.frame;
    generation.startOrdinal = command.ordinal;
    generation.looping = (command.flags & 1u) != 0u;
    generation.fromReplace = command.kind == AudioKind::Replace;
    if (catalog != nullptr && command.sampleId >= 0) {
        const WaveFacts facts =
            catalog->factsFor(command.sampleId, command.asset);
        if (facts.valid) {
            generation.durationFrames = facts.durationFrames;
            generation.looping = generation.looping || facts.looping;
        }
    }
}

void foldEndpoints(EndpointTable& table, const FrameJournal& journal,
                   const IAssetCatalog* catalog) {
    for (uint16_t i = 0; i < journal.commandCount; ++i) {
        const AudioCommand& command = journal.commands[i];
        switch (command.kind) {
            case AudioKind::Stop: {
                if (EndpointHistory* history =
                        table.findOrAdd(command.endpoint))
                    closeActive(*history, command.frame);
                break;
            }
            case AudioKind::PlayDuplicate:
            case AudioKind::PlayBase:
            case AudioKind::Replace: {
                EndpointHistory* history =
                    table.findOrAdd(command.endpoint);
                if (history == nullptr) break;
                closeActive(*history, command.frame);
                if (command.kind == AudioKind::Replace
                    && command.sampleId < 0)
                    break;
                if (command.loadOutcome == LoadOutcome::Absent) break;
                openGeneration(*history, command, catalog);
                break;
            }
            case AudioKind::LoadAttempt:
            case AudioKind::Unload:
                break;
        }
    }
}

void attachEmissions(EndpointTable& table, const AudioTimeline& timeline,
                     uint32_t restoreFrame, uint32_t frontierFrame) {
    for (uint32_t frame = restoreFrame; frame != frontierFrame; ++frame) {
        const FrameEmissions* emissions = timeline.emissionsFor(frame);
        if (emissions == nullptr) continue;
        for (uint16_t i = 0; i < emissions->count; ++i) {
            const EmittedOccurrence& occurrence = emissions->entries[i];
            EndpointHistory* history = table.find(occurrence.endpoint);
            if (history == nullptr) continue;
            for (size_t g = 0; g < history->count; ++g) {
                Generation& generation = history->generations[g];
                if (generation.startFrame == occurrence.frame
                    && generation.startOrdinal == occurrence.ordinal) {
                    generation.emitted = true;
                    generation.token = occurrence.token;
                    if (occurrence.durationFrames != 0)
                        generation.durationFrames =
                            occurrence.durationFrames;
                    generation.looping =
                        generation.looping || occurrence.looping;
                    break;
                }
            }
        }
    }
}

bool semanticCompatible(const Generation& a, const Generation& b) {
    return a.key == b.key;
}

uint32_t frameDistance(uint32_t a, uint32_t b) {
    return a > b ? a - b : b - a;
}

bool generationExpiredAt(const Generation& generation, uint32_t frontier) {
    if (generation.looping) return false;
    if (generation.durationFrames == 0) return false;
    return generation.startFrame + generation.durationFrames <= frontier;
}

void matchEndpoint(EndpointHistory& oldHistory,
                   EndpointHistory& correctedHistory) {
    int oldIndex = static_cast<int>(oldHistory.count) - 1;
    int correctedIndex = static_cast<int>(correctedHistory.count) - 1;
    while (oldIndex >= 0 && correctedIndex >= 0) {
        Generation& oldGeneration = oldHistory.generations[oldIndex];
        Generation& correctedGeneration =
            correctedHistory.generations[correctedIndex];
        if (semanticCompatible(oldGeneration, correctedGeneration)
            && frameDistance(oldGeneration.startFrame,
                             correctedGeneration.startFrame)
                   <= kMatchMaxFrameDistance) {
            oldGeneration.matchedTo = correctedIndex;
            correctedGeneration.matchedTo = oldIndex;
            --oldIndex;
            --correctedIndex;
            continue;
        }
        if (oldGeneration.startFrame > correctedGeneration.startFrame) {
            --oldIndex;
        } else {
            --correctedIndex;
        }
    }
}

const char* pathForSample(const ResourceMap& map, int16_t sample) {
    if (sample < 0 || sample > kMaxSampleId) return "";
    return map.samples[sample].ownedPath;
}

}

void foldResources(ResourceMap& map, const FrameJournal& journal) {
    for (uint16_t i = 0; i < journal.commandCount; ++i) {
        const AudioCommand& command = journal.commands[i];
        if (command.sampleId < 0 || command.sampleId > kMaxSampleId)
            continue;
        ResourceEntry& entry = map.samples[command.sampleId];
        switch (command.kind) {
            case AudioKind::Unload:
                entry.present = false;
                entry.asset = AssetKey{};
                entry.ownedPath[0] = '\0';
                break;
            case AudioKind::LoadAttempt:
                entry.present = false;
                entry.asset = AssetKey{};
                if (command.loadOutcome == LoadOutcome::Present) {
                    entry.present = true;
                    entry.asset = command.asset;
                    std::memcpy(entry.ownedPath, command.ownedPath,
                                kAudioPathCapacity);
                } else {
                    entry.ownedPath[0] = '\0';
                }
                break;
            default:
                break;
        }
    }
}

void reconcileCorrection(const ReconcileInput& input,
                         ReconcileResult& result) {
    result = ReconcileResult{};
    if (input.timeline == nullptr || input.corrected == nullptr
        || input.baselineResources == nullptr || input.owners == nullptr
        || input.frontierFrame < input.restoreFrame) {
        return;
    }

    struct Workspace {
        EndpointTable oldTable;
        EndpointTable correctedTable;
        ResourceMap oldResources;
        SinkAction ordered[kMaxReconcileActions];
    };
    Workspace* workspace = new (std::nothrow) Workspace{};
    if (workspace == nullptr) return;
    EndpointTable& oldTable = workspace->oldTable;
    EndpointTable& correctedTable = workspace->correctedTable;
    ResourceMap& oldResources = workspace->oldResources;
    oldResources = *input.baselineResources;
    result.correctedResources = *input.baselineResources;

    for (uint32_t frame = input.restoreFrame;
         frame != input.frontierFrame; ++frame) {
        if (const FrameJournal* journal = input.timeline->journalFor(frame)) {
            foldEndpoints(oldTable, *journal, input.catalog);
            foldResources(oldResources, *journal);
        }
    }
    for (size_t i = 0; i < input.correctedCount; ++i) {
        const FrameJournal* journal = input.corrected[i];
        if (journal == nullptr) { delete workspace; return; }
        foldEndpoints(correctedTable, *journal, input.catalog);
        foldResources(result.correctedResources, *journal);
        ++result.counters.correctedTotal;
    }
    if (oldTable.overflowed || correctedTable.overflowed) {
        delete workspace;
        return;
    }
    for (size_t i = 0; i < oldTable.count; ++i) {
        if (oldTable.endpoints[i].overflowed) { delete workspace; return; }
    }
    for (size_t i = 0; i < correctedTable.count; ++i) {
        if (correctedTable.endpoints[i].overflowed) {
            delete workspace;
            return;
        }
    }

    attachEmissions(oldTable, *input.timeline, input.restoreFrame,
                    input.frontierFrame);

    auto appendAction = [&result](const SinkAction& action) -> bool {
        if (result.actionCount >= kMaxReconcileActions) {
            result.valid = false;
            return false;
        }
        result.actions[result.actionCount++] = action;
        return true;
    };
    auto appendEmission = [&result](const PlannedEmission& emission) -> bool {
        if (result.emissionCount >= kMaxCorrectedEmissions) {
            result.valid = false;
            return false;
        }
        result.emissions[result.emissionCount++] = emission;
        return true;
    };
    bool overflowed = false;

    for (int sample = 0; sample <= kMaxSampleId; ++sample) {
        const ResourceEntry& oldEntry = oldResources.samples[sample];
        const ResourceEntry& correctedEntry =
            result.correctedResources.samples[sample];
        const bool sameAsset =
            oldEntry.present == correctedEntry.present
            && (!oldEntry.present
                || oldEntry.asset == correctedEntry.asset);
        if (sameAsset) continue;
        ++result.counters.assetReconciled;

        for (size_t e = 0; e < oldTable.count; ++e) {
            EndpointHistory& history = oldTable.endpoints[e];
            Generation* active = history.active();
            if (active == nullptr || !active->emitted
                || active->sampleId != sample)
                continue;
            SinkAction stop{};
            stop.type = SinkAction::Type::StopIfOwner;
            stop.endpoint = history.endpoint;
            stop.owner = active->token;
            stop.sampleId = active->sampleId;
            if (!appendAction(stop)) { overflowed = true; break; }
        }
        if (overflowed) break;

        SinkAction action{};
        action.sampleId = static_cast<int16_t>(sample);
        if (correctedEntry.present) {
            action.type = SinkAction::Type::Load;
            std::memcpy(action.ownedPath, correctedEntry.ownedPath,
                        kAudioPathCapacity);
        } else {
            action.type = SinkAction::Type::Unload;
        }
        if (!appendAction(action)) { overflowed = true; break; }
    }
    if (overflowed) { delete workspace; return; }

    for (size_t i = 0; i < correctedTable.count && !overflowed; ++i) {
        EndpointHistory& corrected = correctedTable.endpoints[i];
        EndpointHistory* oldHistory = oldTable.find(corrected.endpoint);
        EndpointHistory empty{};
        empty.endpoint = corrected.endpoint;
        EndpointHistory& oldRef = oldHistory ? *oldHistory : empty;
        matchEndpoint(oldRef, corrected);

        result.counters.oldTotal += oldRef.count;
        for (size_t g = 0; g < corrected.count; ++g) {
            Generation& generation = corrected.generations[g];
            const bool activeAtFrontier =
                generation.endFrame == kActiveAtFrontier;
            if (generation.matchedTo >= 0) {
                Generation& oldGeneration =
                    oldRef.generations[generation.matchedTo];
                ++result.counters.matchedTotal;
                if (oldGeneration.emitted) {
                    ++result.counters.replayCreditConsumed;
                    const PhysicalToken owner =
                        input.owners->ownerOf(corrected.endpoint);
                    const bool tokenIsCurrent =
                        oldGeneration.token == owner
                        && oldGeneration.token.valid();
                    if (activeAtFrontier && tokenIsCurrent) {
                        PlannedEmission emission{};
                        emission.occurrence.key = generation.key;
                        emission.occurrence.frame = generation.startFrame;
                        emission.occurrence.ordinal =
                            generation.startOrdinal;
                        emission.occurrence.endpoint = corrected.endpoint;
                        emission.occurrence.sampleId = generation.sampleId;
                        emission.occurrence.token = oldGeneration.token;
                        emission.occurrence.startFrame =
                            generation.startFrame;
                        emission.occurrence.durationFrames =
                            generation.durationFrames;
                        emission.occurrence.looping = generation.looping;
                        if (!appendEmission(emission)) {
                            overflowed = true;
                            break;
                        }
                    } else if (activeAtFrontier) {
                        if (generationExpiredAt(generation,
                                                input.frontierFrame)) {
                            ++result.counters.correctedNewExpiredSkipped;
                        } else {
                            SinkAction resume{};
                            resume.type = SinkAction::Type::PlayAtCursor;
                            resume.endpoint = corrected.endpoint;
                            resume.sampleId = generation.sampleId;
                            resume.flags = generation.flags;
                            resume.key = generation.key;
                            resume.sourceFrame = generation.startFrame;
                            resume.sourceOrdinal = generation.startOrdinal;
                            resume.startFrame = generation.startFrame;
                            resume.durationFrames =
                                generation.durationFrames;
                            resume.looping = generation.looping;
                            resume.cursorBytes =
                                input.frontierFrame - generation.startFrame;
                            ++result.counters.cursorResumed;
                            PlannedEmission emission{};
                            emission.occurrence.key = generation.key;
                            emission.occurrence.frame =
                                generation.startFrame;
                            emission.occurrence.ordinal =
                                generation.startOrdinal;
                            emission.occurrence.endpoint =
                                corrected.endpoint;
                            emission.occurrence.sampleId =
                                generation.sampleId;
                            emission.occurrence.startFrame =
                                generation.startFrame;
                            emission.occurrence.durationFrames =
                                generation.durationFrames;
                            emission.occurrence.looping =
                                generation.looping;
                            emission.tokenFromAction = static_cast<int>(
                                result.actionCount);
                            if (!appendAction(resume)
                                || !appendEmission(emission)) {
                                overflowed = true;
                                break;
                            }
                        }
                    } else {
                        PlannedEmission emission{};
                        emission.occurrence.key = generation.key;
                        emission.occurrence.frame = generation.startFrame;
                        emission.occurrence.ordinal =
                            generation.startOrdinal;
                        emission.occurrence.endpoint = corrected.endpoint;
                        emission.occurrence.sampleId = generation.sampleId;
                        emission.occurrence.token = oldGeneration.token;
                        emission.occurrence.startFrame =
                            generation.startFrame;
                        emission.occurrence.durationFrames =
                            generation.durationFrames;
                        emission.occurrence.looping = generation.looping;
                        if (!appendEmission(emission)) {
                            overflowed = true;
                            break;
                        }
                    }
                    continue;
                }
            }
            if (generationExpiredAt(generation, input.frontierFrame)) {
                ++result.counters.correctedNewExpiredSkipped;
                continue;
            }
            SinkAction play{};
            play.endpoint = corrected.endpoint;
            play.sampleId = generation.sampleId;
            play.flags = generation.flags;
            play.key = generation.key;
            play.sourceFrame = generation.startFrame;
            play.sourceOrdinal = generation.startOrdinal;
            play.startFrame = generation.startFrame;
            play.durationFrames = generation.durationFrames;
            play.looping = generation.looping;
            const uint32_t age =
                input.frontierFrame > generation.startFrame
                    ? input.frontierFrame - generation.startFrame
                    : 0u;
            if (!activeAtFrontier) {
                ++result.counters.correctedNewExpiredSkipped;
                continue;
            }
            if (age > 2u) {
                play.type = SinkAction::Type::PlayAtCursor;
                play.cursorBytes = age;
                ++result.counters.cursorResumed;
            } else {
                play.type = corrected.endpoint.type
                                    == AudioEndpoint::Type::Duplicate
                    ? SinkAction::Type::PlayDuplicate
                    : SinkAction::Type::PlayBase;
            }
            ++result.counters.correctedNewPlayed;
            PlannedEmission emission{};
            emission.occurrence.key = generation.key;
            emission.occurrence.frame = generation.startFrame;
            emission.occurrence.ordinal = generation.startOrdinal;
            emission.occurrence.endpoint = corrected.endpoint;
            emission.occurrence.sampleId = generation.sampleId;
            emission.occurrence.startFrame = generation.startFrame;
            emission.occurrence.durationFrames = generation.durationFrames;
            emission.occurrence.looping = generation.looping;
            emission.tokenFromAction =
                static_cast<int>(result.actionCount);
            if (!appendAction(play) || !appendEmission(emission)) {
                overflowed = true;
                break;
            }
        }
    }

    for (size_t i = 0; i < oldTable.count && !overflowed; ++i) {
        EndpointHistory& oldHistory = oldTable.endpoints[i];
        for (size_t g = 0; g < oldHistory.count; ++g) {
            Generation& generation = oldHistory.generations[g];
            if (generation.matchedTo >= 0 || !generation.emitted) continue;
            if (generation.endFrame != kActiveAtFrontier
                && generationExpiredAt(generation, input.frontierFrame)
                && !generation.looping) {
                continue;
            }
            SinkAction stop{};
            stop.type = SinkAction::Type::StopIfOwner;
            stop.endpoint = oldHistory.endpoint;
            stop.owner = generation.token;
            stop.sampleId = generation.sampleId;
            ++result.counters.oldOnlyCanceled;
            if (!appendAction(stop)) { overflowed = true; break; }
        }
    }

    if (overflowed) {
        result.valid = false;
        result.actionCount = 0;
        result.emissionCount = 0;
        delete workspace;
        return;
    }

    SinkAction* ordered = workspace->ordered;
    size_t orderedCount = 0;
    int remap[kMaxReconcileActions];
    for (size_t i = 0; i < result.actionCount; ++i) remap[i] = -1;
    for (size_t i = 0; i < result.actionCount; ++i) {
        const SinkAction::Type type = result.actions[i].type;
        if (type == SinkAction::Type::StopIfOwner
            || type == SinkAction::Type::Unload
            || type == SinkAction::Type::Load) {
            remap[i] = static_cast<int>(orderedCount);
            ordered[orderedCount++] = result.actions[i];
        }
    }
    for (size_t i = 0; i < result.actionCount; ++i) {
        const SinkAction::Type type = result.actions[i].type;
        if (type != SinkAction::Type::StopIfOwner
            && type != SinkAction::Type::Unload
            && type != SinkAction::Type::Load) {
            remap[i] = static_cast<int>(orderedCount);
            ordered[orderedCount++] = result.actions[i];
        }
    }
    for (size_t i = 0; i < orderedCount; ++i) result.actions[i] = ordered[i];
    for (size_t i = 0; i < result.emissionCount; ++i) {
        if (result.emissions[i].tokenFromAction >= 0) {
            result.emissions[i].tokenFromAction =
                remap[result.emissions[i].tokenFromAction];
        }
    }

    result.valid = true;
    delete workspace;
}

}
