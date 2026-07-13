<#
.SYNOPSIS
    Configure, build and run ProcMon (per-process CPU / RAM / VRAM monitor).

    The procmon executable's target definition (sources/includes/links/
    defines) is authored as Prolog facts in ProcMon.lm and transpiled to
    generated/CMakeLists.txt by CppLogicMake
    (https://github.com/cschladetsch/CppLogiMake). This script regenerates
    that file automatically when $env:LOGICMAKE_ROOT is set, by calling
    build\logicmake.exe directly rather than going through that repo's
    generate.ps1/logimake.ps1 wrappers - those currently mis-forward -Input
    and fail with "-OutputDir is required when passing multiple -Input
    files" even for a single file. If only a bare `logimake` is on PATH
    (no $env:LOGICMAKE_ROOT), it's used as a fallback and may hit that same
    bug. If neither is available, it builds from the checked-in
    generated/CMakeLists.txt snapshot as-is.

.PARAMETER Configuration
    Build configuration: Debug, Release (default), or RelWithDebInfo.

.PARAMETER Compiler
    C++ compiler passed to CMake. Defaults to clang++ (matching
    CppLogicMake's own default) - install LLVM/Clang and make sure
    clang++ is on PATH. Pass -Compiler cl to use MSVC instead.

.PARAMETER Generator
    Optional CMake generator. Defaults to Ninja if it's on PATH (the usual
    pairing with clang++ on Windows); otherwise CMake picks its own default.

.PARAMETER Clean
    Delete the build directory before configuring.

.PARAMETER NoRegenerate
    Skip the CppLogicMake regeneration step and build from whatever
    generated/CMakeLists.txt is already on disk.

.PARAMETER NoRun
    Build only; do not launch the app afterwards.

.EXAMPLE
    .\s.ps1
    Regenerate (if logimake is available) + configure + build Release with
    clang++ + launch.

.EXAMPLE
    .\s.ps1 -Compiler cl -Generator "Visual Studio 17 2022"
    Build with MSVC instead of clang.
#>
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [string]$Compiler = "clang++",
    [string]$Generator = "",
    [switch]$Clean,
    [switch]$NoRegenerate,
    [switch]$NoRun
)

$ErrorActionPreference = "Stop"

# Tall banner announcing the build is driven by CppLogicMake (ProcMon.lm ->
# generated/CMakeLists.txt). Plain ASCII on purpose - box-drawing/Unicode
# glyphs here have previously come out mangled on Windows PowerShell 5.1
# when the .ps1 file's encoding doesn't round-trip cleanly, so this sticks
# to characters that can't get corrupted regardless of encoding.
$banner = @(
    ' _     _____ _____ ________  ___  ___   _   __ _____ ',
    '| |   |  _  |  __ \_   _|  \/  | / _ \ | | / /|  ___|',
    '| |   | | | | |  \/ | | | .  . |/ /_\ \| |/ / | |__  ',
    '| |   | | | | | __  | | | |\/| ||  _  ||    \ |  __| ',
    '| |___\ \_/ / |_\ \_| |_| |  | || | | || |\  \| |___ ',
    '\_____/\___/ \____/\___/\_|  |_/\_| |_/\_| \_/\____/ '
)
$bannerColors = @("Red", "Yellow", "Green", "Cyan", "Blue", "Magenta")
for ($i = 0; $i -lt $banner.Count; ++$i) {
    Write-Host $banner[$i] -ForegroundColor $bannerColors[$i % $bannerColors.Count]
}
Write-Host "        >> ProcMon, built with CppLogicMake <<" -ForegroundColor Gray
Write-Host "           https://github.com/cschladetsch/CppLogiMake" -ForegroundColor DarkGray
Write-Host ""

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$buildDir = Join-Path $root "build"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing existing build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Error "CMake was not found on PATH. Install CMake >= 3.25 (https://cmake.org/download/) and try again."
}

if (-not (Get-Command $Compiler -ErrorAction SilentlyContinue)) {
    Write-Warning "'$Compiler' was not found on PATH. Install LLVM/Clang (https://releases.llvm.org/) or pass -Compiler to use something else (e.g. -Compiler cl for MSVC)."
}

