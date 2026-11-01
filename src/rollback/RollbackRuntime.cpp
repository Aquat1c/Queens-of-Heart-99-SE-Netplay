#include "RollbackRuntime.hpp"

namespace qoh99 { namespace netplay {

RollbackRuntime::RollbackRuntime(MemoryView& m, TickFn tick, InputFeedFn feed,
                                 int ringDepth)
    : m_(m), tick_(std::move(tick)), feed_(std::move(feed)),
      profile_(), hash_(profile_), scenes_(m), ring_(ringDepth), guard_(scenes_) {}

bool RollbackRuntime::saveFrame(std::uint32_t frame) {
    PhasePolicy pol = SceneSystem::policyFor(scenes_.phase());
    if (!pol.saveAllowed)
        return false;
    Savestate& slot = ring_.slotFor(frame);
    if (!profile_.save(m_, slot))
        return false;
    ring_.tag(frame);
    return true;
}

int RollbackRuntime::rollbackAndReplay(std::uint32_t frame, int k) {
    if (k <= 0 || !ring_.has(frame))
        return 0;
    PhasePolicy pol = SceneSystem::policyFor(scenes_.phase());
    if (!pol.rollbackAllowed)
        return 0;
    if (!profile_.restore(m_, ring_.slotFor(frame)))
        return 0;

    RenderSuppressor::Scope rs(render_);
    AudioSuppressor::Scope as(audio_);
    guard_.arm();
    int replayed = 0;
    for (int i = 0; i < k; ++i) {
        if (feed_)
            feed_(frame + static_cast<std::uint32_t>(i));
        tick_();
        ++replayed;
        if (!guard_.stillSafe())
            break;
    }
    return replayed;
}

SyncHash::FirstDiff RollbackRuntime::compareLive(std::uint32_t frame) {
    SyncHash::FirstDiff fd;
    if (!ring_.has(frame)) {
        fd.differs = true;
        fd.region = "no-saved-state";
        return fd;
    }
    Savestate live;
    if (!profile_.save(m_, live)) {
        fd.differs = true;
        fd.region = "live-capture-failed";
        return fd;
    }
    return hash_.firstDiff(ring_.slotFor(frame), live, HashProfile::GameplaySync);
}

}}
