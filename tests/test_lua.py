"""Runs the Lua core under a real Lua 5.4 with UE4SS and the engine faked.

Requires: pip install lupa
Run:      python tests/test_lua.py
"""
import os
import pathlib
import sys
import tempfile

from lupa.lua54 import LuaRuntime

ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = pathlib.Path(os.environ.get("AC8_MAIN", ROOT / "mod" / "ue4ss" / "Mods" / "AC8Tweaks" / "Scripts" / "main.lua"))

FG_ENABLE = "r.Streamline.DLSSG.Enable"
FG_FRAMES = "r.Streamline.DLSSG.FramesToGenerate"

FAKES = r"""
FAKE = { store = {}, execs = {}, pc_valid = true, unsettable = {}, frame = 0, time = 0 }
Key = { F6 = 6, F7 = 7, F8 = 8, F9 = 9, A = 65, OEM_PLUS = 187, OEM_MINUS = 189 }
ModifierKey = { CONTROL = 1, SHIFT = 2, ALT = 3 }
BINDS, LOOPS, LOG = {}, {}, {}
print = function(...) LOG[#LOG + 1] = table.concat({...}, " ") end
RegisterKeyBind = function(key, a, b) BINDS[key] = b or a end
ExecuteInGameThread = function(fn) fn() end
LoopInGameThreadWithDelay = function(ms, fn) LOOPS[#LOOPS + 1] = fn return #LOOPS end
FRAME_LOOPS, NOTIFY = {}, {}
LoopInGameThreadAfterFrames = function(n, fn) FRAME_LOOPS[#FRAME_LOOPS + 1] = fn return #FRAME_LOOPS end
NotifyOnNewObject = function(class, cb) NOTIFY[class] = cb end

local lib = {}
function lib:ExecuteConsoleCommand(ctx, command, pc)
    FAKE.execs[#FAKE.execs + 1] = command
    local name, value = command:match("^(%S+)%s+(%S+)$")
    if name and not FAKE.unsettable[name] then FAKE.store[name] = tonumber(value) end
end
function lib:GetConsoleVariableIntValue(name) return math.floor(FAKE.store[name] or 0) end
function lib:GetConsoleVariableFloatValue(name) return FAKE.store[name] or 0.0 end
function lib:GetFrameCount() return FAKE.frame end
local stats = {}
function stats:GetRealTimeSeconds(ctx) return FAKE.time end
local pc = { IsValid = function() return FAKE.pc_valid end }
package.preload["UEHelpers"] = function()
    return {
        GetKismetSystemLibrary = function() return lib end,
        GetGameplayStatics = function() return stats end,
        GetPlayerController = function() return pc end,
        GetGameViewportClient = function() return { IsValid = function() return false end } end,
    }
end

-- camera objects the fov module walks
local function obj(t) t.IsValid = function() return t.valid ~= false end return t end
FAKE.cam = { Cockpit = obj({ bIsActive = false }), HUD = obj({ bIsActive = false }), ThirdPerson = obj({ bIsActive = false }) }
FAKE.plane = obj({})
FAKE.view = obj({ CachedCockpitCamera = FAKE.cam.Cockpit, CachedFirstPersonCamera = FAKE.cam.HUD,
                  CachedThirdPersonCamera = FAKE.cam.ThirdPerson, GetOwner = function(self) return FAKE.plane end })
FAKE.fov, FAKE.fov_calls = 90, {}
FAKE.pc = obj({ GetViewTarget = function(self) return FAKE.plane end,
                FOV = function(self, v) FAKE.fov_calls[#FAKE.fov_calls + 1] = v; FAKE.fov = v > 0 and v or 90 end })
FAKE.manager = obj({ PCOwner = FAKE.pc, GetFOVAngle = function(self) return FAKE.fov end })
FindAllOf = function(class)
    if class == "LiveCameraViewComponent" then return { FAKE.view } end
    if class == "LivePlayerCameraManager" then return { FAKE.manager } end
end

-- UMG pieces the hud module builds
HUD = { texts = {}, visibility = {}, delays = {} }
local classes = { ["/Script/UMG.UserWidget"] = obj({}), ["/Script/UMG.TextBlock"] = obj({}) }
local widget = obj({ WidgetTree = obj({}),
    AddToViewport = function(self, z) HUD.added = z end,
    SetPositionInViewport = function(self, pos, dpi) HUD.pos = pos end,
    SetVisibility = function(self, v) HUD.visibility[#HUD.visibility + 1] = v end })
local umg = obj({ Create = function(self, ctx, class, owner) if HUD.create_fails then return obj({ valid = false }) end return widget end })
StaticFindObject = function(path)
    if path == "/Script/UMG.Default__WidgetBlueprintLibrary" then return umg end
    return classes[path] or obj({ valid = false })
end
StaticConstructObject = function(class, outer)
    return obj({ SetText = function(self, t) HUD.texts[#HUD.texts + 1] = t end,
                 SetShadowOffset = function() end, SetShadowColorAndOpacity = function() end })
end
FText = function(s) return s end
ExecuteWithDelay = function(ms, fn) HUD.delays[#HUD.delays + 1] = { ms = ms, fn = fn } end
"""

