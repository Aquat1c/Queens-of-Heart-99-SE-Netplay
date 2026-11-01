#include "input/ControllerPreview.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "Log.h"

#include "input/DeviceIdentity.h"
#include "input/SdlLifecycle.h"

namespace qinput {
namespace {

constexpr uint8_t kPadSouth = 0, kPadEast = 1, kPadWest = 2, kPadNorth = 3;
constexpr uint8_t kPadBack = 4, kPadGuide = 5, kPadStart = 6;
constexpr uint8_t kPadLeftStick = 7, kPadRightStick = 8;
constexpr uint8_t kPadL1 = 9, kPadR1 = 10;
constexpr uint8_t kPadDpadUp = 11, kPadDpadDown = 12;
constexpr uint8_t kPadDpadLeft = 13, kPadDpadRight = 14;
constexpr uint8_t kPadButtonProbeCount = 21;
constexpr uint8_t kAxisLeftX = 0, kAxisLeftY = 1;
constexpr uint8_t kAxisRightX = 2, kAxisRightY = 3;
constexpr uint8_t kAxisLeftTrigger = 4, kAxisRightTrigger = 5;
constexpr uint8_t kPadAxisProbeCount = 6;

constexpr uint8_t kRawButtons = 32;
constexpr uint8_t kRawAxes = 8;
constexpr uint8_t kRawHats = 4;

constexpr int32_t kCaptureCandidateQ15 = 21299;
constexpr int32_t kCaptureNeutralQ15 = 8192;
constexpr int32_t kIdentifyAxisQ15 = 22937;

const char* const kPadButtonNames[kPadButtonProbeCount] = {
    "A / Cross", "B / Circle", "X / Square", "Y / Triangle",
    "Select", "Guide", "Start", "L3 (stick click)", "R3 (stick click)",
    "L1 / LB", "R1 / RB",
    "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right",
    "Misc", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"};

std::string padAxisLabel(uint8_t token, AxisPolarity polarity) {
    const bool negative = polarity == AxisPolarity::Negative;
    switch (token) {
        case kAxisLeftX: return negative ? "Left stick Left"
                                         : "Left stick Right";
        case kAxisLeftY: return negative ? "Left stick Up"
                                         : "Left stick Down";
        case kAxisRightX: return negative ? "Right stick Left"
                                          : "Right stick Right";
        case kAxisRightY: return negative ? "Right stick Up"
                                          : "Right stick Down";
        case kAxisLeftTrigger: return "L2 / LT";
        case kAxisRightTrigger: return "R2 / RT";
        default: return "Pad axis " + std::to_string(token);
    }
}

int32_t axisMagnitude(int32_t raw) { return raw < 0 ? -raw : raw; }

}

ControllerPreview::~ControllerPreview() { close(); }

bool ControllerPreview::open() {
    if (backend_ != nullptr) return true;
    sdlWaitForWarmup();
    const SdlBootstrapResult sdl = sdlBootstrapOnMainThread();
    lastBootstrap_ = sdl;
    QLOG("[controller.backend] sdl-bootstrap=%s backend=%s (%s)",
         sdlBootstrapResultText(sdl), controllerBackendName(),
         sdlVersionSummary().c_str());
    backend_ = createControllerBackend(nullptr);
    if (backend_ == nullptr) {
        backendFailure_ = "the controller backend could not be created";
        QLOG("[controller.backend] createControllerBackend returned null");
        return false;
    }
    if (!backend_->initialize()) {
        backendFailure_ =
            std::string("the ") + controllerBackendName()
            + " controller backend failed to start";
        QLOG("[controller.backend] %s", backendFailure_.c_str());
        delete backend_;
        backend_ = nullptr;
        return false;
    }
    backendFailure_.clear();
    return true;
}

const char* sdlBootstrapResultText(SdlBootstrapResult result) {
    switch (result) {
        case SdlBootstrapResult::NotRequired: return "not-required";
        case SdlBootstrapResult::Ready: return "ready";
        case SdlBootstrapResult::UnavailableBackend: return "unavailable";
        case SdlBootstrapResult::ForeignOwner: return "foreign-owner";
    }
    return "?";
}

std::string sdlVersionSummary() {
    char text[128] = {};
    sdlVersionText(text, sizeof(text));
    return text;
}

void ControllerPreview::close() {
    if (backend_ != nullptr) {
        backend_->close();
        backend_->shutdown();
        delete backend_;
        backend_ = nullptr;
    }
    selected_ = false;
    captureActive_ = false;
    sdlReleaseForLaunch();
}

const char* ControllerPreview::backendName() const {
    return controllerBackendName();
}

bool ControllerPreview::usingSdlBackend() const {
    return standardizedBackendAvailable();
}

std::string PreviewDevice::capabilitySummary() const {
    if (!probeOpened) {
        return probeError.empty()
            ? std::string("unavailable - could not be opened")
            : std::string("unavailable - ") + probeError;
    }
    if (axisCount == 0 && buttonCount == 0 && hatCount == 0)
        return "unavailable - reports no buttons, axes or hats";
    std::string shape;
    const auto add = [&shape](uint16_t count, const char* one,
                              const char* many) {
        if (count == 0) return;
        if (!shape.empty()) shape += ", ";
        shape += std::to_string(count);
        shape += ' ';
        shape += count == 1 ? one : many;
    };
    add(axisCount, "axis", "axes");
    add(buttonCount, "button", "buttons");
    add(hatCount, "hat", "hats");
    return (isGamepad ? std::string("SDL gamepad: ")
                      : std::string("Raw joystick: ")) + shape;
}

std::vector<PreviewDevice> ControllerPreview::devices() {
    std::vector<PreviewDevice> out;
    if (backend_ == nullptr) return out;
    DeviceCandidate candidates[kMaxIdentityCandidates];
    const uint32_t count =
        backend_->enumerate(candidates, kMaxIdentityCandidates);
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        PreviewDevice device{};
        device.liveHandle = candidates[i].liveHandle;
        device.name = candidates[i].normalizedName;
        device.guid = candidates[i].guid;
        device.vendor = candidates[i].vendor;
        device.product = candidates[i].product;
        device.version = candidates[i].version;
        device.hasPathFingerprint = candidates[i].hasPathFingerprint;
        device.pathFingerprint = candidates[i].pathFingerprint;
        device.hasSerial = candidates[i].hasSerial;
        device.serialHash = candidates[i].serialHash;
        device.isGamepad = candidates[i].isGamepad;
        device.probeOpened = candidates[i].probeOpened;
        device.axisCount = candidates[i].axisCount;
        device.buttonCount = candidates[i].buttonCount;
        device.hatCount = candidates[i].hatCount;
        device.probeError = candidates[i].probeError;
        if (device.name.empty()) device.name = "Controller";
        out.push_back(std::move(device));
    }
    return out;
}

bool ControllerPreview::select(const PreviewDevice& device, bool bindRaw,
                               const char* savedMapping,
                               uint32_t savedMappingLength) {
    if (backend_ == nullptr) return false;
    backend_->preferRawJoystick(bindRaw);
    DeviceCandidate candidate{};
    candidate.liveHandle = device.liveHandle;
    std::snprintf(candidate.guid, sizeof(candidate.guid), "%s",
                  device.guid.c_str());
    candidate.vendor = device.vendor;
    candidate.product = device.product;
    candidate.version = device.version;
    std::snprintf(candidate.normalizedName,
                  sizeof(candidate.normalizedName), "%s",
                  device.name.c_str());
    if (!backend_->open(candidate, savedMapping, savedMappingLength)) {
        QLOG("[controller.recognize] OPEN FAILED name='%s' live=%u "
             "guid=%s vid=%04X pid=%04X ver=%04X",
             device.name.c_str(), device.liveHandle, device.guid.c_str(),
             device.vendor, device.product, device.version);
        return false;
    }
    selection_ = device;
    selected_ = true;
    selectionUsedSdl_ = usingSdlBackend();
    selectionMappingLength_ = backend_->currentMapping(
        selectionMapping_, sizeof(selectionMapping_));
    const bool semanticMode = semantic();
    QLOG("[controller.recognize] name='%s' driven=%s sdlKnowsIt=%d "
         "layoutMapping=%s axes=%u buttons=%u hats=%u "
         "identity[serial=%s path=%s guid=%s vid=%04X pid=%04X ver=%04X] "
         "live=%u",
         device.name.c_str(),
         semanticMode ? "standardized gamepad tokens"
                      : "raw positional controls",
         device.isGamepad ? 1 : 0,
         selectionMappingLength_ > 0 ? "recorded" : "none",
         static_cast<unsigned>(device.axisCount),
         static_cast<unsigned>(device.buttonCount),
         static_cast<unsigned>(device.hatCount),
         device.hasSerial ? "yes" : "NO",
         device.hasPathFingerprint ? "yes" : "NO",
         device.guid.c_str(), device.vendor, device.product, device.version,
         device.liveHandle);
    if (!device.hasSerial && !device.hasPathFingerprint) {
        QLOG("[controller.recognize] WARNING: this device offers neither a "
             "serial nor a device path, so it can only be identified by its "
             "class (GUID/VID/PID/name). A second identical controller "
             "cannot be told apart from it.");
    }
    if (isMultiConsoleAdapterBox(device.vendor)) {
        QLOG("[controller.recognize] known family: WiseGroup multi-console "
             "adapter box (vid=0925%s). The plugged console pad decides the "
             "report: a Saturn pad's d-pad arrives as full-scale axes 0/1, a "
             "PS pad's on the hat - so this profile fits the pad TYPE that "
             "is plugged in now, and swapping pad types needs a rebind. Raw "
             "positional capture is the intended path for this family.%s",
             device.product == kProductPsSsDcJoypadBox
                 ? ", 'PS/SS/DC Joypad to USB BOX'"
                 : "",
             isMultiPortAdapterBox(device.vendor, device.product)
                 ? " This model has MULTIPLE PORTS that enumerate as "
                   "identical joysticks: use port 1 and leave the others "
                   "empty, or identity resolution will refuse to guess "
                   "between them."
                 : "");
    }
    return true;
}

void ControllerPreview::deselect() {
    if (backend_ != nullptr) backend_->close();
    selected_ = false;
    captureActive_ = false;
}

bool ControllerPreview::directionsMustBindIndividually() const {
    if (!selected_ || semantic()) return false;
    if (selection_.hatCount != 0) return false;
    if (selection_.axisCount >= 2) return false;
    return true;
}

bool ControllerPreview::semantic() const {
    return backend_ != nullptr && selected_ && backend_->semanticGamepad();
}

void ControllerPreview::applySelectionTo(InputProfile& profile) const {
    if (!selected_) return;
    profile.device.guid = selection_.guid;
    profile.device.vendor = selection_.vendor;
    profile.device.product = selection_.product;
    profile.device.version = selection_.version;
    profile.device.normalizedName = selection_.name;
    profile.device.pathFingerprint =
        selection_.hasPathFingerprint ? selection_.pathFingerprint : 0;
    profile.device.serialHash =
        selection_.hasSerial ? selection_.serialHash : 0;
    profile.mapping = selectionMappingLength_ > 0
        ? std::string(selectionMapping_, selectionMappingLength_)
        : std::string();
    profile.probeBackend = selectionUsedSdl_ ? "sdl" : "dinput8";
}

// Probe index space: semantic = [buttons 0..20][axes 21..26]; raw = [buttons 0..31][axes 32..39][hats 40..43].

namespace {

uint32_t buildProbeSetFor(bool semanticMode, const PreviewDevice& device,
                          BackendProbeRequest* out, uint32_t capacity);

static_assert(kPadButtonProbeCount + kPadAxisProbeCount <= kPreviewProbeSlots,
              "semantic probe window must fit the preview probe buffers");
static_assert(kRawButtons + kRawAxes + kRawHats <= kPreviewProbeSlots,
              "raw probe window must fit the preview probe buffers");

uint32_t buildProbeSet(bool semanticMode, BackendProbeRequest* out,
                       uint32_t capacity) {
    uint32_t n = 0;
    const auto add = [&](ProbeKind kind, uint32_t count) {
        for (uint32_t i = 0; i < count && n < capacity; ++i) {
            out[n].kind = kind;
            out[n++].token = static_cast<uint8_t>(i);
        }
    };
    if (semanticMode) {
        add(ProbeKind::GamepadButton, kPadButtonProbeCount);
        add(ProbeKind::GamepadAxis, kPadAxisProbeCount);
        return n;
    }
    add(ProbeKind::RawButton, kRawButtons);
    add(ProbeKind::RawAxis, kRawAxes);
    add(ProbeKind::RawHat, kRawHats);
    return n;
}

uint32_t buildProbeSetFor(bool semanticMode, const PreviewDevice& device,
                          BackendProbeRequest* out, uint32_t capacity) {
    if (semanticMode) return buildProbeSet(semanticMode, out, capacity);
    if (device.axisCount == 0 && device.buttonCount == 0
        && device.hatCount == 0) {
        return buildProbeSet(semanticMode, out, capacity);
    }
    uint32_t n = 0;
    const auto add = [&](ProbeKind kind, uint32_t count) {
        for (uint32_t i = 0; i < count && n < capacity; ++i) {
            out[n].kind = kind;
            out[n++].token = static_cast<uint8_t>(i);
        }
    };
    add(ProbeKind::RawHat, device.hatCount);
    add(ProbeKind::RawButton, device.buttonCount);
    add(ProbeKind::RawAxis, device.axisCount);
    return n;
}

const char* probeKindName(ProbeKind kind) {
    switch (kind) {
        case ProbeKind::GamepadButton: return "pad-button";
        case ProbeKind::GamepadAxis:   return "pad-axis";
        case ProbeKind::RawButton:     return "raw-button";
        case ProbeKind::RawAxis:       return "raw-axis";
        case ProbeKind::RawHat:        return "raw-hat";
        default: break;
    }
    return "unknown";
}

bool movementEligible(bool semanticMode, const BackendProbeRequest& probe,
                      bool individualDirections) {
    if (individualDirections && !semanticMode)
        return probe.kind == ProbeKind::RawButton;
    if (semanticMode) {
        if (probe.kind == ProbeKind::GamepadButton)
            return probe.token >= kPadDpadUp && probe.token <= kPadDpadRight;
        if (probe.kind == ProbeKind::GamepadAxis)
            return probe.token <= kAxisRightY;
        return false;
    }
    if (probe.kind == ProbeKind::RawHat) return true;
    if (probe.kind == ProbeKind::RawAxis) return probe.token < 6;
    return false;
}

ProfileBinding padButton(uint8_t token) {
    ProfileBinding binding{};
    binding.type = CompiledBindingType::ProbeButton;
    binding.probeKind = ProbeKind::GamepadButton;
    binding.token = token;
    return binding;
}

ProfileBinding padAxis(uint8_t token, AxisPolarity polarity) {
    ProfileBinding binding{};
    binding.type = CompiledBindingType::ProbeAxis;
    binding.probeKind = ProbeKind::GamepadAxis;
    binding.token = token;
    binding.polarity = polarity;
    return binding;
}

ProfileBinding rawAxis(uint8_t token, AxisPolarity polarity) {
    ProfileBinding binding{};
    binding.type = CompiledBindingType::ProbeAxis;
    binding.probeKind = ProbeKind::RawAxis;
    binding.token = token;
    binding.polarity = polarity;
    return binding;
}

ProfileBinding rawHat(uint8_t token, uint8_t bit) {
    ProfileBinding binding{};
    binding.type = CompiledBindingType::ProbeHat;
    binding.probeKind = ProbeKind::RawHat;
    binding.token = token;
    binding.hatBit = bit;
    return binding;
}

}

void ControllerPreview::beginCapture(bool movement) {
    captureActive_ = true;
    captureMovement_ = movement;
    captureSeeded_ = false;
    candidate_ = -1;
    candidateNegative_ = false;
    candidateHatBits_ = 0;
    std::memset(ineligible_, 0, sizeof(ineligible_));
}

ControllerPreview::Step ControllerPreview::stepCapture(
    ProfileBinding& single, std::string& singleLabel,
    MovementBindings& cluster) {
    if (!captureActive_ || backend_ == nullptr || !selected_)
        return Step::Pending;
    const bool semanticMode = semantic();
    const bool individualDirections = directionsMustBindIndividually();
    BackendProbeRequest requests[kPreviewProbeSlots];
    const uint32_t count = buildProbeSetFor(
        semanticMode, selection_, requests,
        static_cast<uint32_t>(sizeof(requests) / sizeof(requests[0])));
    uint32_t values[kPreviewProbeSlots] = {};
    if (!backend_->readProbes(requests, count, values)) {
        captureActive_ = false;
        return Step::Failed;
    }

    if (!captureSeeded_) {
        captureSeeded_ = true;
        unsigned held = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const bool axis = requests[i].kind == ProbeKind::GamepadAxis
                || requests[i].kind == ProbeKind::RawAxis;
            const bool active = axis
                ? axisMagnitude(static_cast<int32_t>(values[i]))
                      > kCaptureNeutralQ15
                : values[i] != 0;
            ineligible_[i] = active;
            observedActive_[i] = active;
            if (active) {
                QLOG("[controller.watch] HELD AT ENTRY %s index=%u value=%d "
                     "- ignored until it returns to rest",
                     probeKindName(requests[i].kind), requests[i].token,
                     static_cast<int>(values[i]));
                ++held;
            }
        }
        observeSeeded_ = true;
        QLOG("[controller.watch] capture armed mode=%s probes=%u "
             "movement=%d heldAtEntry=%u",
             semanticMode ? "semantic" : "raw", count,
             captureMovement_ ? 1 : 0, held);
        return Step::Pending;
    }

