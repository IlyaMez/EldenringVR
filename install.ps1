<#
.SYNOPSIS
  Copy the built mod into the ELDEN RING\Game folder (next to eldenring.exe).
.DESCRIPTION
  Installs dxgi.dll (bundled ReShade with add-on support), ReShade.ini, dinput8.dll (proxy loader),
  ERVR\ERVR.dll and ERVR\ERVR.ini. Existing ERVR.ini / ReShade.ini are kept. A foreign dxgi.dll is
  backed up to dxgi.dll.ervr-bak; a foreign dinput8.dll (another mod loader) is never overwritten.
.PARAMETER GameDir   Override the game folder.
.PARAMETER Uninstall Remove ERVR and the bundled ReShade (keeps ERVR.log/ini and ReShade.ini, restores a backed-up dxgi.dll).
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
$release = Join-Path $root "build\Release"

# The `dist` custom target only refreshes on a full build, so a `--target ERVR` build
# leaves dist stale. Always take the freshest DLLs from build\Release when they exist.
function Freshest([string]$name) {
    $d = Join-Path $dist $name; $r = Join-Path $release (Split-Path $name -Leaf)
    if ((Test-Path $r) -and (-not (Test-Path $d) -or (Get-Item $r).LastWriteTime -ge (Get-Item $d).LastWriteTime)) { return $r }
    return $d
}
function Sha([string]$p) { (Get-FileHash $p -Algorithm SHA256).Hash }
function IsReShade([string]$p) { (Test-Path $p) -and ((Get-Item $p).VersionInfo.ProductName -eq "ReShade") }

if (-not (Test-Path (Join-Path $GameDir "eldenring.exe"))) { throw "eldenring.exe not found in '$GameDir' - pass -GameDir." }

$gameDxgi    = Join-Path $GameDir "dxgi.dll"
$gameDxgiBak = "$gameDxgi.ervr-bak"
$gameProxy   = Join-Path $GameDir "dinput8.dll"
$gameRsIni   = Join-Path $GameDir "ReShade.ini"

if ($Uninstall) {
    Remove-Item -Force -ErrorAction SilentlyContinue $gameProxy
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $GameDir "ERVR\ERVR.dll")
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $GameDir "ERVR\bhaptics_library.dll")
    if (IsReShade $gameDxgi) {
        Remove-Item -Force $gameDxgi
        if (Test-Path $gameDxgiBak) { Move-Item -Force $gameDxgiBak $gameDxgi; Write-Host "Restored the previous dxgi.dll from dxgi.dll.ervr-bak." }
        Write-Host "Removed dinput8.dll, ERVR\ERVR.dll and dxgi.dll (ReShade) from $GameDir. ERVR.ini / ERVR.log / ReShade.ini left in place." -ForegroundColor Yellow
    } else {
        Write-Host "Removed dinput8.dll and ERVR\ERVR.dll from $GameDir. dxgi.dll is not ReShade, left untouched." -ForegroundColor Yellow
    }
    return
}

if (-not (Test-Path (Join-Path $dist "ERVR\ERVR.dll"))) { throw "Build first: .\build.ps1" }
$srcDxgi = Join-Path $dist "dxgi.dll"
if (-not (Test-Path $srcDxgi)) { $srcDxgi = Join-Path $root "third_party\reshade\ReShade64.dll" }
$srcRsIni = Join-Path $dist "ReShade.ini"
if (-not (Test-Path $srcRsIni)) { $srcRsIni = Join-Path $root "third_party\reshade\ReShade.ini" }

# ---- dinput8.dll: never clobber another mod loader ---------------------------------------
if ((Test-Path $gameProxy) -and ((Sha (Join-Path $dist "dinput8.dll")) -ne (Sha $gameProxy))) {
    Write-Warning "A different dinput8.dll already exists in the game folder (another mod loader?)."
    Write-Warning "ERVR will NOT overwrite it. Instead, put ERVR\ERVR.dll where that loader picks up DLLs (e.g. mods\)."
    Write-Warning "Or delete it manually and re-run install.ps1."
    Copy-Item -Force (Join-Path $dist "ERVR\*") (New-Item -ItemType Directory -Force (Join-Path $GameDir "ERVR")).FullName -Exclude ERVR.ini
    return
}

