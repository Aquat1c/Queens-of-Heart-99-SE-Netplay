#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <cstdio>
#include <cstring>

#include "Log.h"

#include "input/DeviceIdentity.h"
#include "input/InputWorkers.h"

namespace qinput {
namespace {

constexpr uint32_t kMaxEnumeratedDevices = kMaxIdentityCandidates;

struct EnumeratedDevice {
    GUID instance{};
    GUID product{};
    char name[128] = {};
    uint16_t vendor = 0;
    uint16_t product16 = 0;
};

struct EnumContext {
    EnumeratedDevice* devices = nullptr;
    uint32_t capacity = 0;
    uint32_t count = 0;
};

void guidToText(const GUID& guid, char* out, size_t capacity) {
    std::snprintf(out, capacity,
                  "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned long>(guid.Data1), guid.Data2,
                  guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
                  guid.Data4[3], guid.Data4[4], guid.Data4[5],
                  guid.Data4[6], guid.Data4[7]);
}

BOOL CALLBACK enumDeviceCallback(const DIDEVICEINSTANCEA* instance,
                                 void* context) {
    EnumContext* ctx = static_cast<EnumContext*>(context);
    if (ctx->count >= ctx->capacity) return DIENUM_STOP;
    EnumeratedDevice& device = ctx->devices[ctx->count];
    device = EnumeratedDevice{};
    device.instance = instance->guidInstance;
    device.product = instance->guidProduct;
    size_t written = 0;
    for (const char* p = instance->tszProductName;
         *p != '\0' && written + 1 < sizeof(device.name); ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        device.name[written++] =
            (c < 0x20 || c > 0x7E) ? '?' : static_cast<char>(c);
    }
    device.name[written] = '\0';
    device.vendor = static_cast<uint16_t>(instance->guidProduct.Data1
                                          & 0xFFFFu);
    device.product16 = static_cast<uint16_t>(
        (instance->guidProduct.Data1 >> 16) & 0xFFFFu);
    ++ctx->count;
    return DIENUM_CONTINUE;
}

BOOL CALLBACK enumAxisCallback(const DIDEVICEOBJECTINSTANCEA* object,
                               void* context) {
    IDirectInputDevice8A* device =
        static_cast<IDirectInputDevice8A*>(context);
    DIPROPRANGE range{};
    range.diph.dwSize = sizeof(DIPROPRANGE);
    range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    range.diph.dwHow = DIPH_BYID;
    range.diph.dwObj = object->dwType;
    range.lMin = -32768;
    range.lMax = 32767;
    (void)device->SetProperty(DIPROP_RANGE, &range.diph);
    return DIENUM_CONTINUE;
}

uint32_t povToHatMask(DWORD pov) {
    if (LOWORD(pov) == 0xFFFFu) return 0;
    const uint32_t degrees = (pov / 100u) % 360u;
    uint32_t mask = 0;
    if (degrees > 270u || degrees < 90u) mask |= kHatUp;
    if (degrees > 0u && degrees < 180u) mask |= kHatRight;
    if (degrees > 90u && degrees < 270u) mask |= kHatDown;
    if (degrees > 180u) mask |= kHatLeft;
    if (degrees == 0u) mask = kHatUp;
    if (degrees == 90u) mask = kHatRight;
    if (degrees == 180u) mask = kHatDown;
    if (degrees == 270u) mask = kHatLeft;
    return mask;
}

int32_t axisByIndex(const DIJOYSTATE2& state, uint8_t index) {
    switch (index) {
        case 0: return state.lX;
        case 1: return state.lY;
        case 2: return state.lZ;
        case 3: return state.lRx;
        case 4: return state.lRy;
        case 5: return state.lRz;
        case 6: return state.rglSlider[0];
        case 7: return state.rglSlider[1];
        default: return 0;
    }
}

}

constexpr wchar_t kFocusWindowClass[] = L"Qoh99DirectInputFocus";

HWND createHiddenFocusWindow() {
    static ATOM atom = 0;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (atom == 0) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = kFocusWindowClass;
        atom = RegisterClassExW(&wc);
        if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
    }
    return CreateWindowExW(0, kFocusWindowClass, L"", WS_POPUP, 0, 0, 0, 0,
                           nullptr, nullptr, instance, nullptr);
}

class DirectInput8ControllerBackend final : public IControllerBackend {
public:
    explicit DirectInput8ControllerBackend(void* focusWindow)
        : suppliedWindow_(static_cast<HWND>(focusWindow)) {}
    ~DirectInput8ControllerBackend() override {
        close();
        shutdown();
    }

