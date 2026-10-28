#pragma once
/**
 * Android helpers the POSIX backends use (threading_posix.cpp and memory_posix.cpp look up functions newer than
 * the minimum API level at run time).
 */

#include <android/api-level.h>

namespace rex {

// The API level of the device the app runs on (not the one it was built for).
inline int GetAndroidApiLevel() {
  return android_get_device_api_level();
}

}  // namespace rex
