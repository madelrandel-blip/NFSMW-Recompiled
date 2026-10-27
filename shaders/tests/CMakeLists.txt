# Checks for the direct SPIR-V translator (XenosRecomp/spirv_recompiler.cpp). A standalone project: it builds in a
# few minutes, without the game's generated code.
#
#   cmake -S shaders/tests -B out/spirv-check -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
#   cmake --build out/spirv-check
#   out/spirv-check/nfsmw_spirv_check <corpus folder>
#
# A corpus folder holds pairs NNN_<v|p>_<fingerprint>.cont / .spv: the original 2005 containers and the SPIR-V DXC
# made from them, as extracted from a nfsmw_shaders*.nfsp library built by the HLSL path. The Vulkan SDK provides
# SPIRV-Tools (validation) and the Vulkan loader (pipeline creation and the GPU comparison).

cmake_minimum_required(VERSION 3.25)
project(nfsmw_spirv_check LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")  # the Vulkan SDK libraries are /MD

get_filename_component(NFSMW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(NFSMW_THIRDPARTY "${NFSMW_ROOT}/sdk/thirdparty")

find_package(Vulkan REQUIRED COMPONENTS SPIRV-Tools)

# glslang's SPIR-V builder, configured like the SDK does (sdk/thirdparty/CMakeLists.txt).
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
set(ENABLE_RTTI ON CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
add_subdirectory("${NFSMW_THIRDPARTY}/glslang" glslang EXCLUDE_FROM_ALL)
add_subdirectory("${NFSMW_THIRDPARTY}/fmt" fmt EXCLUDE_FROM_ALL)

add_executable(nfsmw_spirv_check
    nfsmw_spirv_check.cpp
    nfsmw_spirv_gpu.cpp
    "${NFSMW_ROOT}/shaders/XenosRecomp/spirv_recompiler.cpp"
    "${NFSMW_ROOT}/app/src/nfsmw_shader_library.cpp")
target_include_directories(nfsmw_spirv_check PRIVATE
    "${NFSMW_ROOT}/shaders"
    "${NFSMW_ROOT}/shaders/XenosRecomp"
    "${NFSMW_ROOT}/app/src"
    "${NFSMW_THIRDPARTY}/glslang/SPIRV"
    "${NFSMW_THIRDPARTY}/xxHash")
target_compile_definitions(nfsmw_spirv_check PRIVATE NFSMW_RECOMP NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_precompile_headers(nfsmw_spirv_check PRIVATE "${NFSMW_ROOT}/shaders/XenosRecomp/pch_min.h")
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(nfsmw_spirv_check PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions)
endif()
target_link_libraries(nfsmw_spirv_check PRIVATE SPIRV fmt::fmt Vulkan::Vulkan Vulkan::SPIRV-Tools)
