#pragma once

#include <cstdint>

namespace qgame {

// Native KO/round-result sequencer stages (dword_88098C, sequencer sub_40EF30); durations are native ticks.
inline const char* koStageName(const uint32_t stage) {
    switch (stage) {
        case 0:   return "combat";
        case 1:   return "decide-outcome";  // 1 tick: picks 2 / 10 / 20
        case 2:   return "ko-zoom";         // ~25 ticks, zoom ease
        case 3:   return "knockdown-hold";  // 90 ticks, the drama pause
        case 4:   return "un-zoom";         // ~20 ticks
        case 5:   return "pose-wait";       // variable, waits +341 flags
        case 6:   return "win-pose-wait";   // variable, win-type resolve
        case 7:   return "result-pose";     // variable, ANY BUTTON skips
        case 8:   return "tail-countdown";  // <=60 ticks, then round advance
        case 10:  return "double-ko";
        case 11:  return "draw";
        case 20:  return "draw-both-dead";
        case 101: return "time-over";
        default:  break;
    }
    return "unknown";
}

inline const char* sceneName(const uint32_t scene) {
    switch (scene) {
        case 1: return "menu";
        case 2: return "match";
        case 3: return "charselect";
        default: break;
    }
    return "other";
}

inline const char* matchSubName(const int32_t sub) {
    switch (sub) {
        case -1: return "setup";
        case 0:  return "gameplay";
        case 1:  return "round-end";
        case 2:  return "match-end";
        case 5:  return "continue-doorway";
        default: break;
    }
    return "transition";
}

struct LifecycleStateSample {
    uint32_t nativeTick    = 0;   // 0x4AB778
    uint32_t scene         = 0;
    uint32_t sceneNext     = 0;
    int32_t  matchSub      = 0;
    uint32_t scene3Sub     = 0;
    uint32_t koStage       = 0;   // 0x88098C
    uint32_t roundCounter  = 0;   // 0x871950
    uint32_t matchOver     = 0;   // 0x8719A4
    uint32_t roundWinsP1   = 0;
    uint32_t roundWinsP2   = 0;
    uint32_t formationRow  = 0;   // 0x62369C
    uint32_t cpuMask       = 0;   // 0x6236CC
    uint32_t finishTimerA  = 0;   // 0x4AA3D0
    uint32_t finishTimerB  = 0;   // 0x4AA408

    bool differsIgnoringTimers(const LifecycleStateSample& o) const {
        return scene != o.scene || sceneNext != o.sceneNext
            || matchSub != o.matchSub || scene3Sub != o.scene3Sub
            || koStage != o.koStage || roundCounter != o.roundCounter
            || matchOver != o.matchOver || roundWinsP1 != o.roundWinsP1
            || roundWinsP2 != o.roundWinsP2 || formationRow != o.formationRow
            || cpuMask != o.cpuMask;
    }
};

class KoWindowAccounting {
public:
    static constexpr size_t kSlots = 16;

    void open(const uint32_t frame, const uint64_t nowUs) {
        active_ = true;
        startFrame_ = frame;
        startUs_ = nowUs;
        frames_ = 0;
        for (size_t i = 0; i < kSlots; ++i) {
            stageFrames_[i] = 0;
            stageUs_[i] = 0;
        }
    }

    void accrue(const uint32_t stage, const uint64_t deltaUs) {
        if (!active_) return;
        ++frames_;
        const size_t slot = slotFor(stage);
        ++stageFrames_[slot];
        stageUs_[slot] += deltaUs;
    }

    void close() { active_ = false; }

    bool active() const { return active_; }
    uint32_t frames() const { return frames_; }
    uint32_t startFrame() const { return startFrame_; }
    uint64_t elapsedUs(const uint64_t nowUs) const {
        return nowUs > startUs_ ? nowUs - startUs_ : 0u;
    }
    uint32_t stageFrames(const size_t slot) const {
        return slot < kSlots ? stageFrames_[slot] : 0u;
    }
    uint64_t stageUs(const size_t slot) const {
        return slot < kSlots ? stageUs_[slot] : 0u;
    }
    static uint32_t stageForSlot(const size_t slot) {
        return slot <= 8u ? static_cast<uint32_t>(slot) : 0xFFFFFFFFu;
    }
    static size_t slotFor(const uint32_t stage) {
        return stage <= 8u ? static_cast<size_t>(stage) : 9u;
    }

    static uint64_t nativeCostUs(const uint32_t frames) {
        return (static_cast<uint64_t>(frames) * 1000000ull * 100ull) / 6060ull;
    }
    static uint32_t stretchPermille(const uint32_t frames,
                                    const uint64_t elapsedUs) {
        const uint64_t want = nativeCostUs(frames);
        if (want == 0u || elapsedUs == 0u) return 1000u;
        const uint64_t permille = (elapsedUs * 1000ull) / want;
        return permille > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                        : static_cast<uint32_t>(permille);
    }

private:
    bool     active_ = false;
    uint32_t startFrame_ = 0;
    uint64_t startUs_ = 0;
    uint32_t frames_ = 0;
    uint32_t stageFrames_[kSlots] = {};
    uint64_t stageUs_[kSlots] = {};
};

struct CadenceBudget {
    uint64_t windowUs        = 0;
    uint32_t executedFrames  = 0;
    uint32_t holdExactInput  = 0;
    uint32_t holdPrediction  = 0;
    uint32_t holdFrameAdv    = 0;
    uint32_t holdCorrection  = 0;
    uint32_t holdOther       = 0;
    uint64_t usTick          = 0;
    uint64_t usExactInput    = 0;
    uint64_t usPrediction    = 0;
    uint64_t usFrameAdv      = 0;
    uint64_t usCorrection    = 0;
    uint64_t usOther         = 0;

    void reset() { *this = CadenceBudget{}; }

    uint64_t accountedUs() const {
        return usTick + usExactInput + usPrediction + usFrameAdv
             + usCorrection + usOther;
    }
    uint64_t unaccountedUs() const {
        const uint64_t used = accountedUs();
        return windowUs > used ? windowUs - used : 0u;
    }
    uint32_t opportunities() const {
        return static_cast<uint32_t>((windowUs * 6060ull) / 100000000ull);
    }
};

}
