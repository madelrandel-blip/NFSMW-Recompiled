/**
 * @file        vulkan_driver_loader.h
 * @brief       Custom Vulkan driver loader via libadrenotools (Turnip/Mesa support)
 *
 * Attempts to replace the system libvulkan.so with the Mesa Turnip open-source
 * Adreno driver before the ReXGlue SDK initialises Vulkan. This gives us:
 *
 *   - Vulkan 1.3/1.4 on devices whose stock driver only reports 1.1
 *   - Full shaderInt64 / VK_KHR_buffer_device_address support
 *   - A single validated driver path instead of per-OEM driver quirks
 *   - Automatic Xiaomi HyperOS 3+ UBWC interop fix (FD_DEV_FEATURES)
 *   - Multi-path driver provisioning (Android/media and Android/data support)
 *
 * Usage:
 *   Call TryLoadCustomVulkanDriver() once, as early as possible in
 *   android_main(), before rex::cvar::Init() and before any Vulkan call.
 *
 * Driver installation:
 *   Place the Turnip driver SO (e.g. libvulkan_freedreno.so) inside:
 *       <mediaDataPath>/driver_import/libvulkan_freedreno.so
 *       or <externalDataPath>/driver_import/libvulkan_freedreno.so
 *       or <internalDataPath>/turnip/libvulkan_freedreno.so
 *
 *   The app automatically stages drivers from media or external storage into
 *   internal storage (which is required by the dynamic linker) and executes them.
 *
 * Fallback:
 *   If libadrenotools.so is not found in nativeLibraryDir, or if the custom
 *   driver directory/file is absent, the function returns false and the stock
 *   system driver is used normally. No crash, no error — just a log line.
 *
 * @copyright   BSD-2-Clause (libadrenotools itself is BSD-2-Clause, Billy Laws)
 *              Project-specific glue: same licence as the rest of NFSMW-RECOMP.
 */

#pragma once

#include <android/log.h>
#include <android_native_app_glue.h>
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/system_properties.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

#define ATAG "NFS-VkLoader"
#define ATLOG(...) __android_log_print(ANDROID_LOG_INFO,  ATAG, __VA_ARGS__)
#define ATERR(...) __android_log_print(ANDROID_LOG_ERROR, ATAG, __VA_ARGS__)
#define ATWRN(...) __android_log_print(ANDROID_LOG_WARN,  ATAG, __VA_ARGS__)

// ──────────────────────────────────────────────────────────────────────────────
// libadrenotools C API surface we actually use
// Declared here so we don't need the full adrenotools headers at build time
// when the lib is absent (header-only stub path).
// ──────────────────────────────────────────────────────────────────────────────
extern "C" {
// adrenotools feature flags (from include/adrenotools/priv.h)
enum {
    ADRENOTOOLS_DRIVER_CUSTOM           = 1 << 0,
    ADRENOTOOLS_DRIVER_FILE_REDIRECT    = 1 << 1,
    ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT = 1 << 2,
};

// adrenotools_open_libvulkan signature
using PFN_adrenotools_open_libvulkan = void* (*)(
    int       dlopenMode,
    int       featureFlags,
    const char* tmpLibDir,
    const char* hookLibDir,
    const char* customDriverDir,
    const char* customDriverName,
    const char* fileRedirectDir);
}  // extern "C"

namespace nfsmw::android {

// Subdirectory inside internalDataPath where the Turnip SO is placed and executed.
static constexpr const char* kTurnipSubdir      = "turnip";
static constexpr const char* kTurnipDriverName  = "libvulkan_freedreno.so";

/**
 * @brief Checks whether a file exists and is readable.
 */
static inline bool FileExists(const std::string& path) {
    struct stat st{};
    return (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode));
}

/**
 * @brief Reads a trimmed single line or short text from a file.
 */
