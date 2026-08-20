<#
.SYNOPSIS
  Unattended ELDEN RING test run: launch, wait, kill, report. The loop that lets a change be
  tested without a human driving the game.

.DESCRIPTION
  Kills any running instance, archives ERVR.log, launches eldenring.exe directly (ReShade as
  dxgi.dll + ERVR as dinput8.dll load themselves), waits, kills it, then prints the log lines
  you asked for.

  NOTE: this takes over the screen for the duration, and with -AutoPilot it presses buttons in
  the game. It cannot test anything in-headset: OpenXR only comes up when the headset is
  actually streaming, so unattended runs exercise the flat path, the hooks and the M2 probe.

.PARAMETER Seconds     How long to let the game run before killing it (default 90).
.PARAMETER AutoPilot   Seconds of scripted A-presses (needs [Experimental] AutoPilotSeconds).
.PARAMETER Filter      Regex to grep the log for afterwards. Default: the interesting tags.
.PARAMETER Tail        Print the last N lines instead of a filtered view.

.EXAMPLE
  .\tools\run_game.ps1 -Seconds 120 -Filter '\[m2\]'
  .\tools\run_game.ps1 -Seconds 180 -AutoPilot 60 -Filter '\[stereo\]|\[xr\]'
#>
param(
    [int]$Seconds = 90,
    [int]$AutoPilot = 0,
    [int]$Shots = 0,          # screenshot interval in seconds; 0 = none
    [string]$ShotDir = "$env:TEMP\claude\ervr_shots",
    [string]$GameDir = "${env:ProgramFiles(x86)}\Steam\steamapps\common\ELDEN RING\Game",
    [string]$Filter = '\[m2\]|\[xr\]|\[stereo\]|\[blit\]|ERROR|WARN',
    [int]$Tail = 0
)
$ErrorActionPreference = "Stop"

$exe = Join-Path $GameDir "eldenring.exe"
$ervr = Join-Path $GameDir "ERVR"
$log = Join-Path $ervr "ERVR.log"
$ini = Join-Path $ervr "ERVR.ini"
if (-not (Test-Path $exe)) { throw "eldenring.exe not found in '$GameDir'" }

# Steam launches start_protected_game.exe, which on this install IS a copy of eldenring.exe,
# so the process that ends up running is usually NOT called "eldenring". Track both.
$GameProcNames = @('eldenring', 'start_protected_game')

function Get-GameProcs { Get-Process -Name $GameProcNames -ErrorAction SilentlyContinue }

function Stop-Game {
    foreach ($p in @(Get-GameProcs)) {
        Write-Host "  killing $($p.ProcessName).exe (pid $($p.Id))" -ForegroundColor DarkYellow
        try { $p.Kill(); $p.WaitForExit(10000) } catch {}
    }
}

# The autopilot lives in ERVR.ini, so set it there rather than passing it through.
if ($PSBoundParameters.ContainsKey('AutoPilot')) {
    $lines = Get-Content $ini
    if ($lines | Select-String -Pattern '^AutoPilotSeconds=') {
        $lines = $lines -replace '^AutoPilotSeconds=.*', "AutoPilotSeconds=$AutoPilot"
    } else {
        $lines = $lines | ForEach-Object {
            $_
            if ($_ -eq '[Experimental]') { "AutoPilotSeconds=$AutoPilot" }
        }
    }
    Set-Content -Path $ini -Value $lines -Encoding utf8
    Write-Host "AutoPilotSeconds=$AutoPilot" -ForegroundColor DarkGray
}

Stop-Game
if (Test-Path $log) { Move-Item -Force $log "$log.prev" }

Write-Host "launching eldenring.exe for ${Seconds}s ..." -ForegroundColor Cyan
# Do NOT track the process we start: eldenring.exe relaunches itself through Steam and the
# original exits within a second. Track the NAME, and note when the pid changes.
Start-Process -FilePath $exe -WorkingDirectory $GameDir | Out-Null
$deadline = (Get-Date).AddSeconds($Seconds)
$seen = @{}
$shotN = 0
$nextShot = (Get-Date).AddSeconds($Shots)
if ($Shots -gt 0) {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    New-Item -ItemType Directory -Force $ShotDir | Out-Null
    Get-ChildItem $ShotDir -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force
    Write-Host "  screenshots every ${Shots}s -> $ShotDir" -ForegroundColor DarkGray
}
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    foreach ($proc in @(Get-GameProcs)) {
        if (-not $seen.ContainsKey($proc.Id)) {
            $seen[$proc.Id] = $true
            Write-Host "  $($proc.ProcessName).exe pid $($proc.Id) running" -ForegroundColor DarkGray
        }
    }
    if ($Shots -gt 0 -and (Get-Date) -ge $nextShot) {
        $nextShot = (Get-Date).AddSeconds($Shots)
        $shotN++
        try {
            $scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
            $full = New-Object System.Drawing.Bitmap $scr.Width, $scr.Height
            $gfx = [System.Drawing.Graphics]::FromImage($full)
            $gfx.CopyFromScreen(0, 0, 0, 0, $full.Size)
            $gfx.Dispose()
            # Downscale: these get read back by eye, and full-res PNGs are needlessly large.
            $w = 960; $h = [int]($scr.Height * 960 / $scr.Width)
            $small = New-Object System.Drawing.Bitmap $w, $h
            $g2 = [System.Drawing.Graphics]::FromImage($small)
            $g2.DrawImage($full, 0, 0, $w, $h); $g2.Dispose()
            $name = "{0:d3}_{1}.png" -f $shotN, (Get-Date -Format 'HHmmss')
            $small.Save((Join-Path $ShotDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
            $small.Dispose(); $full.Dispose()
        } catch { Write-Host "  screenshot failed: $_" -ForegroundColor Yellow }
    }
}
if ($seen.Count -eq 0) { Write-Host "  WARNING: no game process was ever seen" -ForegroundColor Yellow }
Stop-Game
Start-Sleep -Milliseconds 500
if ($Shots -gt 0) { Write-Host "$shotN screenshots in $ShotDir" -ForegroundColor Green }

if (-not (Test-Path $log)) { throw "no ERVR.log was produced - did the mod load?" }
$size = (Get-Item $log).Length
Write-Host "`nERVR.log: $size bytes" -ForegroundColor Green
if ($Tail -gt 0) { Get-Content $log -Tail $Tail }
else { Get-Content $log | Select-String -Pattern $Filter | ForEach-Object { $_.Line } }