SETTINGS = """; test settings
[ConsoleVariables]
; frame generation
r.Streamline.DLSSG.Enable=1
r.Streamline.DLSSG.FramesToGenerate=1
t.Streamline.Reflex.Mode=1
r.Streamline.DeepDVC.Intensity=0.5

[Hotkeys]
CycleFrameGeneration=F7
CycleReflex=CTRL+F8
ReloadSettings=F9
"""

ADAPTIVE = SETTINGS + """
[Adaptive]
Enabled=1
TargetFps=240
HoldSeconds=3
ProbeSeconds=0
"""

FOV = SETTINGS + """
[FOV]
Cockpit=100
HUD=0
ThirdPerson=120
"""

failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print(f"    FAIL {msg}")


def boot(tmp, offline=True, settings=SETTINGS):
    os.environ["AC8TWEAKS_OFFLINE"] = "1" if offline else "0"
    os.environ["AC8TWEAKS_ROOT"] = str(tmp)
    (tmp / "AC8Tweaks").mkdir(exist_ok=True)
    if settings is not None:
        (tmp / "AC8Tweaks" / "settings.ini").write_text(settings)
    L = LuaRuntime(unpack_returned_tuples=True)
    L.execute("AC8TWEAKS_TEST = true")
    L.execute(f"AC8TWEAKS_SCRIPT_DIR = {MAIN.parent.as_posix() + '/'!r}")
    L.execute(FAKES)
    M = L.execute(MAIN.read_text())
    return L, M


def tick(L):
    L.eval("LOOPS[1]")()


def frame(L):
    L.eval("FRAME_LOOPS[1]")()


def store(L, name):
    return L.eval("FAKE.store")[name]


def execs(L):
    return len(L.eval("FAKE.execs"))


def fov_calls(L):
    return list(L.eval("FAKE.fov_calls").values())


def hud_texts(L):
    return list(L.eval("HUD.texts").values())


def hud_vis(L):
    return list(L.eval("HUD.visibility").values())


def settings_text(tmp):
    return (tmp / "AC8Tweaks" / "settings.ini").read_text()


def multiplier(L):
    return 1 if not store(L, FG_ENABLE) else int(store(L, FG_FRAMES) or 1) + 1


def gpu(m, uncapped, cap, cost=0.08):
    """Base frame rate a GPU delivers at multiplier m: generation costs a little, the cap hides headroom."""
    base = uncapped / (1 + cost * (m - 1))
    return min(base, cap / m) if cap else base


def advance(L, uncapped, cap):
    """One simulated second of play at the multiplier the engine currently holds, then a tick."""
    L.execute(f"FAKE.frame = FAKE.frame + {gpu(multiplier(L), uncapped, cap)}; FAKE.time = FAKE.time + 1")
    L.execute(f"FAKE.store['t.MaxFPS'] = {cap}")
    tick(L)


