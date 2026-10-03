#include "android_storage.h"
#include <android/log.h>
#include <vector>
#include <algorithm>
#include <fstream>

#define TAG "NFS-AndroidStorage"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace rex::vfs::android {

std::filesystem::path AndroidStorage::internal_path_{};
std::filesystem::path AndroidStorage::external_path_{};
std::filesystem::path AndroidStorage::media_path_{};

void AndroidStorage::Initialize(const std::string& internal_data_path,
                                const std::string& external_data_path) {
  internal_path_ = internal_data_path;
  external_path_ = external_data_path;

  // Derive Android/media directory from external path:
  // e.g. /storage/emulated/0/Android/data/<pkg>/files -> /storage/emulated/0/Android/media/<pkg>
  media_path_.clear();
  if (!external_path_.empty()) {
    std::string str = external_path_.string();
    const std::string marker = "/Android/data/";
    size_t marker_pos = str.find(marker);
    if (marker_pos != std::string::npos) {
      size_t pkg_begin = marker_pos + marker.size();
      size_t pkg_end = str.find('/', pkg_begin);
      std::string pkg = (pkg_end == std::string::npos)
          ? str.substr(pkg_begin)
          : str.substr(pkg_begin, pkg_end - pkg_begin);
      if (!pkg.empty()) {
        media_path_ = std::filesystem::path(str.substr(0, marker_pos)) / "Android" / "media" / pkg;
      }
    }
  }

  std::error_code ec;
  if (!media_path_.empty()) {
    std::filesystem::create_directories(media_path_, ec);
    std::filesystem::create_directories(media_path_ / "driver_import", ec);
    std::filesystem::create_directories(media_path_ / "turnip", ec);
  }
  if (!external_path_.empty()) {
    std::filesystem::create_directories(external_path_ / "driver_import", ec);
    std::filesystem::create_directories(external_path_ / "turnip", ec);
  }

  LOGI("AndroidStorage initialized: internal=%s, external=%s, media=%s",
       internal_path_.c_str(), external_path_.c_str(), media_path_.c_str());
}

std::filesystem::path AndroidStorage::GetInternalPath() {
  return internal_path_;
}

std::filesystem::path AndroidStorage::GetExternalPath() {
  return external_path_;
}

std::filesystem::path AndroidStorage::GetMediaPath() {
  return media_path_;
}

std::filesystem::path AndroidStorage::FindGameDataRoot() {
  std::error_code ec;

  // 0. Check if a custom path was configured by the user via TitleActivity
  const std::filesystem::path config_files[] = {
      media_path_ / "selected_game_path.txt",
      external_path_ / "selected_game_path.txt",
      internal_path_ / "selected_game_path.txt"
  };

  for (const auto& cfg : config_files) {
    if (std::filesystem::is_regular_file(cfg, ec)) {
      std::ifstream f(cfg);
      std::string line;
      if (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
          line.pop_back();
        }
        if (!line.empty()) {
          std::filesystem::path chosen_path(line);
          if (std::filesystem::exists(chosen_path, ec)) {
            LOGI("Found user-selected game data path: %s", chosen_path.c_str());
            return chosen_path;
          }
        }
      }
    }
  }

  std::vector<std::filesystem::path> search_dirs = {
      media_path_,
      media_path_ / "NFSMW",
      external_path_,
      internal_path_,
      "/sdcard/NFSMW",
      "/sdcard/Download/NFSMW",
      "/storage/emulated/0/NFSMW"
  };

  for (const auto& dir : search_dirs) {
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
      continue;
    }

    // 1. Look for extracted game_root folder
    auto game_root = dir / "game_root";
    if (std::filesystem::is_directory(game_root, ec)) {
      LOGI("Found extracted game_root at %s", game_root.c_str());
      return game_root;
    }

    // 2. Look for ISO files
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) break;
      if (!entry.is_regular_file(ec)) continue;

      std::string ext = entry.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      if (ext == ".iso") {
        LOGI("Found game ISO at %s", entry.path().c_str());
        return entry.path();
      }
    }
  }

  LOGW("No game data found in searched directories. Defaulting to external path.");
  return external_path_ / "game_root";
}

void AndroidStorage::ConfigureAppPaths(rex::PathConfig& paths) {
  std::error_code ec;

  if (paths.game_data_root.empty()) {
    paths.game_data_root = FindGameDataRoot();
  }

  // Create user, cache, and save folders
  std::filesystem::path base_user = external_path_.empty() ? internal_path_ : external_path_;

  paths.user_data_root = base_user / "user";
  paths.cache_root = base_user / "cache";
  paths.metadata_root = base_user / "metadata";
  paths.update_data_root = base_user / "updates";
  paths.config_path = base_user / "nfsmw.toml";

  std::filesystem::create_directories(paths.user_data_root, ec);
  std::filesystem::create_directories(paths.cache_root, ec);
  std::filesystem::create_directories(paths.metadata_root, ec);
  std::filesystem::create_directories(paths.update_data_root, ec);

  LOGI("Paths configured:\n  Game: %s\n  User: %s\n  Cache: %s",
       paths.game_data_root.c_str(),
       paths.user_data_root.c_str(),
       paths.cache_root.c_str());
}

}  // namespace rex::vfs::android
