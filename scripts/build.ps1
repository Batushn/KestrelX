param(
    [string]$Config = "Release",
    [switch]$SkipTests
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio 2022 Build Tools." }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "Visual Studio 2022 C++ x64 build tools not found." }

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

function Resolve-Tool {
    param(
        [string]$Name,
        [string[]]$Candidates
    )
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) {
        return $cmd.Source
    }
    return $null
}

$cmake = Resolve-Tool -Name "cmake" -Candidates @(
    (Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"),
    "$env:ProgramFiles\CMake\bin\cmake.exe",
    "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe",
    "$env:LOCALAPPDATA\Microsoft\WinGet\Links\cmake.exe"
)
if (-not $cmake) {
    throw "cmake.exe not found in Visual Studio CommonExtensions, PATH, or standard install paths. Install CMake (e.g. 'winget install Kitware.CMake' or VS C++ CMake tools)."
}

$ninja = Resolve-Tool -Name "ninja" -Candidates @(
    (Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"),
    "$env:LOCALAPPDATA\Microsoft\WinGet\Links\ninja.exe",
    "$env:ProgramFiles\Ninja\ninja.exe",
    "C:\ProgramData\chocolatey\bin\ninja.exe"
)
if (-not $ninja) {
    throw "ninja.exe not found in Visual Studio CommonExtensions, PATH, or standard install paths. Install Ninja (e.g. 'winget install Ninja-build.Ninja' or VS C++ CMake tools)."
}

$buildDir = Join-Path $root "build\$Config"

$cmd = "`"$vcvars`" >nul && `"$cmake`" -S `"$root`" -B `"$buildDir`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$Config && `"$cmake`" --build `"$buildDir`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $SkipTests) {
    Write-Host "=== Running kestrel_tests ==="
    & (Join-Path $buildDir "kestrel_tests.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
