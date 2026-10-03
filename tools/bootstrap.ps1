# NFSMW Recomp - bootstrap (Windows)
# Checks prerequisites, clones the ReXGlue SDK and builds and installs it.
# Usage:  .\tools\bootstrap.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Test-Cmd($name) {
    return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

Write-Host "== Checking prerequisites ==" -ForegroundColor Cyan

$missing = @()
foreach ($t in @("git", "cmake", "ninja", "clang", "python")) {
    if (Test-Cmd $t) {
        $v = (& $t --version 2>&1 | Select-Object -First 1)
        Write-Host ("  [ok] {0,-8} {1}" -f $t, $v)
    } else {
        Write-Host ("  [--] {0,-8} NOT FOUND" -f $t) -ForegroundColor Red
        $missing += $t
    }
}

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "Missing: $($missing -join ', ')" -ForegroundColor Red
    Write-Host "Install Visual Studio 2022 with the 'Desktop development with C++' workload"
    Write-Host "and the individual components:"
    Write-Host "  - C++ Clang Compiler for Windows (20.x or higher)"
    Write-Host "  - MSBuild support for LLVM (clang-cl) toolset"
    exit 1
}

# Clang must be 20+
$clangVer = (clang --version | Select-String -Pattern '(\d+)\.\d+\.\d+' | ForEach-Object { $_.Matches[0].Groups[1].Value })
if ([int]$clangVer -lt 20) {
    Write-Host "Clang $clangVer detected; ReXGlue needs 20 or higher." -ForegroundColor Red
    Write-Host "MSVC and GCC are not supported: the generated code depends on Clang intrinsics."
    exit 1
}

Write-Host ""
Write-Host "== ReXGlue SDK ==" -ForegroundColor Cyan

$sdk = Join-Path (Split-Path -Parent $root) "rexglue-sdk"

if (Test-Path $sdk) {
    Write-Host "  Already exists at $sdk - updating"
    Push-Location $sdk
    git pull --ff-only
    git submodule update --init --recursive
    Pop-Location
} else {
    Write-Host "  Cloning into $sdk"
    git clone --recursive https://github.com/rexglue/rexglue-sdk.git $sdk
}

Push-Location $sdk
Write-Host ""
Write-Host "== Building (win-amd64) ==" -ForegroundColor Cyan
cmake --preset win-amd64
cmake --build out/build/win-amd64 --target install
Pop-Location

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "Check that the CLI is accessible:  rexglue --help"
Write-Host "If it is not found, add to PATH:  $sdk\out\install\win-amd64\bin"
Write-Host ""
Write-Host "Next step: docs\01-extraccion-xex.md"
