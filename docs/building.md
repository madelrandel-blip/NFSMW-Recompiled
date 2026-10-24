# Building

The build has two halves: generating C++ from your copy of the game, and compiling that C++ together with this
repository. The generated code contains the game's code, so it is never committed and the executable built from it
must not be redistributed.

## Requirements

- Windows 10/11 x64 (Linux works for the SDK and the app, but is less tested)
- [LLVM/Clang 20](https://github.com/llvm/llvm-project/releases) (`clang`, `clang++`)
- CMake 3.25+, Ninja, Python 3
- Visual Studio 2022 Build Tools (for the Windows SDK and the C runtime)
- Optional: the [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler/releases) release, so the
  launcher can generate the shader library (`-DNFSMW_DXC_DIR=<dxc folder>`)
- Your own `default.xex` from the supported edition

## Steps

```bash
# 1. Third-party sources of the SDK
python tools/fetch_thirdparty.py

# 2. Extract the game (prints the XEX information)
python tools/fase1_extraer.py <path to the .iso>

# 3. Build the code generator
cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out/host --target rexglue

# 4. Generate the C++ (app/generated/, ignored by git)
cd app && ../sdk/out/win-amd64/rexglue codegen nfsmw_manifest.toml && cd ..
python tools/llamadas_directas.py --gen app/generated/default
python tools/copia_literal.py app/generated/default app/src/copias_literales

# 5. Build the game
cmake -S app -B out/pc -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
      -DREXSDK_DIR=$PWD/sdk -DREXGLUE_USE_VULKAN=ON -DNFSMW_BUILD_LAUNCHER=OFF \
      -DNFSMW_DXC_DIR=<dxc folder>
cmake --build out/pc --target nfsmw
```

`out/pc/nfsmw.exe` is the game, with the launcher built in. Copy `dxcompiler.dll` and `dxil.dll` next to it (the
build does it when `NFSMW_DXC_DIR` is set).

## Tools

[tools/README.md](../tools/README.md) describes each script.
