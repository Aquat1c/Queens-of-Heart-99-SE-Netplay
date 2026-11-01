#pragma once

#include <cstdint>

namespace qtraining {

// Append only, never renumber: these values cross the DLL/overlay ABI.
enum class StatusEvent : uint32_t {
    None = 0,
    PositionReset = 1,
    StateSaved = 2,
    StateLoaded = 3,
    StateCleared = 4,
    StateSlotSelected = 5,
    MacroSlotSelected = 6,
    MacroCleared = 7,
    MacroRecorded = 8,
    MacroStopped = 9,
    RoundReset = 10,
    Count = 11,
};

enum class StatusMode : uint32_t {
    None = 0,
    PreRecord = 1,
    Recording = 2,
    Playing = 3,
    Count = 4,
};

enum class StatusTone : uint32_t {
    Neutral = 0,
    Good = 1,
    Active = 2,
    Warning = 3,
    Count = 4,
};

constexpr bool statusEventValid(uint32_t value) {
    return value < static_cast<uint32_t>(StatusEvent::Count);
}
constexpr bool statusModeValid(uint32_t value) {
    return value < static_cast<uint32_t>(StatusMode::Count);
}

constexpr StatusTone statusEventTone(StatusEvent event) {
    switch (event) {
        case StatusEvent::StateSaved:
        case StatusEvent::MacroRecorded:
            return StatusTone::Good;
        default:
            return StatusTone::Neutral;
    }
}

constexpr StatusTone statusModeTone(StatusMode mode) {
    switch (mode) {
        case StatusMode::PreRecord: return StatusTone::Warning;
        case StatusMode::Recording:
        case StatusMode::Playing:   return StatusTone::Active;
        default:                    return StatusTone::Neutral;
    }
}

constexpr bool statusEventHasSlot(StatusEvent event) {
    switch (event) {
        case StatusEvent::StateSaved:
        case StatusEvent::StateLoaded:
        case StatusEvent::StateCleared:
        case StatusEvent::StateSlotSelected:
        case StatusEvent::MacroSlotSelected:
        case StatusEvent::MacroCleared:
        case StatusEvent::MacroStopped:
            return true;
        default:
            return false;
    }
}

}
