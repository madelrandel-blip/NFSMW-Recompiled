#pragma once

#include <rex/ui/window.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/surface_android.h>
#include <android/native_window.h>

namespace rex::ui {

class AndroidWindowedAppContext;

class AndroidWindow final : public Window {
 public:
  AndroidWindow(WindowedAppContext& app_context,
                const std::string_view title,
                uint32_t desired_logical_width,
                uint32_t desired_logical_height);
  ~AndroidWindow() override;

  void AttachNativeWindow(ANativeWindow* window);
  void DetachNativeWindow();
  void UpdateDimensions(uint32_t width, uint32_t height);
  void PaintFrame();

  // Touch/keyboard injection from the Android input driver. The base Window
  // dispatchers are protected; the driver only knows this window, so these
  // public wrappers forward events to the listeners (ImGuiDrawer included).
  void DispatchTouchEvent(uint32_t pointer_id, TouchEvent::Action action, float x, float y);
  void DispatchKeyChar(uint32_t codepoint);
  void DispatchKey(VirtualKey virtual_key, bool is_down);

  // Shows/hides the Android soft keyboard following ImGui's WantTextInput,
  // so ImGui InputText fields (XamShowKeyboardUI, in-game menus) can be used
  // on touch-only devices.
  void UpdateSoftKeyboard();

  static AndroidWindow* GetActiveWindow();

  ANativeWindow* native_window() const { return native_window_; }
  void* GetNativeWindowHandle() const override { return native_window_; }

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

  void ApplyNewFullscreen() override;
  void ApplyNewTitle() override;

 private:
  ANativeWindow* native_window_ = nullptr;
  bool is_attached_ = false;
  bool soft_keyboard_visible_ = false;
};

}  // namespace rex::ui