def test_pure(tmp):
    L, M = boot(tmp)
    ini = M.parse_ini("[A]\nx = 1\n; c=2\ny=two words \n[B]\nz=3")
    check(ini["A"]["x"] == "1" and ini["A"]["y"] == "two words" and ini["B"]["z"] == "3", "parse_ini values")
    check(ini["A"]["; c"] is None, "parse_ini skips comments")
    text = "; head\n[A]\nx=1\n\n[B]\ny=2\n"
    check(M.update_ini(text, "A", "x", "9") == "; head\n[A]\nx=9\n\n[B]\ny=2\n", "update_ini replaces in place")
    check(M.update_ini(text, "A", "new", "5") == "; head\n[A]\nx=1\nnew=5\n\n[B]\ny=2\n", "update_ini appends to section")
    check(M.update_ini(text, "C", "k", "v") == "; head\n[A]\nx=1\n\n[B]\ny=2\n\n[C]\nk=v\n", "update_ini adds section")
    check(M.update_ini("", "A", "k", "v") == "\n[A]\nk=v\n", "update_ini from empty")
    check(M.update_ini("[A]\r\nx=1\r\n", "A", "x", "2") == "[A]\nx=2\n", "update_ini handles CRLF")
    seq, e, f = [], 0, 3
    for _ in range(5):
        e, f = M.next_frame_generation(e, f)
        seq.append((e, f))
    check(seq == [(1, 1), (1, 2), (1, 3), (0, 3), (1, 1)], f"frame generation cycle {seq}")
    k, mods = M.parse_key("ctrl+Shift+F7")
    check(k == "F7" and list(mods.values()) == ["CONTROL", "SHIFT"], "parse_key")
    k, mods = M.parse_key("F9")
    check(k == "F9" and len(mods) == 0, "parse_key no modifiers")
    check(M.same("1", "1.0") and not M.same("1", "2") and M.same("0.5", 0.5), "same")
    cfg = M.parse_fov(L.table(Cockpit="200", HUD="10", ThirdPerson="abc"))
    check((cfg.Cockpit, cfg.HUD, cfg.ThirdPerson) == (130, 40, 0), "fov values clamp to 40..130, junk means off")


def run_controller(M, L, uncapped, cap, seconds, start=1, hold=3, probe=10, target=240):
    """Drives adaptive_step against the simulated GPU. Returns the multiplier per second."""
    state = L.table(m=start)
    trace = []
    for _ in range(seconds):
        base = gpu(state.m, uncapped, cap)
        trace.append(M.adaptive_step(state, base, target, cap, hold, probe))
    return trace


def test_adaptive_controller(tmp):
    L, M = boot(tmp)
    t = run_controller(M, L, uncapped=100, cap=0, seconds=40)
    check(t[-1] == 3 and 4 not in t, f"base 100 settles on 3x, never 4x: {t}")
    check(t[:2] == [1, 1] and t[2] == 2 and t[5] == 3, f"steps up once per hold period: {t[:8]}")
    t = run_controller(M, L, uncapped=300, cap=0, seconds=20, start=3)
    check(t[-1] == 1 and t[2] == 1, f"base above target switches off: {t}")
    t = run_controller(M, L, uncapped=55, cap=0, seconds=20)
    check(t[-1] == 4, f"base 55 needs 4x: {t}")
    t = run_controller(M, L, uncapped=100, cap=244, seconds=60, start=3)
    probes = [i for i in range(1, len(t)) if t[i] == 2 and t[i - 1] == 3]
    check(t[-1] == 3 and probes and all(t[p + 3] == 3 for p in probes if p + 3 < len(t)), f"capped at 3x: probes down, comes back: {t}")
    check(len(probes) >= 2 and probes[1] - probes[0] > 10, f"failed probe doubles the wait: {probes}")
    t = run_controller(M, L, uncapped=200, cap=244, seconds=40)
    check(t[-1] == 2 and 3 not in t, f"headroom under the cap stays at 2x: {t}")
    t = run_controller(M, L, uncapped=100, cap=0, seconds=40, probe=0, start=4)
    check(t[-1] == 3 and 4 not in t[3:], f"probe=0: steps down without probing when the base covers 3x: {t}")
    t = run_controller(M, L, uncapped=100, cap=244, seconds=40, probe=0, start=4)
    check(t[-1] == 4, f"probe=0 under a cap: headroom unknown, stays put: {t}")
    state = L.table(m=3)
    check(M.adaptive_step(state, None, 240, 0, 3, 10) == 3 and M.adaptive_step(state, 0, 240, 0, 3, 10) == 3, "no measurement keeps the multiplier")


