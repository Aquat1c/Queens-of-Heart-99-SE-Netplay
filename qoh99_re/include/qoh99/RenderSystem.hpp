#pragma once
#include "AddressMap.hpp"
#include "MemoryView.hpp"

namespace qoh99 {

class CameraSystem {
public:
    explicit CameraSystem(MemoryView& m) : m_(m) {}
    double zoomA() const { return m_.get<double>(addr::kCameraZoomA); }
    double zoomB() const { return m_.get<double>(addr::kCameraZoomB); }
    static constexpr GameAddr kVisualRegionBase = 0x49B530;
    static constexpr std::uint32_t kVisualRegionSize = 0x8;
    static constexpr GameAddr kControlRegionBase = 0x49B538;
    static constexpr std::uint32_t kControlRegionSize = 0xC;
private:
    MemoryView& m_;
};

class RenderCosmeticState {
public:
    explicit RenderCosmeticState(MemoryView& m) : m_(m) {}
    std::uint32_t cosmeticFrameCounter() const {
        return m_.get<std::uint32_t>(addr::kFrameCounterCosmetic);
    }
private:
    MemoryView& m_;
};

class RenderSystem {
public:
    explicit RenderSystem(MemoryView& m) : m_(m), camera_(m), cosmetic_(m) {}

    CameraSystem& camera() { return camera_; }
    RenderCosmeticState& cosmetic() { return cosmetic_; }

    bool appActive() const { return m_.get<int>(addr::kAppActiveFlag) != 0; }

    static constexpr int kInternalW = 320, kInternalH = 240;
    static constexpr int kPrimaryW  = 640, kPrimaryH  = 480;

private:
    MemoryView& m_;
    CameraSystem camera_;
    RenderCosmeticState cosmetic_;
};

}
