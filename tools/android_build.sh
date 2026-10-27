#!/bin/bash
# Builds the Android test APK of NFSMW Recompiled: the native code with the NDK (CMake + Ninja into out/android),
# its shared libraries copied into out/android-jni, and the APK with Gradle (app/android).
#
#   tools/android_build.sh [configure] [native] [apk]   (no arguments: all three)
#
# Expects the Android SDK, NDK and a JDK 17 (see docs/android.md); the paths can be overridden with ANDROID_HOME,
# ANDROID_NDK, JAVA_HOME and GRADLE.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ANDROID_HOME="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
ANDROID_NDK="${ANDROID_NDK:-$ANDROID_HOME/ndk/27.2.12479018}"
JAVA_HOME="${JAVA_HOME:-$(ls -d "$LOCALAPPDATA"/Android/jdk/jdk-17* 2>/dev/null | head -1)}"
GRADLE="${GRADLE:-$(ls -d "$LOCALAPPDATA"/Android/gradle/gradle-*/bin/gradle.bat 2>/dev/null | head -1)}"
EDITION="${NFSMW_EDICION:-es}"
BUILD="$ROOT/out/android"
JNI="$ROOT/out/android-jni/arm64-v8a"
export ANDROID_HOME JAVA_HOME

steps=("$@")
[ ${#steps[@]} -eq 0 ] && steps=(configure native apk)

for step in "${steps[@]}"; do
  case "$step" in
    configure)
      cmake -S "$ROOT/app" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
        -DREXSDK_DIR="$ROOT/sdk" -DREXGLUE_USE_VULKAN=ON -DNFSMW_EDICION="$EDITION" \
        -DNFSMW_VERSION=1.0.2 -DREXGLUE_RECOMP_DEBUG_INFO=none
      ;;
    native)
      ninja -C "$BUILD" nfsmw
      # The game, the runtime and SDL3 (the last two come out in the SDK's own folder) and the
      # shared C++ library (ANDROID_STL=c++_shared), without debug symbols.
      rm -rf "$JNI" && mkdir -p "$JNI"
      cp "$BUILD/libmain.so" "$JNI/"
      # No Xenos GPU plugin: the game brings its own graphics system (the Vulkan 1.1 renderer).
      for lib in librexruntime.so libSDL3.so; do
        cp "$ROOT/sdk/out/linux-arm64/$lib" "$JNI/"
      done
      cp "$ANDROID_NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$JNI/"
      for lib in "$JNI"/*.so; do
        "$ANDROID_NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe" --strip-unneeded "$lib"
      done
      ls -la "$JNI"
      # The touch controls' icons: the game's own DDS images, which stay out of the repository. From NFSMW_BOTONES
      # (default: Documents/Buttons); without them the controls show text labels.
      BOTONES="${NFSMW_BOTONES:-$USERPROFILE/Documents/Buttons}"
      rm -rf "$ROOT/out/android-assets" && mkdir -p "$ROOT/out/android-assets/botones"
      if [ -d "$BOTONES" ]; then
        cp "$BOTONES"/*.dds "$ROOT/out/android-assets/botones/" 2>/dev/null || true
      fi
      ls "$ROOT/out/android-assets/botones"
      ;;
    apk)
      (cd "$ROOT/app/android" && cmd //c "$(cygpath -w "$GRADLE")" --no-daemon assembleRelease)
      ls -la "$ROOT/app/android/app/build/outputs/apk/release/"
      ;;
    *)
      echo "unknown step: $step" >&2
      exit 2
      ;;
  esac
done