def test_guard(tmp):
    L, M = boot(tmp, offline=False)
    check(M.main() is False, "inactive when not launched offline")
    check(len(L.eval("LOOPS")) == 0 and len(L.eval("BINDS")) == 0, "nothing registered when inactive")


def test_apply_and_state(tmp):
    L, M = boot(tmp)
    check(M.main() is True, "active")
    check(len(L.eval("LOOPS")) == 1, "one loop registered")
    check(set(L.eval("BINDS").keys()) == {6, 7, 8, 9, 187, 189}, "six hotkeys bound, defaults fill the gaps")
    tick(L)
    check(store(L, FG_ENABLE) == 1, "int applied")
    check(store(L, "r.Streamline.DeepDVC.Intensity") == 0.5, "float applied")
    check(execs(L) == 4, f"one command per variable, got {execs(L)}")
    state = (tmp / "AC8Tweaks" / "state.ini").read_text()
    check("r.Streamline.DLSSG.FramesToGenerate=1" in state and "r.Streamline.DeepDVC.Intensity=0.5" in state, "state.ini written")
    check("[Adaptive]" not in state, "no adaptive block while off")
    tick(L)
    check(execs(L) == 4, "nothing re-sent when values match")


def test_enforce_drift(tmp):
    L, M = boot(tmp)
    M.main()
    tick(L)
    L.execute(f"FAKE.store['{FG_ENABLE}'] = 0")  # the game switched it off
    tick(L)
    check(store(L, FG_ENABLE) == 1, "drifted value pushed back")
    check(execs(L) == 5, f"exactly one extra command, got {execs(L)}")


def test_live_edit(tmp):
    L, M = boot(tmp)
    M.main()
    tick(L)
    path = tmp / "AC8Tweaks" / "settings.ini"
    path.write_text(path.read_text().replace("FramesToGenerate=1", "FramesToGenerate=3"))
    tick(L)
    check(store(L, FG_FRAMES) == 3, "edit on disk applied")
    check("FramesToGenerate=3" in (tmp / "AC8Tweaks" / "state.ini").read_text(), "state follows")


def test_hotkeys(tmp):
    L, M = boot(tmp)
    M.main()
    tick(L)
    f7 = L.eval("BINDS[7]")
    seen = []
    for _ in range(4):
        f7()
        seen.append((store(L, FG_ENABLE), store(L, FG_FRAMES)))
    check(seen == [(1, 2), (1, 3), (0, 3), (1, 1)], f"F7 cycle {seen}")
    text = settings_text(tmp)
    check(f"{FG_ENABLE}=1\n" in text and "FramesToGenerate=1\n" in text, "hotkey persisted to settings.ini")
    check(text.startswith("; test settings\n[ConsoleVariables]\n; frame generation\n"), "comments and order kept")
    tick(L)
    check(store(L, FG_FRAMES) == 1, "poller does not fight the hotkey")
    f8 = L.eval("BINDS[8]")
    f8()
    f8()
    check(store(L, "t.Streamline.Reflex.Mode") == 0, "reflex wraps 1 -> 2 -> 0")


