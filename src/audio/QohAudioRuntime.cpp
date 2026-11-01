#include "audio/QohAudioRuntime.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace qaudio {

void AssetCatalog::reset() {
    for (Entry& entry : entries_) entry = Entry{};
    nextGeneration_ = 1;
    sealed_ = false;
    misses_ = 0;
}

void AssetCatalog::observeLoad(int16_t sample, const char* path,
                               bool succeeded, const WaveFacts& facts) {
    if (!validSampleId(sample)) return;
    Entry& entry = entries_[sample];
    entry = Entry{};
    if (!succeeded) return;
    entry.present = true;
    entry.asset.pathHash = normalizedPathHash(path);
    entry.asset.generation = nextGeneration_++;
    entry.facts = facts;
    if (path != nullptr) {
        std::strncpy(entry.path, path, kAudioPathCapacity - 1);
        entry.path[kAudioPathCapacity - 1] = '\0';
    }
}

void AssetCatalog::observeUnload(int16_t sample) {
    if (!validSampleId(sample)) return;
    entries_[sample] = Entry{};
}

bool AssetCatalog::present(int16_t sample) const {
    return validSampleId(sample) && entries_[sample].present;
}

AssetKey AssetCatalog::assetOf(int16_t sample) const {
    return validSampleId(sample) ? entries_[sample].asset : AssetKey{};
}

const char* AssetCatalog::pathOf(int16_t sample) const {
    return validSampleId(sample) ? entries_[sample].path : "";
}

WaveFacts AssetCatalog::factsFor(int16_t sample,
                                 const AssetKey& asset) const {
    if (!validSampleId(sample)) return WaveFacts{};
    const Entry& entry = entries_[sample];
    if (!entry.present || !(entry.asset == asset)) {
        ++misses_;
        return WaveFacts{};
    }
    return entry.facts;
}

void AssetCatalog::snapshot(ResourceMap& out) const {
    for (int sample = 0; sample <= kMaxSampleId; ++sample) {
        const Entry& entry = entries_[sample];
        ResourceEntry& resource = out.samples[sample];
        resource.present = entry.present;
        resource.asset = entry.asset;
        std::memcpy(resource.ownedPath, entry.path, kAudioPathCapacity);
    }
}

void QohDirectSoundSink::reset() {
    owners_ = OwnerSnapshot{};
    nextEndpointGeneration_ = 1;
    failures_ = 0;
    staleStops_ = 0;
}

PhysicalToken QohDirectSoundSink::issue(const AudioEndpoint& endpoint) {
    PhysicalToken token{deviceGeneration_, nextEndpointGeneration_++};
    owners_.set(endpoint, token);
    return token;
}

bool QohDirectSoundSink::load(int16_t sample, const char* ownedPath) {
    if (ops_.load == nullptr || !validSampleId(sample)) return false;
    const bool ok = ops_.load(sample, ownedPath) != 0;
    if (!ok) ++failures_;
    return ok;
}

bool QohDirectSoundSink::unload(int16_t sample) {
    if (ops_.unload == nullptr || !validSampleId(sample)) return false;
    (void)ops_.unload(sample);
    return true;
}

SinkResult QohDirectSoundSink::playDuplicate(int16_t sample, uint16_t slot,
                                             uint32_t flags) {
    SinkResult result{};
    if (ops_.playDuplicate == nullptr || !validSampleId(sample)
        || !validDuplicateSlot(slot)) {
        ++failures_;
        return result;
    }
    if (!muted_) {
        const int native = ops_.playDuplicate(
            sample, slot, 0, static_cast<int>(flags));
        if (native < 0) {
            ++failures_;
            return result;
        }
    }
    result.ok = true;
    result.token = issue({AudioEndpoint::Type::Duplicate, slot});
    return result;
}

SinkResult QohDirectSoundSink::playBase(int16_t sample, uint32_t flags) {
    SinkResult result{};
    if (ops_.playBase == nullptr || !validSampleId(sample)) {
        ++failures_;
        return result;
    }
    if (!muted_) {
        const int native = ops_.playBase(sample, static_cast<int>(flags));
        if (native < 0) {
            ++failures_;
            return result;
        }
    }
    result.ok = true;
    result.token = issue({AudioEndpoint::Type::Base,
                          static_cast<uint16_t>(sample)});
    return result;
}

