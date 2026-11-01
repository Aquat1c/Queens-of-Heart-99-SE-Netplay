#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace qreplay {

// In-process snapshots only; raw native pointers never enter a replay file.
template<class State>
class ReplaySeekCache {
public:
    struct Entry {
        uint32_t nextRecord;
        size_t bytes;
        std::unique_ptr<State> state;
    };
    explicit ReplaySeekCache(size_t budget = 256u * 1024u * 1024u)
        : budget_(budget) {}
    bool wants(uint32_t cursor) const {
        return cursor != 0u && cursor % stride_ == 0u
            && (entries_.empty() || entries_.back().nextRecord < cursor);
    }
    bool insert(uint32_t cursor, size_t bytes, std::unique_ptr<State> state) {
        if (!state || !wants(cursor) || bytes > budget_) return false;
        while (bytes_ > budget_ - bytes) {
            if (stride_ > UINT32_MAX / 2u) return false;
            stride_ *= 2u;
            for (auto it = entries_.begin(); it != entries_.end();) {
                if (it->nextRecord % stride_ != 0u) {
                    bytes_ -= it->bytes;
                    it = entries_.erase(it);
                } else {
                    ++it;
                }
            }
            if (cursor % stride_ != 0u) return false;
        }
        entries_.push_back({cursor, bytes, std::move(state)});
        bytes_ += bytes;
        return true;
    }
    const Entry* find(uint32_t target) const {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
            if (it->nextRecord <= target) return &*it;
        return nullptr;
    }
    void discardAfter(uint32_t cursor) {
        while (!entries_.empty() && entries_.back().nextRecord > cursor) {
            bytes_ -= entries_.back().bytes;
            entries_.pop_back();
        }
    }
    void clear() {
        entries_.clear();
        bytes_ = 0u;
        stride_ = 16u;
    }
    uint32_t stride() const { return stride_; }
    size_t bytes() const { return bytes_; }

private:
    size_t budget_;
    size_t bytes_ = 0u;
    uint32_t stride_ = 16u;
    std::vector<Entry> entries_;
};

}