# ---- dxgi.dll: the bundled ReShade (add-on build) ----------------------------------------
$leftovers = @("RealVR64.dll", "openvr_api.dll", "cudart64_12.dll", "RealVR.ini") | Where-Object { Test-Path (Join-Path $GameDir $_) }
if ($leftovers) {
    Write-Warning "R.E.A.L. VR files found in the game folder: $($leftovers -join ', ')."
    Write-Warning "ERVR does not use R.E.A.L. and replaces its dxgi.dll. Delete those files so a second VR runtime does not fight ERVR."
}
$flatMods = @(Get-ChildItem -LiteralPath $GameDir -Filter *.dll | Where-Object { $_.VersionInfo.ProductName -match 'DLSS Enabler|OptiScaler' }) +
             @(Get-ChildItem -LiteralPath (Join-Path $GameDir 'reshade-addons') -Filter *.addon* -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '\.addon(64)?$' })
if ($flatMods) {
    Write-Warning "Flat render mods are active: $($flatMods.Name -join ', '). They break the headset image (HDR re-encoding, DXGI wrapping)."
    Write-Warning "Park them for VR with .\tools\game_mode.ps1 -Mode vr (restore with -Mode flat)."
}
if (Test-Path $gameDxgi) {
    if ((Sha $gameDxgi) -eq (Sha $srcDxgi)) {
        Write-Host "dxgi.dll is already the bundled ReShade."
    } else {
        if (IsReShade $gameDxgi) { $what = "ReShade $((Get-Item $gameDxgi).VersionInfo.FileVersion)" }
        else { $what = "not ReShade (R.E.A.L. VR or another wrapper)" }
        Write-Warning "Existing dxgi.dll is $what. Backing it up to dxgi.dll.ervr-bak and installing the bundled ReShade $((Get-Item $srcDxgi).VersionInfo.FileVersion)."
        Copy-Item -Force $gameDxgi $gameDxgiBak
        Copy-Item -Force $srcDxgi $gameDxgi
    }
} else {
    Copy-Item -Force $srcDxgi $gameDxgi
}
if (Test-Path $gameRsIni) { Write-Host "Keeping existing ReShade.ini" } else { Copy-Item $srcRsIni $gameRsIni }

# ---- ERVR itself --------------------------------------------------------------------------
New-Item -ItemType Directory -Force (Join-Path $GameDir "ERVR") | Out-Null
Copy-Item -Force (Freshest "dinput8.dll") $GameDir
Copy-Item -Force (Freshest "ERVR\ERVR.dll") (Join-Path $GameDir "ERVR")
# bHaptics' library (body haptics, [bHaptics] in ERVR.ini) and the notices that travel with it.
foreach ($f in @("bhaptics_library.dll", "LICENSE-bHaptics.txt", "bHaptics-THIRD_PARTY_NOTICES.txt")) {
    $src = Join-Path $dist "ERVR\$f"
    if (Test-Path $src) { Copy-Item -Force $src (Join-Path $GameDir "ERVR") }
}
$ini = Join-Path $GameDir "ERVR\ERVR.ini"
if (Test-Path $ini) { Write-Host "Keeping existing ERVR.ini (new defaults in build\dist\ERVR\ERVR.ini)" }
else { Copy-Item (Join-Path $dist "ERVR\ERVR.ini") $ini }

Write-Host "`nInstalled to $GameDir" -ForegroundColor Green
Write-Host "  dxgi.dll           (ReShade $((Get-Item $gameDxgi).VersionInfo.FileVersion), add-on build)"
Write-Host "  ReShade.ini        (ReShade config)"
Write-Host "  dinput8.dll        (proxy loader)"
Write-Host "  ERVR\ERVR.dll      (mod)"
Write-Host "  ERVR\ERVR.ini      (config, hot-reloads)"
Write-Host "  ERVR\bhaptics_library.dll (bHaptics SDK, body haptics with the bHaptics Player)"
Write-Host "Launch the game offline with EAC disabled and put the headset on. Logs: ERVR\ERVR.log and ReShade.log"
