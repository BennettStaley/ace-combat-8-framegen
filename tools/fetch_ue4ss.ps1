# Downloads the pinned UE4SS build listed in third_party/ue4ss/SOURCE.txt, verifies its hash,
# and lays it out the way the drop-in folder expects (proxy stored disabled as dwmapi.dll.off).
# Usage: powershell -File tools\fetch_ue4ss.ps1 [-Destination <folder>]
param([string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) "third_party\ue4ss"))

$ErrorActionPreference = "Stop"
$source = Get-Content (Join-Path (Split-Path $PSScriptRoot -Parent) "third_party\ue4ss\SOURCE.txt")
$url = ($source | Where-Object { $_ -match '^https://' }).Trim()
$sha = (($source | Where-Object { $_ -match '^sha256 ' }) -replace '^sha256 ', '').Trim()
if (-not $url -or -not $sha) { throw "SOURCE.txt must list the download URL and its sha256" }

$zip = Join-Path $env:TEMP "ue4ss-pinned.zip"
Invoke-WebRequest -Uri $url -OutFile $zip
$actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
if ($actual -ne $sha.ToLower()) { Remove-Item $zip; throw "hash mismatch: expected $sha, got $actual" }

$stage = Join-Path $env:TEMP "ue4ss-pinned"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
Expand-Archive $zip $stage
New-Item -ItemType Directory -Force $Destination | Out-Null
if (Test-Path (Join-Path $Destination "ue4ss")) { Remove-Item (Join-Path $Destination "ue4ss") -Recurse -Force }
Move-Item (Join-Path $stage "ue4ss") (Join-Path $Destination "ue4ss")
Move-Item (Join-Path $stage "dwmapi.dll") (Join-Path $Destination "dwmapi.dll.off") -Force
Remove-Item $stage -Recurse -Force
Remove-Item $zip
"UE4SS ready in $Destination"
