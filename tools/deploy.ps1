# Copies dist\ into the game folder. Keeps an existing settings.ini. Refuses while the game runs.
# Usage: powershell -File tools\deploy.ps1 [-GameRoot "D:\SteamLibrary\steamapps\common\ACE COMBAT 8"]
param([string]$GameRoot = "D:\SteamLibrary\steamapps\common\ACE COMBAT 8")

$ErrorActionPreference = "Stop"
$dist = Join-Path (Split-Path $PSScriptRoot -Parent) "dist"
if (-not (Test-Path (Join-Path $dist "ac8tweaks.exe"))) { throw "dist is empty, run cmake --install first" }
if (-not (Test-Path (Join-Path $GameRoot "Game\Binaries\Win64\AceCombat8.exe"))) { throw "game not found at $GameRoot" }
if (Get-Process AceCombat8 -ErrorAction SilentlyContinue) { throw "close the game first" }
# The launcher disarms a few seconds after the game exits; copying proxies while armed would leave both copies behind.
if (Get-Process start_protected_game -ErrorAction SilentlyContinue) { throw "the launcher is still running, wait a few seconds" }
if (Test-Path (Join-Path $GameRoot "AC8Tweaks\armed.state")) { throw "a session is still armed, wait for the launcher to disarm" }

$keep = Join-Path $GameRoot "AC8Tweaks\settings.ini"
$hadSettings = Test-Path $keep
if ($hadSettings) { $saved = Get-Content $keep -Raw }

# Scripts that are no longer part of the mod must not stay behind in the game.
$scripts = Join-Path $GameRoot "Game\Binaries\Win64\ue4ss\Mods\AC8Tweaks"
if (Test-Path $scripts) { Remove-Item $scripts -Recurse -Force }

Copy-Item (Join-Path $dist "*") $GameRoot -Recurse -Force

if ($hadSettings) { Set-Content $keep $saved -NoNewline }

# Offline mode active: the stand-in launcher must be the build that was just copied.
if (Test-Path (Join-Path $GameRoot "start_protected_game.exe.original")) {
    Copy-Item (Join-Path $dist "ac8tweaks.exe") (Join-Path $GameRoot "start_protected_game.exe") -Force
    "deployed, offline mode stays on"
} else {
    "deployed, run ac8tweaks.exe in the game folder to switch to offline mode"
}
