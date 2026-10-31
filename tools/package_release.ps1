<#
.SYNOPSIS
  Assemble the redistributable zip exactly as it is published (GitHub Releases and Nexus Mods).
.DESCRIPTION
  Takes build\dist (run .\build.ps1 first, or pass -Build), lays it out as the contents of the
  ELDEN RING\Game folder, puts the documentation under ERVR\ so the game folder root only gains
  the DLLs it needs, drops stray logs, zips it, and writes a .sha256 next to the zip.

  Layout inside the zip:
    ERVR Launcher.exe       finds the game, installs the files below into it, starts it with EAC off
    dxgi.dll                ReShade 6.8.0 add-on build (unmodified)
    ReShade.ini
    dinput8.dll             proxy loader
    LICENSE-ReShade.txt
    ERVR\ERVR.dll
    ERVR\ERVR.ini
    ERVR\README.md  ERVR\CHANGELOG.md  ERVR\LICENSE.txt  ERVR\THIRD_PARTY.md
.PARAMETER Version   Version string for the file name (default: read from CMakeLists.txt).
.PARAMETER OutDir    Where to put the zip (default build\release).
.PARAMETER Build     Run .\build.ps1 first.
.EXAMPLE
  .\tools\package_release.ps1 -Build
  .\tools\package_release.ps1 -Version 0.4.0
#>
param(
    [string]$Version = "",
    [string]$OutDir = "",
    [switch]$Build
)
$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$dist = Join-Path $root "build\dist"
if (-not $OutDir) { $OutDir = Join-Path $root "build\release" }

if ($Build) { & (Join-Path $root "build.ps1"); if ($LASTEXITCODE) { throw "build failed" } }

if (-not $Version) {
    $m = [regex]::Match((Get-Content (Join-Path $root "CMakeLists.txt") -Raw), 'project\(\S+\s+VERSION\s+([0-9.]+)')
    if (-not $m.Success) { throw "could not read the version from CMakeLists.txt; pass -Version" }
    $Version = $m.Groups[1].Value
}

$required = @("ERVR Launcher.exe", "dxgi.dll", "ReShade.ini", "dinput8.dll", "LICENSE-ReShade.txt", "ERVR\ERVR.dll", "ERVR\ERVR.ini")
$missing = $required | Where-Object { -not (Test-Path (Join-Path $dist $_)) }
if ($missing) { throw "missing from build\dist: $($missing -join ', ') - run .\build.ps1" }

# The DLL must carry the version we are about to stamp on the file name.
$dll = Join-Path $dist "ERVR\ERVR.dll"
$stamp = (Get-Item $dll).VersionInfo.FileVersion
if ($stamp -ne $Version) { throw "ERVR.dll in build\dist is version '$stamp', not $Version - rebuild (.\build.ps1) before packaging" }
# The launcher installs what it is packed with; it must be the same release.
$launcherStamp = (Get-Item (Join-Path $dist "ERVR Launcher.exe")).VersionInfo.FileVersion
if ($launcherStamp -ne $Version) { throw "ERVR Launcher.exe in build\dist is version '$launcherStamp', not $Version - rebuild before packaging" }

$name  = "ERVR-v$Version"
$stage = Join-Path $OutDir "stage\$name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage "ERVR") | Out-Null

foreach ($f in $required) { Copy-Item (Join-Path $dist $f) (Join-Path $stage $f) }
Copy-Item (Join-Path $root "README.md")       (Join-Path $stage "ERVR\README.md")
Copy-Item (Join-Path $root "CHANGELOG.md")    (Join-Path $stage "ERVR\CHANGELOG.md")
Copy-Item (Join-Path $root "THIRD_PARTY.md")  (Join-Path $stage "ERVR\THIRD_PARTY.md")
Copy-Item (Join-Path $root "LICENSE")         (Join-Path $stage "ERVR\LICENSE.txt")

$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal
Remove-Item -Recurse -Force (Join-Path $OutDir "stage")

$sha = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$sha  $name.zip" | Set-Content -Encoding ascii "$zip.sha256"

Write-Host "`n$zip" -ForegroundColor Green
Write-Host "SHA256 $sha"
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::OpenRead($zip).Entries | Sort-Object FullName | ForEach-Object { "{0,10}  {1}" -f $_.Length, $_.FullName }
if ($env:GITHUB_OUTPUT) {
    "zip=$zip" | Out-File -Append $env:GITHUB_OUTPUT -Encoding utf8
    "sha=$sha" | Out-File -Append $env:GITHUB_OUTPUT -Encoding utf8
    "name=$name" | Out-File -Append $env:GITHUB_OUTPUT -Encoding utf8
}
