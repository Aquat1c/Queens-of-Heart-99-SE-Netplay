#include "training/TrainingRecovery.h"

namespace qtraining {

namespace {

uint32_t forwardBit(uint8_t facing) {
    return qinput::applyNativeFacing(qinput::kLogicalRight, facing);
}

uint32_t backBit(uint8_t facing) {
    return qinput::applyNativeFacing(qinput::kLogicalLeft, facing);
}

uint32_t tapBitFor(RecoveryBranch branch, uint8_t facing) {
    switch (branch) {
        case RecoveryBranch::Air22:
            return qinput::kLogicalDown;
        case RecoveryBranch::Ground88:
            return qinput::kLogicalUp;
        case RecoveryBranch::Wall66:
        case RecoveryBranch::TerminalForward66:
            return forwardBit(facing);
        case RecoveryBranch::TerminalBack44:
            return backBit(facing);
        case RecoveryBranch::ThrowEscape:
            return 0;
        default:
            return 0;
    }
}

RecoveryState stateForEmitIndex(RecoveryBranch branch, uint32_t index) {
    if (branch == RecoveryBranch::ThrowEscape)
        return RecoveryState::Idle;
    switch (index) {
        case 0: return RecoveryState::EmitPreNeutral;
        case 1: return RecoveryState::EmitTap1;
        case 2: return RecoveryState::EmitNeutral;
        default: return RecoveryState::EmitTap2;
    }
}

bool emissionStepIsTap(RecoveryBranch branch, uint32_t index) {
    if (branch == RecoveryBranch::ThrowEscape) return false;
    return index == 1 || index == 3;
}

}

bool recoveryBranchAcceptsResult(RecoveryBranch branch, uint32_t action) {
    switch (branch) {
        case RecoveryBranch::Air22:
            return action == 31;
        case RecoveryBranch::Ground88:
            return action == 33;
        case RecoveryBranch::Wall66:
            return action == 33 || action == 34;
        case RecoveryBranch::TerminalForward66:
            return action == 13;
        case RecoveryBranch::TerminalBack44:
            return action == 14;
        case RecoveryBranch::ThrowEscape:
            return false;
        default:
            return false;
    }
}

bool recoveryBranchActionEligible(RecoveryBranch branch, uint32_t action) {
    switch (branch) {
        case RecoveryBranch::Air22:
            return recoveryAirEligibleAction(action);
        case RecoveryBranch::Ground88:
            return recoveryGroundEligibleAction(action);
        case RecoveryBranch::Wall66:
            return recoveryWallEligibleAction(action);
        case RecoveryBranch::TerminalForward66:
        case RecoveryBranch::TerminalBack44:
            return recoveryTerminalEligibleAction(action);
        case RecoveryBranch::ThrowEscape:
            return false;
        default:
            return false;
    }
}

uint32_t recoveryBranchWindow(RecoveryBranch branch) {
    switch (branch) {
        case RecoveryBranch::Air22:
            return kRecoveryWindowAir;
        case RecoveryBranch::Ground88:
        case RecoveryBranch::Wall66:
            return kRecoveryWindowGround;
        case RecoveryBranch::TerminalForward66:
        case RecoveryBranch::TerminalBack44:
            return kRecoveryWindowTerminal;
        case RecoveryBranch::ThrowEscape:
            return 0;
        default:
            return 0;
    }
}

uint32_t recoveryEmissionStepCount(RecoveryBranch branch) {
    return branch == RecoveryBranch::ThrowEscape ? 0u : 4u;
}

RecoveryStepMask recoveryEmissionStep(RecoveryBranch branch,
                                      uint32_t index, uint8_t facing) {
    RecoveryStepMask mask{};
    if (branch == RecoveryBranch::ThrowEscape) return mask;
    if (index == 1 || index == 3) mask.directions = tapBitFor(branch, facing);
    return mask;
}

RecoveryStepMask recoveryAwaitMask(RecoveryBranch branch, uint8_t facing) {
    RecoveryStepMask mask{};
    switch (branch) {
        case RecoveryBranch::TerminalForward66:
        case RecoveryBranch::TerminalBack44:
            mask.directions = tapBitFor(branch, facing);
            return mask;
        case RecoveryBranch::ThrowEscape:
            return mask;
        default:
            return mask;
    }
}

uint64_t recoverySplitMix64(uint64_t& state) {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint64_t recoverySeedFor(uint32_t trainingGeneration) {
    uint64_t seed = 0x243F6A8885A308D3ull
        ^ (static_cast<uint64_t>(trainingGeneration) * 0x9E3779B97F4A7C15ull);
    return seed == 0 ? 0x9E3779B97F4A7C15ull : seed;
}

uint32_t recoveryStandDownMask(const RecoveryObservation& observation) {
    uint32_t mask = RecoveryStandDownNone;
    if (!observation.readable) mask |= RecoveryStandDownNotReadable;
    if (!observation.lifecycleAvailable) mask |= RecoveryStandDownLifecycleDown;
    if (!observation.sandboxActive) mask |= RecoveryStandDownSandboxInactive;
    if (observation.menuCaptured) mask |= RecoveryStandDownMenuCaptured;
    if (observation.operationNeutral) mask |= RecoveryStandDownOperationNeutral;
    if (observation.macroPlaybackActive) mask |= RecoveryStandDownMacroPlayback;
    if (observation.macroOverrideP1) mask |= RecoveryStandDownMacroOwnsP1;
    if (observation.comControlled) mask |= RecoveryStandDownComControlled;
    if (observation.directionSuppressed)
        mask |= RecoveryStandDownDirectionSuppressed;
    if (observation.koLatch) mask |= RecoveryStandDownKoLatch;
    return mask;
}

namespace {

bool recoveryCommonGates(const RecoveryObservation& observation) {
    return (recoveryStandDownMask(observation)
            & kRecoveryStandDownCommonGateMask) == 0u;
}

}

bool terminalRecoveryWouldMatch(const DirectionRingSnapshot& ring,
                                uint32_t nextDirection) {
    if (nextDirection != kRecoveryRingBack
        && nextDirection != kRecoveryRingForward) {
        return false;
    }
    constexpr uint32_t kMask = kRecoveryDirectionRingSize - 1u;

    DirectionRingSnapshot after = ring;
    uint32_t head = ring.head & kMask;
    if (after.values[head] == nextDirection) {
        ++after.dwell[head];
    } else {
        head = (head + 1u) & kMask;
        after.values[head] = nextDirection;
        after.dwell[head] = 0u;
    }

    const uint32_t pattern[3] = { nextDirection, 0u, nextDirection };
    uint32_t matched = 0u;
    uint32_t dwellSum = 0u;
    uint32_t position = head;
    for (uint32_t scanned = 0u; scanned < kRecoveryRingScanCap; ++scanned) {
        dwellSum += after.dwell[position];
        if (dwellSum >= kRecoveryWindowTerminal) return false;
        if (after.values[position] == pattern[matched]) {
            if (++matched == 3u) return true;
        }
        position = (position - 1u) & kMask;
    }
    return false;
}

uint32_t recoveryEligibleMask(const RecoveryObservation& observation) {
    if (!observation.readable) return 0;
    uint32_t mask = recoveryArmableMask(observation);
    if (observation.directionRingHead != kRecoveryRingForward) {
        mask &= ~recoveryBranchBit(RecoveryBranch::TerminalForward66);
    }
    if (observation.directionRingHead != kRecoveryRingBack) {
        mask &= ~recoveryBranchBit(RecoveryBranch::TerminalBack44);
    }
    mask &= ~recoveryBranchBit(RecoveryBranch::ThrowEscape);
    return mask;
}

uint32_t recoveryArmableMask(const RecoveryObservation& observation) {
    if (!observation.readable) return 0;
    if (!recoveryCommonGates(observation)) return 0;

    uint32_t mask = 0;
    const bool airPermitted =
        (observation.permission & kRecoveryPermitAir) != 0;
    const bool groundPermitted =
        (observation.permission & kRecoveryPermitGround) != 0;
    const bool lockClear = observation.recoveryLock == 0;

    if (recoveryPostureAdmitsManual(observation.postureNibble) && lockClear) {
        if (airPermitted && recoveryAirEligibleAction(observation.action))
            mask |= recoveryBranchBit(RecoveryBranch::Air22);
        if (groundPermitted && !airPermitted) {
            if (recoveryGroundEligibleAction(observation.action))
                mask |= recoveryBranchBit(RecoveryBranch::Ground88);
            if (recoveryWallEligibleAction(observation.action))
                mask |= recoveryBranchBit(RecoveryBranch::Wall66);
        }
    }

    if (observation.terminalReady
        && recoveryPostureAdmitsTerminal(observation.postureNibble)
        && recoveryTerminalEligibleAction(observation.action)) {
        mask |= recoveryBranchBit(RecoveryBranch::TerminalForward66)
            | recoveryBranchBit(RecoveryBranch::TerminalBack44);
    }
    return mask;
}

void TrainingRecoveryController::resetTimeline() {
    attempt_ = Attempt{};
    cached_ = RecoveryEmission{};
    lastVerdict_ = RecoveryVerdict::None;
    lastAttemptTaps_ = 0;
    previousArmableMask_ = 0;
    previousAction_ = 0;
    previousHitEpoch_ = 0;
    retryNotBeforeStep_ = 0;
    haveObservation_ = false;
    haveStep_ = false;
    lastStepSerial_ = 0;
}

void TrainingRecoveryController::reset(uint32_t trainingGeneration) {
    resetTimeline();
    counters_ = RecoveryCounters{};
    rng_ = recoverySeedFor(trainingGeneration);
    generation_ = trainingGeneration;
}

void TrainingRecoveryController::publishVerdict(RecoveryEmission& result,
                                                RecoveryVerdict verdict,
                                                uint32_t taps) {
    lastVerdict_ = verdict;
    lastAttemptTaps_ = taps;
    result.lastVerdict = verdict;
    result.verdictIsNew = true;
}

void TrainingRecoveryController::cancelAttempt(RecoveryVerdict verdict) {
    if (!attempt_.active) return;
    attempt_ = Attempt{};
    switch (verdict) {
        case RecoveryVerdict::Cancelled: ++counters_.cancelled; break;
        case RecoveryVerdict::MissedExpired: ++counters_.missed; break;
        default: break;
    }
    if (verdict == RecoveryVerdict::Cancelled
        || verdict == RecoveryVerdict::MissedExpired) {
        retryNotBeforeStep_ = lastStepSerial_ + kRecoveryRetryGapSteps;
    }
}

bool TrainingRecoveryController::generationStillValid(
    const RecoveryObservation& observation) const {
    if (observation.slot != attempt_.slot) return false;
    if (observation.commandChild != attempt_.commandChild) return false;
    if (!recoveryCommonGates(observation)) return false;
    if (recoveryBranchConsultsManualLock(attempt_.branch)) {
        if (!recoveryPostureAdmitsManual(observation.postureNibble))
            return false;
    } else if (!recoveryPostureAdmitsTerminal(observation.postureNibble)) {
        return false;
    }
    return recoveryBranchActionEligible(attempt_.branch, observation.action);
}

uint32_t TrainingRecoveryController::delayForMode(
    ManualRecoveryMode mode, const RecoverySettings& settings) {
    switch (mode) {
        case ManualRecoveryMode::Earliest:
            return 0;
        case ManualRecoveryMode::FixedDelay:
            return settings.delaySteps;
        case ManualRecoveryMode::RandomDelay: {
            const uint32_t low = settings.randomMinSteps;
            const uint32_t high = settings.randomMaxSteps < low
                ? low : settings.randomMaxSteps;
            const uint32_t span = high - low + 1u;
            return low + static_cast<uint32_t>(recoverySplitMix64(rng_) % span);
        }
        case ManualRecoveryMode::Late:
            return kRecoveryLateDelaySteps;
        case ManualRecoveryMode::Off:
        default:
            return 0;
    }
}

ManualRecoveryMode TrainingRecoveryController::modeForBranch(
    const RecoverySettings& settings, RecoveryBranch branch) const {
    ManualRecoveryMode mode = ManualRecoveryMode::Off;
    switch (branch) {
        case RecoveryBranch::Air22: mode = settings.air; break;
        case RecoveryBranch::Ground88: mode = settings.ground; break;
        case RecoveryBranch::Wall66: mode = settings.wall; break;
        case RecoveryBranch::TerminalForward66:
        case RecoveryBranch::TerminalBack44: mode = settings.terminal; break;
        case RecoveryBranch::ThrowEscape:
            return ManualRecoveryMode::Off;
        default: return ManualRecoveryMode::Off;
    }
    if (!nativeRecoveryReleaseQualified(mode, settings.evaluatorQualified))
        return ManualRecoveryMode::Off;
    return mode;
}

bool TrainingRecoveryController::chooseArmableBranch(
    const RecoverySettings& settings, uint32_t risingMask,
    RecoveryBranch& branch) {
    const RecoveryBranch order[] = {
        RecoveryBranch::Air22,
        RecoveryBranch::Ground88,
        RecoveryBranch::Wall66,
    };
    for (RecoveryBranch candidate : order) {
        if ((risingMask & recoveryBranchBit(candidate)) == 0) continue;
        if (modeForBranch(settings, candidate) == ManualRecoveryMode::Off)
            continue;
        branch = candidate;
        return true;
    }

    if (modeForBranch(settings, RecoveryBranch::TerminalForward66)
        != ManualRecoveryMode::Off) {
        RecoveryBranch terminal = RecoveryBranch::TerminalForward66;
        bool wantTerminal = true;
        switch (settings.terminalChoice) {
            case TerminalRecoveryChoice::Forward:
                terminal = RecoveryBranch::TerminalForward66;
                break;
            case TerminalRecoveryChoice::Back:
                terminal = RecoveryBranch::TerminalBack44;
                break;
            case TerminalRecoveryChoice::Random:
                terminal = (recoverySplitMix64(rng_) & 1u) != 0
                    ? RecoveryBranch::TerminalForward66
                    : RecoveryBranch::TerminalBack44;
                break;
            case TerminalRecoveryChoice::Preserve:
            default:
                wantTerminal = false;
                break;
        }
        if (wantTerminal
            && (risingMask & recoveryBranchBit(terminal)) != 0) {
            branch = terminal;
            return true;
        }
    }

    return false;
}

RecoveryEmission TrainingRecoveryController::advance(
    uint64_t realStepSerial, const RecoverySettings& settings,
    const RecoveryObservation& observation) {
    const uint32_t standDown = recoveryStandDownMask(observation);

    if (haveStep_) {
        if (realStepSerial == lastStepSerial_) {
            RecoveryEmission duplicate = cached_;
            duplicate.duplicateStep = true;
            return duplicate;
        }
        if (realStepSerial < lastStepSerial_) {
            const uint32_t taps = attempt_.tapsEmitted;
            cancelAttempt(RecoveryVerdict::Cancelled);
            RecoveryEmission failed{};
            failed.valid = false;
            failed.standDownMask = standDown;
            publishVerdict(failed, RecoveryVerdict::Cancelled, taps);
            failed.tapsEmitted = reportedTaps();
            return failed;
        }
    }
    haveStep_ = true;
    lastStepSerial_ = realStepSerial;

    if (observation.trainingGeneration != generation_)
        reset(observation.trainingGeneration);

    RecoveryEmission result{};
    result.lastVerdict = lastVerdict_;
    result.branch = attempt_.branch;
    result.standDownMask = standDown;

    const bool available =
        (standDown & kRecoveryStandDownAvailabilityMask) == 0u;
    if (!available) {
        if (attempt_.active) {
            const uint32_t taps = attempt_.tapsEmitted;
            cancelAttempt(RecoveryVerdict::Cancelled);
            publishVerdict(result, RecoveryVerdict::Cancelled, taps);
        }
        previousArmableMask_ = 0;
        haveObservation_ = false;
        result.tapsEmitted = reportedTaps();
        cached_ = result;
        return result;
    }

    const uint32_t eligible = recoveryEligibleMask(observation);
    const uint32_t armable = recoveryArmableMask(observation);
    result.eligibleMask = eligible;
    result.armableMask = armable;
    result.contactTestUnknown = false;

    const bool actionChanged =
        haveObservation_ && observation.action != previousAction_;
    const bool resultEntered =
        actionChanged && isRecoveryResultAction(observation.action);

    const bool attemptMayOwnResult = attempt_.active && attempt_.tapsEmitted > 0;
    if (attemptMayOwnResult) {
        if (resultEntered
            && recoveryBranchAcceptsResult(attempt_.branch,
                                           observation.action)) {
            const bool buffered =
                attempt_.state == RecoveryState::ObservingBuffer;
            counters_.lastResultAction = observation.action;
            if (buffered) {
                ++counters_.bufferedObserved;
                publishVerdict(result, RecoveryVerdict::BufferedObserved,
                               attempt_.tapsEmitted);
            } else {
                ++counters_.succeeded;
                publishVerdict(result, RecoveryVerdict::Succeeded,
                               attempt_.tapsEmitted);
            }
            attempt_ = Attempt{};
            attempt_.state = RecoveryState::Cooldown;
            retryNotBeforeStep_ = 0;
        } else if (!actionChanged
                   && observation.transitionFlag == 1
                   && recoveryBranchAcceptsResult(attempt_.branch,
                                                  observation.nextAction)
                   && observation.koLatch) {
            ++counters_.commitBlocked;
            publishVerdict(result, RecoveryVerdict::CommitBlocked,
                           attempt_.tapsEmitted);
        }
    } else if (resultEntered) {
        ++counters_.automaticObserved;
        counters_.lastResultAction = observation.action;
        publishVerdict(result, RecoveryVerdict::AutomaticObserved,
                       attempt_.tapsEmitted);
        retryNotBeforeStep_ = 0;
    }

    if (attempt_.active && !generationStillValid(observation)) {
        const uint32_t taps = attempt_.tapsEmitted;
        cancelAttempt(RecoveryVerdict::Cancelled);
        publishVerdict(result, RecoveryVerdict::Cancelled, taps);
    }

    if (attempt_.active
        && recoveryBranchConsultsManualLock(attempt_.branch)
        && attempt_.state != RecoveryState::AwaitNativeTransition
        && attempt_.state != RecoveryState::ObservingBuffer
        && observation.recoveryLock != 0) {
        if (attempt_.tapsEmitted == 0) {
            cancelAttempt(RecoveryVerdict::Cancelled);
            publishVerdict(result, RecoveryVerdict::Cancelled, 0u);
        } else {
            attempt_.state = RecoveryState::ObservingBuffer;
            attempt_.patternLiveUntilStep = attempt_.lastTapStep
                + recoveryBranchWindow(attempt_.branch);
        }
    }

    if (attempt_.active) {
        switch (attempt_.state) {
            case RecoveryState::DelayEligibleSteps:
                if (attempt_.delayRemaining > 0) {
                    --attempt_.delayRemaining;
                    result.stepsRemaining = attempt_.delayRemaining;
                    break;
                }
                attempt_.state = RecoveryState::EmitPreNeutral;
                attempt_.emitIndex = 0;
                if (attempt_.branch != RecoveryBranch::ThrowEscape
                    && observation.directionRingHead == 0) {
                    attempt_.emitIndex = 1;
                }
                break;
            default:
                break;
        }
    }

    if (!attempt_.active) {
        const uint32_t rising = armable & ~previousArmableMask_;
        const bool newHitEpisode = haveObservation_
            && observation.hitEpoch != 0u
            && observation.hitEpoch != previousHitEpoch_;
        const bool retryDue = retryNotBeforeStep_ != 0u
            && realStepSerial >= retryNotBeforeStep_;
        const uint32_t candidates =
            (newHitEpisode || retryDue) ? armable : rising;
        RecoveryBranch chosen = RecoveryBranch::Air22;
        if (candidates != 0
            && chooseArmableBranch(settings, candidates, chosen)) {
            retryNotBeforeStep_ = 0;
            attempt_ = Attempt{};
            attempt_.active = true;
            attempt_.branch = chosen;
            attempt_.state = RecoveryState::DelayEligibleSteps;
            attempt_.slot = observation.slot;
            attempt_.commandChild = observation.commandChild;
            attempt_.delayRemaining =
                delayForMode(modeForBranch(settings, chosen), settings);
            ++counters_.attempts;
            if (attempt_.delayRemaining == 0) {
                attempt_.state = RecoveryState::EmitPreNeutral;
                attempt_.emitIndex = 0;
                if (chosen != RecoveryBranch::ThrowEscape
                    && observation.directionRingHead == 0) {
                    attempt_.emitIndex = 1;
                }
            } else {
                --attempt_.delayRemaining;
            }
        }
    }

    if (attempt_.active) {
        result.branch = attempt_.branch;
        const uint32_t stepCount = recoveryEmissionStepCount(attempt_.branch);
        if (attempt_.state == RecoveryState::DelayEligibleSteps) {
            result.state = RecoveryState::DelayEligibleSteps;
            result.stepsRemaining = attempt_.delayRemaining;
        } else if (attempt_.emitIndex < stepCount
                   && attempt_.state != RecoveryState::AwaitNativeTransition
                   && attempt_.state != RecoveryState::ObservingBuffer) {
            const RecoveryStepMask mask = recoveryEmissionStep(
                attempt_.branch, attempt_.emitIndex, observation.facing);
            result.active = true;
            result.directions = mask.directions;
            result.buttons = mask.buttons;
            result.ownsButtons =
                attempt_.branch == RecoveryBranch::ThrowEscape;
            result.state =
                stateForEmitIndex(attempt_.branch, attempt_.emitIndex);
            attempt_.state = result.state;
            if (emissionStepIsTap(attempt_.branch, attempt_.emitIndex)) {
                ++attempt_.tapsEmitted;
                attempt_.lastTapStep = realStepSerial;
            }
            ++attempt_.emitIndex;
            if (attempt_.emitIndex >= stepCount) {
                attempt_.state = RecoveryState::AwaitNativeTransition;
                attempt_.awaitRemaining = kRecoveryAwaitCapSteps;
                attempt_.patternLiveUntilStep = attempt_.lastTapStep
                    + recoveryBranchWindow(attempt_.branch);
            }
            result.stepsRemaining = stepCount - attempt_.emitIndex;
        } else if (attempt_.state == RecoveryState::AwaitNativeTransition) {
            const RecoveryStepMask mask =
                recoveryAwaitMask(attempt_.branch, observation.facing);
            result.active = true;
            result.directions = mask.directions;
            result.buttons = mask.buttons;
            result.ownsButtons =
                attempt_.branch == RecoveryBranch::ThrowEscape;
            result.state = RecoveryState::AwaitNativeTransition;
            if (attempt_.awaitRemaining > 0) --attempt_.awaitRemaining;
            result.stepsRemaining = attempt_.awaitRemaining;
            if (attempt_.awaitRemaining == 0) {
                attempt_.state = RecoveryState::ObservingBuffer;
            }
        } else if (attempt_.state == RecoveryState::ObservingBuffer) {
            result.state = RecoveryState::ObservingBuffer;
            if (realStepSerial >= attempt_.patternLiveUntilStep) {
                const uint32_t taps = attempt_.tapsEmitted;
                cancelAttempt(RecoveryVerdict::MissedExpired);
                publishVerdict(result, RecoveryVerdict::MissedExpired, taps);
                result.state = RecoveryState::Idle;
            } else {
                result.stepsRemaining = static_cast<uint32_t>(
                    attempt_.patternLiveUntilStep - realStepSerial);
            }
        }
    } else if (attempt_.state == RecoveryState::Cooldown) {
        result.state = RecoveryState::Cooldown;
        attempt_.state = RecoveryState::Idle;
    } else {
        result.state = armable != 0 ? RecoveryState::WaitEligible
                                    : RecoveryState::Idle;
    }

    result.tapsEmitted = reportedTaps();

    if (armable == 0u) retryNotBeforeStep_ = 0;
    previousArmableMask_ = armable;
    previousAction_ = observation.action;
    previousHitEpoch_ = observation.hitEpoch;
    haveObservation_ = true;
    cached_ = result;
    return result;
}

}
