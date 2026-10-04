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
Key = { F6 = 6, F7 = 7, F8 = 8, F9 = 9, F10 = 10, F11 = 11, A = 65, OEM_PLUS = 187, OEM_MINUS = 189 }
BOUND_MODS = {}
ModifierKey = { CONTROL = 1, SHIFT = 2, ALT = 3 }
BINDS, LOOPS, LOG = {}, {}, {}
print = function(...) LOG[#LOG + 1] = table.concat({...}, " ") end
RegisterKeyBind = function(key, a, b) if b then BINDS['mod' .. key] = b; BOUND_MODS[key] = a else BINDS[key] = a end end
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
        GetPlayerController = function() FAKE.pc_searches = (FAKE.pc_searches or 0) + 1 return pc end,
        GetGameViewportClient = function() return { IsValid = function() return false end } end,
        GetEngine = function() return FAKE.engine end,
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
    FAKE.constructed = (FAKE.constructed or 0) + 1
    return obj({ SetText = function(self, t) HUD.texts[#HUD.texts + 1] = t end,
                 SetShadowOffset = function() end, SetShadowColorAndOpacity = function() end })
end
FText = function(s) return s end

-- the engine, its viewport and the console keys, as the console set-up reads them
FAKE.viewport = obj({ valid = false, ViewportConsole = obj({ valid = false }) })
FAKE.engine = obj({ GameViewport = FAKE.viewport })
local function console_key(name) return { KeyName = { ToString = function() return name end } } end
classes["/Script/Engine.Default__InputSettings"] = obj({ ConsoleKeys = { console_key("Tilde"), console_key("F12") } })

"""

SETTINGS = """; test settings
[ConsoleVariables]
; frame generation
r.Streamline.DLSSG.Enable=1
r.Streamline.DLSSG.FramesToGenerate=1
t.Streamline.Reflex.Mode=1
r.Streamline.DeepDVC.Intensity=0.5

[Keys]
CycleFrameGeneration=F7
CycleReflex=CTRL+F8
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
    seq, e, f = [], 0, 1
    for _ in range(5):
        e, f = M.next_frame_generation(e, f)
        seq.append((e, f))
    check(seq == [(1, 1), (1, 2), (1, 3), (0, 3), (1, 1)], f"frame generation cycle {seq}")
    k, mods = M.parse_key("ctrl+Shift+F7")
    check(k == "F7" and list(mods.values()) == ["CONTROL", "SHIFT"], "parse_key")
    k, mods = M.parse_key("F9")
    check(k == "F9" and len(mods) == 0, "parse_key no modifiers")
    names = L.table(F7=118, F10=121, IME_HANGUL=21, IME_KANA=21)
    check(M.key_code("F10", names) == 121 and M.key_code("shift+ctrl+f7", names) == 118 + 256 + 512, "a key as one number")
    check(M.key_code("", names) == 0 and M.key_code("WHAT", names) == 0 and M.key_code("CTRL+", names) == 0, "no such key is 0")
    check(M.key_name(121, names) == "F10" and M.key_name(118 + 256 + 1024, names) == "CTRL+ALT+F7", "and back, modifiers in one order")
    check(M.key_name(21, names) == "IME_KANA" and M.key_name(200, names) is None, "one name per key, none for a key without a name")
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
    check(len(L.eval("LOOPS")) == 2, "two loops registered: the settings tick and the menu")
    check(len(L.eval("BINDS")) == 0, "no keys are bound: the menu is the only control, and its key belongs to the DLL")
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


def test_menu_frame_generation(tmp):
    L, M = boot(tmp)
    M.main()
    tick(L)
    seen = []
    for choice in (2, 3, 0, 1):  # 3x, 4x, off, 2x
        use_menu(L, M, tmp, f"set\tfg\t{choice}")
        seen.append((store(L, FG_ENABLE), store(L, FG_FRAMES)))
    check(seen == [(1, 2), (1, 3), (0, 3), (1, 1)], f"frame generation choices {seen}")
    text = settings_text(tmp)
    check(f"{FG_ENABLE}=1\n" in text and "FramesToGenerate=1\n" in text, "the choice is saved to settings.ini")
    check(text.startswith("; test settings\n[ConsoleVariables]\n; frame generation\n"), "comments and order kept")
    tick(L)
    check(store(L, FG_FRAMES) == 1, "the settings tick does not fight the menu")
    use_menu(L, M, tmp, "set\treflex\t2", "set\treflex\t0")
    check(store(L, "t.Streamline.Reflex.Mode") == 0 and "t.Streamline.Reflex.Mode=0" in settings_text(tmp), "reflex set and saved")


def test_adaptive_in_game(tmp):
    L, M = boot(tmp, settings=ADAPTIVE)
    M.main()
    for _ in range(12):
        advance(L, uncapped=100, cap=0)
    check(multiplier(L) == 3, f"controller drove the engine to 3x, got {multiplier(L)}")
    log = list(L.eval("LOG").values())
    check(not any("failed" in line for line in log), f"no tick fails on a fractional frame rate: {[l for l in log if 'failed' in l]}")
    state = (tmp / "AC8Tweaks" / "state.ini").read_text()
    check("[Adaptive]\nmultiplier=3x\n" in state and "target_fps=240" in state, f"state.ini reports adaptive: {state}")

    path = tmp / "AC8Tweaks" / "settings.ini"
    path.write_text(path.read_text().replace(f"{FG_ENABLE}=1", f"{FG_ENABLE}=0"))
    advance(L, uncapped=100, cap=0)
    check(multiplier(L) == 3, "manual values are not enforced while adaptive owns them")

    use_menu(L, M, tmp, "set\tadaptive\t0")  # adaptive off, keep 3x
    text = settings_text(tmp)
    check("Enabled=0" in text and f"{FG_ENABLE}=1\n" in text and "FramesToGenerate=2\n" in text, f"switching adaptive off keeps the multiplier it reached: {text}")
    for _ in range(5):
        advance(L, uncapped=300, cap=0)
    check(multiplier(L) == 3, "no more adaptive changes after it is off")

    use_menu(L, M, tmp, "set\tadaptive\t1")
    check("Enabled=1" in settings_text(tmp), "switching it on is saved")
    for _ in range(6):
        advance(L, uncapped=300, cap=0)
    check(multiplier(L) == 1, f"switches off when the base clears the target, got {multiplier(L)}")

    use_menu(L, M, tmp, "set\tfg\t1")  # picking a multiplier by hand leaves adaptive
    check("Enabled=0" in settings_text(tmp) and multiplier(L) == 2, "choosing 2x leaves adaptive")
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
    lines = use_menu(L, M, tmp, "set\tadaptive\t0", "set\tadaptive\t1")
    check("note\tAdaptive on, target 224" in lines, f"the menu says what happened: {lines}")
    check("row\ttarget\tnumber\tAdaptive target fps (0 = monitor)\t0\t1\t0\t1000\t0" in lines, "target row shows 0 for automatic")
    menu_send(L, tmp, f"{int(M.menu.last_seq) + 1}\tmenu\t0")
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


def test_menu_fov(tmp):
    L, M = boot(tmp, settings=FOV)  # cockpit 100, HUD the game's own, third person 120
    M.main()
    tick(L)
    lines = use_menu(L, M, tmp)
    for row in ("row\tfov_Cockpit\tnumber\tField of view, cockpit (0 = game's own)\t100\t5\t0\t130\t0",
                "row\tfov_HUD\tnumber\tField of view, HUD (0 = game's own)\t0\t5\t0\t130\t0",
                "row\tfov_ThirdPerson\tnumber\tField of view, third person (0 = game's own)\t120\t5\t0\t130\t0"):
        check(row in lines, f"every view has its own row whichever one is in use: {row!r} in {lines}")
    use_menu(L, M, tmp, "set\tfov_HUD\t5")
    check(M.fov_cfg.HUD == 40 and "HUD=40" in settings_text(tmp), "one step up from the game's own is the lowest angle")
    use_menu(L, M, tmp, "set\tfov_HUD\t98")
    L.execute("FAKE.cam.HUD.bIsActive = true")
    frame(L)
    check(M.fov_cfg.HUD == 98 and fov_calls(L)[-1] == 98, "a typed angle is saved and applied to the camera")
    use_menu(L, M, tmp, "set\tfov_HUD\t12")
    check(M.fov_cfg.HUD == 40, "an angle that is too low becomes the lowest angle")
    use_menu(L, M, tmp, "set\tfov_HUD\t35")
    check(M.fov_cfg.HUD == 0 and "HUD=0" in settings_text(tmp), "one step below the lowest angle hands the view back to the game")
    check(M.fov_cfg.Cockpit == 100 and M.fov_cfg.ThirdPerson == 120, "other views untouched")


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
    path = tmp / "AC8Tweaks" / "settings.ini"
    path.write_text(path.read_text() + "\n")
    tick(L)
    reflex = [c for c in L.eval("FAKE.execs").values() if c.startswith("t.Streamline.Reflex.Mode")]
    check(len(reflex) == 4, "a change to settings.ini makes it try again")


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
    check(len(L.eval("LOOPS")) == 2, "the settings tick and the menu still run")


def use_menu(L, M, tmp, *events):
    """Does things the way the player does: in the open menu. Events are numbered here."""
    seq = int(M.menu.last_seq)
    if not M.menu.open:
        menu_send(L, tmp, f"{seq + 1}\tmenu\t1", f"{seq + 2}\tsection\tkeys\t1", f"{seq + 3}\tsection\tmenu\t1")
        seq += 3
    return menu_send(L, tmp, *[f"{seq + 1 + i}\t{e}" for i, e in enumerate(events)])


def menu_send(L, tmp, *events):
    """What the DLL does: leaves events in menu-events.txt, then the menu loop runs once."""
    (tmp / "AC8Tweaks" / "menu-events.txt").write_text("".join(e + "\n" for e in events))
    L.eval("LOOPS[2]")()
    return (tmp / "AC8Tweaks" / "menu.txt").read_text().split("\n")


def test_menu_graphics(tmp):
    L, M = boot(tmp)
    M.main()
    tick(L)
    for _ in range(10):
        L.eval("LOOPS[2]")()
    lines = (tmp / "AC8Tweaks" / "menu.txt").read_text().split("\n")
    check("ack\t0" in lines and not any(l.startswith("section") for l in lines), f"closed menu is only a header: {lines}")
    lines = menu_send(L, tmp, "1\tmenu\t1")
    check("ack\t1" in lines and "section\tgraphics\tGraphics\t1" in lines, f"opening lists the groups: {lines}")
    check("row\tfg\tchoice\tFrame generation\t1\toff|2x|3x|4x" in lines, f"frame generation row: {lines}")
    check("row\tadaptive\tswitch\tAdaptive frame generation\t0" in lines, "adaptive row")
    lines = menu_send(L, tmp, "1\tmenu\t1", "2\tset\tfg\t3")
    check(store(L, FG_ENABLE) == 1 and store(L, FG_FRAMES) == 3, "choosing 4x in the menu sets the engine")
    check("row\tfg\tchoice\tFrame generation\t3\toff|2x|3x|4x" in lines and "ack\t2" in lines, "the menu shows 4x and acknowledges")
    before = execs(L)
    menu_send(L, tmp, "1\tmenu\t1", "2\tset\tfg\t3")
    check(execs(L) == before, "events already handled are not run twice")
    menu_send(L, tmp, "3\tset\tfg\t0", "4\tset\treflex\t2", "5\tset\tadaptive\t1")
    check(store(L, FG_ENABLE) == 0 or M.adaptive_cfg.enabled, "off in the menu turns generation off")
    check(store(L, "t.Streamline.Reflex.Mode") == 2, "reflex set from the menu")
    check(M.adaptive_cfg.enabled and "Enabled=1" in settings_text(tmp), "adaptive switched on and saved")
    check([l.split("\t")[1] for l in lines if l.startswith("section")] == ["graphics", "keys", "menu"], f"the groups, and nothing else: {lines}")
    lines = menu_send(L, tmp, "6\tsection\tkeys\t1", "7\tsection\tmenu\t1")
    check("key\t10\tF10" in lines, f"the DLL is told the menu key and its name (F10 is 10 in the fake key table): {lines}")
    for row in ("row\thdr\tchoice\tMenu in HDR\t0\tauto|on|off", "row\tnits\tnumber\tMenu brightness, nits\t200\t20\t80\t1000\t0"):
        check(row in lines, f"everything that used to be a file edit is a row: {row!r}")
    lines = menu_send(L, tmp, "8\tset\thdr\t1", "9\tset\tnits\t300", "10\tset\ttarget\t200")
    check("hdr\t1" in lines and "nits\t300" in lines, f"menu drawing settings take effect at once: {lines[:4]}")
    check("Hdr=on" in settings_text(tmp) and "Nits=300" in settings_text(tmp) and "TargetFps=200" in settings_text(tmp), "and are saved")
    menu_send(L, tmp, "11\tset\ttarget\t0")
    check("TargetFps=auto" in settings_text(tmp), "target 0 means match the monitor")
    lines = menu_send(L, tmp, "12\tmenu\t0")
    check(not any(l.startswith("section") for l in lines), "closing empties the menu again")
    menu_send(L, tmp, "13\tmenu\t1")
    M.notify("hello")
    for _ in range(3):  # values on screen refresh every third pass
        lines = menu_send(L, tmp)
    check("note\thello" in lines and hud_texts(L) == [], "messages go into the menu and nowhere else")


def test_keys(tmp):
    L, M = boot(tmp)  # the fixture sets frame generation to F7 and Reflex to CTRL+F8
    M.main()
    tick(L)
    lines = use_menu(L, M, tmp, "set\tfg\t2")
    check("key\t10\tF10" in lines and "hotkey\tCycleFrameGeneration\t7" in lines and f"hotkey\tCycleReflex\t{8 + 256}" in lines,
          f"the DLL is told which keys to watch: {lines[:8]}")
    check(not any(l.startswith("toast") for l in lines), "a change made in the menu is not put on screen a second time")
    for row in ("row\tkey_Menu\tkey\tOpen and close this menu\tF10",
                "row\tkey_CycleFrameGeneration\tkey\tFrame generation: off, 2x, 3x, 4x\tF7",
                "row\tkey_CycleReflex\tkey\tReflex: off, low latency, boost\tCTRL+F8",
                "row\tkey_FovUp\tkey\tField of view up, in the view you are in\t"):
        check(row in lines, f"every key is a row showing what it is set to: {row!r} in {lines}")

    # what the DLL reports when a quick key is pressed
    seen = []
    for _ in range(3):
        lines = use_menu(L, M, tmp, "key\tCycleFrameGeneration")
        seen.append((store(L, FG_ENABLE), store(L, FG_FRAMES)))
    check(seen == [(1, 3), (0, 3), (1, 1)], f"the frame generation key steps through 4x, off, 2x: {seen}")
    check("toast\tFrame generation 2x" in lines, f"and what it did goes on screen: {lines[:8]}")
    check(f"{FG_ENABLE}=1\n" in settings_text(tmp) and "FramesToGenerate=1\n" in settings_text(tmp), "and is saved")
    use_menu(L, M, tmp, "key\tCycleReflex", "key\tCycleReflex")
    check(store(L, "t.Streamline.Reflex.Mode") == 0, "reflex wraps 1 -> 2 -> 0")
    lines = use_menu(L, M, tmp, "key\tFovUp")
    check(any(l.startswith("toast\tField of view: not in") for l in lines) and M.fov_cfg.HUD == 0, "no view, no change")
    L.execute("FAKE.cam.HUD.bIsActive = true")
    frame(L)
    lines = use_menu(L, M, tmp, "key\tFovUp")
    check(M.fov_cfg.HUD == 95 and "HUD=95" in settings_text(tmp), "a view on the game's own value starts from the angle on screen (90)")
    check("toast\tField of view, HUD: 95" in lines, "and says so")
    use_menu(L, M, tmp, "key\tFovDown", "key\tFovDown")
    check(M.fov_cfg.HUD == 85 and M.fov_cfg.Cockpit == 0, "down twice, the other views untouched")
    use_menu(L, M, tmp, "key\tToggleAdaptive")
    check(M.adaptive_cfg.enabled and "Enabled=1" in settings_text(tmp), "adaptive key")
    use_menu(L, M, tmp, "key\tNoSuchThing")

    # picking keys in the menu: the DLL sends the key pressed as a number
    lines = use_menu(L, M, tmp, "set\tkey_FovUp\t187", f"set\tkey_ToggleAdaptive\t{6 + 512}")
    check("FovUp=OEM_PLUS" in settings_text(tmp) and "ToggleAdaptive=SHIFT+F6" in settings_text(tmp), f"picked keys are saved by name: {settings_text(tmp)}")
    check("hotkey\tFovUp\t187" in lines and f"hotkey\tToggleAdaptive\t{6 + 512}" in lines, "and watched from then on")
    check("row\tkey_FovUp\tkey\tField of view up, in the view you are in\tOEM_PLUS" in lines, "and shown")
    lines = use_menu(L, M, tmp, "set\tkey_CycleReflex\t7")
    check("CycleReflex=F7" in settings_text(tmp) and "CycleFrameGeneration=\n" in settings_text(tmp), "a key does one thing: its old use is dropped")
    check(not any(l.startswith("hotkey\tCycleFrameGeneration") for l in lines), "and no longer watched")
    lines = use_menu(L, M, tmp, "set\tkey_CycleReflex\t0")
    check("CycleReflex=\n" in settings_text(tmp) and "row\tkey_CycleReflex\tkey\tReflex: off, low latency, boost\t" in lines, "0 means no key")
    lines = use_menu(L, M, tmp, "set\tkey_FovDown\t200")
    check(M["keys"]["FovDown"] == "" and "note\tThat key cannot be used" in lines, f"a key UE4SS has no name for is refused: {lines[:8]}")
    lines = use_menu(L, M, tmp, "set\tkey_FovDown\t10")
    check(M["keys"]["FovDown"] == "" and "note\tF10 already opens the menu" in lines, "the menu key is not given away")
    lines = use_menu(L, M, tmp, "set\tkey_Menu\t0")
    check(M["keys"]["Menu"] == "F10" and "note\tThe menu needs a key" in lines, "the menu cannot be left without a key")
    lines = use_menu(L, M, tmp, f"set\tkey_Menu\t{11 + 256}")
    check("Menu=CTRL+F11" in settings_text(tmp) and f"key\t{11 + 256}\tCTRL+F11" in lines, f"the menu key itself can be picked: {lines[:8]}")

    # and by hand in the file
    path = tmp / "AC8Tweaks" / "settings.ini"
    path.write_text(path.read_text().replace("Menu=CTRL+F11", "Menu=nonsense\nFovDown=alt+a"))
    tick(L)
    menu_key, fov_down = M["keys"]["Menu"], M["keys"]["FovDown"]
    check(menu_key == "F10" and fov_down == "ALT+A", f"a key the file gets wrong falls back, one it gets right is read in any case: {menu_key} {fov_down}")


def test_console(tmp):
    L, M = boot(tmp)
    M.main()
    tick(L)
    FAKE = L.eval("FAKE")
    check(FAKE.constructed is None, "no viewport yet, no console")
    L.execute("FAKE.viewport.valid = true")
    tick(L)
    tick(L)
    check(FAKE.constructed == 1 and FAKE.viewport.ViewportConsole.IsValid(), "the console is made once the viewport exists, and once only")
    check(any("console ready, keys: Tilde, F12" in l for l in L.eval("LOG").values()), "and the log says which keys the game has for it")
    mods = (ROOT / "mod" / "ue4ss" / "Mods" / "mods.txt").read_text()
    check("ConsoleEnablerMod : 0" in mods, "UE4SS's own console mod stays off: it puts the console on F10, the menu's key")


def test_keys_without_menu(tmp):
    L, M = boot(tmp, settings=SETTINGS + "\n[Menu]\nEnabled=0\n")
    M.main()
    tick(L)
    check(M.menu is None and len(L.eval("LOOPS")) == 1, "the menu can be switched off altogether")
    f7 = L.eval("BINDS[7]")
    seen = []
    for _ in range(4):
        f7()
        seen.append((store(L, FG_ENABLE), store(L, FG_FRAMES)))
    check(seen == [(1, 2), (1, 3), (0, 3), (1, 1)], f"the quick keys then go through UE4SS: {seen}")
    check(L.eval("BOUND_MODS[8][1]") == 1 and L.eval("BOUND_MODS[8][2]") is None, "with their modifiers (CTRL is 1 in the fake table)")
    L.eval("BINDS['mod8']")()
    check(store(L, "t.Streamline.Reflex.Mode") == 2, "CTRL+F8 steps Reflex")
    check(L.eval("BINDS[10]") is None and L.eval("BINDS[187]") is None, "the menu key and keys not set are not bound")
    path = tmp / "AC8Tweaks" / "settings.ini"
    path.write_text(path.read_text().replace("CycleFrameGeneration=F7", "CycleFrameGeneration="))
    tick(L)
    before = execs(L)
    f7()
    check(execs(L) == before, "a key taken out of the file stops working, though UE4SS still reports it")


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
