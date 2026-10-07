# AC8 Tweaks

Field of view and DLSS frame generation control for ACE COMBAT 8: WINGS OF THEVE on PC, from an
in-game menu. Offline single-player only.

Two things the game does not let you change: the field of view, and the DLSS frame generation
it ships with but never exposes. AC8 Tweaks gives you both. Press F10 while flying and a menu
opens over the game: set a separate field of view for the cockpit, the HUD-only view and the
external camera, turn frame generation on at 2x, 3x or 4x, or let adaptive mode pick the
multiplier for your monitor's refresh rate. Every change applies at once and is saved for the
next launch.

## Features

- An in-game menu on F10, worked with the mouse or the keyboard. No hotkeys to remember, no files to edit
- Field of view per view: cockpit, HUD-only first person and third person, each remembered separately, 40 to 130 degrees
- DLSS frame generation off, 2x, 3x or 4x, switchable while flying
- The DLSS super resolution model: the game's own CNN or NVIDIA's transformer models, with a line on what each one is, switchable while flying
- Adaptive mode: measures the base frame rate once a second and picks the smallest multiplier that reaches a target, by default the G-Sync cap for your monitor
- Reflex off, low latency, or low latency with boost
- Optional quick keys for frame generation, adaptive mode, Reflex and field of view, picked in the menu by pressing the key you want
- One settings file in the game folder for those who prefer it. Edit it in Notepad while playing; changes apply within a second
- Any Unreal console variable listed in that file, applied live and kept applied if the game changes it back

<img width="5120" height="1440" alt="20261004144026_1" src="https://github.com/user-attachments/assets/75f5f7a1-77a9-4529-bbdf-f2e723ce8b5e" />
<img width="514" height="363" alt="image" src="https://github.com/user-attachments/assets/0b0ccaa1-bf08-405f-a885-cac71cdfd5bc" />
<img width="5120" height="1440" alt="20260930191124_1" src="https://github.com/user-attachments/assets/52bac981-c2ca-4e62-90b1-f07c2231f4b0" />


## Requirements

- The Steam version of the game. Any Windows version the game itself runs on
- An RTX 40 series card for 2x, RTX 50 series for 3x and 4x
- Offline play. The mod never runs alongside Easy Anti-Cheat, so online modes are unavailable while it is on

Nothing else needs installing. The release contains everything, including the UE4SS scripting
runtime it runs on.

## Install

1. Extract the release into the game folder, the one that contains `start_protected_game.exe`.
2. Run `ac8tweaks.exe`. It reports "Offline mode is ON".
3. Play from Steam as usual, and press F10 in the game.

Run `ac8tweaks.exe` again to go back online. It restores the official launcher and disables every
mod file. Run it again after each game update, since updates put the official launcher back.

If you installed UE4SS or another field of view mod into the game folder by hand before, remove it
first, or use Steam's "Verify integrity of game files", so that only one copy is in there.

Updating from an earlier release: extract the new one over it. That replaces `settings.ini`, so
note your values first or set them again in the menu.

## The menu

F10 opens and closes it. Use the mouse, or the arrow keys with Space and Enter. While it is open
the keys and clicks it uses do not reach the game; a gamepad keeps flying the aircraft.

| Group | What is in it |
| --- | --- |
| Graphics | Frame generation, adaptive mode and its target, Reflex, the DLSS model, and the field of view of each of the three views |
| Keys | The key that opens the menu, and the optional quick keys |
| Menu | How the menu is drawn on an HDR screen, and how bright |

Everything in the menu is written to `settings.ini` as you change it.

## Field of view

The game has three camera views and AC8 Tweaks keeps a value for each: cockpit, HUD (first person
without the cockpit) and third person. Each has its own row in the menu, 40 to 130 degrees. Set a
view to 0 to hand it back to the game's own dynamic field of view.

The zoom on a target you hold focus on still works with your own value: the view narrows by the
same proportion as it does in the unmodified game.

## DLSS model

DLSS super resolution has several neural network models, which NVIDIA names by letter. The game
ships DLSS 310.2.1 and leaves the choice to it, which gives the older convolutional (CNN) models.
The transformer models, introduced with DLSS 4, resolve more detail and ghost less at a higher
GPU cost. The DLSS model row in the Graphics group picks one; the line under it says what the
chosen model is. The change applies at once, on the next frame, so two models can be compared on
the same scene. It only matters while the game's anti-aliasing option is DLSS.

| Model | What it is |
| --- | --- |
| default | The game's own choice. CNN with the DLSS the game ships |
| A | CNN. The oldest, least ghosting of the CNN set |
| B | CNN. A tuned for ultra performance |
| C | CNN. Favours the current frame: less ghosting, less stable |
| D | CNN. Favours past frames: more stable, more ghosting |
| E | CNN. An improved D, the usual CNN default |
| F | CNN. The CNN default for ultra performance and DLAA |
| J | Transformer. The first DLSS 4 model: less ghosting than K, some flicker |
| K | Transformer. Best image quality, costs the most |
| L | Second generation transformer for ultra performance. Needs DLSS 310.5 or newer |
| M | Second generation transformer for performance mode. Needs DLSS 310.5 or newer |