    bool initialize() override {
        if (directInput_ != nullptr) return true;
        const HRESULT hr = DirectInput8Create(
            GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
            IID_IDirectInput8A,
            reinterpret_cast<void**>(&directInput_), nullptr);
        if (FAILED(hr)) {
            QLOG("[controller.di8] DirectInput8Create failed hr=0x%08lX",
                 static_cast<unsigned long>(hr));
            return false;
        }
        resolveFocusWindow();
        if (focusWindow_ == nullptr) {
            QLOG("[controller.di8] no usable cooperative-level window; "
                 "devices will enumerate but cannot be opened");
            return false;
        }
        return true;
    }

    void shutdown() override {
        close();
        if (directInput_ != nullptr) {
            directInput_->Release();
            directInput_ = nullptr;
        }
        if (ownedWindow_ != nullptr) {
            DestroyWindow(ownedWindow_);
            ownedWindow_ = nullptr;
        }
        focusWindow_ = nullptr;
    }

    uint32_t enumerate(DeviceCandidate* out, uint32_t capacity) override {
        if (directInput_ == nullptr || out == nullptr || capacity == 0)
            return 0;
        EnumeratedDevice found[kMaxEnumeratedDevices];
        EnumContext context{found, kMaxEnumeratedDevices, 0};
        if (FAILED(directInput_->EnumDevices(
                DI8DEVCLASS_GAMECTRL, &enumDeviceCallback, &context,
                DIEDFL_ATTACHEDONLY))) {
            return 0;
        }
        const uint32_t total =
            context.count < capacity ? context.count : capacity;
        for (uint32_t i = 0; i < total; ++i) {
            out[i] = DeviceCandidate{};
            out[i].liveHandle = i;
            guidToText(found[i].product, out[i].guid, sizeof(out[i].guid));
            out[i].vendor = found[i].vendor;
            out[i].product = found[i].product16;
            out[i].version = 0;
            std::snprintf(out[i].normalizedName,
                          sizeof(out[i].normalizedName), "%s",
                          found[i].name);
            char instanceText[64];
            guidToText(found[i].instance, instanceText,
                       sizeof(instanceText));
            out[i].hasPathFingerprint = true;
            out[i].pathFingerprint = identityStringHash(instanceText);
            probeCapabilities(found[i], out[i]);
        }
        for (uint32_t i = 0; i < context.count; ++i)
            enumerated_[i] = found[i];
        enumeratedCount_ = context.count;
        return total;
    }

    void probeCapabilities(const EnumeratedDevice& device,
                           DeviceCandidate& out) {
        IDirectInputDevice8A* probe = nullptr;
        if (FAILED(directInput_->CreateDevice(device.instance, &probe,
                                              nullptr))
            || probe == nullptr) {
            return;
        }
        DIDEVCAPS caps{};
        caps.dwSize = sizeof(caps);
        if (SUCCEEDED(probe->GetCapabilities(&caps))) {
            out.probeOpened = true;
            out.axisCount = static_cast<uint16_t>(
                caps.dwAxes > 0xFFFFu ? 0xFFFFu : caps.dwAxes);
            out.buttonCount = static_cast<uint16_t>(
                caps.dwButtons > 0xFFFFu ? 0xFFFFu : caps.dwButtons);
            out.hatCount = static_cast<uint16_t>(
                caps.dwPOVs > 0xFFFFu ? 0xFFFFu : caps.dwPOVs);
        }
        probe->Release();
    }

