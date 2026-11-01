#ifdef QOH99_WITH_SDL3

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

#include "Log.h"

#include "input/DeviceIdentity.h"
#include "input/InputWorkers.h"

namespace qinput {
namespace {

void fillCandidate(SDL_JoystickID id, DeviceCandidate& out) {
    out = DeviceCandidate{};
    out.liveHandle = static_cast<uint32_t>(id);
    out.isGamepad = SDL_IsGamepad(id);
    SDL_GUID guid = SDL_GetJoystickGUIDForID(id);
    SDL_GUIDToString(guid, out.guid, sizeof(out.guid));
    out.vendor = SDL_GetJoystickVendorForID(id);
    out.product = SDL_GetJoystickProductForID(id);
    out.version = SDL_GetJoystickProductVersionForID(id);
    const char* name = SDL_GetJoystickNameForID(id);
    if (name != nullptr) {
        size_t w = 0;
        for (const char* p = name;
             *p != '\0' && w + 1 < sizeof(out.normalizedName); ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            out.normalizedName[w++] =
                (c < 0x20 || c > 0x7E) ? '?' : static_cast<char>(c);
        }
        out.normalizedName[w] = '\0';
    }
    const char* path = SDL_GetJoystickPathForID(id);
    if (path != nullptr) {
        out.hasPathFingerprint = true;
        uint64_t hash = identityStringHash(path);
        hash ^= (static_cast<uint64_t>(SDL_VERSION) << 32);
        out.pathFingerprint = hash;
    }
    return;
}

void probeSerial(SDL_JoystickID id, DeviceCandidate& candidate) {
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    if (joystick == nullptr) {
        const char* error = SDL_GetError();
        if (error != nullptr) {
            std::snprintf(candidate.probeError, sizeof(candidate.probeError),
                          "%s", error);
        }
        return;
    }
    candidate.probeOpened = true;
    const char* serial = SDL_GetJoystickSerial(joystick);
    if (serial != nullptr && serial[0] != '\0') {
        candidate.hasSerial = true;
        candidate.serialHash = identityStringHash(serial);
    }
    const int axes = SDL_GetNumJoystickAxes(joystick);
    const int buttons = SDL_GetNumJoystickButtons(joystick);
    const int hats = SDL_GetNumJoystickHats(joystick);
    candidate.axisCount = axes > 0 ? static_cast<uint16_t>(axes) : 0u;
    candidate.buttonCount = buttons > 0 ? static_cast<uint16_t>(buttons) : 0u;
    candidate.hatCount = hats > 0 ? static_cast<uint16_t>(hats) : 0u;
    SDL_CloseJoystick(joystick);
}

}

class SdlControllerBackend final : public IControllerBackend {
public:
    ~SdlControllerBackend() override { close(); }

    bool initialize() override {
        if (!SDL_WasInit(SDL_INIT_GAMEPAD)) return false;
        SDL_SetJoystickEventsEnabled(false);
        SDL_SetGamepadEventsEnabled(false);
        return true;
    }

    void shutdown() override { close(); }

    uint32_t enumerate(DeviceCandidate* out, uint32_t capacity) override {
        SDL_UpdateJoysticks();
        int count = 0;
        SDL_JoystickID* ids = SDL_GetJoysticks(&count);
        if (ids == nullptr) return 0;
        uint32_t written = 0;
        for (int i = 0; i < count && written < capacity; ++i) {
            fillCandidate(ids[i], out[written]);
            if (openJoystickId_ == 0
                || ids[i] != static_cast<SDL_JoystickID>(
                       openJoystickId_)) {
                probeSerial(ids[i], out[written]);
            }
            ++written;
        }
        SDL_free(ids);
        QLOG("[controller.sdl] enumerate present=%d reported=%u", count,
             written);
        for (uint32_t i = 0; i < written; ++i) {
            QLOG("[controller.sdl]   [%u] live=%u name='%s' gamepad=%d "
                 "probeOpen=%d axes=%u buttons=%u hats=%u serial=%d path=%d "
                 "vid=%04X pid=%04X ver=%04X%s%s",
                 i, out[i].liveHandle, out[i].normalizedName,
                 out[i].isGamepad ? 1 : 0, out[i].probeOpened ? 1 : 0,
                 static_cast<unsigned>(out[i].axisCount),
                 static_cast<unsigned>(out[i].buttonCount),
                 static_cast<unsigned>(out[i].hatCount),
                 out[i].hasSerial ? 1 : 0,
                 out[i].hasPathFingerprint ? 1 : 0,
                 out[i].vendor, out[i].product, out[i].version,
                 out[i].probeError[0] == '\0' ? "" : " probeError=",
                 out[i].probeError);
        }
        return written;
    }

