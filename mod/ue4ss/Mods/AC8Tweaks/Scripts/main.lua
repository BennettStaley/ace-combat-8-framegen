--[[
AC8 Tweaks runtime core. Runs under UE4SS.

Applies the console variables listed in AC8Tweaks\settings.ini, pushes them again whenever the
engine reports a different value, and re-reads the file whenever it changes on disk. What the
engine actually reports is written to AC8Tweaks\state.ini whenever it changes.

Adaptive mode picks the frame generation multiplier (off, 2x, 3x, 4x) once a second so that
base frame rate times multiplier reaches a target, and probes downward when the output is capped.

Field of view per camera view lives in fov.lua next to this file and reads the [FOV] section.
The in-game menu (F10) holds every setting. ac8overlay.dll draws it, menu.lua feeds it, and the
rows are in this file. The quick keys under [Keys] are optional and start out unset; the menu DLL
watches them, so a key picked in the menu works at once.

Does nothing unless the offline launcher started the game (AC8TWEAKS_OFFLINE=1).
]]

local M = {}
local TAG = "[AC8Tweaks] "
local FG_ENABLE = "r.Streamline.DLSSG.Enable"
local FG_FRAMES = "r.Streamline.DLSSG.FramesToGenerate"
local FG_KEYS = { [FG_ENABLE] = true, [FG_FRAMES] = true }
local REFLEX_MODE = "t.Streamline.Reflex.Mode"
local MAX_FPS = "t.MaxFPS"
local MAX_STRIKES = 3 -- pushes that did not stick before a value is left alone
local FOV_STEP = 5
local FOV_LABELS = { Cockpit = "cockpit", HUD = "HUD", ThirdPerson = "third person" }
local DEFAULT_ADAPTIVE = { enabled = false, target = 240, hold = 3, probe = 10, auto = false, margin = "auto" }
local FOV_VIEWS = { "Cockpit", "HUD", "ThirdPerson" }
local FOV_MIN, FOV_MAX = 40, 130
local REFLEX_NAMES = { [0] = "off", [1] = "low latency", [2] = "boost" }
local NOTE_SECONDS = 6 -- how long a message stays at the top of the menu
local TOAST_SECONDS = 4 -- how long what a quick key did stays on screen
local KEY_ACTIONS = { "Menu", "CycleFrameGeneration", "ToggleAdaptive", "CycleReflex", "FovUp", "FovDown" }
local KEY_LABELS = { Menu = "Open and close this menu", CycleFrameGeneration = "Frame generation: off, 2x, 3x, 4x",
                     ToggleAdaptive = "Adaptive frame generation on or off", CycleReflex = "Reflex: off, low latency, boost",
                     FovUp = "Field of view up, in the view you are in", FovDown = "Field of view down" }
local DEFAULT_KEYS = { Menu = "F10" } -- the quick keys start out unset
local MODIFIERS = { { "CONTROL", "CTRL", 256 }, { "SHIFT", "SHIFT", 512 }, { "ALT", "ALT", 1024 } }

local function log(fmt, ...)
    print(TAG .. string.format(fmt, ...) .. "\n")
end

local function multiplier_name(m)
    return m == 1 and "off" or m .. "x"
end

-------------------------------------------------------------------------------
-- Pure helpers. Covered by tests/test_lua.py without UE4SS.
-------------------------------------------------------------------------------