def test_adaptive_in_game(tmp):
    L, M = boot(tmp, settings=ADAPTIVE)
    M.main()
    for _ in range(12):
        advance(L, uncapped=100, cap=0)
    check(multiplier(L) == 3, f"controller drove the engine to 3x, got {multiplier(L)}")
    state = (tmp / "AC8Tweaks" / "state.ini").read_text()
    check("[Adaptive]\nmultiplier=3x\n" in state and "target_fps=240" in state, f"state.ini reports adaptive: {state}")

    path = tmp / "AC8Tweaks" / "settings.ini"
    path.write_text(path.read_text().replace(f"{FG_ENABLE}=1", f"{FG_ENABLE}=0"))
    advance(L, uncapped=100, cap=0)
    check(multiplier(L) == 3, "manual values are not enforced while adaptive owns them")

    L.eval("BINDS[6]")()  # adaptive off, keep 3x
    text = settings_text(tmp)
    check("Enabled=0" in text and f"{FG_ENABLE}=1\n" in text and "FramesToGenerate=2\n" in text, f"F6 off persists the reached multiplier: {text}")
    for _ in range(5):
        advance(L, uncapped=300, cap=0)
    check(multiplier(L) == 3, "no more adaptive changes after F6 off")

    L.eval("BINDS[6]")()  # adaptive on again
    check("Enabled=1" in settings_text(tmp), "F6 on persists")
    for _ in range(6):
        advance(L, uncapped=300, cap=0)
    check(multiplier(L) == 1, f"switches off when the base clears the target, got {multiplier(L)}")

    L.eval("BINDS[7]")()  # F7 leaves adaptive and cycles manually from off
    check("Enabled=0" in settings_text(tmp) and multiplier(L) == 2, "F7 leaves adaptive and cycles to 2x")
    advance(L, uncapped=300, cap=0)
    check(multiplier(L) == 2, "manual again")


def test_auto_target(tmp):
    settings = SETTINGS + "\n[Adaptive]\nEnabled=1\nTargetFps=auto\nHoldSeconds=3\nProbeSeconds=0\n"
    L, M = boot(tmp, settings=settings)
    M.main()
    check(M.adaptive_cfg.auto and M.adaptive_cfg.target == 240, "no display.ini yet: default target kept")
    check([M.gsync_cap(h) for h in (60, 120, 144, 165, 240, 360, 480)] == [59, 116, 138, 157, 224, 324, 416], "G-Sync cap table")
    display = tmp / "AC8Tweaks" / "display.ini"
    display.write_text("; launcher\n[Display]\nRefreshHz=240\nWidth=5120\nHeight=1440\n")
    advance(L, uncapped=100, cap=0)
    check(M.adaptive_cfg.target == 224 and M.refresh_hz == 240, f"auto resolves to the G-Sync cap: {M.adaptive_cfg.target}")
    for _ in range(12):
        advance(L, uncapped=100, cap=0)
    state = (tmp / "AC8Tweaks" / "state.ini").read_text()
    check("target_fps=224" in state and "refresh_hz=240" in state, f"state shows the resolved target: {state}")
    L.eval("BINDS[6]")()
    L.eval("BINDS[6]")()
    check(hud_texts(L)[-1].startswith("Adaptive on, target 224\n") and "Adaptive: on, target 224 (auto, 240 Hz)" in hud_texts(L)[-1], f"toast: {hud_texts(L)[-1]}")
    display.write_text("[Display]\nRefreshHz=144\n")
    advance(L, uncapped=100, cap=0)
    check(M.adaptive_cfg.target == 138, "follows the monitor the game moved to")
    path = tmp / "AC8Tweaks" / "settings.ini"
    path.write_text(path.read_text().replace("TargetFps=auto", "TargetFps=auto\nRefreshMargin=5"))
    advance(L, uncapped=100, cap=0)
    check(M.adaptive_cfg.target == 139, "a numeric margin is subtracted instead")
    path.write_text(path.read_text().replace("TargetFps=auto", "TargetFps=200"))
    advance(L, uncapped=100, cap=0)
    check(not M.adaptive_cfg.auto and M.adaptive_cfg.target == 200, "a number switches auto off")