    bool open(const DeviceCandidate& candidate, const char* mapping,
              uint32_t mappingLength) override {
        close();
        const SDL_JoystickID id =
            static_cast<SDL_JoystickID>(candidate.liveHandle);
        if (SDL_GetJoystickNameForID(id) == nullptr) {
            QLOG("[controller.sdl] open REFUSED live=%u: that instance is no "
                 "longer connected (it enumerated, then went away)",
                 candidate.liveHandle);
            return false;
        }
        QLOG("[controller.sdl] open live=%u name='%s' path=%s isGamepad=%d",
             candidate.liveHandle, candidate.normalizedName,
             preferRaw_
                 ? "raw-joystick (chosen)"
                 : ((mapping != nullptr && mappingLength > 0)
                        ? "profile-mapping"
                        : (SDL_IsGamepad(id) ? "database-mapping"
                                             : "raw-joystick")),
             SDL_IsGamepad(id) ? 1 : 0);
        if (!preferRaw_ && mapping != nullptr && mappingLength > 0) {
            char bounded[2048];
            const uint32_t length =
                mappingLength < sizeof(bounded) - 1 ? mappingLength
                                                    : sizeof(bounded) - 1;
            std::memcpy(bounded, mapping, length);
            bounded[length] = '\0';
            // Apply the per-instance mapping before the mapped open. Never call SDL_AddGamepadMapping: it changes every device of this model process-wide.
            if (!SDL_SetGamepadMapping(id, bounded)) {
                QLOG("[controller.sdl] open FAILED live=%u: the profile's "
                     "saved mapping was rejected by SDL: %s",
                     candidate.liveHandle, SDL_GetError());
                return false;
            }
            gamepad_ = SDL_OpenGamepad(id);
            if (gamepad_ == nullptr) {
                QLOG("[controller.sdl] open FAILED live=%u mapped: %s",
                     candidate.liveHandle, SDL_GetError());
                return false;
            }
            if (SDL_GetGamepadID(gamepad_) != id) {
                QLOG("[controller.sdl] open FAILED live=%u: the opened "
                     "gamepad is a different instance than the one selected",
                     candidate.liveHandle);
                close();
                return false;
            }
            if (!recordOpenMapping(candidate.liveHandle, bounded)) {
                close();
                return false;
            }
            QLOG("[controller.sdl] open OK profile-mapping live=%u",
                 candidate.liveHandle);
            joystick_ = SDL_GetGamepadJoystick(gamepad_);
        } else if (!preferRaw_ && SDL_IsGamepad(id)) {
            gamepad_ = SDL_OpenGamepad(id);
            if (gamepad_ == nullptr) {
                QLOG("[controller.sdl] open FAILED live=%u: SDL reports a "
                     "database mapping for this device but would not open it "
                     "as a gamepad: %s",
                     candidate.liveHandle, SDL_GetError());
                return false;
            }
            if (SDL_GetGamepadID(gamepad_) != id) {
                QLOG("[controller.sdl] open FAILED live=%u: the opened "
                     "gamepad is a different instance than the one selected",
                     candidate.liveHandle);
                close();
                return false;
            }
            QLOG("[controller.sdl] open OK mapped live=%u", candidate.liveHandle);
            if (!recordOpenMapping(candidate.liveHandle, nullptr)) {
                close();
                return false;
            }
            joystick_ = SDL_GetGamepadJoystick(gamepad_);
        } else {
            joystick_ = SDL_OpenJoystick(id);
            if (joystick_ == nullptr) {
                QLOG("[controller.sdl] open FAILED raw live=%u: %s",
                     candidate.liveHandle, SDL_GetError());
                return false;
            }
            ownsJoystick_ = true;
            mapping_[0] = '\0';
            QLOG("[controller.sdl] open OK raw live=%u axes=%d buttons=%d "
                 "hats=%d",
                 candidate.liveHandle,
                 SDL_GetNumJoystickAxes(joystick_),
                 SDL_GetNumJoystickButtons(joystick_),
                 SDL_GetNumJoystickHats(joystick_));
        }
        openJoystickId_ = static_cast<uint32_t>(id);
        return true;
    }

