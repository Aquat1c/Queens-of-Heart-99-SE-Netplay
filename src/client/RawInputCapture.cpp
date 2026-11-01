#include "client/RawInputCapture.h"

#include <cstdio>
#include <cwchar>
#include <deque>
#include <string>

#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#endif

namespace qclient {
namespace {

static_assert(kRawKeyMake == RI_KEY_MAKE, "RI_KEY_MAKE drifted");
static_assert(kRawKeyBreak == RI_KEY_BREAK, "RI_KEY_BREAK drifted");
static_assert(kRawKeyE0 == RI_KEY_E0, "RI_KEY_E0 drifted");
static_assert(kRawKeyE1 == RI_KEY_E1, "RI_KEY_E1 drifted");

constexpr std::size_t kQueueLimit = 32;
constexpr UINT kMsgShutdown = WM_APP + 0x51;
constexpr UINT kMsgFlush = WM_APP + 0x52;

std::string formatLastError(const char* what, DWORD code) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s failed (Windows error %lu)",
                  what, static_cast<unsigned long>(code));
    return std::string(buffer);
}

std::wstring uniqueSuffix(const void* owner) {
    static const wchar_t kDigits[] = L"0123456789abcdef";
    std::wstring out = L"_";
    unsigned long pid = GetCurrentProcessId();
    for (int shift = 28; shift >= 0; shift -= 4)
        out.push_back(kDigits[(pid >> shift) & 0xF]);
    out.push_back(L'_');
    uintptr_t value = reinterpret_cast<uintptr_t>(owner);
    for (int shift = static_cast<int>(sizeof(uintptr_t) * 8) - 4; shift >= 0;
         shift -= 4) {
        out.push_back(kDigits[(value >> shift) & 0xF]);
    }
    return out;
}

bool processHasForeground() {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

}

struct RawInputCapture::Impl {
    CRITICAL_SECTION lock{};
    bool lockReady = false;

    HANDLE thread = nullptr;
    DWORD threadId = 0;
    HANDLE ready = nullptr;

    HWND messageWindow = nullptr;
    HWND focusWindow = nullptr;
    HWND preferredFocus = nullptr;

    ATOM messageClass = 0;
    ATOM focusClass = 0;
    std::wstring messageClassName;
    std::wstring focusClassName;

    bool registered = false;
    bool startedOk = false;
    std::string startError;

    ScanNormalizer normalizer;
    std::deque<CapturedKey> queue;
    CommitMode commit = CommitMode::Press;

    unsigned long suppressedRepeats = 0;
    unsigned long backgroundDrops = 0;

    Impl() {
        InitializeCriticalSection(&lock);
        lockReady = true;
    }
    ~Impl() {
        if (lockReady) DeleteCriticalSection(&lock);
        if (ready != nullptr) CloseHandle(ready);
        if (thread != nullptr) CloseHandle(thread);
    }

    struct Guard {
        CRITICAL_SECTION& section;
        explicit Guard(CRITICAL_SECTION& s) : section(s) {
            EnterCriticalSection(&section);
        }
        ~Guard() { LeaveCriticalSection(&section); }
    };

    void enqueue(const CapturedKey& key) {
        Guard guard(lock);
        if (queue.size() >= kQueueLimit) queue.pop_front();
        queue.push_back(key);
    }

    void handleRawInput(HRAWINPUT handle) {
        UINT size = 0;
        if (GetRawInputData(handle, RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER)) != 0
            || size == 0 || size > 4096) {
            return;
        }
        BYTE storage[4096];
        if (GetRawInputData(handle, RID_INPUT, storage, &size,
                            sizeof(RAWINPUTHEADER)) != size) {
            return;
        }
        const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(storage);
        if (raw->header.dwType != RIM_TYPEKEYBOARD) return;

        if (!processHasForeground()) {
            ++backgroundDrops;
            normalizer.reset();
            return;
        }

        RawKeyboardPacket packet{};
        packet.makeCode = raw->data.keyboard.MakeCode;
        packet.flags = raw->data.keyboard.Flags;
        packet.virtualKey = raw->data.keyboard.VKey;
        packet.message = raw->data.keyboard.Message;