The descriptions follow NVIDIA's own notes on each model. K is the one to try first. L and M came
with DLSS 4.5 and are not in the DLSS the game ships: they need a newer `nvngx_dlss.dll`, which
the NVIDIA app's DLSS override or DLSS Swapper can put in place. Asked for a model its DLSS does
not have, the game falls back to the default. If the NVIDIA app's model preset override is set
for the game, that override wins over the choice here, so leave it on default to choose from the
menu.

In `settings.ini` the model is `r.NGX.DLSS.Preset` under `[ConsoleVariables]`, by NVIDIA's
number: 0 default, 1 to 6 for A to F, 10 to 13 for J to M.

## Keys

Out of the box the mod uses one key, F10. To change it, open the Keys group, press the button
next to "Open and close this menu" and then the key you want. CTRL, SHIFT and ALT can be held
with it.

The same group has quick keys for use without opening the menu. They start out unset:

| Quick key | What it does |
| --- | --- |
| Frame generation | Steps through off, 2x, 3x, 4x |
| Adaptive frame generation | On or off. Off keeps the multiplier it had reached |
| Reflex | Steps through off, low latency, boost |
| DLSS model | Steps through default, A to F, J to M, and shows what the model is |
| Field of view up and down | Moves the view you are looking through by 5 degrees and saves it. The other two views keep their own values |

Pressing a quick key shows what it did in the corner of the screen for a few seconds. Backspace
while picking takes a key away again, Esc keeps the old one.

The tilde key opens the game's own console, for typing any variable directly.

## Settings

`AC8Tweaks\settings.ini` in the game folder, read at launch and whenever it changes. The menu
writes the same file, so the two never disagree.

| Section | What it holds |
| --- | --- |
| `[ConsoleVariables]` | Unreal console variables and their values. Frame generation, Reflex, the DLSS model and anything else you add |
| `[Adaptive]` | `Enabled`, `TargetFps`, `HoldSeconds` between decisions, `ProbeSeconds` between attempts to step down when a frame cap hides headroom |
| `[FOV]` | `Cockpit`, `HUD`, `ThirdPerson` in degrees, 40 to 130. 0 keeps the game's own value |
| `[Keys]` | `Menu` and the quick keys, by name: `F7`, `CTRL+F7`, `NUM_ZERO`, `OEM_PLUS`. Empty means no key |
| `[Menu]` | `Enabled`, and `Hdr` and `Nits` for how the menu is drawn on an HDR screen |

So frame generation needs no key at all: whatever `[ConsoleVariables]` and `[Adaptive]` say is
applied every time the game starts.

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

## Questions

**Do I need UE4SS or any other mod?** No. The release is complete.

**Does it need Windows 11?** The mod has no requirement of its own. If the game runs, it runs.

**Can I play online with it?** No. While offline mode is on, the game starts without Easy
Anti-Cheat and online modes do not work. Run `ac8tweaks.exe` to switch back, which also disables
every mod file.

**The menu does not open.** `AC8Tweaks\overlay.log` says what happened. If another program
already uses F10, pick a different key under `[Keys]` in `settings.ini`. To run without the menu
altogether, set `Enabled=0` under `[Menu]`: the quick keys in the file still work, read once when
the game starts.

**Windows Security removed `start_protected_game.exe`.** It has flagged the launcher on at least
one machine as `Trojan:Win32/Bearfoos.A!ml`. That is a machine-learning guess and a false positive;
the launcher's source is in this repository. If it happens while the game is running, the mod
files are still switched on afterwards, so do not press Play. Use Steam's "Verify integrity of
game files" to get the official launcher back, then run `ac8tweaks.exe`: once to carry on playing
offline, a second time if you want to go back online. Either way the mod files are put back in
order. Allow the file in your antivirus to keep it from happening again.


## How it works

`ac8tweaks.exe` stands in for the anti-cheat launcher. When Steam starts the game it enables the
mod files, starts `AceCombat8.exe` directly with the same arguments the publisher's own no-anti-cheat
launch option uses, waits until every game process has exited, and disables the mod files again.
Outside a running session no mod file is active and the config folder is untouched, so a game
update that restores the official launcher always starts a clean anti-cheat session. The game
executable is never modified.

Inside the game, UE4SS runs a Lua script that talks to the engine through its own reflection
system: console variables through the Kismet library and the field of view through the player
controller. The menu is a small DLL the script loads. It draws with Dear ImGui onto the frames the
game presents, generated ones included, by hooking the Present call of the system's swap chain,
and it draws nothing while the menu is closed. The game's own code is not patched and nothing
depends on signatures of our own.

## Building

Visual Studio 2022 with its bundled CMake, Python 3 with `lupa` and `pillow` for the tests.

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
controller. The menu test loads the real DLL into a D3D12 swap chain, works every kind of row
from the keyboard and the mouse, and checks the pixels it drew in SDR, HDR10 and scRGB.

## Credits

- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) for the scripting runtime, MIT licensed
- [Dear ImGui](https://github.com/ocornut/imgui) for the menu, MIT licensed
- emoose, whose DLSS Framegen Enabler showed which startup variable the game needs
- 4598llj, whose FOV Change mod found how the game selects its camera view and sets the field of view
- techiew's Elden Ring EAC toggler for the launcher stand-in approach

## Disclaimer

Single-player use only. Do not attempt online play with the mod active. Not affiliated with
Bandai Namco or NVIDIA.
