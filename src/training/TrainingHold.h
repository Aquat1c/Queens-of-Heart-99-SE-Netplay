// The menu reason is latched on the game thread only; the render thread must never write it.
#pragma once

#include <cstdint>

#include "training/TrainingTypes.h"

namespace qtraining {

struct TrainingHoldAdmission {
    bool admit = true;
    bool consumedCredit = false;
    bool presentHeldFrame = false;
    uint32_t reasons = TrainingHoldNone;
};

class TrainingHold {
public:
    void reset() {
        reasons_ = TrainingHoldNone;
        credits_ = 0;
    }

    uint32_t reasons() const { return reasons_; }
    bool held() const { return reasons_ != TrainingHoldNone; }
    bool heldFor(uint32_t reason) const { return (reasons_ & reason) != 0; }
    uint32_t frameStepCredits() const { return credits_; }

    void toggleUserHold() {
        reasons_ ^= TrainingHoldUser;
        credits_ = 0;
    }

    void requestFrameStep() {
        if ((reasons_ & TrainingHoldMenu) == 0u)
            reasons_ |= TrainingHoldUser;
        credits_ = 1;
    }

    void setMenuCaptured(bool captured) {
        if (captured) reasons_ |= TrainingHoldMenu;
        else reasons_ &= ~static_cast<uint32_t>(TrainingHoldMenu);
    }

    TrainingHoldAdmission admit() {
        TrainingHoldAdmission result{};
        result.reasons = reasons_;
        if (reasons_ == TrainingHoldNone) return result;
        if (credits_ == 0) {
            result.admit = false;
            result.presentHeldFrame = true;
            return result;
        }
        --credits_;
        result.consumedCredit = true;
        return result;
    }

private:
    uint32_t reasons_ = TrainingHoldNone;
    uint32_t credits_ = 0;
};

constexpr const char* trainingHoldStatusText(uint32_t reasons) {
    const bool user = (reasons & TrainingHoldUser) != 0;
    const bool menu = (reasons & TrainingHoldMenu) != 0;
    if (user && menu)
        return "Paused (menu open) -- still paused when the menu closes";
    if (menu) return "Held: the training menu is open";
    if (user) return "Paused: press 9 to step one frame, 8 to resume";
    return "";
}

}
