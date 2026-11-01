#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include "input/InputWorkers.h"

namespace qinput {

class DirectInput8KeyboardBackend final : public IKeyboardBackend {
public:
    explicit DirectInput8KeyboardBackend(HWND focusWindow)
        : focusWindow_(focusWindow) {}
    ~DirectInput8KeyboardBackend() override { destroy(); }

    bool create() override {
        destroy();
        if (FAILED(DirectInput8Create(
                GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                IID_IDirectInput8A,
                reinterpret_cast<void**>(&directInput_), nullptr))) {
            return false;
        }
        if (FAILED(directInput_->CreateDevice(GUID_SysKeyboard, &device_,
                                              nullptr))) {
            destroy();
            return false;
        }
        if (FAILED(device_->SetDataFormat(&c_dfDIKeyboard))) {
            destroy();
            return false;
        }
        if (focusWindow_ == nullptr || !IsWindow(focusWindow_)) {
            destroy();
            return false;
        }
        if (FAILED(device_->SetCooperativeLevel(
                focusWindow_, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
            destroy();
            return false;
        }
        (void)device_->Acquire();
        return true;
    }

    void destroy() override {
        if (device_ != nullptr) {
            device_->Unacquire();
            device_->Release();
            device_ = nullptr;
        }
        if (directInput_ != nullptr) {
            directInput_->Release();
            directInput_ = nullptr;
        }
    }

    bool poll(uint8_t (&keys)[256]) override {
        if (device_ == nullptr) return false;
        HRESULT hr = device_->GetDeviceState(sizeof(keys), keys);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            if (FAILED(device_->Acquire())) return false;
            hr = device_->GetDeviceState(sizeof(keys), keys);
        }
        return SUCCEEDED(hr);
    }

private:
    HWND focusWindow_ = nullptr;
    IDirectInput8A* directInput_ = nullptr;
    IDirectInputDevice8A* device_ = nullptr;
};

IKeyboardBackend* createDirectInput8KeyboardBackend(void* focusWindow) {
    return new DirectInput8KeyboardBackend(
        static_cast<HWND>(focusWindow));
}

}
