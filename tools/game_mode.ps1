<#
.SYNOPSIS
  Switch the ELDEN RING folder between VR (ERVR) and a flat render-mod setup by renaming files.
.DESCRIPTION
  ERVR needs the game's own frame: one present per engine pass, in the game's SDR format, with
  nothing between ReShade and DXGI. Flat-only render mods break that:
    * RenoDX re-encodes the back buffer as HDR10 PQ, which the SDR headset blit shows washed out,
      and it re-routes the UI draws the HUD layer captures;
    * DLSS Enabler / OptiScaler wrap DXGI underneath ReShade (and frame generation inserts
      presents between the two eye passes);
    * ReShade effects run once per eye pass, and temporal ones (motion vectors) break in stereo.
  -Mode vr parks those (renamed to <name>.ervr-off) and enables ERVR's dinput8.dll; -Mode flat
  parks ERVR and restores them. Nothing is deleted. Only files identified by their version
  resource (DLLs) or by ReShade.ini (add-on folder, preset) are touched.
.PARAMETER Mode     vr | flat | status (default: status - show what is active, change nothing).
.PARAMETER GameDir  Override the game folder.
.EXAMPLE
  .\tools\game_mode.ps1 -Mode vr
  .\tools\game_mode.ps1 -Mode flat
#>
param(
    [ValidateSet('vr', 'flat', 'status')][string]$Mode = 'status',
    [string]$GameDir = "${env:ProgramFiles(x86)}\Steam\steamapps\common\ELDEN RING\Game"
)
$ErrorActionPreference = 'Stop'
$Off = '.ervr-off'
if (-not (Test-Path (Join-Path $GameDir 'eldenring.exe'))) { throw "eldenring.exe not found in '$GameDir' - pass -GameDir." }

function Product([string]$path) { try { (Get-Item -LiteralPath $path).VersionInfo.ProductName } catch { '' } }
function IniValue([string]$file, [string]$key) {
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    foreach ($line in Get-Content -LiteralPath $file) { if ($line -match "^\s*$key\s*=\s*(.+?)\s*$") { return $Matches[1] } }
    return $null
}
function Resolve-GamePath([string]$p) { if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $GameDir $p } }

# ---- what belongs to which side ---------------------------------------------------------
# Each item: the live path; parked = live + '.ervr-off'.
$ervr = @()
foreach ($name in @('dinput8.dll')) {
    $live = Join-Path $GameDir $name
    foreach ($p in @($live, "$live$Off")) {
        if ((Test-Path -LiteralPath $p) -and (Product $p) -like 'ERVR*') { $ervr += $live; break }
    }
}
$flat = @()
foreach ($f in Get-ChildItem -LiteralPath $GameDir -File | Where-Object { $_.Name -match '\.dll(\.ervr-off)?$' }) {
    if ((Product $f.FullName) -match 'DLSS Enabler|OptiScaler') { $flat += ($f.FullName -replace ([regex]::Escape($Off) + '$'), '') }
}
$rsIni = Join-Path $GameDir 'ReShade.ini'
$addonDir = IniValue $rsIni 'AddonPath'
$addonDir = if ($addonDir) { Resolve-GamePath $addonDir } else { $GameDir }
if (Test-Path -LiteralPath $addonDir) {
    foreach ($f in Get-ChildItem -LiteralPath $addonDir -File | Where-Object { $_.Name -match '\.addon(64)?(\.ervr-off)?$' }) {
        $flat += ($f.FullName -replace ([regex]::Escape($Off) + '$'), '')
    }
}
$preset = IniValue $rsIni 'PresetPath'
if ($preset) {
    $preset = Resolve-GamePath $preset
    if ((Test-Path -LiteralPath $preset) -or (Test-Path -LiteralPath "$preset$Off")) { $flat += $preset }
}
$flat = $flat | Select-Object -Unique

function State([string]$live) {
    if (Test-Path -LiteralPath $live) { 'active' } elseif (Test-Path -LiteralPath "$live$Off") { 'parked' } else { 'missing' }
}
function Show {
    Write-Host 'ERVR:' -ForegroundColor Cyan
    if (-not $ervr) { Write-Host '  (ERVR dinput8.dll not installed - run install.ps1)' }
    foreach ($p in $ervr) { Write-Host ("  {0,-8} {1}" -f (State $p), $p.Substring($GameDir.Length + 1)) }
    Write-Host 'Flat-only render mods:' -ForegroundColor Cyan
    if (-not $flat) { Write-Host '  (none found)' }
    foreach ($p in $flat) { Write-Host ("  {0,-8} {1}" -f (State $p), $p.Substring($GameDir.Length + 1)) }
}
function Park([string]$live) {
    if (-not (Test-Path -LiteralPath $live)) { return }
    if (Test-Path -LiteralPath "$live$Off") {
        # A parked copy already exists (e.g. ReShade wrote a fresh preset while parked): keep both.
        $keep = "$live.ervr-new-" + (Get-Date -Format 'yyyyMMdd-HHmmss')
        Move-Item -LiteralPath $live -Destination $keep
        Write-Host "  kept newer $([IO.Path]::GetFileName($live)) as $([IO.Path]::GetFileName($keep))" -ForegroundColor DarkYellow
        return
    }
    Move-Item -LiteralPath $live -Destination "$live$Off"
    Write-Host "  parked   $($live.Substring($GameDir.Length + 1))"
}
function Unpark([string]$live) {
    if (-not (Test-Path -LiteralPath "$live$Off")) { return }
    if (Test-Path -LiteralPath $live) {
        $keep = "$live.ervr-new-" + (Get-Date -Format 'yyyyMMdd-HHmmss')
        Move-Item -LiteralPath $live -Destination $keep
        Write-Host "  kept the file made while parked as $([IO.Path]::GetFileName($keep))" -ForegroundColor DarkYellow
    }
    Move-Item -LiteralPath "$live$Off" -Destination $live
    Write-Host "  restored $($live.Substring($GameDir.Length + 1))"
}

if ($Mode -eq 'status') { Show; return }
if (Get-Process -Name eldenring, start_protected_game -ErrorAction SilentlyContinue) { throw 'Close ELDEN RING first (its DLLs are locked while it runs).' }
if ($Mode -eq 'vr') {
    if (-not $ervr) { throw 'ERVR is not installed - run install.ps1 first.' }
    Write-Host 'Switching to VR (ERVR on, flat-only render mods parked):' -ForegroundColor Green
    foreach ($p in $flat) { Park $p }
    foreach ($p in $ervr) { Unpark $p }
} else {
    Write-Host 'Switching to flat (render mods restored, ERVR parked):' -ForegroundColor Green
    foreach ($p in $ervr) { Park $p }
    foreach ($p in $flat) { Unpark $p }
}
Show
