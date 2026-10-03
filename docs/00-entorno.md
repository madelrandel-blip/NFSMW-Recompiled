# Phase 0 — Build environment

ReXGlue **only** works with Clang. MSVC and GCC are not supported: the generated
code depends on intrinsics and optimization behavior specific to Clang/LLVM. Don't
try to force it, it breaks in strange ways.

## Windows

| Tool | Minimum version | Note |
|---|---|---|
| Visual Studio 2022 Community | — | *Desktop development with C++* workload |
| C++ Clang Compiler for Windows | 20.x | optional component inside the VS installer |
| MSBuild support for LLVM (clang-cl) | — | optional component |
| CMake | 3.25+ | the one bundled with VS works |
| Ninja | any | the one bundled with VS works |
| Windows SDK + D3D12 headers | — | they come with the workload |
| Python | 3.8+ | only for the extraction tools |
| Git | — | with `--recursive` for submodules |

In the VS installer: the **Individual components** tab → search for "clang" → check
*C++ Clang Compiler for Windows* and *MSBuild support for LLVM (clang-cl) toolset*.

Verification:
```powershell
cmake --version
ninja --version
clang --version     # must say 20.x or higher
python --version
```

## Linux

Debian / Ubuntu:
```bash
sudo apt update
sudo apt install -y clang-20 lld-20 cmake ninja-build git python3 libgtk-3-dev
```

If your distro doesn't have `clang-20` in its repos, use the LLVM installer:
```bash
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 20
```

Arch:
```bash
sudo pacman -S clang lld cmake ninja git python gtk3
```

Verification:
```bash
clang --version    # 20+
cmake --version    # 3.25+
ninja --version
pkg-config --modversion gtk+-3.0
```

## Fast path (Windows)

Double-click **`FASE0_ENTORNO.bat`** in the project root. It locates Visual Studio
with `vswhere` (including Insiders/Preview versions), loads the x64 build
environment, checks every tool, and if everything is there, clones and builds the SDK.

### The classic mistake: checking only the Clang components

It's tempting to go to *Individual components*, search for "clang", check the two
that show up, and hit install. **It doesn't work.** Clang on Windows is not
self-sufficient: it needs the headers and libraries of the **Windows SDK** and the
**MSVC** toolchain to link. And ReXGlue also needs the **D3D12** headers, which also
come from the SDK.

If the installer asks you *"Do you want to continue without workloads?"*, the
answer is **no**: click *Add workloads* and check
**"Desktop development with C++"**.

### Why the .bat is needed and a normal console won't do

The `cmake`, `ninja` and `clang-cl` that Visual Studio installs **are not on the
global PATH**. Neither are the `INCLUDE` and `LIB` variables that point to the
Windows SDK. All of that only exists after running `vcvars64.bat`, which is what the
*Developer Command Prompt* does.

The Phase 0 `.bat` does that for you. If you prefer to work by hand, open
**"Developer PowerShell for VS"** from the Start menu instead of a normal
PowerShell.

## Installing the SDK

```bash
git clone --recursive https://github.com/rexglue/rexglue-sdk.git
cd rexglue-sdk

# Windows
cmake --preset win-amd64
cmake --build out/build/win-amd64 --target install

# Linux
cmake --preset linux-amd64
cmake --build out/build/linux-amd64 --target install
```

`install` registers the SDK in CMake's *user package registry*, so your project
finds it with just `find_package(rexglue)` and no absolute paths.

Check that the CLI ended up on the PATH:
```bash
rexglue --help
```

If it doesn't show up, add the installation's `bin/` to PATH, or call the binary by
full path from `out/install/<preset>/bin/`.

## About building on Windows and Linux at the same time

It's viable and in fact recommended: both toolchains use Clang, so codegen errors
show up the same on both, but Linux gives you better sanitizers (ASan/UBSan) to hunt
down guest memory corruption, and Windows gives you convenient RenderDoc/PIX for
debugging the graphics side. Keep a single `config/nfsmw_config.toml` and two
separate build directories.

---

## Known issues

### `error: expected identifier or '('` in `lzxd.c` (libmspack)

```
lzxd.c:1:1: error: expected identifier or '('
    1 | ../../libmspack/mspack/lzxd.c
```

The file has no C code inside: it has a **path**. Several submodules (libmspack
first) use symbolic links in their tree. Creating symlinks on Windows requires
special permissions, so git —when it doesn't have them— checks out the link as a
normal text file whose only content is the destination path. The compiler opens it
expecting C and finds that.

`FASE0_ENTORNO.bat` fixes it by itself before building. To run it manually:

```powershell
python tools\arreglar_symlinks.py ..\rexglue-sdk
python tools\arreglar_symlinks.py ..\rexglue-sdk --simular   # see without touching
```

It replaces each broken link with a real copy of the target file. You don't need to
be an administrator or clone again.

**Note:** git will see those files as modified, and a `git submodule update`
reverts them. If you update the SDK again, re-run the script (it's idempotent).

The "correct" alternative is to enable Windows Developer Mode, set
`git config --global core.symlinks true` and clone again — but that's 800 MB of
downloading again to fix three files.

### The `win-amd64` preset shows as disabled

```
CMake Error: Cannot use disabled configure preset ... "win-amd64"
```

You have MSYS2, Cygwin or Git Bash ahead of Visual Studio in the PATH. Their
`cmake` reports `${hostSystemName}` as `MSYS` instead of `Windows`, and the preset
requires `Windows`. `FASE0_ENTORNO.bat` avoids that by putting Visual Studio's
`cmake`, `ninja` and `clang` at the front of the PATH, and flags with `[??]` any
tool that doesn't come from there.
