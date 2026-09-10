<#
.SYNOPSIS
Install the built VR DLL and synchronize an existing renamed game launcher with Steam's executable.
Requires the game to be closed. Backs up both replaced files in the workspace.
Use -CheckOnly to inspect the proposed update without changing anything.
#>
param(
    [string]$GameDir = "${env:ProgramFiles(x86)}\Steam\steamapps\common\ELDEN RING\Game",
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskGameRoot = (Resolve-Path -LiteralPath $GameDir).Path
$taskExe = Join-Path $taskGameRoot 'eldenring.exe'
$taskLauncher = Join-Path $taskGameRoot 'start_protected_game.exe'
$taskInstalledDll = Join-Path $taskGameRoot 'ERVR\ERVR.dll'
$taskBuiltDll = Join-Path $taskRoot 'build\Release\ERVR.dll'
$taskVersion = (Get-Item -LiteralPath $taskExe).VersionInfo.FileVersion
$taskLauncherVersion = (Get-Item -LiteralPath $taskLauncher).VersionInfo.FileVersion
if ($taskVersion -notin @('2.6.2.0', '2.7.1.0')) { throw "No verified VR address map for installed game version $taskVersion" }
if ($taskLauncherVersion -notin @('2.6.2.0', '2.7.1.0')) { throw 'The destination is not a recognized renamed game executable.' }
foreach ($taskFile in @($taskBuiltDll, $taskInstalledDll)) {
    if (!(Test-Path -LiteralPath $taskFile -PathType Leaf)) { throw "Missing file: $taskFile" }
}
[pscustomobject]@{InstalledVersion=$taskVersion; LauncherVersion=$taskLauncherVersion; NewDll=$taskBuiltDll; Destination=$taskGameRoot}
if ($CheckOnly) { return }
if (Get-Process -Name eldenring,start_protected_game -ErrorAction SilentlyContinue) { throw 'Close Elden Ring before installing.' }
$taskBackup = Join-Path $taskRoot ('build\backups\launcher-sync-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $taskBackup | Out-Null
Copy-Item -LiteralPath $taskLauncher -Destination (Join-Path $taskBackup 'start_protected_game.exe')
Copy-Item -LiteralPath $taskInstalledDll -Destination (Join-Path $taskBackup 'ERVR.dll')
# A newer game build may migrate save data on first load. Preserve the local
# vanilla saves before the first launch, alongside the executable/DLL backup.
$taskSaveRoot = Join-Path $env:APPDATA 'EldenRing'
if (Test-Path -LiteralPath $taskSaveRoot -PathType Container) {
    foreach ($taskSave in Get-ChildItem -LiteralPath $taskSaveRoot -Filter '*.sl2*' -File -Recurse) {
        $taskSaveDestination = Join-Path $taskBackup ('saves\' + $taskSave.Directory.Name)
        New-Item -ItemType Directory -Path $taskSaveDestination -Force | Out-Null
        Copy-Item -LiteralPath $taskSave.FullName -Destination (Join-Path $taskSaveDestination $taskSave.Name)
    }
}
try {
    Copy-Item -LiteralPath $taskBuiltDll -Destination $taskInstalledDll -Force
    Copy-Item -LiteralPath $taskExe -Destination $taskLauncher -Force
    if ((Get-FileHash -LiteralPath $taskExe).Hash -ne (Get-FileHash -LiteralPath $taskLauncher).Hash -or
        (Get-FileHash -LiteralPath $taskBuiltDll).Hash -ne (Get-FileHash -LiteralPath $taskInstalledDll).Hash) { throw 'Installed file verification failed.' }
} catch {
    Copy-Item -LiteralPath (Join-Path $taskBackup 'ERVR.dll') -Destination $taskInstalledDll -Force
    Copy-Item -LiteralPath (Join-Path $taskBackup 'start_protected_game.exe') -Destination $taskLauncher -Force
    throw
}
"Installed matching launcher and VR DLL. Backup: $taskBackup"