SinkResult QohDirectSoundSink::playAtCursor(const AudioEndpoint& endpoint,
                                            int16_t sample, uint32_t flags,
                                            uint32_t cursorBytes) {
    SinkResult result = endpoint.type == AudioEndpoint::Type::Duplicate
        ? playDuplicate(sample, endpoint.index, flags)
        : playBase(sample, flags);
    if (result.ok && !muted_ && ops_.setCursor != nullptr && cursorBytes != 0)
        (void)ops_.setCursor(endpoint, cursorBytes);
    return result;
}

bool QohDirectSoundSink::stopIfOwner(const AudioEndpoint& endpoint,
                                     const PhysicalToken& owner) {
    const PhysicalToken current = owners_.ownerOf(endpoint);
    if (!(current == owner) || !owner.valid()
        || owner.deviceGeneration != deviceGeneration_) {
        ++staleStops_;
        return false;
    }
    if (ops_.stop != nullptr && !muted_) {
        const int domain =
            endpoint.type == AudioEndpoint::Type::Duplicate ? 1 : 2;
        (void)ops_.stop(endpoint.index, domain);
    }
    owners_.set(endpoint, PhysicalToken{});
    return true;
}

PhysicalToken QohDirectSoundSink::currentOwner(
    const AudioEndpoint& endpoint) {
    return owners_.ownerOf(endpoint);
}

void QohDirectSoundSink::discardReplayTakeoverTransientOwners() {
    for (size_t i = 0u; i < owners_.count; ++i) {
        OwnerSnapshotEntry& entry = owners_.entries[i];
        if (!entry.valid) continue;
        const bool preservedBgm =
            entry.endpoint.type == AudioEndpoint::Type::Base
            && entry.endpoint.index <= 1u;
        if (!preservedBgm) entry.token = PhysicalToken{};
    }
}

QohAudioRuntime& runtime() {
    static QohAudioRuntime* instance = new QohAudioRuntime();
    return *instance;
}

void QohAudioRuntime::configure(const NativeAudioOps& ops) {
    sink_.configure(ops);
    ops_ = ops;
}

void QohAudioRuntime::onDeviceTeardown() {
    sink_.bumpDeviceGeneration();
    ++telemetry_.deviceGenerationChanged;
}

void QohAudioRuntime::resetSession() {
    ++replayCheckpointSession_;
    replayTakeoverBaseline_.valid = false;
    replayStartBaseline_.valid = false;
    timeline_.reset();
    catalog_.seal(false);
    sink_.reset();
    bgm_.reset();
    telemetry_ = AudioTelemetry{};
    resources_ = ResourceMap{};
    disposition_ = AudioTickDisposition::ImmediateOffline;
    epoch_ = 0;
    layoutGeneration_ = 0;
    frame_ = 0;
    ordinal_ = 0;
    correcting_ = false;
    ticking_ = false;
    pendingCount_ = 0;
    correctedNewExpired_ = 0;
    failClosed_ = false;
    catalog_.snapshot(resources_);
}

void QohAudioRuntime::setHorizons(const AudioHorizonConfig& horizons) {
    horizons_.hiddenForwardAudioHorizonFrames =
        clampHiddenForwardHorizon(horizons.hiddenForwardAudioHorizonFrames);
    horizons_.spectatorAudioHorizonFrames =
        clampSpectatorHorizon(horizons.spectatorAudioHorizonFrames);
}

bool QohAudioRuntime::failedClosed() const {
    return failClosed_ || timeline_.overflowFailClosed()
        || bgm_.failedClosed();
}

bool QohAudioRuntime::capturesIntent() const {
    return dispositionProducesIntent(disposition_);
}

bool QohAudioRuntime::forbidsNativeAudio() const {
    return dispositionForbidsNativeAudio(disposition_)
        || (correcting_
            && disposition_ != AudioTickDisposition::PhysicalFlush);
}

bool QohAudioRuntime::beginTick(uint32_t epoch, uint32_t layoutGeneration,
                                uint32_t frame) {
    if (!capturesIntent()) return true;
    if (ticking_) {
        timeline_.abortCapture();
    }
    epoch_ = epoch;
    layoutGeneration_ = layoutGeneration;
    frame_ = frame;
    ordinal_ = 0;
    ticking_ = timeline_.beginCapture(epoch, layoutGeneration, frame);
    return ticking_;
}