# --- Regenerate generated/CMakeLists.txt from ProcMon.lm via CppLogicMake ---
if (-not $NoRegenerate) {
    $lmFile = Join-Path $root "ProcMon.lm"
    $genFile = Join-Path $root "generated\CMakeLists.txt"
    $attempted = $false
    $regenerated = $false

    if ($env:LOGICMAKE_ROOT) {
        # Call the driver binary directly instead of generate.ps1/logimake.ps1:
        # those wrapper scripts currently mis-forward -Input and throw
        # "-OutputDir is required when passing multiple -Input files" even
        # for a single input file. The driver itself works fine standalone.
        $driverCandidates = @(
            (Join-Path $env:LOGICMAKE_ROOT "build\logicmake.exe"),
            (Join-Path $env:LOGICMAKE_ROOT "build\Release\logicmake.exe")
        )
        $driver = $driverCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
        $schema = Join-Path $env:LOGICMAKE_ROOT "prolog\targets.pl"

        if ($driver -and (Test-Path $schema)) {
            $attempted = $true
            Write-Host "Regenerating generated/CMakeLists.txt from ProcMon.lm (logicmake.exe via `$env:LOGICMAKE_ROOT)..." -ForegroundColor Cyan
            Push-Location $root
            try {
                & $driver --schema $schema --input $lmFile --output $genFile
                if ($LASTEXITCODE -eq 0) {
                    $regenerated = $true
                } else {
                    Write-Warning "CppLogicMake generation failed; continuing with the existing generated/CMakeLists.txt."
                }
            } finally {
                Pop-Location
            }
        } else {
            Write-Warning "`$env:LOGICMAKE_ROOT is set but build\logicmake.exe / prolog\targets.pl weren't found under it - run its scripts\build.ps1 first. Continuing with the existing generated/CMakeLists.txt."
        }
    } else {
        $logimakeCmd = Get-Command logimake -ErrorAction SilentlyContinue
        if ($logimakeCmd) {
            $attempted = $true
            Write-Host "Regenerating generated/CMakeLists.txt from ProcMon.lm (logimake on PATH)..." -ForegroundColor Cyan
            & logimake generate -Input $lmFile -Output $genFile -WorkingDirectory $root
            if ($LASTEXITCODE -eq 0) {
                $regenerated = $true
            } else {
                Write-Warning "CppLogicMake generation failed (known bug in the logimake wrapper's argument forwarding - set `$env:LOGICMAKE_ROOT to call the driver directly instead); continuing with the existing generated/CMakeLists.txt."
            }
        } else {
            Write-Host "CppLogicMake not found (no 'logimake' on PATH, `$env:LOGICMAKE_ROOT unset) - building from the checked-in generated/CMakeLists.txt." -ForegroundColor Yellow
            Write-Host "  Install it from: https://github.com/cschladetsch/CppLogiMake" -ForegroundColor Yellow
        }
    }

    if ($attempted -and -not $regenerated -and -not (Test-Path (Join-Path $root ".git"))) {
        Write-Warning "This also isn't a git checkout - CppLogicMake resolves ProcMon.lm's source globs via 'git ls-files', which would fail regardless. Run 'git init; git add -A' first, or pass -NoRegenerate to build from the checked-in generated/CMakeLists.txt."
    }
}

$configureArgs = @("-S", $root, "-B", $buildDir, "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_CXX_COMPILER=$Compiler")
if ($Generator -ne "") {
    $configureArgs += @("-G", $Generator)
} elseif (Get-Command ninja -ErrorAction SilentlyContinue) {
    $configureArgs += @("-G", "Ninja")
}

Write-Host "Configuring (first run also fetches GLFW + Dear ImGui via CMake FetchContent, be patient)..." -ForegroundColor Cyan
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed." }

Write-Host "Building ($Configuration, $Compiler)..." -ForegroundColor Cyan
& cmake --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed." }

# procmon is defined inside the generated/ subdirectory (add_subdirectory,
# not the top-level CMakeLists.txt), so CMake mirrors that under the build
# tree: build\generated\procmon.exe for single-config generators (Ninja/
# Makefiles), build\generated\<Config>\procmon.exe for multi-config (VS).
# Target name is lowercase "procmon" (CppLogicMake's Prolog atoms are
# lowercase), so that's the binary name too.
$candidates = @(
    (Join-Path $buildDir "generated\$Configuration\procmon.exe"),
    (Join-Path $buildDir "generated\procmon.exe"),
    (Join-Path $buildDir "$Configuration\procmon.exe"),
    (Join-Path $buildDir "procmon.exe")
)
$exe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $exe) {
    Write-Error "Could not locate procmon.exe under $buildDir"
}

Write-Host "Built: $exe" -ForegroundColor Green

if (-not $NoRun) {
    Write-Host "Launching ProcMon..." -ForegroundColor Cyan
    Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
}