def test_fov_hotkeys(tmp):
    L, M = boot(tmp, settings=FOV)
    M.main()
    tick(L)
    L.eval("BINDS[187]")()  # no camera view active yet
    check(M.fov_cfg.Cockpit == 100 and hud_texts(L)[-1].startswith("FOV: not in a cockpit"), "nothing changes outside a view")
    L.execute("FAKE.cam.HUD.bIsActive = true; FAKE.fov = 92.6")  # HUD view is on the game's own value
    L.eval("BINDS[187]")()
    check(M.fov_cfg.HUD == 98 and "HUD=98" in settings_text(tmp), f"plus starts from the rendered angle, rounded, plus 5: {M.fov_cfg.HUD}")
    check(hud_texts(L)[-1].startswith("FOV HUD 98\n"), f"toast: {hud_texts(L)[-1]}")
    frame(L)
    check(fov_calls(L)[-1] == 98, "applied to the camera")
    L.eval("BINDS[189]")()
    L.eval("BINDS[189]")()
    check(M.fov_cfg.HUD == 88, "minus steps down from the saved value")
    L.execute("FAKE.cam.HUD.bIsActive = false; FAKE.cam.Cockpit.bIsActive = true")
    for _ in range(10):
        L.eval("BINDS[187]")()
    check(M.fov_cfg.Cockpit == 130 and "Cockpit=130" in settings_text(tmp), "cockpit steps from its own value and clamps at 130")
    check(M.fov_cfg.HUD == 88, "other views untouched")


def test_hud(tmp):
    L, M = boot(tmp)
    M.main()
    tick(L)
    L.eval("BINDS[7]")()  # F7 -> 3x
    texts = hud_texts(L)
    check(texts and texts[-1] == "Frame generation 3x\n\nFG: 3x\nReflex: low latency\nAdaptive: off\nFOV: cockpit default, HUD default, third person default", f"toast text: {texts}")
    check(hud_vis(L)[-1] == 3, "shown")
    check(L.eval("HUD.added") == 1000 and L.eval("HUD.pos").X == 80, "widget added to the viewport and positioned")
    delays = L.eval("HUD.delays")
    check(len(delays) == 1 and delays[1].ms == 6000, "hide scheduled after 6 s")
    L.eval("BINDS[8]")()  # reflex -> boost, before the first hide fires
    check(hud_texts(L)[-1].startswith("Reflex boost\n\nFG: 3x\nReflex: boost\n"), f"reflex toast: {hud_texts(L)[-1]}")
    L.eval("HUD.delays[1].fn")()
    check(hud_vis(L)[-1] == 3, "stale hide ignored while a newer message is up")
    L.eval("HUD.delays[2].fn")()
    check(hud_vis(L)[-1] == 2, "hidden after the latest delay")
    path = tmp / "AC8Tweaks" / "settings.ini"
    path.write_text(path.read_text().replace("t.Streamline.Reflex.Mode=2", "t.Streamline.Reflex.Mode=0") + "\n[FOV]\nCockpit=100\n")
    tick(L)
    check(hud_texts(L)[-1] == "Reflex off, FOV cockpit 100\n\nFG: 3x\nReflex: off\nAdaptive: off\nFOV: cockpit 100, HUD default, third person default", f"disk edit toast: {hud_texts(L)[-1]}")
    L.eval("BINDS[6]")()
    check(hud_texts(L)[-1].startswith("Adaptive on, target 240\n"), f"adaptive toast: {hud_texts(L)[-1]}")
    L.eval("BINDS[9]")()
    check(hud_texts(L)[-1].startswith("Settings reloaded\n"), "reload toast")


def test_hud_unavailable(tmp):
    L, M = boot(tmp)
    L.execute("HUD.create_fails = true")
    M.main()
    tick(L)
    L.eval("BINDS[7]")()
    check(hud_texts(L) == [] and store(L, FG_FRAMES) == 2, "no widget, setting still applied")
    L.eval("BINDS[7]")()
    logs = [l for l in L.eval("LOG").values() if "on-screen text unavailable" in l]
    check(len(logs) == 1, f"unavailability logged exactly once: {len(logs)}")