static inline bool ReadTrimmedTextFile(const std::filesystem::path& path, char* buffer, size_t buffer_size) {
    if (!FileExists(path.string())) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    size_t n = fread(buffer, 1, buffer_size - 1, f);
    fclose(f);
    while (n > 0 && (buffer[n - 1] == '\r' || buffer[n - 1] == '\n' || buffer[n - 1] == ' ' || buffer[n - 1] == '\t')) {
        buffer[--n] = '\0';
    }
    buffer[n] = '\0';
    return (n > 0);
}

/**
 * @brief Mesa feature flags via FD_DEV_FEATURES. Xiaomi HyperOS 3 ships a display stack whose
 * composer misreads Turnip's UBWC layout, producing gameplay-impairing glitches; Mesa's
 * enable_tp_ubwc_flag_hint feature fixes the interop, so it is applied automatically on
 * HyperOS 3+. A driver_import/fd_dev_features.txt overrides the value verbatim.
 */
inline void ApplyFdDevFeatures(const std::filesystem::path& media_dir,
                               const std::filesystem::path& external_dir,
                               const std::filesystem::path& internal_dir) {
    char buffer[256]{};

    const std::filesystem::path search_dirs[] = {
        media_dir / "driver_import",
        external_dir / "driver_import",
        internal_dir / "driver_import",
        media_dir / "turnip",
        external_dir / "turnip",
    };

    for (const auto& dir : search_dirs) {
        if (!dir.empty() && ReadTrimmedTextFile(dir / "fd_dev_features.txt", buffer, sizeof(buffer))) {
            setenv("FD_DEV_FEATURES", buffer, 1);
            ATLOG("Applied FD_DEV_FEATURES override from %s: \"%s\"", (dir / "fd_dev_features.txt").c_str(), buffer);
            return;
        }
    }

    // HyperOS exposes version through ro.mi.os.version.name (e.g. "OS3.0")
    char os_version[PROP_VALUE_MAX]{};
    __system_property_get("ro.mi.os.version.name", os_version);
    if (os_version[0] != '\0') {
        const char* digits = os_version;
        while (*digits != '\0' && (*digits < '0' || *digits > '9')) {
            digits++;
        }
        if (atoi(digits) >= 3) {
            setenv("FD_DEV_FEATURES", "enable_tp_ubwc_flag_hint=1", 1);
            ATLOG("HyperOS %s detected: setenv FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1 (UBWC interop fix)", os_version);
        }
    }
}

/**
 * @brief Checks for diagnostic TU_DEBUG override in driver_import/tu_debug.txt.
 */
inline void ApplyTuDebugOverride(const std::filesystem::path& media_dir,
                                 const std::filesystem::path& external_dir,
                                 const std::filesystem::path& internal_dir) {
    char buffer[256]{};
    const std::filesystem::path search_dirs[] = {
        media_dir / "driver_import",
        external_dir / "driver_import",
        internal_dir / "driver_import",
    };

    for (const auto& dir : search_dirs) {
        if (!dir.empty() && ReadTrimmedTextFile(dir / "tu_debug.txt", buffer, sizeof(buffer))) {
            setenv("TU_DEBUG", buffer, 1);
            ATLOG("Applied diagnostic TU_DEBUG override from %s: \"%s\"", (dir / "tu_debug.txt").c_str(), buffer);
            return;
        }
    }
}

/**
 * @brief GPU family detection.
 */
enum class GpuFamily {
    Unknown,
    Adreno,
    Xclipse,
    Mali,
    Other
};

inline GpuFamily DetectGpuFamily(std::string& description) {
    char vulkan_prop[PROP_VALUE_MAX]{};
    char egl_prop[PROP_VALUE_MAX]{};
    __system_property_get("ro.hardware.vulkan", vulkan_prop);
    __system_property_get("ro.hardware.egl", egl_prop);

    description.clear();
    if (vulkan_prop[0] != '\0') {
        description += vulkan_prop;
    }
    if (egl_prop[0] != '\0' && strcmp(vulkan_prop, egl_prop) != 0) {
        if (!description.empty()) {
            description += "/";
        }
        description += egl_prop;
    }

    std::string lowered = description;
    for (char& c : lowered) {
        c = char(tolower(static_cast<unsigned char>(c)));
    }

    if (lowered.empty()) {
        return GpuFamily::Unknown;
    }
    if (lowered.find("adreno") != std::string::npos || lowered.find("qcom") != std::string::npos || lowered.find("freedreno") != std::string::npos) {
        return GpuFamily::Adreno;
    }
    if (lowered.find("samsung") != std::string::npos || lowered.find("sgpu") != std::string::npos || lowered.find("xclipse") != std::string::npos) {
        return GpuFamily::Xclipse;
    }
    if (lowered.find("mali") != std::string::npos || lowered.find("immortalis") != std::string::npos) {
        return GpuFamily::Mali;
    }
    return GpuFamily::Other;
}