    bool open(const DeviceCandidate& candidate, const char* mapping,
              uint32_t mappingLength) override {
        (void)mapping;
        (void)mappingLength;
        close();
        if (directInput_ == nullptr) return false;
        if (candidate.liveHandle >= enumeratedCount_) return false;
        const EnumeratedDevice& target = enumerated_[candidate.liveHandle];
        if (FAILED(directInput_->CreateDevice(target.instance, &device_,
                                              nullptr))) {
            return false;
        }
        if (FAILED(device_->SetDataFormat(&c_dfDIJoystick2))) {
            close();
            return false;
        }
        if (focusWindow_ == nullptr || !IsWindow(focusWindow_))
            resolveFocusWindow();
        if (focusWindow_ == nullptr) {
            QLOG("[controller.di8] open '%s' refused: no cooperative-level "
                 "window", target.name);
            close();
            return false;
        }
        const HRESULT coop = device_->SetCooperativeLevel(
            focusWindow_, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
        if (FAILED(coop)) {
            QLOG("[controller.di8] SetCooperativeLevel failed for '%s' "
                 "hr=0x%08lX hwnd=%p",
                 target.name, static_cast<unsigned long>(coop),
                 reinterpret_cast<void*>(focusWindow_));
            close();
            return false;
        }
        (void)device_->EnumObjects(&enumAxisCallback, device_, DIDFT_AXIS);
        const HRESULT acquired = device_->Acquire();
        if (FAILED(acquired)) {
            QLOG("[controller.di8] '%s' opened but acquire failed hr=0x%08lX%s",
                 target.name, static_cast<unsigned long>(acquired),
                 acquired == DIERR_OTHERAPPHASPRIO
                     ? " (another application holds this device exclusively -"
                       " Steam Input is the usual cause)"
                     : "");
        }
        std::memcpy(&openProduct_, &target.product, sizeof(GUID));
        return true;
    }

    void close() override {
        if (device_ != nullptr) {
            device_->Unacquire();
            device_->Release();
            device_ = nullptr;
        }
    }

    bool mappingUnchanged() override {
        return true;
    }

    bool readProbes(const BackendProbeRequest* requests, uint32_t count,
                    uint32_t* values) override {
        if (device_ == nullptr || requests == nullptr || values == nullptr)
            return false;
        HRESULT hr = device_->Poll();
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            if (FAILED(device_->Acquire())) return false;
            hr = device_->Poll();
        }
        DIJOYSTATE2 state{};
        hr = device_->GetDeviceState(sizeof(state), &state);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            if (FAILED(device_->Acquire())) return false;
            hr = device_->GetDeviceState(sizeof(state), &state);
        }
        if (FAILED(hr)) return false;
        for (uint32_t i = 0; i < count; ++i) {
            switch (requests[i].kind) {
                case ProbeKind::RawButton:
                    values[i] = requests[i].token < 128
                        && (state.rgbButtons[requests[i].token] & 0x80)
                        ? 1u : 0u;
                    break;
                case ProbeKind::RawAxis:
                    values[i] = static_cast<uint32_t>(
                        axisByIndex(state, requests[i].token));
                    break;
                case ProbeKind::GamepadButton:
                case ProbeKind::GamepadAxis:
                    return false;
                case ProbeKind::RawHat:
                    values[i] = requests[i].token < 4
                        ? povToHatMask(state.rgdwPOV[requests[i].token])
                        : 0u;
                    break;
                case ProbeKind::None:
                    values[i] = 0u;
                    break;
            }
        }
        return true;
    }

private:
    void resolveFocusWindow() {
        if (focusWindow_ != nullptr && IsWindow(focusWindow_)) return;
        focusWindow_ = nullptr;
        const char* source = "none";
        if (suppliedWindow_ != nullptr && IsWindow(suppliedWindow_)) {
            focusWindow_ = suppliedWindow_;
            source = "supplied";
        }
        if (focusWindow_ == nullptr) {
            const HWND console = GetConsoleWindow();
            if (console != nullptr && IsWindow(console)) {
                focusWindow_ = console;
                source = "console";
            }
        }
        if (focusWindow_ == nullptr) {
            const HWND foreground = GetForegroundWindow();
            if (foreground != nullptr && IsWindow(foreground)) {
                focusWindow_ = foreground;
                source = "foreground";
            }
        }
        if (focusWindow_ == nullptr) {
            if (ownedWindow_ != nullptr && !IsWindow(ownedWindow_))
                ownedWindow_ = nullptr;
            if (ownedWindow_ == nullptr) ownedWindow_ = createHiddenFocusWindow();
            if (ownedWindow_ != nullptr) {
                focusWindow_ = ownedWindow_;
                source = "hidden";
            }
        }
        QLOG("[controller.di8] cooperative-level window source=%s hwnd=%p",
             source, reinterpret_cast<void*>(focusWindow_));
    }

    HWND suppliedWindow_ = nullptr;
    HWND ownedWindow_ = nullptr;
    HWND focusWindow_ = nullptr;
    IDirectInput8A* directInput_ = nullptr;
    IDirectInputDevice8A* device_ = nullptr;
    GUID openProduct_{};
    EnumeratedDevice enumerated_[kMaxEnumeratedDevices];
    uint32_t enumeratedCount_ = 0;
};

IControllerBackend* createDirectInput8ControllerBackend(void* focusWindow) {
    return new DirectInput8ControllerBackend(focusWindow);
}

}
