<#
.SYNOPSIS
  Copy the built mod into the ELDEN RING\Game folder (next to eldenring.exe and RealVR64.dll).
.PARAMETER GameDir   Override the game folder.
.PARAMETER Uninstall Remove ERVR files (keeps ERVR.log/ini backups).
.EXAMPLE
  .\install.ps1
  .\install.ps1 -Uninstall
#>
param(
    [string]$GameDir = "${env:ProgramFiles(x86)}\Steam\steamapps\common\ELDEN RING\Game",
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$dist = Join-Path $root "build\dist"

if (-not (Test-Path (Join-Path $GameDir "eldenring.exe"))) { throw "eldenring.exe not found in '$GameDir' - pass -GameDir." }

if ($Uninstall) {
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $GameDir "dinput8.dll")
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $GameDir "ERVR\ERVR.dll")
    Write-Host "Removed dinput8.dll and ERVR\ERVR.dll from $GameDir (ERVR.ini / ERVR.log left in place)." -ForegroundColor Yellow
    return
}

if (-not (Test-Path (Join-Path $dist "ERVR\ERVR.dll"))) { throw "Build first: .\build.ps1" }

$existingProxy = Join-Path $GameDir "dinput8.dll"
if (Test-Path $existingProxy) {
    $ours = (Get-FileHash (Join-Path $dist "dinput8.dll")).Hash
    $theirs = (Get-FileHash $existingProxy).Hash
    if ($ours -ne $theirs) {
        Write-Warning "A different dinput8.dll already exists in the game folder (another mod loader?)."
        Write-Warning "ERVR will NOT overwrite it. Instead, put ERVR\ERVR.dll where that loader picks up DLLs (e.g. mods\)."
        Write-Warning "Or delete it manually and re-run install.ps1."
        Copy-Item -Force (Join-Path $dist "ERVR\*") (New-Item -ItemType Directory -Force (Join-Path $GameDir "ERVR")).FullName -Exclude ERVR.ini
        return
    }
}

New-Item -ItemType Directory -Force (Join-Path $GameDir "ERVR") | Out-Null
Copy-Item -Force (Join-Path $dist "dinput8.dll") $GameDir
Copy-Item -Force (Join-Path $dist "ERVR\ERVR.dll") (Join-Path $GameDir "ERVR")
$ini = Join-Path $GameDir "ERVR\ERVR.ini"
if (Test-Path $ini) { Write-Host "Keeping existing ERVR.ini (new defaults in build\dist\ERVR\ERVR.ini)" }
else { Copy-Item (Join-Path $dist "ERVR\ERVR.ini") $ini }

Write-Host "`nInstalled to $GameDir" -ForegroundColor Green
Write-Host "  dinput8.dll        (proxy loader)"
Write-Host "  ERVR\ERVR.dll      (mod)"
Write-Host "  ERVR\ERVR.ini      (config, hot-reloads)"
Write-Host "Launch the game the same way you do for R.E.A.L. VR. Log: ERVR\ERVR.log"