    void close() override {
        if (gamepad_ != nullptr) {
            SDL_CloseGamepad(gamepad_);
            gamepad_ = nullptr;
            joystick_ = nullptr;
        }
        if (joystick_ != nullptr && ownsJoystick_) {
            SDL_CloseJoystick(joystick_);
        }
        joystick_ = nullptr;
        ownsJoystick_ = false;
        openJoystickId_ = 0;
    }

    bool semanticGamepad() const override { return gamepad_ != nullptr; }

    void preferRawJoystick(bool preferRaw) override {
        preferRaw_ = preferRaw;
    }

    uint32_t currentMapping(char* out, uint32_t capacity) override {
        if (gamepad_ == nullptr || out == nullptr || capacity == 0)
            return 0;
        const int written = std::snprintf(out, capacity, "%s", mapping_);
        return written > 0 && static_cast<uint32_t>(written) < capacity
            ? static_cast<uint32_t>(written) : 0;
    }

    bool mappingUnchanged() override {
        if (gamepad_ == nullptr) return true;
        char* current = SDL_GetGamepadMapping(gamepad_);
        const bool unchanged =
            current != nullptr && std::strcmp(current, mapping_) == 0;
        if (!unchanged) {
            QLOG("[controller.sdl] LAYOUT CHANGED on the open device; the "
                 "compiled bindings no longer describe it"
                 "\n  at open: %.240s"
                 "\n  now:     %.240s",
                 mapping_, current != nullptr ? current : "(none)");
        }
        SDL_free(current);
        return unchanged;
    }

    bool readProbes(const BackendProbeRequest* requests, uint32_t count,
                    uint32_t* values) override {
        if (gamepad_ != nullptr) {
            SDL_UpdateGamepads();
            if (!SDL_GamepadConnected(gamepad_)) return false;
        } else if (joystick_ != nullptr) {
            SDL_UpdateJoysticks();
            if (!SDL_JoystickConnected(joystick_)) return false;
        } else {
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            switch (requests[i].kind) {
                case ProbeKind::GamepadButton:
                    values[i] = gamepad_ != nullptr
                        && SDL_GetGamepadButton(
                               gamepad_,
                               static_cast<SDL_GamepadButton>(
                                   requests[i].token)) ? 1u : 0u;
                    break;
                case ProbeKind::GamepadAxis:
                    values[i] = gamepad_ != nullptr
                        ? static_cast<uint32_t>(static_cast<int32_t>(
                              SDL_GetGamepadAxis(
                                  gamepad_,
                                  static_cast<SDL_GamepadAxis>(
                                      requests[i].token))))
                        : 0u;
                    break;
                case ProbeKind::RawButton:
                    values[i] = joystick_ != nullptr
                        && SDL_GetJoystickButton(joystick_,
                                                 requests[i].token)
                        ? 1u : 0u;
                    break;
                case ProbeKind::RawAxis:
                    values[i] = joystick_ != nullptr
                        ? static_cast<uint32_t>(static_cast<int32_t>(
                              SDL_GetJoystickAxis(joystick_,
                                                  requests[i].token)))
                        : 0u;
                    break;
                case ProbeKind::RawHat:
                    values[i] = joystick_ != nullptr
                        ? SDL_GetJoystickHat(joystick_, requests[i].token)
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
    // SDL regenerates mapping text on read-back: use it only to test existence, and never byte-compare it against the text that was sent.
    bool recordOpenMapping(uint32_t liveHandle, const char* sent) {
        char* kept = SDL_GetGamepadMapping(gamepad_);
        const bool have = kept != nullptr && kept[0] != '\0';
        if (have) {
            std::snprintf(mapping_, sizeof(mapping_), "%s", kept);
            if (sent != nullptr && std::strcmp(kept, sent) != 0) {
                QLOG("[controller.sdl] mapping normalised by SDL live=%u"
                     "\n  sent:  %.240s"
                     "\n  kept:  %.240s",
                     liveHandle, sent, kept);
            }
        }
        SDL_free(kept);
        if (!have) {
            mapping_[0] = '\0';
            QLOG("[controller.sdl] open FAILED live=%u: the device opened as "
                 "a gamepad but SDL reports no mapping for it at all",
                 liveHandle);
        }
        return have;
    }

    SDL_Gamepad* gamepad_ = nullptr;
    SDL_Joystick* joystick_ = nullptr;
    bool preferRaw_ = false;
    bool ownsJoystick_ = false;
    uint32_t openJoystickId_ = 0;
    char mapping_[2048] = {};
};

IControllerBackend* createSdlControllerBackend() {
    return new SdlControllerBackend();
}

}

#endif
