#pragma once

#include <cstdint>

#include "game/NetplaySchedulePolicy.h"

namespace qgame {

enum class NetplayRunState : uint8_t {
    Running,
    CorrectionCatchUp,
    PredictionPressure,
    HoldNegotiating,
    EmergencyNetworkHold,
    RecoveryCatchUp,
    ExactLifecycleBarrier,
    ExternalSuspension,
    Terminated,
};

constexpr const char* runStateName(const NetplayRunState s) {
    switch (s) {
        case NetplayRunState::Running:               return "running";
        case NetplayRunState::CorrectionCatchUp:     return "correction-catchup";
        case NetplayRunState::PredictionPressure:    return "prediction-pressure";
        case NetplayRunState::HoldNegotiating:       return "hold-negotiating";
        case NetplayRunState::EmergencyNetworkHold:  return "emergency-hold";
        case NetplayRunState::RecoveryCatchUp:       return "recovery-catchup";
        case NetplayRunState::ExactLifecycleBarrier: return "lifecycle-barrier";
        case NetplayRunState::ExternalSuspension:    return "external-suspension";
        case NetplayRunState::Terminated:            return "terminated";
    }
    return "?";
}

constexpr bool runStateStopsCanonicalTime(const NetplayRunState s) {
    return s == NetplayRunState::EmergencyNetworkHold
        || s == NetplayRunState::HoldNegotiating
        || s == NetplayRunState::ExactLifecycleBarrier
        || s == NetplayRunState::ExternalSuspension
        || s == NetplayRunState::Terminated;
}

constexpr bool runStateIsNetworkEmergency(const NetplayRunState s) {
    return s == NetplayRunState::EmergencyNetworkHold
        || s == NetplayRunState::HoldNegotiating;
}

struct RunStateInputs {
    uint32_t missingRemoteDepth = 0;
    uint32_t localMaxRollback = 0;
    uint32_t warningMargin = 2;
    uint32_t recoverableDebtFrames = 0;

    WorkKind kind = WorkKind::Advance;
    HoldCause hold = HoldCause::None;

    bool sessionTerminated = false;
    bool externalSuspension = false;
    bool nativeFreezeActive = false;
};

constexpr NetplayRunState classifyRunState(const RunStateInputs& in) {
    if (in.sessionTerminated)   return NetplayRunState::Terminated;
    if (in.externalSuspension)  return NetplayRunState::ExternalSuspension;

    if (in.kind == WorkKind::Hold) {
        switch (in.hold) {
            case HoldCause::LifecycleBoundary:
                return NetplayRunState::ExactLifecycleBarrier;
            case HoldCause::PredictionLimit:
                return NetplayRunState::EmergencyNetworkHold;
            case HoldCause::LocalInputMissing:
                return NetplayRunState::PredictionPressure;
            case HoldCause::NotCadence:
            case HoldCause::None:
                break;
        }
    }
    if (in.kind == WorkKind::Correct) return NetplayRunState::CorrectionCatchUp;

    if (in.localMaxRollback != 0
        && in.missingRemoteDepth + in.warningMargin >= in.localMaxRollback) {
        return NetplayRunState::PredictionPressure;
    }
    if (in.recoverableDebtFrames != 0) return NetplayRunState::RecoveryCatchUp;
    return NetplayRunState::Running;
}

class HoldEpisodeLedger {
public:
    struct Episode {
        uint32_t generation = 0;
        uint64_t startUs = 0;
        uint64_t endUs = 0;
        uint32_t firstMissingFrame = 0;
        uint32_t localMaxRollback = 0;
        uint32_t opportunitiesHeld = 0;
        uint32_t recoveryDepth = 0;

        uint64_t durationUs() const {
            return endUs > startUs ? endUs - startUs : 0u;
        }
    };

    void observe(const NetplayRunState state, const uint64_t nowUs,
                 const uint32_t firstMissing, const uint32_t maxRollback) {
        const bool emergency = runStateIsNetworkEmergency(state);
        if (emergency) {
            if (!_active) {
                _active = true;
                _current = Episode{};
                _current.generation = ++_generation;
                _current.startUs = nowUs;
                _current.firstMissingFrame = firstMissing;
                _current.localMaxRollback = maxRollback;
            }
            _current.endUs = nowUs;
            ++_current.opportunitiesHeld;
            ++_heldOpportunities;
        } else if (_active) {
            _active = false;
            _current.endUs = nowUs;
            ++_episodes;
            _totalHeldUs += _current.durationUs();
            if (_current.durationUs() > _worst.durationUs()) _worst = _current;
            _awaitingRecovery = true;
        }
    }

    void observeRecovery(const uint32_t rollbackDepth) {
        if (!_awaitingRecovery) return;
        _awaitingRecovery = false;
        _current.recoveryDepth = rollbackDepth;
        if (_worst.generation == _current.generation)
            _worst.recoveryDepth = rollbackDepth;
        if (rollbackDepth > _deepestRecovery) _deepestRecovery = rollbackDepth;
    }

    void reset() { *this = HoldEpisodeLedger{}; }

    bool active() const { return _active; }
    uint32_t episodes() const { return _episodes + (_active ? 1u : 0u); }
    uint32_t generation() const { return _generation; }
    uint64_t totalHeldUs() const {
        return _totalHeldUs + (_active ? _current.durationUs() : 0u);
    }
    uint64_t heldOpportunities() const { return _heldOpportunities; }
    uint32_t deepestRecovery() const { return _deepestRecovery; }
    const Episode& current() const { return _current; }
    const Episode& worst() const { return _worst; }

private:
    Episode _current{};
    Episode _worst{};
    uint32_t _generation = 0;
    uint32_t _episodes = 0;
    uint64_t _totalHeldUs = 0;
    uint64_t _heldOpportunities = 0;
    uint32_t _deepestRecovery = 0;
    bool _active = false;
    bool _awaitingRecovery = false;
};

}
