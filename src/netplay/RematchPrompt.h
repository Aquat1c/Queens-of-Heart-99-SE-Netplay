#pragma once

#include <cstdint>

#include "WireProtocol.h"

namespace qnetplay {

constexpr uint64_t kRematchDecisionTimeoutMs = 60000;

constexpr uint64_t kRematchChoiceResendMs = 96;
constexpr uint64_t kRematchChoiceReadyResendMs = 48;

enum class RematchPromptPhase : uint8_t {
    Idle = 0,
    Deciding = 1,
    CommitPending = 2,
    Advancing = 3,
};

class RematchPrompt {
public:
    void reset() { *this = RematchPrompt{}; }

    bool begin(uint32_t epoch, uint32_t promptFrame, uint8_t side,
               uint64_t nowMs) {
        if (phase_ != RematchPromptPhase::Idle || side > 1) return false;
        phase_ = RematchPromptPhase::Deciding;
        epoch_ = epoch;
        promptFrame_ = promptFrame;
        side_ = side;
        startMs_ = nowMs;
        localChoice_ = wire::kRematchChoiceUndecided;
        localReady_ = false;
        remoteSeen_ = false;
        remoteChoice_ = wire::kRematchChoiceUndecided;
        remoteReady_ = false;
        sendDirty_ = true;
        lastSendMs_ = 0;
        return true;
    }

    bool active() const { return phase_ != RematchPromptPhase::Idle; }
    bool deciding() const { return phase_ == RematchPromptPhase::Deciding; }
    bool advancing() const { return phase_ == RematchPromptPhase::Advancing; }
    RematchPromptPhase phase() const { return phase_; }
    uint32_t epoch() const { return epoch_; }
    uint32_t promptFrame() const { return promptFrame_; }
    uint64_t startMs() const { return startMs_; }
    uint8_t localChoice() const { return localChoice_; }
    bool localReady() const { return localReady_; }
    bool remoteSeen() const { return remoteSeen_; }
    uint8_t remoteChoice() const { return remoteChoice_; }
    bool remoteReady() const { return remoteReady_; }

    bool latchLocalConfirm(bool cursorYes) {
        if (phase_ != RematchPromptPhase::Deciding) return false;
        if (localReady_) return false;
        localChoice_ = cursorYes ? wire::kRematchChoiceRematch
                                 : wire::kRematchChoiceLeave;
        localReady_ = true;
        sendDirty_ = true;
        return true;
    }

    bool latchLocalLeave() {
        if (phase_ != RematchPromptPhase::Deciding) return false;
        localChoice_ = wire::kRematchChoiceLeave;
        localReady_ = true;
        sendDirty_ = true;
        return true;
    }

    bool acceptRemote(const wire::RematchChoicePayload& payload) {
        if (phase_ == RematchPromptPhase::Idle) return false;
        if (payload.epoch != epoch_ || payload.promptFrame != promptFrame_)
            return false;
        if (payload.side == side_ || payload.side > 1) return false;
        remoteSeen_ = true;
        remoteChoice_ = payload.choice;
        remoteReady_ = payload.ready != 0;
        return true;
    }

    bool bothReady() const {
        return active() && localReady_ && remoteSeen_ && remoteReady_;
    }
    bool declineAgreed() const {
        return bothReady()
            && (localChoice_ == wire::kRematchChoiceLeave
                || remoteChoice_ == wire::kRematchChoiceLeave);
    }

    bool agreementReached() const {
        return active()
            && localReady_
            && localChoice_ == wire::kRematchChoiceRematch
            && remoteSeen_ && remoteReady_
            && remoteChoice_ == wire::kRematchChoiceRematch;
    }

    void markCommitPending() {
        if (phase_ == RematchPromptPhase::Deciding)
            phase_ = RematchPromptPhase::CommitPending;
    }

    void markCommitted() {
        if (phase_ == RematchPromptPhase::Deciding
            || phase_ == RematchPromptPhase::CommitPending) {
            phase_ = RematchPromptPhase::Advancing;
        }
    }

    bool decisionTimedOut(uint64_t nowMs) const {
        return phase_ == RematchPromptPhase::Deciding
            && !agreementReached()
            && nowMs - startMs_ >= kRematchDecisionTimeoutMs;
    }

    bool shouldSend(uint64_t nowMs) const {
        if (phase_ != RematchPromptPhase::Deciding
            && phase_ != RematchPromptPhase::CommitPending) {
            return false;
        }
        if (sendDirty_) return true;
        const uint64_t interval = localReady_
            ? kRematchChoiceReadyResendMs
            : kRematchChoiceResendMs;
        return nowMs - lastSendMs_ >= interval;
    }

    wire::RematchChoicePayload localAdvertisement() const {
        wire::RematchChoicePayload payload{};
        payload.epoch = epoch_;
        payload.promptFrame = promptFrame_;
        payload.side = side_;
        payload.choice = localChoice_;
        payload.ready = localReady_ ? 1u : 0u;
        return payload;
    }

    void markSent(uint64_t nowMs) {
        sendDirty_ = false;
        lastSendMs_ = nowMs;
    }

private:
    RematchPromptPhase phase_ = RematchPromptPhase::Idle;
    uint32_t epoch_ = 0;
    uint32_t promptFrame_ = 0;
    uint8_t side_ = 0;
    uint64_t startMs_ = 0;
    uint8_t localChoice_ = wire::kRematchChoiceUndecided;
    bool localReady_ = false;
    bool remoteSeen_ = false;
    uint8_t remoteChoice_ = wire::kRematchChoiceUndecided;
    bool remoteReady_ = false;
    bool sendDirty_ = false;
    uint64_t lastSendMs_ = 0;
};

}
