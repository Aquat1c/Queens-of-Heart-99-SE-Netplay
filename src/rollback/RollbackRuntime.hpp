/* Never re-simulate the frame that flips matchSub 0->5 (winscreen crash). */
#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "SavestateProfile.hpp"
#include "SyncHash.hpp"
#include "qoh99/InputSystem.hpp"
#include "qoh99/MatchSystem.hpp"
#include "qoh99/SceneSystem.hpp"

namespace qoh99 { namespace netplay {

// Install the sub_484BB0 input override hook only after gameplay starts.
class InputOverride {
public:
    void setActive(bool inGameplay) { active_ = inGameplay; }
    bool active() const { return active_; }
    void setOverride(int player, LogicalInput in) {
        if (player >= 0 && player < 2) pending_[player] = in;
    }
    std::uint32_t filter(int player, std::uint32_t original) const {
        if (!active_ || player < 0 || player > 1) return original;
        return pending_[player].mask;
    }
private:
    bool active_ = false;          // MUST stay false outside MatchGameplay
    LogicalInput pending_[2]{};
};

class RollbackStateRing {
public:
    explicit RollbackStateRing(int depth = 8) : slots_(static_cast<std::size_t>(depth)) {}
    int depth() const { return static_cast<int>(slots_.size()); }
    Savestate& slotFor(std::uint32_t frame) { return slots_[frame % slots_.size()].state; }
    void tag(std::uint32_t frame) { slots_[frame % slots_.size()].frame = frame; }
    bool has(std::uint32_t frame) const {
        const Slot& s = slots_[frame % slots_.size()];
        return s.state.valid && s.frame == frame;
    }
private:
    struct Slot { std::uint32_t frame = ~0u; Savestate state; };
    std::vector<Slot> slots_;
};

class ResimulationGuard {
public:
    explicit ResimulationGuard(SceneSystem& scenes) : scenes_(scenes) {}
    void arm() { armed_ = true; before_ = scenes_.matchSub(); }
    bool stillSafe() {
        if (!armed_) return false;
        MatchSub now = scenes_.matchSub();
        if (MatchSystem::exitsGameplay(before_, now)) { armed_ = false; return false; }
        before_ = now;
        return true;
    }
private:
    SceneSystem& scenes_;
    MatchSub before_ = MatchSub::Gameplay;
    bool armed_ = false;
};

class RenderSuppressor {
public:
    bool active() const { return depth_ > 0; }
    class Scope {
    public:
        explicit Scope(RenderSuppressor& s) : s_(s) { ++s_.depth_; }
        ~Scope() { --s_.depth_; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        RenderSuppressor& s_;
    };
private:
    friend class Scope;
    int depth_ = 0;
};

class AudioSuppressor {
public:
    bool active() const { return depth_ > 0; }
    class Scope {
    public:
        explicit Scope(AudioSuppressor& s) : s_(s) { ++s_.depth_; }
        ~Scope() { --s_.depth_; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        AudioSuppressor& s_;
    };
private:
    friend class Scope;
    int depth_ = 0;
};

class RollbackRuntime {
public:
    using TickFn = std::function<void()>;
    using InputFeedFn = std::function<void(std::uint32_t frame)>;

    RollbackRuntime(MemoryView& m, TickFn tick, InputFeedFn feed, int ringDepth = 8);

    bool saveFrame(std::uint32_t frame);
    int rollbackAndReplay(std::uint32_t frame, int k);
    SyncHash::FirstDiff compareLive(std::uint32_t frame);

    RenderSuppressor& render() { return render_; }
    AudioSuppressor& audio() { return audio_; }
    InputOverride& inputOverride() { return override_; }
    const SavestateProfile& profile() const { return profile_; }
    RollbackStateRing& ring() { return ring_; }

private:
    MemoryView& m_;
    TickFn tick_;
    InputFeedFn feed_;
    SavestateProfile profile_;
    SyncHash hash_;
    SceneSystem scenes_;
    RollbackStateRing ring_;
    ResimulationGuard guard_;
    RenderSuppressor render_;
    AudioSuppressor audio_;
    InputOverride override_;
};

}}