        CapturedKey key{};
        const CommitMode mode = [&] {
            Guard guard(lock);
            return commit;
        }();

        switch (normalizer.feed(packet, key)) {
            case ScanNormalizer::Result::Press:
                if (mode == CommitMode::Press) enqueue(key);
                break;
            case ScanNormalizer::Result::Release:
                if (mode == CommitMode::Release) enqueue(key);
                break;
            case ScanNormalizer::Result::Repeat:
                ++suppressedRepeats;
                break;
            case ScanNormalizer::Result::Ignored:
                break;
        }
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
        Impl* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (message) {
            case WM_NCCREATE: {
                const CREATESTRUCTW* create =
                    reinterpret_cast<const CREATESTRUCTW*>(lparam);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(
                                      create->lpCreateParams));
                break;
            }
            case WM_INPUT:
                if (self != nullptr)
                    self->handleRawInput(reinterpret_cast<HRAWINPUT>(lparam));
                return DefWindowProcW(hwnd, message, wparam, lparam);
            case kMsgShutdown:
                PostQuitMessage(0);
                return 0;
            case kMsgFlush:
                if (self != nullptr) self->normalizer.reset();
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    bool createWindows(std::string& error) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const std::wstring suffix = uniqueSuffix(this);
        messageClassName = std::wstring(L"Qoh99CasterRawInput") + suffix;
        focusClassName = std::wstring(L"Qoh99CasterFocus") + suffix;

        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = &Impl::wndProc;
        cls.hInstance = instance;
        cls.lpszClassName = messageClassName.c_str();
        messageClass = RegisterClassExW(&cls);
        if (messageClass == 0) {
            error = formatLastError("RegisterClassExW(raw input)",
                                    GetLastError());
            return false;
        }

        messageWindow = CreateWindowExW(
            0, messageClassName.c_str(), L"QOH99 caster raw input",
            0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        if (messageWindow == nullptr) {
            error = formatLastError("CreateWindowExW(HWND_MESSAGE)",
                                    GetLastError());
            return false;
        }

        cls.lpszClassName = focusClassName.c_str();
        focusClass = RegisterClassExW(&cls);
        if (focusClass != 0) {
            focusWindow = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, focusClassName.c_str(),
                L"QOH99 caster", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                instance, this);
        }
        return true;
    }

    bool registerRawInput(std::string& error) {
        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x01;
        device.usUsage = 0x06;
        device.dwFlags = RIDEV_INPUTSINK;
        device.hwndTarget = messageWindow;
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
            error = formatLastError("RegisterRawInputDevices(keyboard)",
                                    GetLastError());
            return false;
        }
        registered = true;
        return true;
    }

    void unregisterRawInput() {
        if (!registered) return;
        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x01;
        device.usUsage = 0x06;
        device.dwFlags = RIDEV_REMOVE;
        device.hwndTarget = nullptr;
        RegisterRawInputDevices(&device, 1, sizeof(device));
        registered = false;
    }

    void destroyWindows() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        if (focusWindow != nullptr) { DestroyWindow(focusWindow); focusWindow = nullptr; }
        if (messageWindow != nullptr) { DestroyWindow(messageWindow); messageWindow = nullptr; }
        if (focusClass != 0) {
            UnregisterClassW(focusClassName.c_str(), instance);
            focusClass = 0;
        }
        if (messageClass != 0) {
            UnregisterClassW(messageClassName.c_str(), instance);
            messageClass = 0;
        }
    }

    static DWORD WINAPI threadMain(LPVOID parameter) {
        Impl* self = static_cast<Impl*>(parameter);
        std::string error;
        const bool ok = self->createWindows(error)
                     && self->registerRawInput(error);
        {
            Guard guard(self->lock);
            self->startedOk = ok;
            self->startError = ok ? std::string() : error;
        }
        SetEvent(self->ready);
        if (!ok) {
            self->unregisterRawInput();
            self->destroyWindows();
            return 1;
        }

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        self->unregisterRawInput();
        self->destroyWindows();
        return 0;
    }
};

