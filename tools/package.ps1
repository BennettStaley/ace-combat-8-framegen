# Zips the assembled drop-in folder into a release archive.
# Usage: powershell -File tools\package.ps1 -Version 0.1.0
param([Parameter(Mandatory = $true)][string]$Version)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$dist = Join-Path $root "dist"
if (-not (Test-Path (Join-Path $dist "ac8tweaks.exe"))) { throw "dist is empty, run cmake --install first" }
$zip = Join-Path $root "AC8Tweaks-v$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zip
"$zip"