def test_fov(tmp):
    L, M = boot(tmp, settings=FOV)
    M.main()
    check(len(L.eval("FRAME_LOOPS")) == 1, "per-frame loop registered")
    check(set(L.eval("NOTIFY").keys()) == {"/Script/Live.LiveCameraViewComponent", "/Script/Live.LivePlayerCameraManager"}, "watches both classes")
    frame(L)
    check(fov_calls(L) == [0], f"no view active: released once, {fov_calls(L)}")
    L.execute("FAKE.cam.Cockpit.bIsActive = true")
    frame(L)
    check(fov_calls(L)[-1] == 100 and L.eval("FAKE.fov") == 100, "cockpit view locks 100")
    L.execute("FAKE.fov = 95")  # the game re-derived its own value
    frame(L)
    check(fov_calls(L)[-1] == 100 and len(fov_calls(L)) == 3, "drift corrected once")
    frame(L)
    check(len(fov_calls(L)) == 3, "steady state makes no calls")
    L.execute("FAKE.cam.Cockpit.bIsActive = false; FAKE.cam.HUD.bIsActive = true")
    frame(L)
    check(fov_calls(L)[-1] == 0, "HUD at 0 hands the game's value back")
    L.execute("FAKE.cam.HUD.bIsActive = false; FAKE.cam.ThirdPerson.bIsActive = true")
    frame(L)
    check(fov_calls(L)[-1] == 120, "third person locks 120")
    path = tmp / "AC8Tweaks" / "settings.ini"
    path.write_text(path.read_text().replace("ThirdPerson=120", "ThirdPerson=110"))
    tick(L)
    frame(L)
    check(fov_calls(L)[-1] == 110, "live edit applied without restart")
    tick(L)  # state.ini is written once a second, after the frame loop has run
    state = (tmp / "AC8Tweaks" / "state.ini").read_text()
    check("[FOV]\nview=ThirdPerson\napplied=110\n" in state, f"state.ini reports fov: {state}")
    path.write_text(path.read_text().replace("ThirdPerson=110", "ThirdPerson=0").replace("Cockpit=100", "Cockpit=0"))
    tick(L)
    frame(L)
    check(fov_calls(L)[-1] == 0 and L.eval("FAKE.fov") == 90, "all zero releases the lock")
    n = len(fov_calls(L))
    frame(L)
    frame(L)
    check(len(fov_calls(L)) == n, "and then stays idle")


def test_fov_off_costs_nothing(tmp):
    L, M = boot(tmp)
    M.main()
    L.execute("FAKE.cam.Cockpit.bIsActive = true")
    for _ in range(3):
        frame(L)
    check(fov_calls(L) == [], "no FOV calls while every view is 0")


def test_stuck_value_backs_off(tmp):
    L, M = boot(tmp)
    L.execute("FAKE.unsettable['t.Streamline.Reflex.Mode'] = true")
    M.main()
    for _ in range(6):
        tick(L)
    reflex = [c for c in L.eval("FAKE.execs").values() if c.startswith("t.Streamline.Reflex.Mode")]
    check(len(reflex) == 3, f"stopped retrying after 3 pushes, sent {len(reflex)}")
    L.eval("BINDS[9]")()  # reload forces one more push of everything
    reflex = [c for c in L.eval("FAKE.execs").values() if c.startswith("t.Streamline.Reflex.Mode")]
    check(len(reflex) == 4, "reload pushes again")


def test_waits_for_player_controller(tmp):
    L, M = boot(tmp)
    L.execute("FAKE.pc_valid = false")
    M.main()
    tick(L)
    check(execs(L) == 0, "no commands without a player controller")
    L.execute("FAKE.pc_valid = true")
    tick(L)
    check(execs(L) == 4, "applied once the controller exists")


def test_missing_settings(tmp):
    L, M = boot(tmp, settings=None)
    check(M.main() is True, "still active without settings.ini")
    tick(L)
    check(execs(L) == 0, "nothing sent")
    check(set(L.eval("BINDS").keys()) == {6, 7, 8, 9, 187, 189}, "default hotkeys still bound")


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    passed = 0
    for t in tests:
        print(f"  {t.__name__}")
        before = failures
        with tempfile.TemporaryDirectory() as d:
            t(pathlib.Path(d))
        passed += failures == before
    print(f"{passed} of {len(tests)} tests passed")
    sys.exit(1 if failures else 0)