/**
 * @brief Retrieves the native library directory of the running process.
 */
static std::string GetNativeLibraryDir(void* adrenotools_handle) {
#if __ANDROID_API__ >= 21
    Dl_info info{};
    if (dladdr(adrenotools_handle, &info) && info.dli_fname) {
        std::string so_path = info.dli_fname;
        auto slash = so_path.rfind('/');
        if (slash != std::string::npos) {
            return so_path.substr(0, slash);
        }
    }
#endif
    return {};
}

/**
 * @brief Tries to load the custom Turnip Vulkan driver via libadrenotools.
 *
 * @param state  android_app* from android_main.
 * @return true  if Turnip was successfully loaded.
 * @return false if adrenotools is absent, driver files are missing, or load failed.
 */
inline bool TryLoadCustomVulkanDriver(struct android_app* state) {
    if (!state || !state->activity) {
        ATERR("TryLoadCustomVulkanDriver: null android_app, skipping");
        return false;
    }

    const char* internal_path_cstr =
        state->activity->internalDataPath ? state->activity->internalDataPath : "";
    const char* external_path_cstr =
        state->activity->externalDataPath ? state->activity->externalDataPath : "";

    if (internal_path_cstr[0] == '\0') {
        ATWRN("TryLoadCustomVulkanDriver: internalDataPath is empty, skipping");
        return false;
    }

    std::filesystem::path internal_path(internal_path_cstr);
    std::filesystem::path external_path(external_path_cstr);
    std::filesystem::path media_path{};

    if (!external_path.empty()) {
        std::string str = external_path.string();
        const std::string marker = "/Android/data/";
        size_t marker_pos = str.find(marker);
        if (marker_pos != std::string::npos) {
            size_t pkg_begin = marker_pos + marker.size();
            size_t pkg_end = str.find('/', pkg_begin);
            std::string pkg = (pkg_end == std::string::npos)
                ? str.substr(pkg_begin)
                : str.substr(pkg_begin, pkg_end - pkg_begin);
            if (!pkg.empty()) {
                media_path = std::filesystem::path(str.substr(0, marker_pos)) / "Android" / "media" / pkg;
            }
        }
    }

    // ── Apply HyperOS 3 UBWC fix & TU_DEBUG overrides ───────────────────────────
    ApplyFdDevFeatures(media_path, external_path, internal_path);
    ApplyTuDebugOverride(media_path, external_path, internal_path);

    // ── GPU Family Detection ──────────────────────────────────────────────────
    std::string gpu_desc;
    GpuFamily gpu_family = DetectGpuFamily(gpu_desc);
    ATLOG("Detected GPU hardware: \"%s\"", gpu_desc.c_str());

    std::filesystem::path target_turnip_dir = internal_path / kTurnipSubdir;
    std::error_code ec;
    std::filesystem::create_directories(target_turnip_dir, ec);

    // ── Check for driver files in accessible external/media storage ─────────────
    // Android cannot execute shared libraries directly from external storage
    // (mounted with noexec). We copy the driver into internal storage if updated.
    std::string driver_filename = kTurnipDriverName;
    const std::filesystem::path search_candidates[] = {
        media_path / "driver_import" / kTurnipDriverName,
        media_path / "turnip" / kTurnipDriverName,
        external_path / "driver_import" / kTurnipDriverName,
        external_path / "turnip" / kTurnipDriverName,
    };

    for (const auto& src : search_candidates) {
        if (!src.empty() && FileExists(src.string())) {
            std::filesystem::path dst = target_turnip_dir / kTurnipDriverName;
            // Only copy if size differs or destination does not exist
            bool needs_copy = !FileExists(dst.string());
            if (!needs_copy) {
                struct stat st_src{}, st_dst{};
                if (stat(src.c_str(), &st_src) == 0 && stat(dst.c_str(), &st_dst) == 0) {
                    if (st_src.st_size != st_dst.st_size || st_src.st_mtime > st_dst.st_mtime) {
                        needs_copy = true;
                    }
                }
            }
            if (needs_copy) {
                ATLOG("Staging custom driver from %s to %s", src.c_str(), dst.c_str());
                std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
                chmod(dst.c_str(), 0755);
            }
            break;
        }
    }

    std::string driver_path = (target_turnip_dir / driver_filename).string();

    if (!FileExists(driver_path)) {
        ATLOG("Turnip driver not installed at '%s' — using system Vulkan driver", driver_path.c_str());
        return false;
    }

    if (gpu_family != GpuFamily::Adreno && gpu_family != GpuFamily::Unknown) {
        ATWRN("Custom Turnip driver found, but non-Adreno GPU (\"%s\") detected. Turnip is Adreno-only!", gpu_desc.c_str());
    }

    ATLOG("Turnip driver found at: %s", driver_path.c_str());

    // ── 2. Load libadrenotools.so ─────────────────────────────────────────────
    void* adrenotools = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
    if (!adrenotools) {
        ATWRN("libadrenotools.so not found (%s) — using stock Vulkan driver", dlerror());
        return false;
    }

    auto open_libvulkan = reinterpret_cast<PFN_adrenotools_open_libvulkan>(
        dlsym(adrenotools, "adrenotools_open_libvulkan"));
    if (!open_libvulkan) {
        ATERR("adrenotools_open_libvulkan symbol missing (%s)", dlerror());
        dlclose(adrenotools);
        return false;
    }

    // ── 3. Resolve hookLibDir ─────────────────────────────────────────────────
    std::string hook_lib_dir = GetNativeLibraryDir(adrenotools);
    if (hook_lib_dir.empty()) {
        ATERR("Could not determine nativeLibraryDir — skipping Turnip load");
        dlclose(adrenotools);
        return false;
    }

    ATLOG("hookLibDir resolved: %s", hook_lib_dir.c_str());
    ATLOG("customDriverDir:     %s", target_turnip_dir.c_str());
    ATLOG("customDriverName:    %s", driver_filename.c_str());

    // ── 4. Call adrenotools_open_libvulkan ────────────────────────────────────
    void* custom_vulkan = open_libvulkan(
        RTLD_NOW | RTLD_LOCAL,              // dlopenMode
        ADRENOTOOLS_DRIVER_CUSTOM,          // featureFlags
        nullptr,                            // tmpLibDir (not needed on API ≥29)
        hook_lib_dir.c_str(),               // hookLibDir = nativeLibraryDir
        target_turnip_dir.c_str(),          // customDriverDir
        driver_filename.c_str(),            // customDriverName
        nullptr                             // fileRedirectDir
    );

    if (!custom_vulkan) {
        ATERR("adrenotools_open_libvulkan returned null — falling back to stock driver");
        dlclose(adrenotools);
        return false;
    }

    // ── 5. Verify vkGetInstanceProcAddr ──────────────────────────────────────
    auto vkGIPA = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(custom_vulkan, "vkGetInstanceProcAddr"));
    if (!vkGIPA) {
        ATERR("Turnip SO is missing vkGetInstanceProcAddr — not a valid Vulkan driver");
        dlclose(custom_vulkan);
        dlclose(adrenotools);
        return false;
    }

    ATLOG("Turnip/Mesa driver loaded successfully. vkGetInstanceProcAddr @ %p", vkGIPA);
    ATLOG("Stock libvulkan.so is now intercepted by Turnip.");

    return true;
}

}  // namespace nfsmw::android

#undef ATAG
#undef ATLOG
#undef ATERR
#undef ATWRN