bool QohAudioRuntime::commitTick() {
    if (!ticking_) return true;
    ticking_ = false;
    if (!timeline_.commitCapture()) {
        failClosed_ = true;
        return false;
    }
    if (correcting_) {
        return true;
    }
    if (failedClosed()) {
        pendingCount_ = 0;
        bgm_.commitFrame(frame_, telemetry_);
        if (const FrameJournal* journal = timeline_.journalFor(frame_))
            foldResources(resources_, *journal);
        return true;
    }
    if (pendingCount_ < kPendingCapacity) {
        pendingFrames_[pendingCount_++] = PendingFrame{frame_, disposition_};
    } else {
        failClosed_ = true;
        ++telemetry_.journalOverflow;
        pendingCount_ = 0;
    }
    bgm_.commitFrame(frame_, telemetry_);
    if (const FrameJournal* journal = timeline_.journalFor(frame_))
        foldResources(resources_, *journal);
    return true;
}

void QohAudioRuntime::abortTick() {
    if (!ticking_) return;
    ticking_ = false;
    timeline_.abortCapture();
    bgm_.abortFrame(frame_, telemetry_);
}

bool QohAudioRuntime::beginReplayTakeover() {
    return captureReplayBaseline(replayTakeoverBaseline_);
}

bool QohAudioRuntime::captureReplayStart() {
    return captureReplayBaseline(replayStartBaseline_);
}

bool QohAudioRuntime::captureReplayCheckpoint(ReplayCheckpoint& checkpoint) {
    if (ticking_ || correcting_ || pendingCount_ != 0u) return false;
    if (!captureReplayBaseline(checkpoint.baseline)) return false;
    checkpoint.owner = this;
    checkpoint.session = replayCheckpointSession_;
    checkpoint.deviceGeneration = sink_.deviceGeneration();
    return true;
}

bool QohAudioRuntime::restoreReplayCheckpoint(const ReplayCheckpoint& checkpoint) {
    const ReplayTakeoverBaseline& baseline = checkpoint.baseline;
    if (!baseline.valid || checkpoint.owner != this
        || checkpoint.session != replayCheckpointSession_
        || checkpoint.deviceGeneration != sink_.deviceGeneration()
        || baseline.pendingCount != 0u) return false;
    for (int16_t sample = 0; sample <= kMaxSampleId; ++sample) {
        const ResourceEntry& current = resources_.samples[sample];
        const ResourceEntry& saved = baseline.resources.samples[sample];
        if (catalog_.present(sample) != baseline.catalog.present(sample)
            || !(catalog_.assetOf(sample) == baseline.catalog.assetOf(sample))
            || current.present != saved.present
            || !(current.asset == saved.asset)) return false;
    }
    const bool muted = sink_.muted();
    if (!restoreReplayBaseline(baseline)) return false;
    sink_.setMuted(muted);
    return true;
}

bool QohAudioRuntime::captureReplayBaseline(ReplayTakeoverBaseline& baseline) {
    if (ticking_ || correcting_) return false;
    baseline.valid = false;
    baseline.catalog = catalog_;
    baseline.sink = sink_;
    baseline.bgm = bgm_;
    baseline.telemetry = telemetry_;
    baseline.resources = resources_;
    baseline.disposition = disposition_;
    baseline.epoch = epoch_;
    baseline.layoutGeneration = layoutGeneration_;
    baseline.frame = frame_;
    baseline.ordinal = ordinal_;
    for (size_t i = 0u; i < kPendingCapacity; ++i)
        baseline.pendingFrames[i] = pendingFrames_[i];
    baseline.pendingCount = pendingCount_;
    baseline.correctedNewExpired = correctedNewExpired_;
    baseline.failClosed = failClosed_;
    baseline.valid = true;
    return true;
}

bool QohAudioRuntime::restoreReplayStart() {
    return restoreReplayBaseline(replayStartBaseline_);
}

bool QohAudioRuntime::restoreAfterReplayTakeover() {
    return restoreReplayBaseline(replayTakeoverBaseline_);
}

