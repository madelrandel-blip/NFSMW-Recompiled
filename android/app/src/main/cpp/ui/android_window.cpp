#include "android_window.h"
#include "android_app_context.h"
#include <android/log.h>
#include <android/input.h>
#include <imgui.h>
#include <rex/platform/android/android_bridge.h>

#define TAG "NFS-AndroidWindow"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace rex::ui {

static AndroidWindow* g_active_android_window = nullptr;

AndroidWindow::AndroidWindow(WindowedAppContext& app_context,
                             const std::string_view title,
                             uint32_t desired_logical_width,
                             uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {
  g_active_android_window = this;

  auto* android_ctx = dynamic_cast<AndroidWindowedAppContext*>(&app_context);
  if (android_ctx) {
    native_window_ = android_ctx->GetNativeWindow();
  }
  LOGI("AndroidWindow created (desired %ux%u)", desired_logical_width, desired_logical_height);
}

AndroidWindow::~AndroidWindow() {
  EnterDestructor();
  if (g_active_android_window == this) {
    g_active_android_window = nullptr;
  }
  LOGI("AndroidWindow destroyed");
}

void AndroidWindow::AttachNativeWindow(ANativeWindow* window) {
  native_window_ = window;
  is_attached_ = (window != nullptr);
  rex::platform::android::AndroidBridge::SetNativeWindow(window);

  if (native_window_) {
    int32_t w = ANativeWindow_getWidth(native_window_);
    int32_t h = ANativeWindow_getHeight(native_window_);
    if (w > 0 && h > 0) {
      ANativeWindow_setBuffersGeometry(native_window_, w, h, WINDOW_FORMAT_RGBA_8888);
      UpdateDimensions(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    }
  }

  OnSurfaceChanged(native_window_ != nullptr);
}

void AndroidWindow::DetachNativeWindow() {
  native_window_ = nullptr;
  is_attached_ = false;
  rex::platform::android::AndroidBridge::SetNativeWindow(nullptr);
  OnSurfaceChanged(false);
}

void AndroidWindow::UpdateDimensions(uint32_t width, uint32_t height) {
  WindowDestructionReceiver receiver(this);
  OnActualSizeUpdate(width, height, receiver);
  LOGI("AndroidWindow dimensions updated: %ux%u", width, height);
}

bool AndroidWindow::OpenImpl() {
  LOGI("AndroidWindow::OpenImpl called");

  uint32_t width = 1920;
  uint32_t height = 1080;

  if (native_window_) {
    int32_t w = ANativeWindow_getWidth(native_window_);
    int32_t h = ANativeWindow_getHeight(native_window_);
    if (w > 0 && h > 0) {
      width = static_cast<uint32_t>(w);
      height = static_cast<uint32_t>(h);
    }
  }

  WindowDestructionReceiver receiver(this);
  OnDesiredFullscreenUpdate(true);
  OnActualSizeUpdate(width, height, receiver);
  OnFocusUpdate(true, receiver);
  OnSurfaceChanged(native_window_ != nullptr);

  return true;
}

void AndroidWindow::RequestCloseImpl() {
  LOGI("AndroidWindow::RequestCloseImpl called");
  WindowDestructionReceiver receiver(this);
  if (SendCloseRequestToListeners(receiver) && !receiver.IsWindowDestroyed()) {
    OnBeforeClose(receiver);
    if (!receiver.IsWindowDestroyed()) {
      OnAfterClose();
    }
  }
}

std::unique_ptr<Surface> AndroidWindow::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!(allowed_types & Surface::kTypeFlag_AndroidNativeWindow)) {
    LOGE("Surface creation requested but kTypeFlag_AndroidNativeWindow not allowed (flags=0x%08X)", allowed_types);
    return nullptr;
  }

  if (!native_window_) {
    LOGE("CreateSurfaceImpl failed: ANativeWindow is null");
    return nullptr;
  }

  LOGI("Creating AndroidNativeWindowSurface with ANativeWindow %p", native_window_);
  return std::make_unique<AndroidNativeWindowSurface>(native_window_);
}

AndroidWindow* AndroidWindow::GetActiveWindow() {
  return g_active_android_window;
}

void AndroidWindow::PaintFrame() {
  UpdateSoftKeyboard();
  OnPaint(false);
}

void AndroidWindow::DispatchTouchEvent(uint32_t pointer_id, TouchEvent::Action action,
                                       float x, float y) {
  WindowDestructionReceiver receiver(this);
  TouchEvent e(this, pointer_id, action, x, y);
  OnTouchEvent(e, receiver);
}

void AndroidWindow::DispatchKeyChar(uint32_t codepoint) {
  WindowDestructionReceiver receiver(this);
  KeyEvent e(this, VirtualKey(codepoint), /*repeat_count=*/1,
             /*prev_state=*/false,
             /*modifier_shift_pressed=*/false, /*modifier_ctrl_pressed=*/false,
             /*modifier_alt_pressed=*/false, /*modifier_super_pressed=*/false);
  OnKeyChar(e, receiver);
}

void AndroidWindow::DispatchKey(VirtualKey virtual_key, bool is_down) {
  WindowDestructionReceiver receiver(this);
  KeyEvent e(this, virtual_key, /*repeat_count=*/1,
             /*prev_state=*/!is_down,
             /*modifier_shift_pressed=*/false, /*modifier_ctrl_pressed=*/false,
             /*modifier_alt_pressed=*/false, /*modifier_super_pressed=*/false);
  if (is_down) {
    OnKeyDown(e, receiver);
  } else {
    OnKeyUp(e, receiver);
  }
}

void AndroidWindow::UpdateSoftKeyboard() {
  if (!ImGui::GetCurrentContext()) {
    return;
  }
  const bool want = ImGui::GetIO().WantTextInput;
  if (want == soft_keyboard_visible_) {
    return;
  }

  auto* android_ctx = dynamic_cast<AndroidWindowedAppContext*>(&app_context());
  if (!android_ctx || !android_ctx->app() || !android_ctx->app()->activity) {
    return;
  }

  if (want) {
    ANativeActivity_showSoftInput(android_ctx->app()->activity,
                                  ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
  } else {
    ANativeActivity_hideSoftInput(android_ctx->app()->activity,
                                  ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
  }
  soft_keyboard_visible_ = want;
}

void AndroidWindow::RequestPaintImpl() {
  static_cast<AndroidWindowedAppContext&>(app_context()).NotifyUILoopOfPendingFunctions();
}

void AndroidWindow::ApplyNewFullscreen() {
  // Android activities are always fullscreen in immersive mode.
  OnDesiredFullscreenUpdate(true);
}

void AndroidWindow::ApplyNewTitle() {
  // Titles are managed by the Android Manifest/Activity.
}

// Global factory override for ReXGlue Window creation on Android
std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                      const std::string_view title,
                                      uint32_t desired_logical_width,
                                      uint32_t desired_logical_height) {
  return std::make_unique<AndroidWindow>(app_context, title, desired_logical_width, desired_logical_height);
}

}  // namespace rex::ui