RawInputCapture::RawInputCapture() : _impl(new Impl()) {}

RawInputCapture::~RawInputCapture() { stop(); }

bool RawInputCapture::start(std::string& error) {
    Impl& impl = *_impl;
    if (impl.thread != nullptr) return impl.startedOk;

    impl.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl.ready == nullptr) {
        error = formatLastError("CreateEventW(raw input ready)",
                                GetLastError());
        impl.startError = error;
        return false;
    }
    impl.thread = CreateThread(nullptr, 0, &Impl::threadMain, &impl, 0,
                               &impl.threadId);
    if (impl.thread == nullptr) {
        error = formatLastError("CreateThread(raw input)", GetLastError());
        impl.startError = error;
        CloseHandle(impl.ready);
        impl.ready = nullptr;
        return false;
    }

    const DWORD waited = WaitForSingleObject(impl.ready, 5000);
    if (waited != WAIT_OBJECT_0) {
        error = "Raw Input capture did not become ready within 5 s";
        impl.startError = error;
        stop();
        return false;
    }

    Impl::Guard guard(impl.lock);
    if (!impl.startedOk) {
        error = impl.startError.empty()
                    ? std::string("Raw Input capture could not start")
                    : impl.startError;
        return false;
    }
    return true;
}

void RawInputCapture::stop() {
    Impl& impl = *_impl;
    if (impl.thread == nullptr) return;

    if (impl.messageWindow != nullptr)
        PostMessageW(impl.messageWindow, kMsgShutdown, 0, 0);
    else if (impl.threadId != 0)
        PostThreadMessageW(impl.threadId, WM_QUIT, 0, 0);

    if (WaitForSingleObject(impl.thread, 3000) != WAIT_OBJECT_0) {
        // Never TerminateThread here: it corrupts the owned window class and Raw Input registration.
        impl.thread = nullptr;
        impl.threadId = 0;
        impl.startedOk = false;
        return;
    }
    CloseHandle(impl.thread);
    impl.thread = nullptr;
    impl.threadId = 0;
    if (impl.ready != nullptr) {
        CloseHandle(impl.ready);
        impl.ready = nullptr;
    }
    impl.startedOk = false;

    Impl::Guard guard(impl.lock);
    impl.queue.clear();
}

bool RawInputCapture::running() const {
    return _impl->thread != nullptr && _impl->startedOk;
}

bool RawInputCapture::poll(CapturedKey& out) {
    Impl& impl = *_impl;
    Impl::Guard guard(impl.lock);
    if (impl.queue.empty()) return false;
    out = impl.queue.front();
    impl.queue.pop_front();
    return true;
}

void RawInputCapture::flush() {
    Impl& impl = *_impl;
    {
        Impl::Guard guard(impl.lock);
        impl.queue.clear();
    }
    if (impl.messageWindow != nullptr)
        PostMessageW(impl.messageWindow, kMsgFlush, 0, 0);
}

const char* RawInputCapture::backendName() const {
    return running() ? "rawinput" : "unavailable";
}

void RawInputCapture::setCommitMode(CommitMode mode) {
    Impl::Guard guard(_impl->lock);
    _impl->commit = mode;
}

RawInputCapture::CommitMode RawInputCapture::commitMode() const {
    Impl::Guard guard(_impl->lock);
    return _impl->commit;
}

HWND RawInputCapture::focusWindow() const {
    const Impl& impl = *_impl;
    if (impl.preferredFocus != nullptr && IsWindow(impl.preferredFocus))
        return impl.preferredFocus;
    return impl.focusWindow;
}

HWND RawInputCapture::messageWindow() const { return _impl->messageWindow; }

void RawInputCapture::setPreferredFocusWindow(HWND window) {
    _impl->preferredFocus = window;
}

const std::string& RawInputCapture::lastError() const {
    return _impl->startError;
}

unsigned long RawInputCapture::suppressedRepeats() const {
    return _impl->suppressedRepeats;
}

unsigned long RawInputCapture::backgroundDrops() const {
    return _impl->backgroundDrops;
}

}
