/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>

#include <rex/filesystem.h>

#include <cstdlib>
#include <string>
#include <string_view>

namespace rex {
namespace filesystem {

bool CreateParentFolder(const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    auto parent_path = path.parent_path();
    if (!std::filesystem::exists(parent_path)) {
      return std::filesystem::create_directories(parent_path);
    }
  }
  return true;
}

int DescriptorFromProcPath(const std::filesystem::path& path) {
  constexpr std::string_view kPrefix = "/proc/self/fd/";
  const std::string text = path.generic_string();
  if (text.size() <= kPrefix.size() || text.compare(0, kPrefix.size(), kPrefix) != 0) {
    return -1;
  }
  char* end = nullptr;
  const long n = std::strtol(text.c_str() + kPrefix.size(), &end, 10);
  return end && *end == '\0' && n >= 0 ? int(n) : -1;
}

bool IsFileOrHeldDescriptor(const std::filesystem::path& path) {
  if (DescriptorFromProcPath(path) >= 0) {
    return true;
  }
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

}  // namespace filesystem
}  // namespace rex
