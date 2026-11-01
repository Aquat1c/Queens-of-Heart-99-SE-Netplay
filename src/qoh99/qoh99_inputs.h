#pragma once
#include <cstdint>
#include <vector>
#include <climits>
#include <algorithm>

namespace qoh99 {

// Logical gameplay input mask, dword_4A2A38[8] = { 0x10000,0x20000,0x40000,0x80000, 1,2,4,8 }.
enum QohInputBits : uint32_t {
    QOH_IN_DIR0   = 0x10000,
    QOH_IN_DIR1   = 0x20000,
    QOH_IN_LEFT   = 0x40000,
    QOH_IN_RIGHT  = 0x80000,
    QOH_IN_DIRMASK = 0xF0000,

    QOH_IN_BTN0   = 0x1,
    QOH_IN_BTN1   = 0x2,
    QOH_IN_BTN2   = 0x4,
    QOH_IN_BTN3   = 0x8,
    QOH_IN_BTNMASK = 0xF,

    QOH_IN_SYS0   = 0x100,
    QOH_IN_SYS1   = 0x200,
    QOH_IN_SYSMASK = 0x300,

    QOH_IN_GAMEPLAY_MASK = QOH_IN_DIRMASK | QOH_IN_BTNMASK,
};

// The native facing transform tests RIGHT first, so simultaneous LEFT+RIGHT becomes LEFT when facing == 1.
constexpr uint32_t qohApplyFacing(uint32_t logical, uint8_t facing) {
    if (facing != 1) return logical;
    uint32_t rest = logical & 0xFFF3FFFF;
    if (logical & QOH_IN_RIGHT) return rest | QOH_IN_LEFT;
    if (logical & QOH_IN_LEFT)  return rest | QOH_IN_RIGHT;
    return logical;
}
static_assert(qohApplyFacing(QOH_IN_LEFT | QOH_IN_RIGHT, 1) == QOH_IN_LEFT,
              "simultaneous L+R must become LEFT for facing 1");
static_assert(qohApplyFacing(QOH_IN_DIR0 | QOH_IN_DIR1, 1)
                  == (QOH_IN_DIR0 | QOH_IN_DIR1),
              "UP+DOWN stays simultaneous under facing");

// Net packing: buttons->bits0-3, directions->bits8-11, system->bits12-13, seq->bits16-31.
inline uint32_t qohPackGameplayInputForNet(uint32_t logical, uint32_t frameSeq) {
    uint32_t buttons = logical & QOH_IN_BTNMASK;
    uint32_t dirs    = (logical & QOH_IN_DIRMASK) >> 8;
    uint32_t sysbits = (logical & QOH_IN_SYSMASK) << 4;
    return buttons | dirs | sysbits | (frameSeq << 16);
}

// Net unpacking: logical word = (u8)v1 | ((v1 & 0xF00) << 8); managed input rejects bits4-7 and bits14-15.
inline uint32_t qohUnpackGameplayInputFromNet(uint32_t packed) {
    uint32_t buttons = packed & QOH_IN_BTNMASK;
    uint32_t dirs    = (packed & 0xF00) << 8;
    return buttons | dirs;
}
inline uint32_t qohUnpackSystemBits(uint32_t packed) { return (packed >> 12) & 0x3; }
inline uint32_t qohUnpackNetSeq(uint32_t packed)     { return packed >> 16; }

// IndexedFrame packing: index in the high 12 bits, frame in the low 20 bits.
struct IndexedFrame {
    uint32_t value = 0;
    IndexedFrame() = default;
    IndexedFrame(uint32_t index, uint32_t frame) : value((index << 20) | (frame & 0xFFFFF)) {}
    uint32_t index() const { return value >> 20; }
    uint32_t frame() const { return value & 0xFFFFF; }
    bool operator<(const IndexedFrame& o) const { return value < o.value; }
    bool operator==(const IndexedFrame& o) const { return value == o.value; }
    bool operator<=(const IndexedFrame& o) const { return value <= o.value; }
};

constexpr uint32_t kMaxIndexedFrame = 0xFFFFFFFFu;

template <typename T>
class InputsContainer {
public:
    T get(uint32_t index, uint32_t frame) const {
        if (index >= _inputs.size() || _inputs[index].empty())
            return lastInputBefore(index);
        if (frame >= _inputs[index].size())
            return _inputs[index].back();
        return _inputs[index][frame];
    }

    void set(uint32_t index, uint32_t frame, T v) {
        if (_inputs.size() > index && _inputs[index].size() > frame) return;
        resize(index, frame);
        _inputs[index][frame] = v;
    }

    void assign(uint32_t index, uint32_t frame, T v) {
        resize(index, frame);
        _inputs[index][frame] = v;
    }

    void setRange(uint32_t index, uint32_t frame, const T* t, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            IndexedFrame f(index, (uint32_t)(frame + i));
            if (get(f.index(), f.frame()) == t[i]) continue;
            if (f.value < _lastChangedFrame.value) _lastChangedFrame = f;
            break;
        }
        resize(index, (uint32_t)(frame + n - 1));
        for (size_t i = 0; i < n; ++i) _inputs[index][frame + i] = t[i];
    }

    uint32_t endIndex() const { return (uint32_t)_inputs.size(); }
    uint32_t endFrame() const { return _inputs.empty() ? 0 : (uint32_t)_inputs.back().size(); }
    uint32_t endFrame(size_t index) const { return (index >= _inputs.size()) ? 0 : (uint32_t)_inputs[index].size(); }
    bool empty() const { return _inputs.empty(); }

    IndexedFrame lastChangedFrame() const { return _lastChangedFrame; }
    void clearLastChangedFrame() { _lastChangedFrame.value = kMaxIndexedFrame; }
    void clear() { _inputs.clear(); _lastChangedFrame.value = kMaxIndexedFrame; }

private:
    std::vector<std::vector<T>> _inputs;
    IndexedFrame _lastChangedFrame{ kMaxIndexedFrame >> 20, kMaxIndexedFrame & 0xFFFFF };

    void resize(uint32_t index, uint32_t frame) {
        T last = 0;
        if (index >= _inputs.size()) { last = lastInputBefore((uint32_t)_inputs.size()); _inputs.resize(index + 1); }
        else if (!_inputs[index].empty()) last = _inputs[index].back();
        if (frame + 1 > _inputs[index].size()) _inputs[index].resize(frame + 1, last);
    }

    T lastInputBefore(uint32_t index) const {
        if (_inputs.empty() || index == 0) return 0;
        if (index > _inputs.size()) index = (uint32_t)_inputs.size();
        do { --index; if (!_inputs[index].empty()) return _inputs[index].back(); } while (index > 0);
        return 0;
    }
};

}
