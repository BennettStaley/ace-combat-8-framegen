# AC8 Tweaks

Field of view and DLSS frame generation control for ACE COMBAT 8: WINGS OF THEVE on PC,
changeable mid-mission with hotkeys. Offline single-player only.

Two things the game does not let you change: the field of view, and the DLSS frame generation
it ships with but never exposes. AC8 Tweaks gives you both. Set a separate field of view for the
cockpit, the HUD-only view and the external camera with two keys while flying. Turn frame
generation on at 2x, 3x or 4x, or let adaptive mode pick the multiplier for your monitor's
refresh rate. Every change shows on screen and is saved for the next launch.

## Features

- Field of view per view: cockpit, HUD-only first person and third person, each remembered separately, 40 to 130 degrees
- DLSS frame generation off, 2x, 3x or 4x, switchable while flying
- Adaptive mode: measures the base frame rate once a second and picks the smallest multiplier that reaches a target, by default the G-Sync cap for your monitor
- Reflex off, low latency, or low latency with boost
- Any Unreal console variable listed in one ini file, applied live and kept applied if the game changes it back
- On-screen text for every change, drawn by the game's own UI
- One settings file in the game folder. Edit it in Notepad while playing; changes apply within a second

<img width="5120" height="1440" alt="20260930191121_1" src="https://github.com/user-attachments/assets/68488bfe-aba4-4527-ac1b-4c00af4c1671" />
<img width="5120" height="1440" alt="20260930183712_1" src="https://github.com/user-attachments/assets/6bfa7738-5ffd-4964-9a76-6e68a2041f84" />
<img width="358" height="141" alt="image" src="https://github.com/user-attachments/assets/100cbe70-05ed-48ab-9a19-4ba6d7c00a8e" />


## Requirements

- Steam version of the game on Windows 11
- An RTX 40 series card for 2x, RTX 50 series for 3x and 4x
- Offline play. The mod never runs alongside Easy Anti-Cheat, so online modes are unavailable while it is on

## Install

1. Extract the release into the game folder, the one that contains `start_protected_game.exe`.
2. Run `ac8tweaks.exe`. It reports "Offline mode is ON".
3. Play from Steam as usual.

Run `ac8tweaks.exe` again to go back online. It restores the official launcher and disables every
mod file. Run it again after each game update, since updates put the official launcher back.

## Field of view

The game has three camera views and AC8 Tweaks keeps a value for each: `Cockpit`, `HUD` (first
person without the cockpit) and `ThirdPerson`. Press `=` or `-` while flying and the view you are
looking through moves 5 degrees and is saved. The other two views keep their own values. A view
still on the game's default starts from the angle currently on screen, so the first press is a
small nudge, not a jump. The on-screen text names the view and shows all three.

Values live under `[FOV]` in `settings.ini`, 40 to 130 degrees. Set a view to 0 to hand it back to
the game's own dynamic field of view. Edits in the file apply within a second, no restart.

## Hotkeys

| Key | Action |
| --- | --- |
| F7 | Cycle frame generation: off, 2x, 3x, 4x |
| F8 | Cycle Reflex: off, low latency, boost |
| F6 | Adaptive mode on or off. Off keeps the multiplier it had reached |
| = and - | Field of view of the view you are in, 5 degrees per press |
| F9 | Reload `settings.ini` and apply everything again |
| Tilde | The game's console, for typing any variable directly |

All keys are rebindable under `[Hotkeys]` in `AC8Tweaks\settings.ini`, with `CTRL+`, `SHIFT+` and
`ALT+` prefixes. An empty value unbinds.

## Settings

`AC8Tweaks\settings.ini` in the game folder, read at launch and whenever it changes.

| Section | What it holds |
| --- | --- |
| `[ConsoleVariables]` | Unreal console variables and their values. Frame generation, Reflex and anything else you add |
| `[Adaptive]` | `Enabled`, `TargetFps`, `HoldSeconds` between decisions, `ProbeSeconds` between attempts to step down when a frame cap hides headroom |
| `[FOV]` | `Cockpit`, `HUD`, `ThirdPerson` in degrees, 40 to 130. 0 keeps the game's own value |
| `[Overlay]` | `ToastSeconds`, how long the on-screen text stays |
| `[Hotkeys]` | Key for each action |

`AC8Tweaks\state.ini` next to it is written by the mod and shows what the engine actually reports,
plus the adaptive controller's current multiplier and base frame rate.

`AC8Tweaks\Engine.ini` holds the startup-only variables that must be in place before the game
initialises Streamline. It is copied over the game's `Engine.ini` for the session and the original
is put back on exit.

## Frame caps and G-Sync

Variable refresh only works while the frame rate stays below the monitor's refresh rate, and the
gap it needs grows with the refresh rate because it is really a fixed slice of frame time. The
cap NVIDIA Reflex applies with G-Sync is refresh minus refresh squared over 3600, which is what
`TargetFps=auto` uses:

| Refresh rate | Cap to use |
| --- | --- |
| 60 Hz | 59 |
| 100 Hz | 97 |
| 120 Hz | 116 |
| 144 Hz | 138 |
| 165 Hz | 157 |
| 175 Hz | 166 |
| 180 Hz | 171 |
| 200 Hz | 188 |
| 240 Hz | 224 |
| 280 Hz | 258 |
| 300 Hz | 275 |
| 360 Hz | 324 |
| 480 Hz | 416 |

Set the system-wide limit in the NVIDIA app (Max Frame Rate) to the value for your monitor, and
leave the in-game limit at or above it. The adaptive controller then aims at the same number, so
frame generation never pushes the output past the variable refresh range, and a limit that is
lower than the target never leaves the controller chasing a number it cannot reach. To aim at a
fixed offset instead, set `RefreshMargin` to a number and the target becomes refresh minus that.

The launcher records the refresh rate of the monitor the game window opens on in
`AC8Tweaks\display.ini`, so the target follows the monitor even on multi-display systems.

## How it works

`ac8tweaks.exe` stands in for the anti-cheat launcher. When Steam starts the game it enables the
mod files, starts `AceCombat8.exe` directly with the same arguments the publisher's own no-anti-cheat
launch option uses, waits until every game process has exited, and disables the mod files again.
Outside a running session no mod file is active and the config folder is untouched, so a game
update that restores the official launcher always starts a clean anti-cheat session. The game
executable is never modified.

Inside the game, UE4SS runs a Lua script that talks to the engine through its own reflection
system: console variables through the Kismet library, the field of view through the player
controller, and the on-screen text through a UMG widget. No memory patching, no signatures of
our own.

## Building

Visual Studio 2022 with its bundled CMake, Python 3 with `lupa` for the Lua tests.

```
powershell -File tools\fetch_ue4ss.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
cmake --install build --config Release --prefix dist
```

`dist` is the drop-in folder. `tools\deploy.ps1` copies it into the game and keeps your settings.

Tests: the launcher self-test builds a fake install in a temp folder and checks that every mix of
toggle, arm, crash and disarm leaves the install byte-for-byte as it was. The Lua tests run the
real scripts under Lua 5.4 with the engine faked, including a simulated GPU for the adaptive
controller.

## Credits

- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) for the scripting runtime, MIT licensed
- emoose, whose DLSS Framegen Enabler showed which startup variable the game needs
- 4598llj, whose FOV Change mod found how the game selects its camera view and sets the field of view
- techiew's Elden Ring EAC toggler for the launcher stand-in approach

## Disclaimer

Single-player use only. Do not attempt online play with the mod active. Not affiliated with
Bandai Namco or NVIDIA.
