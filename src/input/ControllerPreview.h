#pragma once

#include <string>

#include "input/SdlLifecycle.h"
#include <vector>

#include "input/InputProfile.h"
#include "input/InputWorkers.h"

namespace qinput {

constexpr uint32_t kPreviewProbeSlots = 64;

struct PreviewDevice {
    uint32_t liveHandle = 0;
    std::string name;
    std::string guid;
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint16_t version = 0;
    uint64_t pathFingerprint = 0;
    bool hasPathFingerprint = false;
    bool hasSerial = false;
    uint64_t serialHash = 0;

    bool isGamepad = false;
    bool probeOpened = false;
    uint16_t axisCount = 0;
    uint16_t buttonCount = 0;
    uint16_t hatCount = 0;
    std::string probeError;

    std::string capabilitySummary() const;
    bool selectable() const {
        return probeOpened
            && (axisCount != 0 || buttonCount != 0 || hatCount != 0);
    }
};

struct MovementBindings {
    ProfileBinding up, down, left, right;
    std::string label;
};

const char* sdlBootstrapResultText(SdlBootstrapResult result);
std::string sdlVersionSummary();

class ControllerPreview {
public:
    const std::string& backendFailure() const { return backendFailure_; }
    SdlBootstrapResult lastBootstrap() const { return lastBootstrap_; }
    ~ControllerPreview();

    bool directionsMustBindIndividually() const;

    enum class Identify : uint8_t { Pending, Identified, Failed };
    bool beginIdentify(const std::vector<PreviewDevice>& tied);
    Identify stepIdentify(size_t& winnerIndex);
    void endIdentify();
    bool identifying() const { return !identifyLanes_.empty(); }

    static bool indistinguishable(const PreviewDevice& a,
                                  const PreviewDevice& b);

    bool open();
    void close();
    bool isOpen() const { return backend_ != nullptr; }
    const char* backendName() const;
    bool usingSdlBackend() const;

    std::vector<PreviewDevice> devices();

    bool select(const PreviewDevice& device, bool bindRaw = false,
                const char* savedMapping = nullptr,
                uint32_t savedMappingLength = 0);
    void deselect();
    bool hasSelection() const { return selected_; }
    const PreviewDevice& selection() const { return selection_; }
    bool semantic() const;

    void applySelectionTo(InputProfile& profile) const;

    enum class Step : uint8_t { Pending, Committed, Failed };

    void beginCapture(bool movement);
    Step stepCapture(ProfileBinding& single, std::string& singleLabel,
                     MovementBindings& cluster);

private:
    std::string backendFailure_;
    SdlBootstrapResult lastBootstrap_ = SdlBootstrapResult::NotRequired;

    IControllerBackend* backend_ = nullptr;
    bool selected_ = false;
    PreviewDevice selection_{};
    char selectionMapping_[1024] = {};
    bool selectionUsedSdl_ = false;
    uint32_t selectionMappingLength_ = 0;

    bool captureActive_ = false;
    bool captureMovement_ = false;
    bool captureSeeded_ = false;
    bool ineligible_[kPreviewProbeSlots] = {};
    bool observedActive_[kPreviewProbeSlots] = {};
    bool observeSeeded_ = false;

    struct IdentifyLane {
        IControllerBackend* backend = nullptr;
        PreviewDevice device{};
        size_t index = 0;
        uint32_t probeCount = 0;
        BackendProbeRequest probes[kPreviewProbeSlots] = {};
        bool ineligible[kPreviewProbeSlots] = {};
        bool seeded = false;
    };
    std::vector<IdentifyLane> identifyLanes_;
    int candidate_ = -1;
    bool candidateNegative_ = false;
    uint32_t candidateHatBits_ = 0;
};

std::string controllerBindingLabel(const ProfileBinding& binding);

}