bool QohAudioRuntime::restoreReplayBaseline(
    const ReplayTakeoverBaseline& baseline) {
    if (!baseline.valid) return false;
    abortTick();
    abortCorrection();
    timeline_.reset();
    catalog_ = baseline.catalog;
    sink_ = baseline.sink;
    sink_.discardReplayTakeoverTransientOwners();
    bgm_ = baseline.bgm;
    telemetry_ = baseline.telemetry;
    resources_ = baseline.resources;
    disposition_ = baseline.disposition;
    epoch_ = baseline.epoch;
    layoutGeneration_ = baseline.layoutGeneration;
    frame_ = baseline.frame;
    ordinal_ = baseline.ordinal;
    for (size_t i = 0u; i < kPendingCapacity; ++i)
        pendingFrames_[i] = baseline.pendingFrames[i];
    pendingCount_ = baseline.pendingCount;
    correctedNewExpired_ = baseline.correctedNewExpired;
    failClosed_ = baseline.failClosed;
    correcting_ = false;
    ticking_ = false;
    return true;
}

void QohAudioRuntime::endReplayTakeover() {
    replayTakeoverBaseline_.valid = false;
}

void QohAudioRuntime::emitJournal(const FrameJournal& journal) {
    const AudioTickDisposition saved = disposition_;
    disposition_ = AudioTickDisposition::PhysicalFlush;
    for (uint16_t i = 0; i < journal.commandCount; ++i) {
        const AudioCommand& command = journal.commands[i];
        switch (command.kind) {
            case AudioKind::LoadAttempt:
                if (command.loadOutcome == LoadOutcome::Present)
                    (void)sink_.load(command.sampleId, command.ownedPath);
                else
                    (void)sink_.unload(command.sampleId);
                break;
            case AudioKind::Unload:
                (void)sink_.unload(command.sampleId);
                break;
            case AudioKind::Stop: {
                const PhysicalToken owner =
                    sink_.currentOwner(command.endpoint);
                if (owner.valid())
                    (void)sink_.stopIfOwner(command.endpoint, owner);
                break;
            }
            case AudioKind::PlayDuplicate:
            case AudioKind::PlayBase:
            case AudioKind::Replace: {
                if (command.sampleId < 0
                    || command.loadOutcome == LoadOutcome::Absent) {
                    const PhysicalToken owner =
                        sink_.currentOwner(command.endpoint);
                    if (owner.valid())
                        (void)sink_.stopIfOwner(command.endpoint, owner);
                    break;
                }
                const SinkResult played =
                    command.endpoint.type == AudioEndpoint::Type::Duplicate
                        ? sink_.playDuplicate(
                              command.sampleId, command.endpoint.index,
                              command.flags)
                        : sink_.playBase(command.sampleId, command.flags);
                if (!played.ok) {
                    ++telemetry_.sinkFailure;
                    break;
                }
                ++telemetry_.physicalPlayed;
                EmittedOccurrence occurrence{};
                occurrence.key = semanticKeyOf(command);
                occurrence.frame = command.frame;
                occurrence.ordinal = command.ordinal;
                occurrence.endpoint = command.endpoint;
                occurrence.sampleId = command.sampleId;
                occurrence.token = played.token;
                occurrence.startFrame = command.frame;
                const WaveFacts facts =
                    catalog_.factsFor(command.sampleId, command.asset);
                occurrence.durationFrames =
                    facts.valid ? facts.durationFrames : 0;
                occurrence.looping =
                    (command.flags & 1u) != 0u
                    || (facts.valid && facts.looping);
                (void)timeline_.recordEmission(occurrence, telemetry_);
                break;
            }
        }
    }
    disposition_ = saved;
}

void QohAudioRuntime::flushPendingMuted(uint32_t confirmedThrough) {
    const bool wasMuted = sink_.muted();
    sink_.setMuted(true);
    flushPending(confirmedThrough);
    sink_.setMuted(wasMuted);
}

