# Third-party notices

NFSMW Recompiled is built on the work of other projects. Their licenses apply to their code and to the parts of this
repository derived from it.

## Projects this one is derived from

| Project | Used for | License |
|---|---|---|
| [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) by StevensND | The base of `app/`, `shaders/` and the changes in `sdk/`: the native Vulkan renderer, the game hooks and the shader pipeline | GPL-3.0 |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, with portions from [Xenia](https://xenia.jp) (Ben Vanik and the Xenia contributors) | `sdk/`: the recompiler, runtime and kernel | BSD-3-Clause |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev and contributors | `shaders/XenosRecomp`: Xenos microcode to HLSL translator | MIT |

The changes in `sdk/` are under the SDK's BSD-3-Clause license and the changes in `shaders/` under its MIT license.
The rest of this repository is GPL-3.0 (`LICENSE`).

## Libraries

Libraries used by the SDK, the executable and the code generator, fetched from the submodules of ReXGlue SDK v0.10.0
(`tools/fetch_thirdparty.py`):

| Library | License |
|---|---|
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil: the WMV3 decoder for the cutscenes), from `wmarti/FFmpeg` at `0604b464c7cb` | LGPL-2.1-or-later |
| [libmspack](https://www.cabextract.org.uk/libmspack/) by Stuart Caie (LZX decompression) | LGPL-2.1 |
| [SDL3](https://libsdl.org) (input) | Zlib |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause and others (see its LICENSE.txt) |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | Apache-2.0 |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | MIT, Apache-2.0 |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | MIT |
| [fmt](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog) | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | BSD-2-Clause |
| [SIMDe](https://github.com/simd-everywhere/simde) | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | MIT |
| [utfcpp](https://github.com/nemtrif/utfcpp) | BSL-1.0 |
| [CLI11](https://github.com/CLIUtils/CLI11) | BSD-3-Clause |
| AES-128 by LuoPeng (`thirdparty/aes_128`) | MIT |

The launcher's shader generator loads [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler)
(`dxcompiler.dll`, `dxil.dll`) at run time. It is distributed next to the executable under the University of
Illinois/NCSA Open Source License, with LLVM's license terms.

### FFmpeg and libmspack (LGPL)

The modified FFmpeg files are in `sdk/thirdparty`; the rest of FFmpeg and libmspack is the unmodified upstream source
at the commits pinned by ReXGlue SDK v0.10.0. Everything needed to rebuild and relink the executable with a modified
version of either library is in this repository and in [docs/building.md](docs/building.md).

## Trademarks

Need for Speed and Need for Speed: Most Wanted are trademarks of Electronic Arts Inc. Xbox 360 is a trademark of
Microsoft. This project is not affiliated with or endorsed by any of them.
