#include "training/TrainingCombatTools.h"

#include <limits>

namespace qtraining {

namespace {

uint8_t triggerEventPriority(TrainingTriggerEvent event) {
    switch (event) {
        case TrainingTriggerEvent::GuardCancel:
            return 0u;
        case TrainingTriggerEvent::OnWakeup:
        case TrainingTriggerEvent::AfterAirRecovery:
        case TrainingTriggerEvent::AfterGroundRecovery:
        case TrainingTriggerEvent::AfterThrowTech:
            return 1u;
        case TrainingTriggerEvent::AfterBlock:
        case TrainingTriggerEvent::AfterHitstun:
            return 2u;
        case TrainingTriggerEvent::OnHit:
            return 3u;
        default:
            return 0xFFu;
    }
}

uint64_t mixTriggerToken(uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

uint64_t saturatedAdd(uint64_t left, uint64_t right) {
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return left > maximum - right ? maximum : left + right;
}

int32_t saturatedFrameDifference(uint64_t defender, uint64_t attacker) {
    if (defender >= attacker) {
        const uint64_t magnitude = defender - attacker;
        const uint64_t maximum =
            static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
        return magnitude > maximum
            ? std::numeric_limits<int32_t>::max()
            : static_cast<int32_t>(magnitude);
    }

    const uint64_t magnitude = attacker - defender;
    const uint64_t negativeLimit =
        static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + 1ull;
    if (magnitude >= negativeLimit) return std::numeric_limits<int32_t>::min();
    return -static_cast<int32_t>(magnitude);
}

}

TrainingTriggerController::TrainingTriggerController() {
    reset();
}

void TrainingTriggerController::reset() {
    for (std::size_t i = 0; i < kTrainingTriggerEventCount; ++i)
        configs_[i] = TrainingTriggerConfig{};
    timelineEstablished_ = false;
    clearTimelineState(0, false);
    timelineEstablished_ = false;
}

void TrainingTriggerController::resetTimeline(
    uint32_t trainingGeneration, bool preserveObservationCounters) {
    clearTimelineState(trainingGeneration, preserveObservationCounters);
    timelineEstablished_ = true;
}

void TrainingTriggerController::journal(
    TrainingTriggerEvent event, TriggerJournalKind kind,
    TriggerJournalCause cause, uint64_t stepSerial, uint16_t moveIndex,
    uint8_t macroSlot, uint32_t detail) {
    if (journalCount_ >= kTrainingTriggerJournalCapacity) {
        ++journalDropped_;
        return;
    }
    TrainingTriggerJournalEntry& entry = journal_[journalCount_++];
    entry.stepSerial = stepSerial;
    entry.detail = detail;
    entry.moveIndex = moveIndex;
    entry.event = event;
    entry.kind = kind;
    entry.cause = cause;
    entry.macroSlot = macroSlot;
}

void TrainingTriggerController::markEventAnswered(TrainingTriggerEvent event,
                                                  uint64_t stepSerial) {
    if (!trainingTriggerEventValid(event)) return;
    bool* latch = nullptr;
    switch (event) {
        case TrainingTriggerEvent::AfterBlock: latch = &blockLatched_; break;
        case TrainingTriggerEvent::AfterHitstun:
            latch = &hitstunLatched_; break;
        case TrainingTriggerEvent::OnWakeup: latch = &wakeupLatched_; break;
        case TrainingTriggerEvent::AfterAirRecovery:
            latch = &airRecoveryLatched_; break;
        case TrainingTriggerEvent::AfterGroundRecovery:
            latch = &groundRecoveryLatched_; break;
        case TrainingTriggerEvent::AfterThrowTech:
            latch = &throwTechLatched_; break;
        case TrainingTriggerEvent::GuardCancel:
            latch = &guardCancelLatched_; break;
        default: break;
    }
    bool cleared = false;
    if (latch != nullptr && *latch) {
        *latch = false;
        cleared = true;
    }
    for (PendingRequest& request : pending_) {
        if (!request.active || request.event != event) continue;
        request.active = false;
        if (status_.pendingCount > 0u) --status_.pendingCount;
        cleared = true;
        recordOutcome(request.event, request.macroSlot, request.moveIndex,
                      stepSerial, TriggerRequestOutcome::DroppedSuperseded);
    }
    clearPrebuffer(event);
    if (cleared) {
        journal(event, TriggerJournalKind::Cleared,
                TriggerJournalCause::Fired, stepSerial,
                configs_[static_cast<std::size_t>(event)].moveIndex);
    }
}

uint32_t TrainingTriggerController::drainJournal(
    TrainingTriggerJournalEntry* out, uint32_t max) {
    if (out == nullptr || max == 0u) {
        journalCount_ = 0u;
        return 0u;
    }
    const uint32_t taken = journalCount_ < max ? journalCount_ : max;
    for (uint32_t i = 0u; i < taken; ++i) out[i] = journal_[i];
    journalCount_ = 0u;
    return taken;
}

void TrainingTriggerController::clearTimelineState(
    uint32_t trainingGeneration, bool preserveObservationCounters) {
    const uint32_t startedCount = status_.startedCount;
    const uint32_t chanceSkippedCount = status_.chanceSkippedCount;
    const uint32_t droppedCount = status_.droppedCount;
    const uint32_t invalidContactCount = status_.invalidContactCount;
    for (PendingRequest& request : pending_) request = PendingRequest{};
    status_ = TrainingTriggerStatus{};
    status_.trainingGeneration = trainingGeneration;
    if (preserveObservationCounters) {
        status_.startedCount = startedCount;
        status_.chanceSkippedCount = chanceSkippedCount;
        status_.droppedCount = droppedCount;
        status_.invalidContactCount = invalidContactCount;
    }
    haveSample_ = false;
    lastStepSerial_ = 0;
    previousDummyAction_ = 0;
    previousDummyActorToken_ = 0;
    previousDummyActionEpoch_ = 0;
    lastContactSerial_ = 0;
    nextSequence_ = 1;
    blockLatched_ = false;
    hitstunLatched_ = false;
    wakeupLatched_ = false;
    airRecoveryLatched_ = false;
    groundRecoveryLatched_ = false;
    throwTechLatched_ = false;
    for (std::size_t i = 0; i < kTrainingTriggerEventCount; ++i) {
        prebufferPrepared_[i] = false;
        prebufferAccepted_[i] = false;
        prebufferMoveIndex_[i] = 0u;
    }
    guardCancelLatched_ = false;
    blockLatchStep_ = 0;
    hitstunLatchStep_ = 0;
    wakeupLatchStep_ = 0;
    airRecoveryLatchStep_ = 0;
    groundRecoveryLatchStep_ = 0;
    throwTechLatchStep_ = 0;
    guardCancelLatchStep_ = 0;
    guardCancelContactSerial_ = 0;
    diagnostics_ = TrainingTriggerDiagnostics{};
}

bool TrainingTriggerController::eventHasResponse(
    TrainingTriggerEvent event) const {
    if (!trainingTriggerEventValid(event)) return false;
    const TrainingTriggerConfig& config =
        configs_[static_cast<std::size_t>(event)];
    return (config.macroSlot != 0u || config.moveIndex != 0u)
        && config.chancePercent != 0u;
}

uint8_t TrainingTriggerController::armedEventMask() const {
    uint8_t mask = 0u;
    const auto arm = [&mask](bool latched, TrainingTriggerEvent event) {
        if (latched) {
            mask |= static_cast<uint8_t>(
                1u << static_cast<uint8_t>(event));
        }
    };
    arm(blockLatched_, TrainingTriggerEvent::AfterBlock);
    arm(hitstunLatched_, TrainingTriggerEvent::AfterHitstun);
    arm(wakeupLatched_, TrainingTriggerEvent::OnWakeup);
    arm(airRecoveryLatched_, TrainingTriggerEvent::AfterAirRecovery);
    arm(groundRecoveryLatched_, TrainingTriggerEvent::AfterGroundRecovery);
    arm(throwTechLatched_, TrainingTriggerEvent::AfterThrowTech);
    arm(guardCancelLatched_, TrainingTriggerEvent::GuardCancel);
    return mask;
}

void TrainingTriggerController::explainStep(
    const TrainingTriggerStepResult& result, uint64_t stepSerial,
    bool macroIdle, bool moveResponseReady, bool guardCancelWindowOpen,
    bool zeroDelayWakeupMoveReady, bool wakeupPrebufferInFlight) {
    TrainingTriggerDiagnostics out{};
    out.responseReady = moveResponseReady;
    out.guardCancelWindowOpen = guardCancelWindowOpen;
    out.macroIdle = macroIdle;
    out.armedEventMask = armedEventMask();

    const TrainingTriggerEvent armedOrder[] = {
        TrainingTriggerEvent::GuardCancel,
        TrainingTriggerEvent::OnWakeup,
        TrainingTriggerEvent::AfterThrowTech,
        TrainingTriggerEvent::AfterGroundRecovery,
        TrainingTriggerEvent::AfterAirRecovery,
        TrainingTriggerEvent::AfterBlock,
        TrainingTriggerEvent::AfterHitstun,
    };
    for (const TrainingTriggerEvent event : armedOrder) {
        const uint8_t bit = static_cast<uint8_t>(
            1u << static_cast<uint8_t>(event));
        if ((out.armedEventMask & bit) == 0u) continue;
        out.haveArmedEvent = true;
        out.armedEvent = event;
        break;
    }

    const PendingRequest* best = nullptr;
    for (const PendingRequest& request : pending_) {
        if (!request.active) continue;
        if (best == nullptr
            || triggerEventPriority(request.event)
                < triggerEventPriority(best->event)
            || (triggerEventPriority(request.event)
                    == triggerEventPriority(best->event)
                && request.sequence < best->sequence)) {
            best = &request;
        }
    }
    if (best != nullptr) {
        out.havePending = true;
        out.pendingEvent = best->event;
        out.pendingMacroSlot = best->macroSlot;
        out.pendingMoveIndex = best->moveIndex;
        out.pendingDueStep = best->dueStep;
    }

    if (result.outcome == TriggerRequestOutcome::Started) {
        out.reason = TriggerSuppressionReason::None;
    } else if (!macroIdle) {
        out.reason = TriggerSuppressionReason::BlockedMacroBusy;
    } else if (best != nullptr) {
        if (best->event == TrainingTriggerEvent::GuardCancel
            && !guardCancelWindowOpen) {
            out.reason = TriggerSuppressionReason::WaitingForGuardCancelWindow;
        } else if (best->macroSlot == 0u
                   && best->event != TrainingTriggerEvent::GuardCancel
                   && !moveResponseReady) {
            out.reason = TriggerSuppressionReason::WaitingForResponseReady;
        } else if (best->dueStep > stepSerial) {
            out.reason = TriggerSuppressionReason::WaitingForDelay;
        } else if (best->requiresWakeupRelease && !zeroDelayWakeupMoveReady
                   && wakeupPrebufferInFlight) {
            out.reason = TriggerSuppressionReason::WaitingForWakeupRelease;
        } else {
            out.reason = TriggerSuppressionReason::WaitingForDelay;
        }
    } else if (out.haveArmedEvent) {
        out.reason = eventHasResponse(out.armedEvent)
            ? TriggerSuppressionReason::WaitingForResponseReady
            : TriggerSuppressionReason::EventArmedNotConfigured;
    } else {
        switch (status_.lastOutcome) {
            case TriggerRequestOutcome::ChanceSkipped:
                out.reason = TriggerSuppressionReason::ChanceSkipped;
                break;
            case TriggerRequestOutcome::DroppedUnavailableMove:
                out.reason = TriggerSuppressionReason::BlockedMoveUnavailable;
                break;
            case TriggerRequestOutcome::DroppedEmptySlot:
                out.reason = TriggerSuppressionReason::BlockedEmptySlot;
                break;
            case TriggerRequestOutcome::DroppedWindowClosed:
                out.reason =
                    TriggerSuppressionReason::WaitingForGuardCancelWindow;
                break;
            default:
                out.reason = TriggerSuppressionReason::NoEventArmed;
                break;
        }
    }
    diagnostics_ = out;
}

bool TrainingTriggerController::setConfig(
    TrainingTriggerEvent event, const TrainingTriggerConfig& configValue) {
    if (!trainingTriggerEventValid(event)
        || !trainingTriggerConfigValid(configValue)) {
        return false;
    }
    for (PendingRequest& request : pending_) {
        if (!request.active || request.event != event) continue;
        request.active = false;
        --status_.pendingCount;
        ++status_.droppedCount;
        recordOutcome(request.event, request.macroSlot, request.moveIndex,
                      lastStepSerial_,
                      TriggerRequestOutcome::DroppedConfigurationChanged);
    }
    configs_[static_cast<std::size_t>(event)] = configValue;
    clearPrebuffer(event);
    return true;
}

void TrainingTriggerController::clearPrebuffer(TrainingTriggerEvent event) {
    if (prebufferEvent(event)
        && prebufferPrepared_[static_cast<std::size_t>(event)]) {
        journal(event, TriggerJournalKind::PrebufferCleared,
                TriggerJournalCause::None, lastStepSerial_,
                prebufferMoveIndex_[static_cast<std::size_t>(event)]);
    }
    if (!prebufferEvent(event)) return;
    const std::size_t index = static_cast<std::size_t>(event);
    prebufferPrepared_[index] = false;
    prebufferAccepted_[index] = false;
    prebufferMoveIndex_[index] = 0u;
}

void TrainingTriggerController::preparePrebuffer(
    TrainingTriggerEvent event, uint64_t stepSerial) {
    if (!prebufferEvent(event)) return;
    const std::size_t index = static_cast<std::size_t>(event);
    const TrainingTriggerConfig& config = configs_[index];
    prebufferPrepared_[index] = config.macroSlot == 0u
        && config.moveIndex != 0u
        && config.delaySteps == 0u;
    prebufferMoveIndex_[index] =
        prebufferPrepared_[index] ? config.moveIndex : 0u;
    if (!prebufferPrepared_[index]) {
        prebufferAccepted_[index] = false;
        journal(event, TriggerJournalKind::PrebufferPrepared,
                TriggerJournalCause::None, stepSerial, config.moveIndex,
                config.macroSlot,
                prebufferAccepted_[index] ? 1u : 0u);
        journal(event, TriggerJournalKind::PrebufferCleared,
                config.moveIndex == 0u && config.macroSlot == 0u
                    ? TriggerJournalCause::NotConfigured
                    : TriggerJournalCause::None,
                stepSerial, config.moveIndex, config.macroSlot);
        return;
    }
    const uint64_t token = mixTriggerToken(
        (static_cast<uint64_t>(status_.trainingGeneration) << 32)
        ^ (static_cast<uint64_t>(static_cast<uint8_t>(event))
           * 0xD6E8FEB86659FD93ull)
        ^ (stepSerial * 0xA0761D6478BD642Full)
        ^ nextSequence_);
    prebufferAccepted_[index] = config.chancePercent != 0u
        && (config.chancePercent >= 100u
            || token % 100u < config.chancePercent);
}

TrainingTriggerConfig TrainingTriggerController::config(
    TrainingTriggerEvent event) const {
    if (!trainingTriggerEventValid(event)) return TrainingTriggerConfig{};
    return configs_[static_cast<std::size_t>(event)];
}

void TrainingTriggerController::discardPendingForNewCombatEpisode(
    uint64_t stepSerial) {
    for (PendingRequest& request : pending_) {
        if (!request.active) continue;
        request.active = false;
        --status_.pendingCount;
        ++status_.droppedCount;
        recordOutcome(request.event, request.macroSlot, request.moveIndex,
                      stepSerial,
                      TriggerRequestOutcome::DroppedSuperseded);
    }
}

void TrainingTriggerController::recordOutcome(
    TrainingTriggerEvent event, uint8_t macroSlot, uint16_t moveIndex,
    uint64_t stepSerial, TriggerRequestOutcome outcome) {
    journal(event, TriggerJournalKind::Outcome,
            outcome == TriggerRequestOutcome::ChanceSkipped
                ? TriggerJournalCause::None
                : (outcome == TriggerRequestOutcome::DroppedSuperseded
                       ? TriggerJournalCause::Superseded
                       : TriggerJournalCause::None),
            stepSerial, moveIndex, macroSlot,
            static_cast<uint32_t>(outcome));

    status_.lastEvent = event;
    status_.lastMacroSlot = macroSlot;
    status_.lastMoveIndex = moveIndex;
    status_.lastEventStep = stepSerial;
    status_.lastOutcome = outcome;
}

bool TrainingTriggerController::schedule(
    TrainingTriggerEvent event, uint64_t edgeStep, uint64_t sourceToken,
    bool chanceAlreadyDecided, bool chanceAccepted) {
    if (!trainingTriggerEventValid(event)) return false;
    const TrainingTriggerConfig& setting =
        configs_[static_cast<std::size_t>(event)];
    if (setting.macroSlot == 0 && setting.moveIndex == 0) return false;

    if ((event == TrainingTriggerEvent::OnHit && setting.moveIndex != 0u)
        || event == TrainingTriggerEvent::GuardCancel) {
        for (PendingRequest& request : pending_) {
            if (!request.active || request.event != event) continue;
            request.active = false;
            --status_.pendingCount;
            ++status_.droppedCount;
            recordOutcome(request.event, request.macroSlot, request.moveIndex,
                          edgeStep,
                          TriggerRequestOutcome::DroppedSuperseded);
        }
    }

    const uint64_t chanceToken = mixTriggerToken(
        (static_cast<uint64_t>(status_.trainingGeneration) << 32)
        ^ (static_cast<uint64_t>(static_cast<uint8_t>(event))
           * 0xD6E8FEB86659FD93ull)
        ^ (edgeStep * 0xA0761D6478BD642Full)
        ^ sourceToken);
    const bool accepted = chanceAlreadyDecided
        ? chanceAccepted
        : setting.chancePercent != 0
            && (setting.chancePercent >= 100
                || chanceToken % 100u < setting.chancePercent);
    if (!accepted) {
        ++status_.chanceSkippedCount;
        recordOutcome(event, setting.macroSlot, setting.moveIndex, edgeStep,
                      TriggerRequestOutcome::ChanceSkipped);
        return false;
    }

    PendingRequest* freeRequest = nullptr;
    for (PendingRequest& request : pending_) {
        if (!request.active) {
            freeRequest = &request;
            break;
        }
    }
    if (freeRequest == nullptr) {
        ++status_.droppedCount;
        recordOutcome(event, setting.macroSlot, setting.moveIndex, edgeStep,
                      TriggerRequestOutcome::DroppedQueueFull);
        return false;
    }

    freeRequest->active = true;
    freeRequest->event = event;
    freeRequest->macroSlot = setting.macroSlot;
    freeRequest->moveIndex = setting.moveIndex;
    freeRequest->dueStep = saturatedAdd(edgeStep, setting.delaySteps);
    freeRequest->sequence = nextSequence_++;
    freeRequest->requiresWakeupRelease =
        prebufferEvent(event)
        && setting.moveIndex != 0u && setting.delaySteps == 0u;
    ++status_.pendingCount;
    return true;
}

TrainingTriggerStepResult TrainingTriggerController::serviceDue(
    uint64_t stepSerial, bool macroIdle, uint8_t validMacroSlotMask,
    uint64_t validMoveMask, bool moveResponseReady,
    bool guardCancelWindowOpen, bool zeroDelayWakeupMoveReady,
    bool wakeupPrebufferInFlight) {
    TrainingTriggerStepResult result{};

    if (!guardCancelWindowOpen) {
        for (PendingRequest& request : pending_) {
            if (!request.active
                || request.event != TrainingTriggerEvent::GuardCancel) {
                continue;
            }
            request.active = false;
            --status_.pendingCount;
            ++status_.droppedCount;
            recordOutcome(request.event, request.macroSlot, request.moveIndex,
                          stepSerial,
                          TriggerRequestOutcome::DroppedWindowClosed);
        }
    }

    const auto inputWindowOpen = [moveResponseReady](
                                     const PendingRequest& request) {
        return request.macroSlot != 0u
            || request.event == TrainingTriggerEvent::GuardCancel
            || moveResponseReady;
    };

    PendingRequest* selected = nullptr;
    for (;;) {
        PendingRequest* best = nullptr;
        for (PendingRequest& request : pending_) {
            if (!request.active) continue;
            if (best == nullptr
                || triggerEventPriority(request.event)
                    < triggerEventPriority(best->event)
                || (triggerEventPriority(request.event)
                        == triggerEventPriority(best->event)
                    && request.sequence < best->sequence)) {
                best = &request;
            }
        }
        if (best == nullptr) break;
        if (best->dueStep > stepSerial || !inputWindowOpen(*best))
            return result;

        if (!macroIdle) {
            const TrainingTriggerEvent missedEvent = best->event;
            const uint8_t missedSlot = best->macroSlot;
            const uint16_t missedMove = best->moveIndex;
            for (PendingRequest& request : pending_) {
                if (!request.active) continue;
                request.active = false;
                --status_.pendingCount;
                ++status_.droppedCount;
            }
            recordOutcome(missedEvent, missedSlot, missedMove, stepSerial,
                          TriggerRequestOutcome::DroppedBusy);
            result.outcome = TriggerRequestOutcome::DroppedBusy;
            return result;
        }

        bool available = false;
        TriggerRequestOutcome unavailable =
            TriggerRequestOutcome::DroppedEmptySlot;
        if (best->macroSlot == kTrainingTriggerRandomMacroSlot) {
            available = validMacroSlotMask != 0u;
        } else if (best->macroSlot != 0u) {
            const uint8_t slotBit = static_cast<uint8_t>(1u
                << static_cast<uint8_t>(best->macroSlot - 1u));
            available = (validMacroSlotMask & slotBit) != 0u;
        } else if (best->moveIndex != 0u) {
            available = best->moveIndex <= kTrainingTriggerMaximumMoveIndex
                && (validMoveMask
                    & (uint64_t{1u} << (best->moveIndex - 1u))) != 0u;
            unavailable = TriggerRequestOutcome::DroppedUnavailableMove;
            if (available && best->requiresWakeupRelease
                && !zeroDelayWakeupMoveReady) {
                if (wakeupPrebufferInFlight) return result;
                bool haveAlternative = false;
                for (const PendingRequest& other : pending_) {
                    if (!other.active || &other == best) continue;
                    haveAlternative = true;
                    break;
                }
                if (!haveAlternative) best->requiresWakeupRelease = false;
                else {
                    available = false;
                    unavailable = TriggerRequestOutcome::DroppedWindowClosed;
                }
            }
        }
        if (available) {
            selected = best;
            break;
        }

        best->active = false;
        --status_.pendingCount;
        ++status_.droppedCount;
        recordOutcome(best->event, best->macroSlot, best->moveIndex,
                      stepSerial, unavailable);
    }

    if (selected == nullptr) {
        result.outcome = status_.lastOutcome;
        return result;
    }

    const TrainingTriggerEvent selectedEvent = selected->event;
    uint8_t selectedSlot = selected->macroSlot;
    const uint16_t selectedMove = selected->moveIndex;
    if (selectedSlot == kTrainingTriggerRandomMacroSlot) {
        uint8_t recorded[kTrainingMacroSlotCount] = {};
        uint8_t count = 0u;
        for (uint8_t slot = 0u; slot < kTrainingMacroSlotCount; ++slot) {
            if ((validMacroSlotMask & (1u << slot)) != 0u)
                recorded[count++] = static_cast<uint8_t>(slot + 1u);
        }
        const uint64_t pick = mixTriggerToken(
            (static_cast<uint64_t>(status_.trainingGeneration) << 32)
            ^ (selected->sequence * 0xE7037ED1A0B428DBull)
            ^ (stepSerial * 0xA0761D6478BD642Full));
        selectedSlot = recorded[pick % count];
    }
    selected->active = false;
    --status_.pendingCount;
    ++status_.startedCount;

    for (PendingRequest& request : pending_) {
        if (!request.active) continue;
        request.active = false;
        --status_.pendingCount;
        ++status_.droppedCount;
        recordOutcome(request.event, request.macroSlot,
                      request.moveIndex, stepSerial,
                      TriggerRequestOutcome::DroppedCollision);
    }
    recordOutcome(selectedEvent, selectedSlot, selectedMove, stepSerial,
                  TriggerRequestOutcome::Started);
    if (selectedSlot != 0u) {
        result.requestMacroStart = true;
        result.macroSlotIndex = static_cast<uint8_t>(selectedSlot - 1u);
    } else {
        result.requestMoveStart = true;
        result.moveIndex = selectedMove;
    }
    result.event = selectedEvent;
    result.outcome = TriggerRequestOutcome::Started;
    return result;
}

TrainingTriggerStepResult TrainingTriggerController::advance(
    const CombatFrameSample& sample, const CombatContactEvent* contacts,
    std::size_t contactCount, bool macroIdle, uint8_t validMacroSlotMask,
    uint64_t validMoveMask, bool zeroDelayWakeupMoveReady,
    bool wakeupPrebufferInFlight) {
    TrainingTriggerStepResult result{};

    if (!timelineEstablished_
        || status_.trainingGeneration != sample.trainingGeneration) {
        resetTimeline(sample.trainingGeneration, false);
    }
    if (!sample.readable || !sample.roundValid) {
        clearTimelineState(sample.trainingGeneration, true);
        timelineEstablished_ = true;
        diagnostics_.reason = TriggerSuppressionReason::TimelineNotMeasurable;
        return result;
    }
    if (haveSample_ && sample.stepSerial <= lastStepSerial_) return result;

    if (contacts == nullptr && contactCount != 0) {
        status_.invalidContactCount += static_cast<uint32_t>(contactCount);
        contactCount = 0;
    }

    bool dummyHitContactObserved = false;
    for (std::size_t index = 0; index < contactCount; ++index) {
        const CombatContactEvent& contact = contacts[index];
        if (contact.serial <= lastContactSerial_) continue;
        lastContactSerial_ = contact.serial;
        status_.lastContactSerial = contact.serial;
        if (!combatContactIsWellFormed(contact)
            || contact.stepSerial > sample.stepSerial) {
            ++status_.invalidContactCount;
            continue;
        }
        if (contact.victimOwner != kCombatDummyOwner) continue;

        discardPendingForNewCombatEpisode(contact.stepSerial);

        wakeupLatched_ = false;
        clearPrebuffer(TrainingTriggerEvent::OnWakeup);

        if (contact.kind == CombatContactKind::Hit) {
            dummyHitContactObserved = true;
            blockLatched_ = false;
            airRecoveryLatched_ = false;
            groundRecoveryLatched_ = false;
            throwTechLatched_ = false;
            guardCancelLatched_ = false;
            hitstunLatched_ = true;
            hitstunLatchStep_ = contact.stepSerial;
            journal(TrainingTriggerEvent::AfterHitstun,
                    TriggerJournalKind::Latched,
                    TriggerJournalCause::ContactHit, contact.stepSerial,
                    configs_[static_cast<std::size_t>(
                        TrainingTriggerEvent::AfterHitstun)].moveIndex,
                    0u, static_cast<uint32_t>(contact.serial));
            clearPrebuffer(TrainingTriggerEvent::AfterBlock);
            preparePrebuffer(TrainingTriggerEvent::AfterHitstun,
                             contact.stepSerial);
            schedule(TrainingTriggerEvent::OnHit, contact.stepSerial,
                     contact.serial);
        } else if (contact.kind == CombatContactKind::Block) {
            hitstunLatched_ = false;
            airRecoveryLatched_ = false;
            groundRecoveryLatched_ = false;
            throwTechLatched_ = false;
            blockLatched_ = true;
            blockLatchStep_ = contact.stepSerial;
            journal(TrainingTriggerEvent::AfterBlock,
                    TriggerJournalKind::Latched,
                    TriggerJournalCause::ContactBlock, contact.stepSerial,
                    configs_[static_cast<std::size_t>(
                        TrainingTriggerEvent::AfterBlock)].moveIndex,
                    0u, static_cast<uint32_t>(contact.serial));
            clearPrebuffer(TrainingTriggerEvent::AfterHitstun);
            preparePrebuffer(TrainingTriggerEvent::AfterBlock,
                             contact.stepSerial);
            guardCancelLatched_ = true;
            guardCancelLatchStep_ = contact.stepSerial;
            journal(TrainingTriggerEvent::GuardCancel,
                    TriggerJournalKind::Latched,
                    TriggerJournalCause::ContactBlock, sample.stepSerial,
                    configs_[static_cast<std::size_t>(
                        TrainingTriggerEvent::GuardCancel)].moveIndex,
                    0u, static_cast<uint32_t>(0u));
            guardCancelContactSerial_ = contact.serial;
        } else {
            blockLatched_ = false;
            hitstunLatched_ = false;
            wakeupLatched_ = false;
            airRecoveryLatched_ = false;
            groundRecoveryLatched_ = false;
            throwTechLatched_ = false;
            guardCancelLatched_ = false;
            clearPrebuffer(TrainingTriggerEvent::AfterBlock);
            clearPrebuffer(TrainingTriggerEvent::AfterHitstun);
        }
    }

    const CombatOwnerSample& dummyOwner =
        sample.owners[kCombatDummyOwner];
    const uint32_t dummyAction = dummyOwner.action;
    const bool identityAvailable = dummyOwner.actorToken != 0u
        && dummyOwner.actionEpoch != 0u;
    const bool actionEntry = haveSample_
        && (dummyAction != previousDummyAction_
            || (identityAvailable
                && (dummyOwner.actorToken != previousDummyActorToken_
                    || dummyOwner.actionEpoch
                           != previousDummyActionEpoch_)));

    if (guardCancelLatched_ && actionEntry
        && !combatActionIsGuard(dummyAction)) {
        guardCancelLatched_ = false;
    }

    if ((actionEntry || dummyHitContactObserved)
        && combatActionIsNaturalWakeupPath(dummyAction)) {
        const bool newWakeupEpisode = !wakeupLatched_;
        wakeupLatched_ = true;
        journal(TrainingTriggerEvent::OnWakeup,
                TriggerJournalKind::Latched,
                TriggerJournalCause::ActionEntry, sample.stepSerial,
                configs_[static_cast<std::size_t>(
                    TrainingTriggerEvent::OnWakeup)].moveIndex,
                0u, static_cast<uint32_t>(dummyAction));
        if (newWakeupEpisode) wakeupLatchStep_ = sample.stepSerial;
        if (newWakeupEpisode) {
            clearPrebuffer(TrainingTriggerEvent::AfterHitstun);
            preparePrebuffer(TrainingTriggerEvent::OnWakeup,
                             sample.stepSerial);
        }
    }
    if (actionEntry && combatActionIsAirRecovery(dummyAction)) {
        wakeupLatched_ = false;
        clearPrebuffer(TrainingTriggerEvent::OnWakeup);
        airRecoveryLatched_ = true;
        airRecoveryLatchStep_ = sample.stepSerial;
        journal(TrainingTriggerEvent::AfterAirRecovery,
                TriggerJournalKind::Latched,
                TriggerJournalCause::ActionEntry, sample.stepSerial,
                configs_[static_cast<std::size_t>(
                    TrainingTriggerEvent::AfterAirRecovery)].moveIndex,
                0u, dummyAction);
        const TrainingTriggerConfig& exactRecovery =
            configs_[static_cast<std::size_t>(
                TrainingTriggerEvent::AfterAirRecovery)];
        const bool exactRecoveryCanCompete =
            (exactRecovery.macroSlot != 0u || exactRecovery.moveIndex != 0u)
            && exactRecovery.chancePercent != 0u;
        if (hitstunLatched_ && !exactRecoveryCanCompete) {
            (void)schedule(TrainingTriggerEvent::AfterHitstun,
                           sample.stepSerial, nextSequence_);
            hitstunLatched_ = false;
        }
    }
    if (actionEntry && combatActionIsGroundRecovery(dummyAction)) {
        wakeupLatched_ = false;
        clearPrebuffer(TrainingTriggerEvent::OnWakeup);
        groundRecoveryLatched_ = true;
        groundRecoveryLatchStep_ = sample.stepSerial;
        journal(TrainingTriggerEvent::AfterGroundRecovery,
                TriggerJournalKind::Latched,
                TriggerJournalCause::ActionEntry, sample.stepSerial,
                configs_[static_cast<std::size_t>(
                    TrainingTriggerEvent::AfterGroundRecovery)].moveIndex,
                0u, static_cast<uint32_t>(dummyAction));
        const TrainingTriggerConfig& exactRecovery =
            configs_[static_cast<std::size_t>(
                TrainingTriggerEvent::AfterGroundRecovery)];
        const bool exactRecoveryCanCompete =
            (exactRecovery.macroSlot != 0u || exactRecovery.moveIndex != 0u)
            && exactRecovery.chancePercent != 0u;
        if (hitstunLatched_ && !exactRecoveryCanCompete) {
            (void)schedule(TrainingTriggerEvent::AfterHitstun,
                           sample.stepSerial, nextSequence_);
            hitstunLatched_ = false;
        }
    }
    if (actionEntry && combatActionIsThrowTechResult(dummyAction)) {
        throwTechLatched_ = true;
        throwTechLatchStep_ = sample.stepSerial;
        journal(TrainingTriggerEvent::AfterThrowTech,
                TriggerJournalKind::Latched,
                TriggerJournalCause::ActionEntry, sample.stepSerial,
                configs_[static_cast<std::size_t>(
                    TrainingTriggerEvent::AfterThrowTech)].moveIndex,
                0u, static_cast<uint32_t>(dummyAction));
    }

    if (guardCancelLatched_
        && sample.stepSerial >= guardCancelLatchStep_
        && combatActionIsGuard(dummyAction)
        && sample.owners[kCombatDummyOwner].guardCancelReady) {
        (void)schedule(TrainingTriggerEvent::GuardCancel, sample.stepSerial,
                       guardCancelContactSerial_);
        guardCancelLatched_ = false;
    }

    if (sample.owners[kCombatDummyOwner].responseReady) {
        const auto prebufferIndex = [](TrainingTriggerEvent event) {
            return static_cast<std::size_t>(event);
        };
        if (blockLatched_ && sample.stepSerial > blockLatchStep_) {
            const std::size_t index =
                prebufferIndex(TrainingTriggerEvent::AfterBlock);
            schedule(TrainingTriggerEvent::AfterBlock, sample.stepSerial,
                     nextSequence_, prebufferPrepared_[index],
                     prebufferAccepted_[index]);
            blockLatched_ = false;
            guardCancelLatched_ = false;
            clearPrebuffer(TrainingTriggerEvent::AfterBlock);
        }
        if (hitstunLatched_ && sample.stepSerial > hitstunLatchStep_) {
            const std::size_t index =
                prebufferIndex(TrainingTriggerEvent::AfterHitstun);
            schedule(TrainingTriggerEvent::AfterHitstun, sample.stepSerial,
                     nextSequence_, prebufferPrepared_[index],
                     prebufferAccepted_[index]);
            hitstunLatched_ = false;
            clearPrebuffer(TrainingTriggerEvent::AfterHitstun);
        }
        if (wakeupLatched_ && sample.stepSerial > wakeupLatchStep_) {
            const std::size_t index =
                prebufferIndex(TrainingTriggerEvent::OnWakeup);
            (void)schedule(TrainingTriggerEvent::OnWakeup, sample.stepSerial,
                           nextSequence_, prebufferPrepared_[index],
                           prebufferAccepted_[index]);
            wakeupLatched_ = false;
            clearPrebuffer(TrainingTriggerEvent::OnWakeup);
        }
        if (airRecoveryLatched_
            && sample.stepSerial >= airRecoveryLatchStep_) {
            (void)schedule(TrainingTriggerEvent::AfterAirRecovery,
                           sample.stepSerial, nextSequence_);
            airRecoveryLatched_ = false;
        }
        if (groundRecoveryLatched_
            && sample.stepSerial >= groundRecoveryLatchStep_) {
            (void)schedule(TrainingTriggerEvent::AfterGroundRecovery,
                           sample.stepSerial, nextSequence_);
            groundRecoveryLatched_ = false;
        }
        if (throwTechLatched_ && sample.stepSerial >= throwTechLatchStep_) {
            (void)schedule(TrainingTriggerEvent::AfterThrowTech,
                           sample.stepSerial, nextSequence_);
            throwTechLatched_ = false;
        }
    }

    previousDummyAction_ = dummyAction;
    previousDummyActorToken_ = dummyOwner.actorToken;
    previousDummyActionEpoch_ = dummyOwner.actionEpoch;
    lastStepSerial_ = sample.stepSerial;
    haveSample_ = true;
    const bool guardCancelWindowOpen = combatActionIsGuard(dummyAction)
        && sample.owners[kCombatDummyOwner].guardCancelReady;
    result = serviceDue(
        sample.stepSerial, macroIdle, validMacroSlotMask, validMoveMask,
        sample.owners[kCombatDummyOwner].responseReady,
        guardCancelWindowOpen, zeroDelayWakeupMoveReady,
        wakeupPrebufferInFlight);
    if (result.outcome == TriggerRequestOutcome::Started
        && result.event == TrainingTriggerEvent::GuardCancel) {
        blockLatched_ = false;
    }
    explainStep(result, sample.stepSerial, macroIdle,
                sample.owners[kCombatDummyOwner].responseReady,
                guardCancelWindowOpen, zeroDelayWakeupMoveReady,
                wakeupPrebufferInFlight);
    return result;
}

FrameAdvantageTracker::FrameAdvantageTracker(uint64_t timeoutSteps)
    : timeoutSteps_(timeoutSteps == 0 ? 1 : timeoutSteps) {
    reset();
}

void FrameAdvantageTracker::reset() {
    enabled_ = true;
    timelineEstablished_ = false;
    haveStep_ = false;
    lastStepSerial_ = 0;
    lastContactSerial_ = 0;
    retainedReadoutAgeSteps_ = 0;
    closeGapCandidate();
    gapDisplaySteps_ = 0;
    status_ = FrameAdvantageStatus{};
}

void FrameAdvantageTracker::resetTimeline(
    uint32_t trainingGeneration, bool preserveObservationCounters) {
    const uint32_t completedCount = status_.completedCount;
    const uint32_t tradeCount = status_.tradeCount;
    const uint32_t timeoutCount = status_.timeoutCount;
    const uint32_t invalidContactCount = status_.invalidContactCount;
    const uint32_t unmeasurableCount = status_.unmeasurableCount;
    const uint32_t gapCount = status_.gapCount;
    haveStep_ = false;
    lastStepSerial_ = 0;
    lastContactSerial_ = 0;
    retainedReadoutAgeSteps_ = 0;
    closeGapCandidate();
    gapDisplaySteps_ = 0;
    status_ = FrameAdvantageStatus{};
    status_.trainingGeneration = trainingGeneration;
    if (preserveObservationCounters) {
        status_.completedCount = completedCount;
        status_.tradeCount = tradeCount;
        status_.timeoutCount = timeoutCount;
        status_.invalidContactCount = invalidContactCount;
        status_.unmeasurableCount = unmeasurableCount;
        status_.gapCount = gapCount;
    }
    timelineEstablished_ = true;
}

void FrameAdvantageTracker::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    const uint32_t generation = status_.trainingGeneration;
    resetTimeline(generation, false);
}

bool FrameAdvantageTracker::identityChanged(
    uint64_t expected, uint64_t observed) const {
    return expected != 0 && observed != 0 && expected != observed;
}

bool FrameAdvantageTracker::validateContactBatch(
    const CombatFrameSample& sample, const CombatContactEvent* contacts,
    std::size_t contactCount, uint32_t& invalidCount) const {
    invalidCount = 0;
    if (contactCount == 0) return true;
    if (contacts == nullptr) {
        invalidCount = contactCount > std::numeric_limits<uint32_t>::max()
            ? std::numeric_limits<uint32_t>::max()
            : static_cast<uint32_t>(contactCount);
        return false;
    }

    uint64_t previousSerial = lastContactSerial_;
    for (std::size_t index = 0; index < contactCount; ++index) {
        const CombatContactEvent& contact = contacts[index];
        const bool valid = combatContactIsWellFormed(contact)
            && contact.stepSerial == sample.stepSerial
            && contact.serial > previousSerial;
        if (!valid && invalidCount != std::numeric_limits<uint32_t>::max())
            ++invalidCount;
        previousSerial = contact.serial;
    }
    return invalidCount == 0;
}

void FrameAdvantageTracker::beginEpisode(
    const CombatContactEvent& contact, const CombatFrameSample& sample) {
    const bool hadResolvedReadout =
        status_.state == FrameAdvantageState::Complete
        || status_.state == FrameAdvantageState::Trade;
    retainedReadoutAgeSteps_ = 0;
    status_.state = FrameAdvantageState::Measuring;
    status_.reason = FrameAdvantageReason::None;
    status_.contactKind = contact.kind;
    status_.attackerOwner = contact.attackerOwner;
    status_.defenderOwner = contact.victimOwner;
    status_.attackerIsPrimary = contact.attackerIsPrimary;
    status_.attackerReady = false;
    status_.defenderReady = false;
    status_.advantageValid = false;
    status_.advantageFrames = 0;
    status_.contactSerial = contact.serial;
    status_.contactStep = contact.stepSerial;
    status_.attackerReadyDelay = 0;
    status_.defenderReadyDelay = 0;
    const CombatOwnerSample& attacker = sample.owners[contact.attackerOwner];
    const CombatOwnerSample& defender = sample.owners[contact.victimOwner];
    status_.attackerActorToken = contact.attackerActorToken != 0
        ? contact.attackerActorToken : attacker.actorToken;
    status_.defenderActorToken = contact.victimActorToken != 0
        ? contact.victimActorToken : defender.actorToken;
    status_.attackerActionEpoch = contact.attackerActionEpoch != 0
        ? contact.attackerActionEpoch : attacker.actionEpoch;
    status_.defenderActionEpoch = contact.victimActionEpoch != 0
        ? contact.victimActionEpoch : defender.actionEpoch;
    status_.defenderLockObserved = defender.defensiveLock;
    status_.chainedActions = 0;
    applyGapToEpisode(contact, attacker, defender, hadResolvedReadout);
}

void FrameAdvantageTracker::markTrade(
    const CombatContactEvent& contact) {
    retainedReadoutAgeSteps_ = 0;
    ++status_.tradeCount;
    status_.state = FrameAdvantageState::Trade;
    status_.reason = FrameAdvantageReason::Trade;
    status_.contactKind = contact.kind;
    status_.attackerOwner = contact.attackerOwner;
    status_.defenderOwner = contact.victimOwner;
    status_.attackerIsPrimary = contact.attackerIsPrimary;
    status_.contactSerial = contact.serial;
    status_.contactStep = contact.stepSerial;
    status_.attackerReady = false;
    status_.defenderReady = false;
    status_.advantageValid = false;
    status_.advantageFrames = 0;
    status_.attackerReadyDelay = 0;
    status_.defenderReadyDelay = 0;
    status_.attackerActorToken = contact.attackerActorToken;
    status_.defenderActorToken = contact.victimActorToken;
    status_.attackerActionEpoch = contact.attackerActionEpoch;
    status_.defenderActionEpoch = contact.victimActionEpoch;
    status_.defenderLockObserved = false;
    status_.chainedActions = 0;
    status_.gapValid = false;
    status_.gapFrames = 0;
    closeGapCandidate();
}

void FrameAdvantageTracker::openGapCandidate(const uint64_t stepSerial) {
    gapCandidateOpen_ = true;
    gapOpenStep_ = stepSerial;
    gapAttackerOwner_ = status_.attackerOwner;
    gapDefenderOwner_ = status_.defenderOwner;
    gapAttackerActorToken_ = status_.attackerActorToken;
    gapDefenderActorToken_ = status_.defenderActorToken;
}

void FrameAdvantageTracker::closeGapCandidate() {
    gapCandidateOpen_ = false;
    gapOpenStep_ = 0;
    gapAttackerOwner_ = 0;
    gapDefenderOwner_ = 0;
    gapAttackerActorToken_ = 0;
    gapDefenderActorToken_ = 0;
}

void FrameAdvantageTracker::ageGapReadout() {
    if (gapDisplaySteps_ == 0u) return;
    if (--gapDisplaySteps_ != 0u) return;
    status_.gapValid = false;
    status_.gapFrames = 0;
}

void FrameAdvantageTracker::applyGapToEpisode(
    const CombatContactEvent& contact, const CombatOwnerSample& attacker,
    const CombatOwnerSample& defender, const bool hadResolvedReadout) {
    status_.gapValid = false;
    status_.gapFrames = 0;
    gapDisplaySteps_ = 0;
    if (hadResolvedReadout) {
        closeGapCandidate();
        return;
    }
    if (!gapCandidateOpen_) return;
    const bool samePair =
        contact.attackerOwner == gapAttackerOwner_
        && contact.victimOwner == gapDefenderOwner_
        && !identityChanged(gapAttackerActorToken_, attacker.actorToken)
        && !identityChanged(gapDefenderActorToken_, defender.actorToken);
    if (samePair && contact.stepSerial >= gapOpenStep_) {
        const uint64_t span = contact.stepSerial - gapOpenStep_;
        if (span != 0u && span <= kFrameAdvantageMaxGapSteps) {
            status_.gapValid = true;
            status_.gapFrames = static_cast<uint32_t>(span);
            gapDisplaySteps_ = kFrameAdvantageGapDisplaySteps;
            if (status_.gapCount != std::numeric_limits<uint32_t>::max())
                ++status_.gapCount;
        }
    }
    closeGapCandidate();
}

void FrameAdvantageTracker::markUnmeasurable(
    FrameAdvantageReason reason) {
    if (status_.state != FrameAdvantageState::Measuring) return;
    closeGapCandidate();
    retainedReadoutAgeSteps_ = 0;
    status_.state = FrameAdvantageState::Unmeasurable;
    status_.reason = reason;
    status_.advantageValid = false;
    status_.advantageFrames = 0;
    ++status_.unmeasurableCount;
}

void FrameAdvantageTracker::clearReadout() {
    const uint32_t generation = status_.trainingGeneration;
    const uint32_t completedCount = status_.completedCount;
    const uint32_t tradeCount = status_.tradeCount;
    const uint32_t timeoutCount = status_.timeoutCount;
    const uint32_t invalidContactCount = status_.invalidContactCount;
    const uint32_t unmeasurableCount = status_.unmeasurableCount;
    const uint32_t gapCount = status_.gapCount;
    retainedReadoutAgeSteps_ = 0;
    closeGapCandidate();
    gapDisplaySteps_ = 0;
    status_ = FrameAdvantageStatus{};
    status_.trainingGeneration = generation;
    status_.completedCount = completedCount;
    status_.tradeCount = tradeCount;
    status_.timeoutCount = timeoutCount;
    status_.invalidContactCount = invalidContactCount;
    status_.unmeasurableCount = unmeasurableCount;
    status_.gapCount = gapCount;
}

void FrameAdvantageTracker::ageRetainedReadout() {
    if (status_.state != FrameAdvantageState::Complete
        && status_.state != FrameAdvantageState::Trade) {
        return;
    }
    ++retainedReadoutAgeSteps_;
    if (retainedReadoutAgeSteps_
        >= kFrameAdvantageReadoutLifetimeSteps) {
        clearReadout();
    }
}

void FrameAdvantageTracker::replaceWithUnmeasurable(
    FrameAdvantageReason reason) {
    if (status_.state == FrameAdvantageState::Measuring) {
        markUnmeasurable(reason);
        return;
    }
    clearReadout();
    status_.state = FrameAdvantageState::Unmeasurable;
    status_.reason = reason;
    ++status_.unmeasurableCount;
}

void FrameAdvantageTracker::invalidateActiveOrClear(
    FrameAdvantageReason reason) {
    if (status_.state == FrameAdvantageState::Measuring) {
        markUnmeasurable(reason);
        return;
    }
    if (status_.state == FrameAdvantageState::Unmeasurable
        && status_.reason == reason) {
        return;
    }
    if (status_.state != FrameAdvantageState::Idle) clearReadout();
}

void FrameAdvantageTracker::advance(
    const CombatFrameSample& sample, const CombatContactEvent* contacts,
    std::size_t contactCount) {
    if (!timelineEstablished_
        || status_.trainingGeneration != sample.trainingGeneration) {
        resetTimeline(sample.trainingGeneration, false);
    }
    if (!enabled_) return;
    if (!sample.readable) {
        invalidateActiveOrClear(FrameAdvantageReason::UnreadableSample);
        haveStep_ = false;
        lastStepSerial_ = 0;
        return;
    }
    if (!sample.roundValid) {
        invalidateActiveOrClear(FrameAdvantageReason::RoundEnded);
        haveStep_ = false;
        lastStepSerial_ = 0;
        return;
    }

    if (!haveStep_ && status_.state == FrameAdvantageState::Unmeasurable
        && (status_.reason == FrameAdvantageReason::UnreadableSample
            || status_.reason == FrameAdvantageReason::RoundEnded)) {
        clearReadout();
    }

    if (haveStep_ && sample.stepSerial <= lastStepSerial_) {
        invalidateActiveOrClear(FrameAdvantageReason::NonMonotonicStep);
        return;
    }
    if (haveStep_ && sample.stepSerial != lastStepSerial_ + 1u) {
        invalidateActiveOrClear(FrameAdvantageReason::StepGap);
    }

    ageRetainedReadout();
    ageGapReadout();

    uint32_t invalidCount = 0;
    if (!validateContactBatch(sample, contacts, contactCount, invalidCount)) {
        const uint32_t room = std::numeric_limits<uint32_t>::max()
            - status_.invalidContactCount;
        status_.invalidContactCount += invalidCount > room ? room : invalidCount;
        replaceWithUnmeasurable(FrameAdvantageReason::InvalidContactBatch);
        lastStepSerial_ = sample.stepSerial;
        haveStep_ = true;
        return;
    }

    if (contactCount != 0) {
        const CombatContactEvent* selected = &contacts[contactCount - 1u];
        const CombatContactEvent* rejected = nullptr;
        FrameAdvantageReason rejectReason = FrameAdvantageReason::None;
        bool sawForward = false;
        bool sawReverse = false;

        for (std::size_t index = 0; index < contactCount; ++index) {
            const CombatContactEvent& contact = contacts[index];
            if (contact.attackerOwner == 0 && contact.victimOwner == 1)
                sawForward = true;
            else if (contact.attackerOwner == 1 && contact.victimOwner == 0)
                sawReverse = true;
        }
        const bool trade = sawForward && sawReverse;

        const std::size_t firstChecked = trade ? 0u : contactCount - 1u;
        for (std::size_t index = firstChecked; index < contactCount; ++index) {
            const CombatContactEvent& contact = contacts[index];
            const CombatOwnerSample& attacker =
                sample.owners[contact.attackerOwner];
            const CombatOwnerSample& defender =
                sample.owners[contact.victimOwner];
            if (!contact.victimIsPrimary) {
                rejected = &contact;
                rejectReason = FrameAdvantageReason::NonPrimaryVictim;
                break;
            }
            if (!contact.defenderLockEligible) {
                rejected = &contact;
                rejectReason = FrameAdvantageReason::DefenderNoLock;
                break;
            }
            if (identityChanged(contact.attackerActorToken,
                                attacker.actorToken)) {
                rejected = &contact;
                rejectReason = FrameAdvantageReason::AttackerActorChanged;
                break;
            }
            if (identityChanged(contact.victimActorToken,
                                defender.actorToken)) {
                rejected = &contact;
                rejectReason = FrameAdvantageReason::DefenderActorChanged;
                break;
            }
        }

        lastContactSerial_ = contacts[contactCount - 1u].serial;
        if (rejected != nullptr) {
            beginEpisode(*rejected, sample);
            markUnmeasurable(rejectReason);
        } else if (trade) {
            markTrade(*selected);
        } else {
            beginEpisode(*selected, sample);
        }
    }

    if (contactCount == 0 && status_.state != FrameAdvantageState::Idle) {
        const CombatOwnerSample& attacker =
            sample.owners[status_.attackerOwner];
        const CombatOwnerSample& defender =
            sample.owners[status_.defenderOwner];
        if (identityChanged(status_.attackerActorToken,
                            attacker.actorToken)) {
            invalidateActiveOrClear(
                FrameAdvantageReason::AttackerActorChanged);
        } else if (identityChanged(status_.defenderActorToken,
                                   defender.actorToken)) {
            invalidateActiveOrClear(
                FrameAdvantageReason::DefenderActorChanged);
        }
    }

    if (status_.state == FrameAdvantageState::Measuring
        && sample.stepSerial >= status_.contactStep) {
        const uint64_t elapsed = sample.stepSerial - status_.contactStep;
        const CombatOwnerSample& attacker =
            sample.owners[status_.attackerOwner];
        const CombatOwnerSample& defender =
            sample.owners[status_.defenderOwner];

        if (identityChanged(status_.attackerActorToken,
                            attacker.actorToken)) {
            markUnmeasurable(FrameAdvantageReason::AttackerActorChanged);
        } else if (identityChanged(status_.defenderActorToken,
                                   defender.actorToken)) {
            markUnmeasurable(FrameAdvantageReason::DefenderActorChanged);
        }

        if (status_.state == FrameAdvantageState::Measuring
            && !status_.attackerReady) {
            if (status_.attackerIsPrimary) {
                PrimaryRecoveryDisposition disposition =
                    attacker.primaryRecovery;
                if (disposition == PrimaryRecoveryDisposition::Pending
                    && attacker.naturalReady) {
                    disposition = PrimaryRecoveryDisposition::Ready;
                }
                switch (disposition) {
                    case PrimaryRecoveryDisposition::Ready:
                        status_.attackerReady = true;
                        status_.attackerReadyDelay = elapsed;
                        break;
                    case PrimaryRecoveryDisposition::Cancelled:
                        markUnmeasurable(
                            FrameAdvantageReason::AttackerCancelled);
                        break;
                    case PrimaryRecoveryDisposition::Interrupted:
                        markUnmeasurable(
                            FrameAdvantageReason::AttackerInterrupted);
                        break;
                    case PrimaryRecoveryDisposition::ActorChanged:
                        markUnmeasurable(
                            FrameAdvantageReason::AttackerActorChanged);
                        break;
                    case PrimaryRecoveryDisposition::Continued:
                        status_.attackerActionEpoch = attacker.actionEpoch;
                        break;
                    case PrimaryRecoveryDisposition::Rechained:
                        status_.attackerActionEpoch = attacker.actionEpoch;
                        ++status_.chainedActions;
                        break;
                    case PrimaryRecoveryDisposition::Pending:
                        if (identityChanged(status_.attackerActionEpoch,
                                            attacker.actionEpoch)) {
                            markUnmeasurable(
                                FrameAdvantageReason::AttackerInterrupted);
                        }
                        break;
                }
            } else if (attacker.responseReady) {
                status_.attackerReady = true;
                status_.attackerReadyDelay = elapsed;
            }
        }

        if (status_.state == FrameAdvantageState::Measuring
            && !status_.defenderReady
            && sample.stepSerial > status_.contactStep) {
            if (defender.responseReady) {
                status_.defenderReady = true;
                status_.defenderReadyDelay = elapsed;
                openGapCandidate(sample.stepSerial);
            } else if (!defender.semanticPathValid) {
                markUnmeasurable(
                    FrameAdvantageReason::DefenderPathInvalid);
            } else {
                if (defender.defensiveLock)
                    status_.defenderLockObserved = true;
                else if (status_.defenderLockObserved)
                    markUnmeasurable(
                        FrameAdvantageReason::DefenderLockLost);
            }
        }

        if (status_.state == FrameAdvantageState::Measuring
            && status_.attackerReady && status_.defenderReady) {
            status_.advantageFrames = saturatedFrameDifference(
                status_.defenderReadyDelay, status_.attackerReadyDelay);
            status_.advantageValid = true;
            status_.state = FrameAdvantageState::Complete;
            status_.reason = FrameAdvantageReason::Completed;
            retainedReadoutAgeSteps_ = 0;
            ++status_.completedCount;
        } else if (status_.state == FrameAdvantageState::Measuring
                   && elapsed >= timeoutSteps_) {
            status_.state = FrameAdvantageState::TimedOut;
            status_.advantageValid = false;
            retainedReadoutAgeSteps_ = 0;
            if (!status_.attackerReady && !status_.defenderReady)
                status_.reason = FrameAdvantageReason::TimeoutBoth;
            else if (!status_.attackerReady)
                status_.reason = FrameAdvantageReason::TimeoutAttacker;
            else
                status_.reason = FrameAdvantageReason::TimeoutDefender;
            ++status_.timeoutCount;
        }
    }

    lastStepSerial_ = sample.stepSerial;
    haveStep_ = true;
}

}