void QohAudioRuntime::flushPending(uint32_t confirmedThrough) {
    if (failedClosed()) {
        pendingCount_ = 0;
        return;
    }
    if (pendingCount_ == 0) {
        const AudioTickDisposition saved = disposition_;
        disposition_ = AudioTickDisposition::PhysicalFlush;
        bgm_.drainConfirmed(
            confirmedThrough,
            [this](const PendingBgmCommand& command) {
                if (sink_.muted()) return;
                if (ops_.bgm != nullptr)
                    (void)ops_.bgm(command.trackId,
                                   static_cast<int>(command.flags));
            },
            telemetry_);
        disposition_ = saved;
        return;
    }

    const uint32_t visibleFrame = pendingFrames_[pendingCount_ - 1].frame;

    for (size_t p = 0; p < pendingCount_; ++p) {
        const uint32_t frame = pendingFrames_[p].frame;
        const AudioTickDisposition frameDisposition =
            pendingFrames_[p].disposition;
        const uint32_t horizon =
            frameDisposition
                        == AudioTickDisposition::SpectatorConfirmedVisible
                    || frameDisposition
                           == AudioTickDisposition::SpectatorConfirmedCatchup
                ? horizons_.spectatorAudioHorizonFrames
                : horizons_.hiddenForwardAudioHorizonFrames;
        const FrameJournal* journal = timeline_.journalFor(frame);
        if (journal == nullptr) continue;
        const bool isVisibleFrame = frame == visibleFrame;
        if (isVisibleFrame) {
            emitJournal(*journal);
            continue;
        }
        FrameJournal filtered = *journal;
        filtered.commandCount = 0;
        for (uint16_t i = 0; i < journal->commandCount; ++i) {
            const AudioCommand& command = journal->commands[i];
            const bool isPlay =
                command.kind == AudioKind::PlayDuplicate
                || command.kind == AudioKind::PlayBase
                || (command.kind == AudioKind::Replace
                    && command.sampleId >= 0);
            if (isPlay) {
                const uint32_t age = visibleFrame - frame;
                const bool looping = (command.flags & 1u) != 0u;
                if (!looping && age > horizon) {
                    ++telemetry_.spectatorStaleSkipped;
                    continue;
                }
                bool superseded = false;
                for (size_t later = p + 1;
                     later < pendingCount_ && !superseded; ++later) {
                    const FrameJournal* laterJournal =
                        timeline_.journalFor(pendingFrames_[later].frame);
                    if (laterJournal == nullptr) continue;
                    for (uint16_t j = 0; j < laterJournal->commandCount;
                         ++j) {
                        const AudioCommand& laterCommand =
                            laterJournal->commands[j];
                        if ((laterCommand.kind == AudioKind::PlayDuplicate
                             || laterCommand.kind == AudioKind::PlayBase
                             || laterCommand.kind == AudioKind::Replace)
                            && laterCommand.endpoint == command.endpoint) {
                            superseded = true;
                            break;
                        }
                    }
                }
                if (superseded) {
                    ++telemetry_.hiddenForwardCoalesced;
                    continue;
                }
                ++telemetry_.hiddenForwardQueued;
            }
            filtered.commands[filtered.commandCount++] = command;
        }
        emitJournal(filtered);
    }
    pendingCount_ = 0;

    const AudioTickDisposition saved = disposition_;
    disposition_ = AudioTickDisposition::PhysicalFlush;
    bgm_.drainConfirmed(
        confirmedThrough,
        [this](const PendingBgmCommand& command) {
            if (sink_.muted()) return;
            if (ops_.bgm != nullptr)
                (void)ops_.bgm(command.trackId,
                               static_cast<int>(command.flags));
        },
        telemetry_);
    disposition_ = saved;
}

void QohAudioRuntime::captureLoad(int16_t sample, const char* path,
                                  bool cachedSuccess, const AssetKey& asset,
                                  uint32_t callsiteRva) {
    if (!capturesIntent()) return;
    if (!validSampleId(sample)) {
        failClosed_ = true;
        ++telemetry_.journalOverflow;
        return;
    }
    AudioCommand command{};
    command.kind = AudioKind::LoadAttempt;
    command.sampleId = sample;
    command.loadOutcome =
        cachedSuccess ? LoadOutcome::Present : LoadOutcome::Absent;
    command.asset = asset;
    command.callsiteRva = callsiteRva;
    command.setPath(path);
    ++telemetry_.loadDeferred;
    (void)timeline_.append(command, telemetry_);
}

void QohAudioRuntime::captureUnload(int16_t sample, uint32_t callsiteRva) {
    if (!capturesIntent()) return;
    if (!validSampleId(sample)) {
        failClosed_ = true;
        return;
    }
    AudioCommand command{};
    command.kind = AudioKind::Unload;
    command.sampleId = sample;
    command.callsiteRva = callsiteRva;
    (void)timeline_.append(command, telemetry_);
}