    if (observeSeeded_) {
        for (uint32_t i = 0; i < count; ++i) {
            const bool axisProbe = requests[i].kind == ProbeKind::GamepadAxis
                || requests[i].kind == ProbeKind::RawAxis;
            const int32_t value = static_cast<int32_t>(values[i]);
            const bool active = axisProbe
                ? axisMagnitude(value) >= kCaptureCandidateQ15
                : values[i] != 0;
            if (active == observedActive_[i]) continue;
            observedActive_[i] = active;
            QLOG("[controller.watch] %s %s index=%u value=%d%s",
                 active ? "DOWN" : "UP  ",
                 probeKindName(requests[i].kind), requests[i].token, value,
                 ineligible_[i] ? " (still ignored: held at entry)" : "");
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        const BackendProbeRequest& probe = requests[i];
        const int32_t raw = static_cast<int32_t>(values[i]);
        const bool axis = probe.kind == ProbeKind::GamepadAxis
            || probe.kind == ProbeKind::RawAxis;
        if (captureMovement_
            && !movementEligible(semanticMode, probe, individualDirections))
            continue;

        if (axis) {
            const int32_t magnitude = axisMagnitude(raw);
            if (ineligible_[i]) {
                if (magnitude <= kCaptureNeutralQ15) ineligible_[i] = false;
                continue;
            }
            if (candidate_ < 0 && magnitude >= kCaptureCandidateQ15) {
                candidate_ = static_cast<int>(i);
                candidateNegative_ = raw < 0;
                continue;
            }
            if (candidate_ != static_cast<int>(i)
                || magnitude > kCaptureNeutralQ15) {
                continue;
            }
            captureActive_ = false;
            const AxisPolarity polarity = candidateNegative_
                ? AxisPolarity::Negative : AxisPolarity::Positive;
            if (!captureMovement_ || individualDirections) {
                if (semanticMode) {
                    single = padAxis(probe.token, polarity);
                    singleLabel = padAxisLabel(probe.token, polarity);
                } else {
                    single = rawAxis(probe.token, polarity);
                    singleLabel = "Axis " + std::to_string(probe.token)
                        + (candidateNegative_ ? "-" : "+");
                }
                QLOG("[controller.bind] committed axis label='%s' mode=%s "
                     "token=%u polarity=%s",
                     singleLabel.c_str(), semanticMode ? "semantic" : "raw",
                     static_cast<unsigned>(probe.token),
                     polarity == AxisPolarity::Negative ? "-" : "+");
                return Step::Committed;
            }
            const uint8_t xToken = probe.token & ~1u;
            const uint8_t yToken = xToken + 1u;
            if (semanticMode) {
                cluster.up = padAxis(yToken, AxisPolarity::Negative);
                cluster.down = padAxis(yToken, AxisPolarity::Positive);
                cluster.left = padAxis(xToken, AxisPolarity::Negative);
                cluster.right = padAxis(xToken, AxisPolarity::Positive);
                cluster.label = xToken == kAxisLeftX ? "Left stick"
                                                     : "Right stick";
            } else {
                cluster.up = rawAxis(yToken, AxisPolarity::Negative);
                cluster.down = rawAxis(yToken, AxisPolarity::Positive);
                cluster.left = rawAxis(xToken, AxisPolarity::Negative);
                cluster.right = rawAxis(xToken, AxisPolarity::Positive);
                cluster.label = "Stick (axes "
                    + std::to_string(xToken) + "/"
                    + std::to_string(yToken) + ")";
            }
            QLOG("[controller.bind] committed movement label='%s' mode=%s "
                 "xToken=%u yToken=%u",
                 cluster.label.c_str(), semanticMode ? "semantic" : "raw",
                 static_cast<unsigned>(xToken),
                 static_cast<unsigned>(yToken));
            return Step::Committed;
        }

        const bool active = values[i] != 0;
        if (ineligible_[i]) {
            if (!active) ineligible_[i] = false;
            continue;
        }
        if (candidate_ < 0 && active) {
            candidate_ = static_cast<int>(i);
            candidateHatBits_ =
                probe.kind == ProbeKind::RawHat ? values[i] : 0;
            continue;
        }
        if (candidate_ != static_cast<int>(i) || active) continue;
        captureActive_ = false;
        if (!captureMovement_ || individualDirections) {
            if (probe.kind == ProbeKind::RawHat) {
                uint8_t bit = kHatUp;
                if (candidateHatBits_ & kHatUp) bit = kHatUp;
                else if (candidateHatBits_ & kHatDown) bit = kHatDown;
                else if (candidateHatBits_ & kHatLeft) bit = kHatLeft;
                else if (candidateHatBits_ & kHatRight) bit = kHatRight;
                single = rawHat(probe.token, bit);
            } else if (semanticMode) {
                single = padButton(probe.token);
            } else {
                ProfileBinding binding{};
                binding.type = CompiledBindingType::ProbeButton;
                binding.probeKind = ProbeKind::RawButton;
                binding.token = probe.token;
                single = binding;
            }
            singleLabel = controllerBindingLabel(single);
            QLOG("[controller.bind] committed action label='%s' mode=%s "
                 "probeKind=%u token=%u hatBits=%02X",
                 singleLabel.c_str(), semanticMode ? "semantic" : "raw",
                 static_cast<unsigned>(single.probeKind),
                 static_cast<unsigned>(single.token),
                 static_cast<unsigned>(candidateHatBits_));
            return Step::Committed;
        }
        if (probe.kind == ProbeKind::RawHat) {
            cluster.up = rawHat(probe.token, kHatUp);
            cluster.down = rawHat(probe.token, kHatDown);
            cluster.left = rawHat(probe.token, kHatLeft);
            cluster.right = rawHat(probe.token, kHatRight);
        } else {
            cluster.up = padButton(kPadDpadUp);
            cluster.down = padButton(kPadDpadDown);
            cluster.left = padButton(kPadDpadLeft);
            cluster.right = padButton(kPadDpadRight);
        }
        cluster.label = "D-pad";
        QLOG("[controller.bind] committed movement label='D-pad' mode=%s "
             "probeKind=%u token=%u",
             semanticMode ? "semantic" : "raw",
             static_cast<unsigned>(probe.kind),
             static_cast<unsigned>(probe.token));
        return Step::Committed;
    }
    return Step::Pending;
}

std::string controllerBindingLabel(const ProfileBinding& binding) {
    char text[48];
    switch (binding.type) {
        case CompiledBindingType::ProbeButton:
            if (binding.probeKind == ProbeKind::GamepadButton
                && binding.token < kPadButtonProbeCount) {
                return kPadButtonNames[binding.token];
            }
            std::snprintf(text, sizeof(text), "Button %u",
                          binding.token + 1u);
            return text;
        case CompiledBindingType::ProbeAxis:
            if (binding.probeKind == ProbeKind::GamepadAxis)
                return padAxisLabel(binding.token, binding.polarity);
            std::snprintf(text, sizeof(text), "Axis %u%c", binding.token,
                          binding.polarity == AxisPolarity::Negative
                              ? '-' : '+');
            return text;
        case CompiledBindingType::ProbeHat: {
            const char* name =
                binding.hatBit == kHatUp ? "Up"
                : binding.hatBit == kHatDown ? "Down"
                : binding.hatBit == kHatLeft ? "Left" : "Right";
            std::snprintf(text, sizeof(text), "D-pad %s", name);
            return text;
        }
        default:
            return std::string();
    }
}

bool ControllerPreview::indistinguishable(const PreviewDevice& a,
                                          const PreviewDevice& b) {
    if (a.hasSerial && b.hasSerial) return a.serialHash == b.serialHash;
    if (a.hasSerial != b.hasSerial) return false;
    if (a.hasPathFingerprint && b.hasPathFingerprint)
        return a.pathFingerprint == b.pathFingerprint;
    if (a.hasPathFingerprint != b.hasPathFingerprint) return false;
    return a.guid == b.guid && a.vendor == b.vendor
        && a.product == b.product && a.version == b.version
        && a.name == b.name;
}

bool ControllerPreview::beginIdentify(
    const std::vector<PreviewDevice>& tied) {
    endIdentify();
    if (tied.size() < 2) return false;
    deselect();
    for (size_t i = 0; i < tied.size(); ++i) {
        IdentifyLane lane{};
        lane.device = tied[i];
        lane.index = i;
        lane.backend = createControllerBackend(nullptr);
        if (lane.backend == nullptr) continue;
        if (!lane.backend->initialize()) {
            delete lane.backend;
            continue;
        }
        DeviceCandidate candidate{};
        candidate.liveHandle = tied[i].liveHandle;
        std::snprintf(candidate.guid, sizeof(candidate.guid), "%s",
                      tied[i].guid.c_str());
        candidate.vendor = tied[i].vendor;
        candidate.product = tied[i].product;
        candidate.version = tied[i].version;
        std::snprintf(candidate.normalizedName,
                      sizeof(candidate.normalizedName), "%s",
                      tied[i].name.c_str());
        if (!lane.backend->open(candidate, nullptr, 0)) {
            lane.backend->shutdown();
            delete lane.backend;
            continue;
        }
        lane.probeCount = buildProbeSetFor(
            lane.backend->semanticGamepad(), tied[i], lane.probes,
            static_cast<uint32_t>(sizeof(lane.probes)
                                  / sizeof(lane.probes[0])));
        identifyLanes_.push_back(lane);
    }
    if (identifyLanes_.size() < 2) {
        QLOG("[controller.identify] cannot identify: %zu of %zu tied "
             "candidates could be opened together",
             identifyLanes_.size(), tied.size());
        endIdentify();
        return false;
    }
    QLOG("[controller.identify] watching %zu identical candidates - waiting "
         "for the player to press one", identifyLanes_.size());
    return true;
}

ControllerPreview::Identify ControllerPreview::stepIdentify(
    size_t& winnerIndex) {
    if (identifyLanes_.empty()) return Identify::Failed;
    for (IdentifyLane& lane : identifyLanes_) {
        uint32_t values[kPreviewProbeSlots] = {};
        if (!lane.backend->readProbes(lane.probes, lane.probeCount, values)) {
            QLOG("[controller.identify] a candidate stopped responding");
            return Identify::Failed;
        }
        if (!lane.seeded) {
            lane.seeded = true;
            for (uint32_t i = 0; i < lane.probeCount; ++i) {
                const bool axis = lane.probes[i].kind == ProbeKind::GamepadAxis
                    || lane.probes[i].kind == ProbeKind::RawAxis;
                lane.ineligible[i] = axis
                    ? axisMagnitude(static_cast<int32_t>(values[i]))
                          > kCaptureNeutralQ15
                    : values[i] != 0;
            }
            continue;
        }
        for (uint32_t i = 0; i < lane.probeCount; ++i) {
            const bool axis = lane.probes[i].kind == ProbeKind::GamepadAxis
                || lane.probes[i].kind == ProbeKind::RawAxis;
            const int32_t raw = static_cast<int32_t>(values[i]);
            if (axis) {
                const int32_t magnitude = axisMagnitude(raw);
                if (lane.ineligible[i]) {
                    if (magnitude <= kCaptureNeutralQ15)
                        lane.ineligible[i] = false;
                    continue;
                }
                if (magnitude < kIdentifyAxisQ15) continue;
            } else {
                if (lane.ineligible[i]) {
                    if (values[i] == 0) lane.ineligible[i] = false;
                    continue;
                }
                if (values[i] == 0) continue;
            }
            winnerIndex = lane.index;
            QLOG("[controller.identify] identified by %s index=%u on "
                 "candidate #%zu (live=%u)",
                 probeKindName(lane.probes[i].kind), lane.probes[i].token,
                 lane.index + 1u, lane.device.liveHandle);
            return Identify::Identified;
        }
    }
    return Identify::Pending;
}

void ControllerPreview::endIdentify() {
    for (IdentifyLane& lane : identifyLanes_) {
        if (lane.backend == nullptr) continue;
        lane.backend->close();
        lane.backend->shutdown();
        delete lane.backend;
        lane.backend = nullptr;
    }
    identifyLanes_.clear();
}

}
