#pragma once
/**
 * Android surface: the ANativeWindow SDL3 creates for its SDLActivity, presented through VK_KHR_android_surface
 * (rex/ui/vulkan/vulkan_presenter.cpp, kTypeIndex_AndroidNativeWindow).
 */

#include <cstdint>

#include <android/native_window.h>

#include <rex/ui/surface.h>

namespace rex {
namespace ui {

class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window) : window_(window) {}
  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
  ANativeWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override {
    if (!window_) {
      return false;
    }
    const int32_t width = ANativeWindow_getWidth(window_);
    const int32_t height = ANativeWindow_getHeight(window_);
    if (width <= 0 || height <= 0) {
      return false;
    }
    width_out = uint32_t(width);
    height_out = uint32_t(height);
    return true;
  }

 private:
  ANativeWindow* window_;
};

}  // namespace ui
}  // namespace rex