void QohAudioRuntime::captureStop(const AudioEndpoint& endpoint,
                                  uint32_t callsiteRva) {
    if (!capturesIntent()) return;
    if (!validEndpoint(endpoint)) {
        failClosed_ = true;
        return;
    }
    AudioCommand command{};
    command.kind = AudioKind::Stop;
    command.endpoint = endpoint;
    command.callsiteRva = callsiteRva;
    (void)timeline_.append(command, telemetry_);
}

void QohAudioRuntime::capturePlayDuplicate(int16_t sample, uint16_t slot,
                                           uint16_t voices, uint32_t flags,
                                           uint32_t callsiteRva) {
    if (!capturesIntent()) return;
    if (!validSampleId(sample) || !validDuplicateSlot(slot) || voices != 0) {
        failClosed_ = true;
        return;
    }
    AudioCommand command{};
    command.kind = AudioKind::PlayDuplicate;
    command.endpoint = {AudioEndpoint::Type::Duplicate, slot};
    command.sampleId = sample;
    command.voices = voices;
    command.flags = flags;
    command.callsiteRva = callsiteRva;
    command.loadOutcome = catalog_.present(sample) ? LoadOutcome::Present
                                                   : LoadOutcome::Absent;
    command.asset = catalog_.assetOf(sample);
    (void)timeline_.append(command, telemetry_);
}

void QohAudioRuntime::capturePlayBase(int16_t sample, uint32_t flags,
                                      uint32_t callsiteRva) {
    if (!capturesIntent()) return;
    if (!validSampleId(sample)) {
        failClosed_ = true;
        return;
    }
    AudioCommand command{};
    command.kind = AudioKind::PlayBase;
    command.endpoint = {AudioEndpoint::Type::Base,
                        static_cast<uint16_t>(sample)};
    command.sampleId = sample;
    command.flags = flags;
    command.callsiteRva = callsiteRva;
    command.loadOutcome = catalog_.present(sample) ? LoadOutcome::Present
                                                   : LoadOutcome::Absent;
    command.asset = catalog_.assetOf(sample);
    (void)timeline_.append(command, telemetry_);
}

bool QohAudioRuntime::captureBgm(uint8_t trackId, uint32_t flags,
                                 uint32_t callsiteRva) {
    if (!capturesIntent()) return false;
    return bgm_.capture(epoch_, frame_, callsiteRva, ordinal_++, trackId,
                        flags, telemetry_);
}

bool QohAudioRuntime::beginCorrection(uint32_t restoreFrame,
                                      uint32_t frontierFrame) {
    if (correcting_) return false;
    correctionRestore_ = restoreFrame;
    correctionFrontier_ = frontierFrame;
    correcting_ = timeline_.beginCorrection();
    if (correcting_)
        bgm_.removeRange(restoreFrame, frontierFrame, telemetry_);
    return correcting_;
}

