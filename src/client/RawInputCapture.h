#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace qclient {

struct CapturedKey {
    uint16_t scanCode = 0;
    uint16_t virtualKey = 0;
    bool extended = false;
    bool e1 = false;

    bool operator==(const CapturedKey& other) const {
        return scanCode == other.scanCode && extended == other.extended
            && e1 == other.e1;
    }
    bool operator!=(const CapturedKey& other) const { return !(*this == other); }
};

struct RawKeyboardPacket {
    uint16_t makeCode = 0;
    uint16_t flags = 0;
    uint16_t virtualKey = 0;
    uint32_t message = 0;
};

inline constexpr uint16_t kRawKeyMake = 0x0000;
inline constexpr uint16_t kRawKeyBreak = 0x0001;
inline constexpr uint16_t kRawKeyE0 = 0x0002;
inline constexpr uint16_t kRawKeyE1 = 0x0004;

inline constexpr uint16_t kFakeVirtualKey = 0xFF;

class ScanNormalizer {
public:
    enum class Result : uint8_t {
        Ignored,
        Press,
        Repeat,
        Release,
    };

    Result feed(const RawKeyboardPacket& packet, CapturedKey& out) {
        out = CapturedKey{};

        const bool isBreak = (packet.flags & kRawKeyBreak) != 0;
        const bool hasE0 = (packet.flags & kRawKeyE0) != 0;
        const bool hasE1 = (packet.flags & kRawKeyE1) != 0;

        if (packet.virtualKey == kFakeVirtualKey) {
            _pendingE1 = false;
            return Result::Ignored;
        }

        CapturedKey key{};
        if (hasE1) {
            if (packet.makeCode != 0x1D) {
                _pendingE1 = false;
                return Result::Ignored;
            }
            _pendingE1 = !isBreak;
            key.scanCode = 0x1D;
            key.extended = false;
            key.e1 = true;
            key.virtualKey = packet.virtualKey != 0 ? packet.virtualKey
                                                    : uint16_t{0x13} ;
        } else {
            if (_pendingE1 && packet.makeCode == 0x45) {
                _pendingE1 = false;
                return Result::Ignored;
            }
            _pendingE1 = false;

            if (hasE0 && (packet.makeCode == 0x2A || packet.makeCode == 0x36))
                return Result::Ignored;

            if (packet.makeCode == 0)
                return Result::Ignored;

            key.scanCode = static_cast<uint16_t>(packet.makeCode & 0x00FF);
            key.extended = hasE0;
            key.e1 = false;
            key.virtualKey = packet.virtualKey;
        }

        const std::size_t index = slot(key);
        if (isBreak) {
            if (!_down[index])
                return Result::Ignored;
            _down[index] = false;
            out = key;
            return Result::Release;
        }

        if (_down[index]) {
            out = key;
            return Result::Repeat;
        }
        _down[index] = true;
        out = key;
        return Result::Press;
    }

    void reset() {
        for (bool& down : _down) down = false;
        _pendingE1 = false;
    }

    bool isDown(const CapturedKey& key) const { return _down[slot(key)]; }
    bool anyDown() const {
        for (bool down : _down)
            if (down) return true;
        return false;
    }

private:
    static std::size_t slot(const CapturedKey& key) {
        return static_cast<std::size_t>(key.scanCode & 0xFF)
             | (key.extended ? 0x100u : 0u)
             | (key.e1 ? 0x200u : 0u);
    }

    bool _down[0x400] = {};
    bool _pendingE1 = false;
};

class RawInputCapture {
public:
    enum class CommitMode : uint8_t { Press, Release };

    RawInputCapture();
    ~RawInputCapture();
    RawInputCapture(const RawInputCapture&) = delete;
    RawInputCapture& operator=(const RawInputCapture&) = delete;

    bool start(std::string& error);
    void stop();
    bool running() const;

    bool poll(CapturedKey& out);

    void flush();

    const char* backendName() const;

    void setCommitMode(CommitMode mode);
    CommitMode commitMode() const;

    HWND focusWindow() const;

    HWND messageWindow() const;

    void setPreferredFocusWindow(HWND window);

    const std::string& lastError() const;
    unsigned long suppressedRepeats() const;
    unsigned long backgroundDrops() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}
