#pragma once

#include <cstdint>

namespace qnet::relay {

constexpr uint64_t kDirectGraceMs = 1500;
constexpr uint64_t kRouteDeadlineMs = 12000;

enum class SelectedRoute : uint8_t {
    None = 0,
    Direct = 1,
    Relay = 2,
};

class RouteSelector {
public:
    explicit RouteSelector(bool forceRelay = false,
                           uint64_t directGraceMs = kDirectGraceMs,
                           uint64_t deadlineMs = kRouteDeadlineMs)
        : forceRelay_(forceRelay),
          directGraceMs_(directGraceMs),
          deadlineMs_(deadlineMs) {}

    bool start(uint64_t nowMs) {
        if (started_ || directGraceMs_ == 0 || deadlineMs_ <= directGraceMs_)
            return false;
        started_ = true;
        startedMs_ = nowMs;
        return true;
    }

    SelectedRoute update(uint64_t nowMs,
                         bool directEstablished,
                         bool relayEstablished) {
        if (!started_ || selected_ != SelectedRoute::None
            || nowMs < startedMs_) {
            return selected_;
        }

        if (!forceRelay_ && directEstablished) {
            selected_ = SelectedRoute::Direct;
        } else if (relayEstablished
                   && (forceRelay_ || elapsed(nowMs) >= directGraceMs_)) {
            selected_ = SelectedRoute::Relay;
        }
        return selected_;
    }

    bool shouldProbeDirect() const {
        return started_ && !forceRelay_
            && selected_ == SelectedRoute::None;
    }

    bool shouldProbeRelay(uint64_t nowMs) const {
        return started_ && selected_ == SelectedRoute::None
            && nowMs >= startedMs_
            && (forceRelay_ || elapsed(nowMs) >= directGraceMs_);
    }

    bool expired(uint64_t nowMs) const {
        return started_ && selected_ == SelectedRoute::None
            && nowMs >= startedMs_
            && elapsed(nowMs) >= deadlineMs_;
    }

    SelectedRoute selected() const { return selected_; }
    bool started() const { return started_; }
    bool forceRelay() const { return forceRelay_; }
    uint64_t elapsed(uint64_t nowMs) const {
        return nowMs >= startedMs_ ? nowMs - startedMs_ : 0;
    }

private:
    bool forceRelay_ = false;
    uint64_t directGraceMs_ = kDirectGraceMs;
    uint64_t deadlineMs_ = kRouteDeadlineMs;
    bool started_ = false;
    uint64_t startedMs_ = 0;
    SelectedRoute selected_ = SelectedRoute::None;
};

}