bool QohAudioRuntime::commitCorrection() {
    if (!correcting_) return false;

    const FrameJournal* staged[kTimelineFrames];
    const size_t stagedCount = timeline_.stagedCount();
    for (size_t i = 0; i < stagedCount; ++i)
        staged[i] = timeline_.staged(i);

    ResourceMap* baseline = new (std::nothrow) ResourceMap(resources_);
    if (baseline == nullptr) {
        abortCorrection();
        failClosed_ = true;
        return false;
    }
    for (uint32_t frame = correctionRestore_;
         frame != correctionFrontier_; ++frame) {
        const FrameJournal* journal = timeline_.journalFor(frame);
        if (journal == nullptr) continue;
        for (uint16_t i = 0; i < journal->commandCount; ++i) {
            const AudioCommand& command = journal->commands[i];
            if (command.kind == AudioKind::LoadAttempt
                || command.kind == AudioKind::Unload) {
                delete baseline;
                abortCorrection();
                failClosed_ = true;
                ++telemetry_.journalOverflow;
                return false;
            }
        }
    }

    OwnerSnapshot owners{};
    for (size_t i = 0; i < stagedCount; ++i) {
        if (staged[i] == nullptr) continue;
        for (uint16_t c = 0; c < staged[i]->commandCount; ++c) {
            const AudioEndpoint endpoint = staged[i]->commands[c].endpoint;
            if (validEndpoint(endpoint))
                (void)owners.set(endpoint, sink_.currentOwner(endpoint));
        }
    }
    for (uint32_t frame = correctionRestore_;
         frame != correctionFrontier_; ++frame) {
        const FrameEmissions* emissions = timeline_.emissionsFor(frame);
        if (emissions == nullptr) continue;
        for (uint16_t i = 0; i < emissions->count; ++i) {
            const AudioEndpoint endpoint = emissions->entries[i].endpoint;
            if (validEndpoint(endpoint))
                (void)owners.set(endpoint, sink_.currentOwner(endpoint));
        }
    }

    ReconcileInput input{};
    input.timeline = &timeline_;
    input.restoreFrame = correctionRestore_;
    input.frontierFrame = correctionFrontier_;
    input.corrected = staged;
    input.correctedCount = stagedCount;
    input.catalog = &catalog_;
    input.owners = &owners;
    input.baselineResources = baseline;
    reconcileCorrection(input, plan_);
    delete baseline;
    if (!plan_.valid) {
        abortCorrection();
        failClosed_ = true;
        return false;
    }

    uint32_t stagedFrames[kTimelineFrames];
    size_t stagedFrameCount = 0;
    for (size_t i = 0; i < stagedCount; ++i) {
        if (staged[i] != nullptr)
            stagedFrames[stagedFrameCount++] = staged[i]->frame;
    }

    executePlan(plan_);
    if (!timeline_.commitCorrection()) {
        failClosed_ = true;
        return false;
    }
    for (size_t i = 0; i < stagedFrameCount; ++i)
        bgm_.commitFrame(stagedFrames[i], telemetry_);
    correcting_ = false;

    telemetry_.replayCreditConsumed += plan_.counters.replayCreditConsumed;
    telemetry_.correctedNewPlayed += plan_.counters.correctedNewPlayed;
    telemetry_.oldOnlyCanceled += plan_.counters.oldOnlyCanceled;
    telemetry_.endpointReplaced += plan_.counters.endpointReplaced;
    telemetry_.cursorResumed += plan_.counters.cursorResumed;
    telemetry_.assetReconciled += plan_.counters.assetReconciled;
    telemetry_.physicalRestartAvoided += plan_.counters.matchedTotal;
    correctedNewExpired_ += plan_.counters.correctedNewExpiredSkipped;
    return true;
}

void QohAudioRuntime::abortCorrection() {
    if (!correcting_) return;
    timeline_.abortCorrection();
    correcting_ = false;
}

void QohAudioRuntime::executePlan(const ReconcileResult& plan) {
    const AudioTickDisposition saved = disposition_;
    disposition_ = AudioTickDisposition::PhysicalFlush;
    PhysicalToken actionTokens[kMaxReconcileActions] = {};
    for (size_t i = 0; i < plan.actionCount; ++i) {
        const SinkAction& action = plan.actions[i];
        switch (action.type) {
            case SinkAction::Type::StopIfOwner:
                if (!sink_.stopIfOwner(action.endpoint, action.owner))
                    ++telemetry_.staleStopRejected;
                break;
            case SinkAction::Type::Load:
                (void)sink_.load(action.sampleId, action.ownedPath);
                break;
            case SinkAction::Type::Unload:
                (void)sink_.unload(action.sampleId);
                break;
            case SinkAction::Type::PlayDuplicate: {
                const SinkResult played = sink_.playDuplicate(
                    action.sampleId, action.endpoint.index, action.flags);
                if (played.ok) {
                    actionTokens[i] = played.token;
                    ++telemetry_.physicalPlayed;
                } else {
                    ++telemetry_.sinkFailure;
                }
                break;
            }
            case SinkAction::Type::PlayBase: {
                const SinkResult played =
                    sink_.playBase(action.sampleId, action.flags);
                if (played.ok) {
                    actionTokens[i] = played.token;
                    ++telemetry_.physicalPlayed;
                } else {
                    ++telemetry_.sinkFailure;
                }
                break;
            }
            case SinkAction::Type::PlayAtCursor: {
                const AssetKey bound = catalog_.assetOf(action.sampleId);
                WaveFacts facts{};
                if (bound.generation == action.key.assetGeneration)
                    facts = catalog_.factsFor(action.sampleId, bound);
                const uint32_t cursor =
                    resumeCursorBytes(facts, action.cursorBytes);
                if (cursor == kCursorExpired) {
                    ++telemetry_.physicalRestartAvoided;
                    break;
                }
                const SinkResult played = sink_.playAtCursor(
                    action.endpoint, action.sampleId, action.flags, cursor);
                if (played.ok) {
                    actionTokens[i] = played.token;
                    ++telemetry_.physicalPlayed;
                } else {
                    ++telemetry_.sinkFailure;
                }
                break;
            }
        }
    }
    disposition_ = saved;

    for (uint32_t frame = correctionRestore_;
         frame != correctionFrontier_; ++frame) {
        EmittedOccurrence corrected[kMaxEmissionsPerFrame];
        size_t count = 0;
        for (size_t i = 0;
             i < plan.emissionCount && count < kMaxEmissionsPerFrame; ++i) {
            const PlannedEmission& emission = plan.emissions[i];
            if (emission.occurrence.frame != frame) continue;
            corrected[count] = emission.occurrence;
            if (emission.tokenFromAction >= 0)
                corrected[count].token =
                    actionTokens[emission.tokenFromAction];
            ++count;
        }
        timeline_.replaceEmissions(frame, corrected, count);
    }
}