function M.split_lines(text)
    local lines, pos = {}, 1
    while true do
        local nl = text:find("\n", pos, true)
        if not nl then
            lines[#lines + 1] = text:sub(pos)
            break
        end
        lines[#lines + 1] = text:sub(pos, nl - 1)
        pos = nl + 1
    end
    for i, line in ipairs(lines) do
        lines[i] = (line:gsub("\r$", ""))
    end
    return lines
end

-- { section = { key = value } }. Comments start with ; or #.
function M.parse_ini(text)
    local sections, current = {}, nil
    for _, line in ipairs(M.split_lines(text)) do
        local name = line:match("^%s*%[([^%]]+)%]")
        if name then
            current = name
            sections[current] = sections[current] or {}
        elseif current and not line:match("^%s*[;#]") then
            local key, value = line:match("^%s*([^=]-)%s*=%s*(.-)%s*$")
            if key and key ~= "" then sections[current][key] = value end
        end
    end
    return sections
end

-- Replaces key=value inside section, keeping every other line as it was. Appends when missing.
function M.update_ini(text, section, key, value)
    local lines = M.split_lines(text)
    local in_section, seen, last = false, false, 0
    for i, line in ipairs(lines) do
        local name = line:match("^%s*%[([^%]]+)%]")
        if name then
            in_section = name == section
            if in_section then
                seen, last = true, i
            end
        elseif in_section then
            local k = line:match("^%s*([^=;#][^=]-)%s*=")
            if k == key then
                lines[i] = key .. "=" .. value
                return table.concat(lines, "\n")
            end
            if line:match("%S") then last = i end
        end
    end
    if seen then
        table.insert(lines, last + 1, key .. "=" .. value)
    else
        if lines[#lines] ~= "" then lines[#lines + 1] = "" end
        lines[#lines + 1] = "[" .. section .. "]"
        lines[#lines + 1] = key .. "=" .. value
        lines[#lines + 1] = ""
    end
    return table.concat(lines, "\n")
end

-- off -> 2x -> 3x -> 4x -> off, as the two console variables: enable, frames
function M.next_frame_generation(enable, frames)
    if enable == 0 then return 1, 1 end
    if frames < 3 then return 1, frames + 1 end
    return 0, frames
end

-- "ctrl+shift+f7" -> "F7", { "CONTROL", "SHIFT" }
function M.parse_key(spec)
    local key, mods = nil, {}
    for part in spec:upper():gmatch("[^+%s]+") do
        if part == "CTRL" or part == "CONTROL" then
            mods[#mods + 1] = "CONTROL"
        elseif part == "SHIFT" or part == "ALT" then
            mods[#mods + 1] = part
        else
            key = part
        end
    end
    return key, mods
end

-- A key as the one number the menu DLL deals in: the Windows key code, plus 256 for CTRL, 512 for
-- SHIFT and 1024 for ALT. `names` is UE4SS's Key table. 0 when the text names no key in it.
function M.key_code(spec, names)
    local key, mods = M.parse_key(spec or "")
    local code = key and names[key]
    if math.type(code) ~= "integer" then return 0 end
    for _, held in ipairs(mods) do
        for _, m in ipairs(MODIFIERS) do
            if m[1] == held then code = code | m[3] end
        end
    end
    return code
end

-- The other way round, written the way settings.ini has it: 377 -> "CTRL+F10". nil for a key
-- UE4SS has no name for. Of several names for one key the shortest wins, so the answer is stable.
function M.key_name(code, names)
    local vk, best = code & 255, nil
    for name, value in pairs(names) do
        if value == vk and (not best or #name < #best or (#name == #best and name < best)) then best = name end
    end
    if not best then return nil end
    local out = {}
    for _, m in ipairs(MODIFIERS) do
        if code & m[3] ~= 0 then out[#out + 1] = m[2] end
    end
    out[#out + 1] = best
    return table.concat(out, "+")
end

function M.is_float(value)
    return value:find(".", 1, true) ~= nil
end

function M.same(a, b)
    local x, y = tonumber(a), tonumber(b)
    if x and y then return math.abs(x - y) < 1e-6 end
    return tostring(a) == tostring(b)
end

-- 0 keeps the game's own value; anything else is clamped to the range the game accepts.
function M.parse_fov(section)
    local cfg = {}
    for _, view in ipairs(FOV_VIEWS) do
        local n = tonumber((section or {})[view]) or 0
        if n ~= 0 then n = math.max(FOV_MIN, math.min(FOV_MAX, n)) end
        cfg[view] = n
    end
    return cfg
end

local function truthy(value)
    value = tostring(value or ""):lower()
    return value == "1" or value == "true" or value == "yes" or value == "on"
end

-- One controller step per second. state.m is the current multiplier, 1 meaning generation off.
-- base is the measured engine frame rate, cap the engine frame limit (0 for none).
-- Returns the multiplier to use next. Pure apart from the counters it keeps in state.
--
-- Rules: keep a multiplier for at least `hold` seconds after any change. Step up while the
-- output misses the target. Step down when the measured base already covers the target one
-- step lower, or to off when the base alone clears it. When the output sits at the target and
-- a frame cap may be hiding headroom, probe one step down every `probe` seconds; a failed
-- probe steps back up and doubles the wait, a successful one resets it.
function M.adaptive_step(state, base, target, cap, hold, probe)
    if cap and cap > 0 and cap < target then target = cap end
    state.held = (state.held or 0) + 1
    state.since_probe = (state.since_probe or 0) + 1
    state.probe_wait = state.probe_wait or probe
    if not base or base <= 0 or state.held < hold then return state.m end

    local m, out = state.m, base * state.m
    local low, at_target = out < target * 0.95, out >= target * 0.97
    if state.probing then
        state.probing = false
        if low then
            m = m + 1
            state.probe_wait = math.min(state.probe_wait * 2, 60)
        else
            state.probe_wait = probe
        end
    elseif low and m < 4 then
        m = m + 1
    elseif m > 1 and base >= target * 0.97 then
        m = 1
    elseif m > 1 and base * (m - 1) >= target then
        m = m - 1 -- the measured base already covers the target one step down, no probe needed
    elseif m > 1 and at_target and probe > 0 and state.since_probe >= state.probe_wait then
        m = m - 1
        state.probing = true
        state.since_probe = 0
    end
    if m ~= state.m then
        state.m, state.held = m, 0
    end
    return m
end

-------------------------------------------------------------------------------
-- Files
-------------------------------------------------------------------------------

local function read_file(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local text = f:read("a")
    f:close()
    return text
end

local function write_file(path, text)
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "wb")
    if not f then return false end
    f:write(text)
    f:close()
    os.remove(path)
    return os.rename(tmp, path) and true or false
end

-------------------------------------------------------------------------------
-- Engine bridge. Replaced wholesale by the test.
-------------------------------------------------------------------------------

local UEHelpers
local engine = {}
M.engine = engine
local cached_pc

local function player_controller()
    if cached_pc and cached_pc:IsValid() then return cached_pc end
    cached_pc = UEHelpers.GetPlayerController()
    return cached_pc
end

-- true when the command reached the engine
function engine.exec(command)
    local lib = UEHelpers.GetKismetSystemLibrary()
    local pc = player_controller()
    if pc:IsValid() then
        lib:ExecuteConsoleCommand(pc, command, pc)
        return true
    end
    local viewport = UEHelpers.GetGameViewportClient()
    if viewport:IsValid() then
        viewport:ConsoleCommand(command)
        return true
    end
    return false
end

function engine.read(cvar, float)
    local lib = UEHelpers.GetKismetSystemLibrary()
    if float then return lib:GetConsoleVariableFloatValue(cvar) end
    return lib:GetConsoleVariableIntValue(cvar)
end

-- engine frame counter and real time in seconds, for measuring the base frame rate
function engine.frame_clock()
    local pc = player_controller()
    if not pc:IsValid() then return nil end
    return UEHelpers.GetKismetSystemLibrary():GetFrameCount(), UEHelpers.GetGameplayStatics():GetRealTimeSeconds(pc)
end

-------------------------------------------------------------------------------
-- Core
-------------------------------------------------------------------------------

M.settings_path, M.state_path, M.display_path = nil, nil, nil
M.refresh_hz = nil -- from display.ini, written by the launcher
M.desired = {} -- cvar -> value string, from settings.ini
M.adaptive_cfg = DEFAULT_ADAPTIVE
M.adapt = {} -- controller state while adaptive mode runs
M.fov_cfg = M.parse_fov(nil)
M.fov = nil -- the fov.lua module once started
M.menu = nil -- the menu.lua module once started
M.menu_cfg = { enabled = true, hdr = "auto", nits = 200 }
M.keys = {} -- action -> key the way settings.ini writes it ("F10", "CTRL+F7"), "" for none
local last_settings_text, last_state_body = nil, nil
local strikes = {} -- cvar -> pushes that did not stick since the value last changed
local unknown_keys = {} -- key texts already reported as unusable

local function key_names()
    return type(Key) == "table" and Key or {}
end

local function apply_settings_text(text)
    local ini = M.parse_ini(text)
    M.desired = ini.ConsoleVariables or {}
    local a = ini.Adaptive or {}
    local spec = tostring(a.TargetFps or ""):lower()
    M.adaptive_cfg = {
        enabled = truthy(a.Enabled),
        auto = spec == "auto",
        margin = tonumber(a.RefreshMargin) or "auto",
        target = tonumber(a.TargetFps) or DEFAULT_ADAPTIVE.target,
        hold = math.max(1, math.floor(tonumber(a.HoldSeconds) or DEFAULT_ADAPTIVE.hold)),
        probe = math.floor(tonumber(a.ProbeSeconds) or DEFAULT_ADAPTIVE.probe),
    }
    M.fov_cfg = M.parse_fov(ini.FOV)
    local menu = ini.Menu or {}
    M.menu_cfg = { enabled = menu.Enabled == nil or truthy(menu.Enabled), hdr = tostring(menu.Hdr or "auto"):lower(),
                   nits = tonumber(menu.Nits) or 200 }
    M.keys = {}
    for _, action in ipairs(KEY_ACTIONS) do
        local spec = (ini.Keys or {})[action] or DEFAULT_KEYS[action] or ""
        local name = M.key_name(M.key_code(spec, key_names()), key_names())
        if spec ~= "" and not name and not unknown_keys[spec] then
            unknown_keys[spec] = true
            log("unknown key '%s' for %s", spec, action)
        end
        M.keys[action] = name or DEFAULT_KEYS[action] or "" -- the menu always has a key
    end
    strikes = {}
end

local function fg_name(desired)
    if (tonumber(desired[FG_ENABLE]) or 0) == 0 then return "off" end
    return ((tonumber(desired[FG_FRAMES]) or 1) + 1) .. "x"
end

-- Human-readable list of what differs between two loaded settings, for edits made on disk.
function M.describe_changes(old_desired, new_desired, old_adaptive, new_adaptive, old_fov, new_fov)
    local out = {}
    if fg_name(old_desired) ~= fg_name(new_desired) then out[#out + 1] = "Frame generation " .. fg_name(new_desired) end
    local r0, r1 = tonumber(old_desired[REFLEX_MODE]), tonumber(new_desired[REFLEX_MODE])
    if r0 ~= r1 then out[#out + 1] = "Reflex " .. (REFLEX_NAMES[r1 or -1] or tostring(r1)) end
    for cvar, value in pairs(new_desired) do
        if cvar ~= FG_ENABLE and cvar ~= FG_FRAMES and cvar ~= REFLEX_MODE and old_desired[cvar] ~= value then
            out[#out + 1] = cvar .. " = " .. value
        end
    end
    if old_adaptive.enabled ~= new_adaptive.enabled then
        out[#out + 1] = new_adaptive.enabled and ("Adaptive on, target " .. new_adaptive.target) or "Adaptive off"
    elseif old_adaptive.target ~= new_adaptive.target then
        out[#out + 1] = "Adaptive target " .. new_adaptive.target
    end
    for _, v in ipairs({ { "Cockpit", "cockpit" }, { "HUD", "HUD" }, { "ThirdPerson", "third person" } }) do
        if old_fov[v[1]] ~= new_fov[v[1]] then
            out[#out + 1] = "FOV " .. v[2] .. " " .. (new_fov[v[1]] > 0 and tostring(new_fov[v[1]]) or "game default")
        end
    end
    return out
end

local from_key = false -- true while a quick key's action runs

-- Logs the message and shows it at the top of the menu. While the menu is closed nothing is drawn,
-- except for what a quick key did: that stays on screen for a moment, there being no menu to look at.
function M.notify(message)
    log("%s", message)
    if not M.menu then return end
    M.menu.say(message, NOTE_SECONDS)
    if from_key then M.menu.toast(message, TOAST_SECONDS) end
end

-- Highest frame rate that stays inside the variable refresh range of a monitor: the limit
-- NVIDIA Reflex applies with G-Sync, refresh minus refresh squared over 3600.
-- 60 -> 59, 120 -> 116, 144 -> 138, 165 -> 157, 240 -> 224, 360 -> 324, 480 -> 416.
function M.gsync_cap(hz)
    return math.floor(hz - hz * hz / 3600)
end

-- Reads the refresh rate the launcher recorded. nil when the file is missing or unreadable.
function M.read_refresh_hz()
    local text = M.display_path and read_file(M.display_path)
    local hz = text and tonumber((M.parse_ini(text).Display or {}).RefreshHz)
    return hz and hz > 0 and hz or nil
end

local refresh_warned = false
-- Resolves TargetFps=auto against the monitor. Numeric targets are left alone.
local function resolve_target()
    local cfg = M.adaptive_cfg
    if not cfg.auto then return end
    M.refresh_hz = M.read_refresh_hz()
    if M.refresh_hz then
        cfg.target = cfg.margin == "auto" and M.gsync_cap(M.refresh_hz) or M.refresh_hz - cfg.margin
    elseif not refresh_warned then
        refresh_warned = true
        log("refresh rate unknown, adaptive target stays at %d", cfg.target)
    end
end

-- true when the file changed since the last read
function M.load_settings()
    local text = read_file(M.settings_path)
    if not text or text == last_settings_text then return false end
    last_settings_text = text
    apply_settings_text(text)
    resolve_target()
    return true
end

local function read_back(cvar, value)
    local ok, current = pcall(engine.read, cvar, M.is_float(value))
    if ok and current ~= nil then return current end
    return nil
end

-- Pushes every value the engine does not report. Returns the number of commands sent.
function M.apply(force)
    local sent = 0
    for cvar, value in pairs(M.desired) do
        if not (M.adaptive_cfg.enabled and FG_KEYS[cvar]) then -- the controller owns these
            local current = read_back(cvar, value)
            local push = force
            if not push and (strikes[cvar] or 0) < MAX_STRIKES then
                push = current == nil or not M.same(current, value)
            end
            if push and engine.exec(cvar .. " " .. value) then
                sent = sent + 1
                strikes[cvar] = (strikes[cvar] or 0) + 1
                if strikes[cvar] == MAX_STRIKES then
                    log("%s=%s does not stick, leaving it alone until settings.ini changes", cvar, value)
                end
            elseif current ~= nil and M.same(current, value) then
                strikes[cvar] = 0
            end
        end
    end
    return sent
end

local function apply_multiplier(m)
    if m == 1 then
        engine.exec(FG_ENABLE .. " 0")
    else
        engine.exec(FG_FRAMES .. " " .. (m - 1))
        engine.exec(FG_ENABLE .. " 1")
    end
end

local clock_failed = false
local function measure_base_fps()
    local ok, frame, now = pcall(engine.frame_clock)
    if not ok and not clock_failed then
        clock_failed = true
        log("frame clock unavailable, adaptive mode cannot measure: %s", tostring(frame))
    end
    if not ok or not frame or not now then return nil end
    local a = M.adapt
    local fps
    if a.last_frame and now > a.last_time then
        fps = (frame - a.last_frame) / (now - a.last_time)
    end
    a.last_frame, a.last_time = frame, now
    return fps
end

local function current_multiplier()
    local enable = tonumber(M.desired[FG_ENABLE]) or 0
    if enable == 0 then return 1 end
    return math.min(4, math.max(2, (tonumber(M.desired[FG_FRAMES]) or 1) + 1))
end

local function adaptive_tick()
    local cfg = M.adaptive_cfg
    if not cfg.enabled then
        M.adapt = {}
        return
    end
    resolve_target()
    local a = M.adapt
    if not a.m then -- just switched on: continue from the manual values
        a.m = current_multiplier()
        apply_multiplier(a.m)
        log("adaptive on, target %d fps, starting at %s", cfg.target, multiplier_name(a.m))
    end
    a.base = measure_base_fps()
    local before = a.m
    local m = M.adaptive_step(a, a.base, cfg.target, read_back(MAX_FPS, "0.0"), cfg.hold, cfg.probe)
    if m ~= before then
        apply_multiplier(m)
        M.notify(string.format("Adaptive %s (base %d fps)", multiplier_name(m), math.floor((a.base or 0) + 0.5)))
    end
end

function M.write_state()
    local keys = {}
    for cvar in pairs(M.desired) do keys[#keys + 1] = cvar end
    table.sort(keys)
    local lines = { "; Written by AC8Tweaks while the game runs. Values are what the engine reports.", "[Actual]" }
    for _, cvar in ipairs(keys) do
        local current = read_back(cvar, M.desired[cvar])
        lines[#lines + 1] = cvar .. "=" .. (current ~= nil and tostring(current) or "?")
    end
    local a = M.adapt
    if M.adaptive_cfg.enabled and a.m then
        local base = math.floor((a.base or 0) + 0.5)
        lines[#lines + 1] = ""
        lines[#lines + 1] = "[Adaptive]"
        lines[#lines + 1] = "multiplier=" .. multiplier_name(a.m)
        lines[#lines + 1] = "base_fps=" .. base
        lines[#lines + 1] = "output_fps=" .. base * a.m
        lines[#lines + 1] = "target_fps=" .. M.adaptive_cfg.target
        if M.adaptive_cfg.auto then lines[#lines + 1] = "refresh_hz=" .. tostring(M.refresh_hz or "unknown") end
    end
    local fov = M.fov and M.fov.status()
    if fov then
        lines[#lines + 1] = ""
        lines[#lines + 1] = "[FOV]"
        lines[#lines + 1] = "view=" .. (fov.view or "other")
        lines[#lines + 1] = "applied=" .. (fov.applied > 0 and tostring(fov.applied) or "game default")
    end
    local body = table.concat(lines, "\n") .. "\n"
    if body == last_state_body then return false end
    last_state_body = body
    return write_file(M.state_path, body)
end

-- The game ships without a console object. One is made here, so the keys the game itself has for
-- it (tilde) open it. UE4SS's own ConsoleEnablerMod is left off: it also puts the console on F10,
-- which is the menu's key. Tried once a second until the viewport exists.
local console_done = false
local function enable_console()
    local engine = UEHelpers.GetEngine()
    local viewport = engine:IsValid() and engine.GameViewport
    if not viewport or not viewport:IsValid() then return end
    if not viewport.ViewportConsole:IsValid() then
        local console = StaticConstructObject(StaticFindObject("/Script/Engine.Console"), viewport)
        if not console:IsValid() then return end
        viewport.ViewportConsole = console
    end
    console_done = true
    local keys = {}
    pcall(function()
        local list = StaticFindObject("/Script/Engine.Default__InputSettings").ConsoleKeys
        for i = 1, #list do keys[#keys + 1] = list[i].KeyName:ToString() end
    end)
    log("console ready, keys: %s", table.concat(keys, ", "))
end

function M.tick()
    if not console_done then
        local ok, err = pcall(enable_console)
        if not ok then
            console_done = true
            log("console not enabled: %s", tostring(err))
        end
    end
    local old_desired, old_adaptive, old_fov = M.desired, M.adaptive_cfg, M.fov_cfg
    if M.load_settings() and old_desired then
        local changes = M.describe_changes(old_desired, M.desired, old_adaptive, M.adaptive_cfg, old_fov, M.fov_cfg)
        M.notify(#changes > 0 and table.concat(changes, ", ") or "Settings updated")
    end
    M.apply(false)
    adaptive_tick()
    M.write_state()
    M.update_menu_header()
end

-- Writes one key in settings.ini and takes the new file as the current settings.
function M.set_option(section, key, value)
    local text = M.update_ini(read_file(M.settings_path) or "", section, key, tostring(value))
    last_settings_text = text -- our own write is not a change to react to
    write_file(M.settings_path, text)
    apply_settings_text(text)
    resolve_target()
end

-- Changes one console variable everywhere: settings.ini, memory and the engine.
function M.set(cvar, value)
    M.set_option("ConsoleVariables", cvar, value)
    engine.exec(cvar .. " " .. tostring(value))
end

-- Leaves adaptive mode, keeping whatever multiplier it had reached.
local function leave_adaptive()
    if not M.adaptive_cfg.enabled then return end
    local m = M.adapt.m or current_multiplier()
    M.set_option("Adaptive", "Enabled", 0)
    M.set(FG_ENABLE, m == 1 and 0 or 1)
    if m > 1 then M.set(FG_FRAMES, m - 1) end
    M.adapt = {}
    return m
end

function M.toggle_adaptive()
    if M.adaptive_cfg.enabled then
        local m = leave_adaptive()
        M.notify("Adaptive off, holding " .. multiplier_name(m))
    else
        M.set_option("Adaptive", "Enabled", 1)
        M.notify("Adaptive on, target " .. M.adaptive_cfg.target)
    end
end

function M.cycle_frame_generation()
    leave_adaptive()
    local enable, frames = M.next_frame_generation(tonumber(M.desired[FG_ENABLE]) or 0, tonumber(M.desired[FG_FRAMES]) or 1)
    M.set(FG_FRAMES, frames)
    M.set(FG_ENABLE, enable)
    M.notify("Frame generation " .. (enable == 0 and "off" or (frames + 1) .. "x"))
end

function M.cycle_reflex()
    local mode = ((tonumber(M.desired[REFLEX_MODE]) or 0) + 1) % 3
    M.set(REFLEX_MODE, mode)
    M.notify("Reflex " .. REFLEX_NAMES[mode])
end

-- Moves the field of view of the view the player is in by one step and saves it.
-- A view still on the game's own value starts from the angle currently rendered.
function M.adjust_fov(delta)
    local now = M.fov and M.fov.current()
    if not now or not now.view then
        M.notify("Field of view: not in a cockpit, HUD or third person view")
        return
    end
    local base = M.fov_cfg[now.view]
    if base == 0 then base = math.floor((now.angle or 90) + 0.5) end
    local value = math.max(FOV_MIN, math.min(FOV_MAX, base + delta))
    M.set_option("FOV", now.view, value)
    M.notify(string.format("Field of view, %s: %d", FOV_LABELS[now.view], value))
end

local ACTIONS = {
    CycleFrameGeneration = M.cycle_frame_generation,
    ToggleAdaptive = M.toggle_adaptive,
    CycleReflex = M.cycle_reflex,
    FovUp = function() M.adjust_fov(FOV_STEP) end,
    FovDown = function() M.adjust_fov(-FOV_STEP) end,
}

-- Carries out what a quick key is set to.
function M.key_pressed(action)
    local fn = ACTIONS[action]
    if not fn then return end
    from_key = true
    local ok, err = pcall(fn)
    from_key = false
    if not ok then log("%s failed: %s", action, tostring(err)) end
end

-- What the menu DLL needs to know besides the rows: whether to draw for HDR, how bright, the key
-- that opens it and the quick keys to watch.
function M.update_menu_header()
    if not M.menu then return end
    local cfg, h = M.menu_cfg, M.menu.header
    local hdr = cfg.hdr == "on" or (cfg.hdr ~= "off" and (read_back("r.HDR.EnableHDROutput", "0") or 0) ~= 0)
    h.hdr, h.nits = hdr and 1 or 0, cfg.nits
    h.key, h.key_name, h.hotkeys = M.key_code(M.keys.Menu, key_names()), M.keys.Menu, {}
    for _, action in ipairs(KEY_ACTIONS) do
        local code = M.key_code(M.keys[action], key_names())
        if ACTIONS[action] and code > 0 then h.hotkeys[#h.hotkeys + 1] = { id = action, code = code } end
    end
end

-- Without the menu DLL the quick keys go through UE4SS instead, as they did before there was a
-- menu. UE4SS cannot take a key back, so these are the keys settings.ini had when the game started.
local function bind_keys_without_menu()
    for action in pairs(ACTIONS) do
        local spec = M.keys[action]
        local name, mods = M.parse_key(spec)
        local key = name and key_names()[name]
        if key then
            local modifiers = {}
            for _, m in ipairs(mods) do modifiers[#modifiers + 1] = ModifierKey[m] end
            local callback = function()
                ExecuteInGameThread(function()
                    if M.keys[action] == spec then M.key_pressed(action) end
                end)
            end
            if #modifiers == 0 then
                RegisterKeyBind(key, callback)
            else
                RegisterKeyBind(key, modifiers, callback)
            end
            log("%s bound to %s", action, spec)
        end
    end
end

-- One row per camera view. 0 hands the view back to the game; anything else is 40 to 130 degrees.
local function fov_row(view)
    return { id = "fov_" .. view, kind = "number", label = "Field of view, " .. FOV_LABELS[view] .. " (0 = game's own)",
        step = FOV_STEP, min = 0, max = FOV_MAX, decimals = 0,
        get = function() return M.fov_cfg[view] end,
        set = function(v)
            v = math.floor(v + 0.5)
            -- one step below the lowest angle is "the game's own"; anything else too low becomes the lowest angle
            if v > 0 and v < FOV_MIN then v = M.fov_cfg[view] == FOV_MIN and 0 or FOV_MIN end
            M.set_option("FOV", view, v)
        end }
end

-- One row per key. Pressing it makes the menu DLL wait for the next key and send it here as a number, 0 for none.
local function key_row(action)
    return { id = "key_" .. action, kind = "key", label = KEY_LABELS[action],
        get = function() return M.keys[action] end,
        set = function(code)
            local spec = ""
            if code > 0 then spec = M.key_name(code, key_names()) end
            if not spec then return M.notify("That key cannot be used") end
            if spec == "" and action == "Menu" then return M.notify("The menu needs a key") end
            for _, other in ipairs(KEY_ACTIONS) do -- a key does one thing
                if other ~= action and spec ~= "" and M.keys[other] == spec then
                    if other == "Menu" then return M.notify(spec .. " already opens the menu") end
                    M.set_option("Keys", other, "")
                end
            end
            M.set_option("Keys", action, spec)
            M.update_menu_header()
        end }
end

local function key_rows()
    local rows = {}
    for _, action in ipairs(KEY_ACTIONS) do rows[#rows + 1] = key_row(action) end
    return rows
end

local HDR_CHOICES = { "auto", "on", "off" }

local function graphics_rows()
    local rows = {
        { id = "fg", kind = "choice", label = "Frame generation", choices = { "off", "2x", "3x", "4x" },
          get = function() return (M.adaptive_cfg.enabled and M.adapt.m or current_multiplier()) - 1 end,
          set = function(i)
              leave_adaptive()
              if i > 0 then M.set(FG_FRAMES, i) end
              M.set(FG_ENABLE, i > 0 and 1 or 0)
          end },
        { id = "adaptive", kind = "switch", label = "Adaptive frame generation",
          get = function() return M.adaptive_cfg.enabled end,
          set = function(on)
              if on ~= M.adaptive_cfg.enabled then M.toggle_adaptive() end
          end },
        { id = "target", kind = "number", label = "Adaptive target fps (0 = monitor)", step = 1, min = 0, max = 1000, decimals = 0,
          get = function() return M.adaptive_cfg.auto and 0 or M.adaptive_cfg.target end,
          set = function(v)
              v = math.floor(v + 0.5)
              M.set_option("Adaptive", "TargetFps", v > 0 and v or "auto")
          end },
        { id = "reflex", kind = "choice", label = "Reflex", choices = { "off", "low latency", "boost" },
          get = function() return tonumber(M.desired[REFLEX_MODE]) or 0 end,
          set = function(i) M.set(REFLEX_MODE, i) end },
    }
    for _, view in ipairs(FOV_VIEWS) do rows[#rows + 1] = fov_row(view) end
    return rows
end

local function menu_rows()
    return {
        { id = "hdr", kind = "choice", label = "Menu in HDR", choices = HDR_CHOICES,
          get = function()
              for i, name in ipairs(HDR_CHOICES) do
                  if name == M.menu_cfg.hdr then return i - 1 end
              end
              return 0
          end,
          set = function(i)
              M.set_option("Menu", "Hdr", HDR_CHOICES[i + 1] or "auto")
              M.update_menu_header()
          end },
        { id = "nits", kind = "number", label = "Menu brightness, nits", step = 20, min = 80, max = 1000, decimals = 0,
          get = function() return M.menu_cfg.nits end,
          set = function(v)
              M.set_option("Menu", "Nits", math.floor(v + 0.5))
              M.update_menu_header()
          end },
    }
end

function M.menu_sections()
    return {
        { id = "graphics", label = "Graphics", open = true, build = graphics_rows },
        { id = "keys", label = "Keys", open = false, build = key_rows },
        { id = "menu", label = "Menu", open = false, build = menu_rows },
    }
end

-- fn wrapped so that an error is logged the first three times and never stops the loop
local function guarded(what, fn)
    local errors = 0
    return function()
        local ok, err = pcall(fn)
        if not ok then
            errors = errors + 1
            if errors <= 3 then log("%s failed: %s", what, tostring(err)) end
        end
    end
end

local function start_loop(ms, fn)
    if type(LoopInGameThreadWithDelay) == "function" then
        LoopInGameThreadWithDelay(ms, fn)
    else
        LoopAsync(ms, function()
            ExecuteInGameThread(fn)
            return false
        end)
    end
end

function M.main()
    if os.getenv("AC8TWEAKS_OFFLINE") ~= "1" then
        log("not started by the offline launcher, staying inactive")
        return false
    end
    local root = os.getenv("AC8TWEAKS_ROOT")
    if not root or root == "" then
        log("AC8TWEAKS_ROOT is not set, staying inactive")
        return false
    end
    M.settings_path = root .. "\\AC8Tweaks\\settings.ini"
    M.state_path = root .. "\\AC8Tweaks\\state.ini"
    M.display_path = root .. "\\AC8Tweaks\\display.ini"
    UEHelpers = require("UEHelpers")
    if not M.load_settings() then
        log("no readable settings at %s", M.settings_path)
        apply_settings_text("")
    end
    start_loop(1000, guarded("tick", function() M.tick() end))
    local dir = AC8TWEAKS_SCRIPT_DIR or debug.getinfo(1, "S").source:sub(2):match("^(.*[/\\])") or ""
    local ok, fov = pcall(dofile, dir .. "fov.lua")
    if ok then
        M.fov = fov
        fov.start(function() return M.fov_cfg end)
    else
        log("fov.lua not loaded: %s", tostring(fov))
    end
    if M.menu_cfg.enabled then
        local ok2, menu = pcall(dofile, dir .. "menu.lua")
        if ok2 then
            M.menu = menu
            menu.sections, menu.on_key = M.menu_sections, M.key_pressed
            menu.start(root .. "\\AC8Tweaks\\", read_file, write_file)
            M.update_menu_header()
            start_loop(100, guarded("menu", menu.tick))
        else
            log("menu.lua not loaded: %s", tostring(menu))
        end
    end
    if not (M.menu and M.menu.loaded) then bind_keys_without_menu() end
    local n = 0
    for _ in pairs(M.desired) do n = n + 1 end
    log("active, %d console variables from %s, adaptive %s", n, M.settings_path, M.adaptive_cfg.enabled and "on" or "off")
    return true
end

if AC8TWEAKS_TEST then return M end
M.main()
