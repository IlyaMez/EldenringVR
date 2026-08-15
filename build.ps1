<#
.SYNOPSIS
  Configure + build ERVR (Release, x64) into .\build\dist.
.EXAMPLE
  .\build.ps1
  .\build.ps1 -Config Debug
#>
param([string]$Config = "Release")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# Locate cmake: PATH first, then the one bundled with Visual Studio.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath
        $candidate = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $candidate) { $cmake = $candidate }
    }
}
if (-not $cmake) { throw "cmake not found. Install CMake or Visual Studio 2022+ with the C++ Desktop workload." }

Write-Host "Using $cmake"
& $cmake -S $root -B "$root\build" -A x64
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& $cmake --build "$root\build" --config $Config
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "`nOutput: $root\build\dist" -ForegroundColor Green
Get-ChildItem -Recurse "$root\build\dist" | Select-Object FullName, Length
