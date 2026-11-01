#pragma once

#include <cstddef>
#include <cstdint>

namespace qnet {

template <size_t Capacity>
class RollingWindowStats {
    static_assert(Capacity > 0, "window needs capacity");

public:
    void reset() {
        count_ = 0;
        next_ = 0;
    }

    void add(int64_t sample) {
        ring_[next_] = sample;
        next_ = (next_ + 1) % Capacity;
        if (count_ < Capacity) {
            ++count_;
        }
    }

    int64_t newest() const {
        if (count_ == 0) return 0;
        return ring_[(next_ + Capacity - 1) % Capacity];
    }

    size_t count() const { return count_; }
    bool empty() const { return count_ == 0; }
    size_t capacity() const { return Capacity; }

    int64_t minimum() const {
        if (count_ == 0) return 0;
        int64_t m = ring_[0];
        for (size_t i = 1; i < count_; ++i)
            if (ring_[i] < m) m = ring_[i];
        return m;
    }

    int64_t maximum() const {
        if (count_ == 0) return 0;
        int64_t m = ring_[0];
        for (size_t i = 1; i < count_; ++i)
            if (ring_[i] > m) m = ring_[i];
        return m;
    }

    int64_t mean() const {
        if (count_ == 0) return 0;
        int64_t sum = 0;
        for (size_t i = 0; i < count_; ++i) sum += ring_[i];
        return sum / static_cast<int64_t>(count_);
    }

    int64_t percentile(int pct) const {
        if (count_ == 0) return 0;
        int64_t sorted[Capacity];
        const size_t n = sortedCopy(sorted);
        if (pct <= 0) return sorted[0];
        if (pct >= 100) return sorted[n - 1];
        const size_t rank = (static_cast<size_t>(pct) * n + 99) / 100;
        size_t idx = rank == 0 ? 0 : rank - 1;
        if (idx >= n) idx = n - 1;
        return sorted[idx];
    }

    int64_t median() const { return percentile(50); }

    int64_t stddev() const {
        if (count_ < 2) return 0;
        const int64_t mu = mean();
        int64_t acc = 0;
        for (size_t i = 0; i < count_; ++i) {
            const int64_t d = ring_[i] - mu;
            acc += d * d;
        }
        return isqrt(acc / static_cast<int64_t>(count_));
    }

    int64_t mad() const {
        if (count_ == 0) return 0;
        const int64_t med = median();
        int64_t dev[Capacity];
        for (size_t i = 0; i < count_; ++i) {
            const int64_t d = ring_[i] - med;
            dev[i] = d < 0 ? -d : d;
        }
        insertionSort(dev, count_);
        const size_t rank = (50u * count_ + 99) / 100;
        size_t idx = rank == 0 ? 0 : rank - 1;
        if (idx >= count_) idx = count_ - 1;
        return dev[idx];
    }

private:
    size_t sortedCopy(int64_t* out) const {
        for (size_t i = 0; i < count_; ++i) out[i] = ring_[i];
        insertionSort(out, count_);
        return count_;
    }

    static void insertionSort(int64_t* a, size_t n) {
        for (size_t i = 1; i < n; ++i) {
            const int64_t key = a[i];
            size_t j = i;
            while (j > 0 && a[j - 1] > key) {
                a[j] = a[j - 1];
                --j;
            }
            a[j] = key;
        }
    }

    static int64_t isqrt(int64_t v) {
        if (v <= 0) return 0;
        int64_t x = v, y = (x + 1) / 2;
        while (y < x) {
            x = y;
            y = (x + v / x) / 2;
        }
        return x;
    }

    int64_t ring_[Capacity] = {};
    size_t count_ = 0;
    size_t next_ = 0;
};

}