void QohAudioRuntime::rotateEpoch(uint32_t newEpoch) {
    flushPending(frame_);
    timeline_.reset();
    catalog_.snapshot(resources_);
    pendingCount_ = 0;
    epoch_ = newEpoch;
}

void QohAudioRuntime::formatLedger(char* out, size_t capacity) const {
    std::snprintf(
        out, capacity,
        "[audio-ledger] intent=%llu played=%llu restartAvoided=%llu "
        "credit=%llu correctedNew=%llu correctedNewExpired=%llu "
        "oldOnlyCanceled=%llu staleStopRejected=%llu cursorResumed=%llu "
        "loadDeferred=%llu assetReconciled=%llu hiddenQueued=%llu "
        "hiddenCoalesced=%llu staleSkipped=%llu journalOverflow=%llu "
        "sinkFailure=%llu bgmQueued=%llu bgmCommitted=%llu "
        "bgmDiscarded=%llu bgmSameTrackPreserved=%llu "
        "bgmSteadySuppressed=%llu bgmOverflow=%llu "
        "bgmUnexpectedCallsite=%llu sinkStaleStops=%llu failedClosed=%d",
        static_cast<unsigned long long>(telemetry_.intentGenerated),
        static_cast<unsigned long long>(telemetry_.physicalPlayed),
        static_cast<unsigned long long>(telemetry_.physicalRestartAvoided),
        static_cast<unsigned long long>(telemetry_.replayCreditConsumed),
        static_cast<unsigned long long>(telemetry_.correctedNewPlayed),
        static_cast<unsigned long long>(correctedNewExpired_),
        static_cast<unsigned long long>(telemetry_.oldOnlyCanceled),
        static_cast<unsigned long long>(telemetry_.staleStopRejected),
        static_cast<unsigned long long>(telemetry_.cursorResumed),
        static_cast<unsigned long long>(telemetry_.loadDeferred),
        static_cast<unsigned long long>(telemetry_.assetReconciled),
        static_cast<unsigned long long>(telemetry_.hiddenForwardQueued),
        static_cast<unsigned long long>(telemetry_.hiddenForwardCoalesced),
        static_cast<unsigned long long>(telemetry_.spectatorStaleSkipped),
        static_cast<unsigned long long>(telemetry_.journalOverflow),
        static_cast<unsigned long long>(telemetry_.sinkFailure),
        static_cast<unsigned long long>(telemetry_.bgmCommandQueued),
        static_cast<unsigned long long>(telemetry_.bgmCommandCommitted),
        static_cast<unsigned long long>(telemetry_.bgmCommandDiscarded),
        static_cast<unsigned long long>(
            telemetry_.bgmSameTrackCommandPreserved),
        static_cast<unsigned long long>(
            telemetry_.bgmSteadyStateSuppressed),
        static_cast<unsigned long long>(telemetry_.bgmQueueOverflow),
        static_cast<unsigned long long>(
            telemetry_.bgmUnexpectedOwnedCallsite),
        static_cast<unsigned long long>(sink_.staleStopsRejected()),
        failedClosed() ? 1 : 0);
}

}
