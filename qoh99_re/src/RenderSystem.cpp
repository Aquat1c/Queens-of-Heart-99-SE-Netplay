#include "qoh99/RenderSystem.hpp"

namespace qoh99 {

static_assert(CameraSystem::kVisualRegionBase <= addr::kCameraZoomA &&
              addr::kCameraZoomA + 8 <=
                  CameraSystem::kVisualRegionBase + CameraSystem::kVisualRegionSize,
              "camera_zoom_visual must cover the render-only zoom double");
static_assert(CameraSystem::kControlRegionBase <= addr::kCameraZoomB &&
              addr::kCameraZoomB + 8 <=
                  CameraSystem::kControlRegionBase + CameraSystem::kControlRegionSize,
              "round_intro_control must cover the control-threshold zoom double");

}
