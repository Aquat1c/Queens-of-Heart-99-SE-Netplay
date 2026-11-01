#include "SpectatorPublisher.h"

namespace qgame::spectator_publish {

ConnectedPublisherQueueV4::ConnectedPublisherQueueV4(size_t capacity) {
    if (capacity < kConnectedPublisherMinCapacityV4
        || capacity > kConnectedPublisherMaxCapacityV4) {
        return;
    }
    slots_.resize(capacity);
    valid_ = true;
}

ConnectedPublishResultV4 ConnectedPublisherQueueV4::publish(
    const qnetplay::spectator::ConnectedPublicationV4& publication) {
    using Kind =
        qnetplay::spectator::ConnectedPublicationKindV4;
    if (!valid_) return ConnectedPublishResultV4::InvalidArgument;
    if (sealQueued_.load(std::memory_order_relaxed)) {
        rejected_.fetch_add(1u, std::memory_order_relaxed);
        return ConnectedPublishResultV4::Disabled;
    }

    const Kind kind = publication.kind;
    const bool open = gameOpen_.load(std::memory_order_relaxed);
    switch (kind) {
        case Kind::BeginGameBundle:
            if (open) {
                rejected_.fetch_add(1u, std::memory_order_relaxed);
                return ConnectedPublishResultV4::OrderViolation;
            }
            break;
        case Kind::ConfirmedRecord:
        case Kind::SetEvent:
        case Kind::Palette:
            break;
        case Kind::EndGameBundle:
            if (!open) {
                rejected_.fetch_add(1u, std::memory_order_relaxed);
                return ConnectedPublishResultV4::OrderViolation;
            }
            break;
        case Kind::SetSeal:
            if (open) {
                rejected_.fetch_add(1u, std::memory_order_relaxed);
                return ConnectedPublishResultV4::OrderViolation;
            }
            break;
        default:
            rejected_.fetch_add(1u, std::memory_order_relaxed);
            return ConnectedPublishResultV4::InvalidArgument;
    }

    const bool ordinary =
        kind == Kind::BeginGameBundle
        || kind == Kind::ConfirmedRecord
        || kind == Kind::SetEvent
        || kind == Kind::Palette;
    if (ordinary
        && overflowLatched_.load(std::memory_order_relaxed)) {
        rejected_.fetch_add(1u, std::memory_order_relaxed);
        return ConnectedPublishResultV4::Disabled;
    }

    const uint64_t write = write_.load(std::memory_order_relaxed);
    const uint64_t read = read_.load(std::memory_order_acquire);
    if (write < read || write - read > slots_.size()) {
        rejected_.fetch_add(1u, std::memory_order_relaxed);
        overflowLatched_.store(true, std::memory_order_release);
        return ConnectedPublishResultV4::Overflow;
    }
    const size_t used = static_cast<size_t>(write - read);
    const size_t free = slots_.size() - used;
    const bool hasCapacity =
        ordinary ? free > 2u
        : kind == Kind::EndGameBundle ? free > 1u
        : free > 0u;
    if (!hasCapacity) {
        rejected_.fetch_add(1u, std::memory_order_relaxed);
        if (ordinary)
            overflowLatched_.store(true, std::memory_order_release);
        return ConnectedPublishResultV4::Overflow;
    }

    slots_[static_cast<size_t>(write % slots_.size())] = publication;
    write_.store(write + 1u, std::memory_order_release);
    accepted_.fetch_add(1u, std::memory_order_relaxed);
    const uint32_t water = static_cast<uint32_t>(used + 1u);
    uint32_t previous = highWater_.load(std::memory_order_relaxed);
    while (previous < water
           && !highWater_.compare_exchange_weak(
               previous, water, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    if (kind == Kind::BeginGameBundle)
        gameOpen_.store(true, std::memory_order_relaxed);
    else if (kind == Kind::EndGameBundle)
        gameOpen_.store(false, std::memory_order_relaxed);
    else if (kind == Kind::SetSeal)
        sealQueued_.store(true, std::memory_order_release);
    return ConnectedPublishResultV4::Accepted;
}

bool ConnectedPublisherQueueV4::pop(
    qnetplay::spectator::ConnectedPublicationV4& out) {
    if (!valid_) return false;
    const uint64_t read = read_.load(std::memory_order_relaxed);
    const uint64_t write = write_.load(std::memory_order_acquire);
    if (read == write) return false;
    out = slots_[static_cast<size_t>(read % slots_.size())];
    read_.store(read + 1u, std::memory_order_release);
    consumed_.fetch_add(1u, std::memory_order_relaxed);
    return true;
}

void ConnectedPublisherQueueV4::reset() {
    read_.store(0u, std::memory_order_relaxed);
    write_.store(0u, std::memory_order_relaxed);
    accepted_.store(0u, std::memory_order_relaxed);
    consumed_.store(0u, std::memory_order_relaxed);
    rejected_.store(0u, std::memory_order_relaxed);
    highWater_.store(0u, std::memory_order_relaxed);
    overflowLatched_.store(false, std::memory_order_relaxed);
    gameOpen_.store(false, std::memory_order_relaxed);
    sealQueued_.store(false, std::memory_order_relaxed);
}

ConnectedPublisherStatsV4 ConnectedPublisherQueueV4::stats() const {
    ConnectedPublisherStatsV4 out{};
    out.accepted = accepted_.load(std::memory_order_relaxed);
    out.consumed = consumed_.load(std::memory_order_relaxed);
    out.rejected = rejected_.load(std::memory_order_relaxed);
    out.highWater = highWater_.load(std::memory_order_relaxed);
    out.overflowLatched =
        overflowLatched_.load(std::memory_order_relaxed);
    out.gameOpen = gameOpen_.load(std::memory_order_relaxed);
    out.sealQueued = sealQueued_.load(std::memory_order_relaxed);
    return out;
}

}
