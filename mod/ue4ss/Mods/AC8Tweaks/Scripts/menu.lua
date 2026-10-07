--[[
The in-game menu, Lua side.

The menu is drawn by AC8Tweaks\ac8overlay.dll, which knows nothing about the game. This file
tells it what to show and carries out what the player does:

  menu.txt         written here: the groups and rows to show, one per line, tab separated, after
                   a header with the key that opens the menu and the quick keys to watch
  menu-events.txt  written by the DLL: "<number> set <row> <value>", "<number> press <row>",
                   "<number> menu <1|0>", "<number> section <group> <1|0>", "<number> key <quick key>"

Each event carries a rising number. The "ack" line in menu.txt tells the DLL the last one handled
here, and the DLL then drops it from its file.

A row is a table: id, kind ("switch", "number", "choice", "key", "action" or "text"), label, and
either `value` or `get()`. Rows the player can change have `set(value)` or `press()`. A "key" row
shows the key's name and is set to the number of the key pressed next. A row whose `get` fails or
returns nil is left out.
]]

local N = {}
local TAG = "[AC8Tweaks] "

N.sections = function() return {} end -- set by the core: returns the groups to show, in order
N.on_key = function(id) end           -- set by the core: a quick key was pressed
N.open = false                        -- whether the menu is on screen, as the DLL reports it
N.section_open = {}                   -- group id -> open, for groups the player opened or closed
N.last_seq = 0                        -- number of the last event handled
N.header = { hdr = 0, nits = 200 }    -- also key, key_name and hotkeys = { { id, code } }, see the core
N.note, N.note_until = nil, 0         -- a message shown at the top of the menu for a while
N.toast_text, N.toast_until = nil, 0  -- a message shown on its own while the menu is closed
N.loaded = false

local by_id = {}  -- rows in the menu as last written, for finding the row an event names
local warned = {}
local ticks = 0

local function log(fmt, ...)
    print(TAG .. string.format(fmt, ...) .. "\n")
end

local function warn_once(id, err)
    if warned[id] then return end
    warned[id] = true
    log("menu: %s left out: %s", tostring(id), tostring(err))
end

local function clean(s)
    return (tostring(s):gsub("[\t\r\n]", " "))
end

-- text that reads back as the same number
local function num(v)
    if math.type(v) == "integer" then return tostring(v) end
    return string.format("%.17g", v)
end

local function row_line(r)
    local v = r.value
    if r.get then
        local ok, got = pcall(r.get)
        if not ok or got == nil then
            warn_once(r.id, got)
            return nil
        end
        v = got
    end
    local parts
    if r.kind == "switch" then
        parts = { v and "1" or "0" }
    elseif r.kind == "number" then
        if type(v) ~= "number" then return nil end
        parts = { num(v), num(r.step or 1), r.min and num(r.min) or "", r.max and num(r.max) or "", r.decimals or 0 }
    elseif r.kind == "choice" then
        parts = { math.tointeger(v) or 0, table.concat(r.choices, "|") }
    else
        parts = { clean(r.text or v or "") }
    end
    return table.concat({ "row", clean(r.id), r.kind, clean(r.label) }, "\t") .. "\t" .. table.concat(parts, "\t")
end

-- The whole of menu.txt. While the menu is closed that is only the header, so nothing is read from the game.
function N.render()
    local out = { "title\tAC8 Tweaks", "hdr\t" .. N.header.hdr, "nits\t" .. num(N.header.nits), "ack\t" .. N.last_seq }
    if (N.header.key or 0) > 0 then out[#out + 1] = "key\t" .. N.header.key .. "\t" .. clean(N.header.key_name or "") end
    for _, h in ipairs(N.header.hotkeys or {}) do out[#out + 1] = "hotkey\t" .. clean(h.id) .. "\t" .. h.code end
    if N.note and os.time() < N.note_until then out[#out + 1] = "note\t" .. clean(N.note) end
    if N.toast_text and os.time() >= N.toast_until then N.toast_text = nil end
    if N.toast_text then out[#out + 1] = "toast\t" .. clean(N.toast_text) end
    if N.header.stats and N.header.stats ~= "" then out[#out + 1] = "stats\t" .. clean(N.header.stats) end
    by_id = {}
    if N.open then
        for _, s in ipairs(N.sections()) do
            local open = N.section_open[s.id]
            if open == nil then open = s.open ~= false end
            out[#out + 1] = table.concat({ "section", clean(s.id), clean(s.label), open and "1" or "0" }, "\t")
            if open then
                local ok, rows = true, s.rows
                if s.build then ok, rows = pcall(s.build) end
                if not ok then
                    warn_once(s.id, rows)
                    rows = {}
                end
                for _, r in ipairs(rows or {}) do
                    local line = row_line(r)
                    if line then
                        out[#out + 1] = line
                        by_id[r.id] = r
                    end
                end
            end
        end
    end
    return table.concat(out, "\n") .. "\n"
end

-- Carries out every event not handled yet. true when there was one.
function N.handle(text)
    local any = false
    for line in text:gmatch("[^\r\n]+") do
        local seq, kind, a, b = line:match("^(%d+)\t(%a+)\t?([^\t]*)\t?([^\t]*)$")
        seq = tonumber(seq)
        if seq and seq > N.last_seq then
            N.last_seq, any = seq, true
            if kind == "menu" then
                N.open = a == "1"
            elseif kind == "section" then
                N.section_open[a] = b == "1"
            elseif kind == "key" then
                local ok, err = pcall(N.on_key, a)
                if not ok then log("menu: key %s failed: %s", a, tostring(err)) end
            else
                local r = by_id[a]
                local ok, err = true, nil
                if r and kind == "press" and r.press then
                    ok, err = pcall(r.press)
                elseif r and kind == "set" and r.set and tonumber(b) then
                    local v = tonumber(b)
                    if r.kind == "switch" then
                        v = v ~= 0
                    elseif r.kind == "choice" or r.kind == "key" then
                        v = math.tointeger(v) or math.floor(v)
                    end
                    ok, err = pcall(r.set, v)
                end
                if not ok then log("menu: %s failed: %s", a, tostring(err)) end
            end
        end
    end
    return any
end

function N.say(message, seconds)
    N.note, N.note_until = message, os.time() + (seconds or 6)
end

function N.toast(message, seconds)
    N.toast_text, N.toast_until = message, os.time() + (seconds or 4)
end

-- Runs ten times a second. Events are handled at once; values on screen refresh about three
-- times a second while the menu is open, and the header once a second while it is closed.
function N.tick()
    ticks = ticks + 1
    local text = N.read(N.events_path)
    local changed = text ~= nil and N.handle(text)
    local active = N.open or N.toast_text or (N.header.stats and N.header.stats ~= "")
    if not changed and ticks % (active and 3 or 10) ~= 0 then return end
    local started = os.clock()
    local body = N.render()
    local took = os.clock() - started
    if took > 0.02 and os.time() ~= N.slow_logged then -- the game waits while the rows are read
        N.slow_logged = os.time()
        log("menu: reading the rows took %d ms", math.floor(took * 1000 + 0.5))
    end
    if body ~= N.last_body and N.write(N.menu_path, body) then N.last_body = body end
end

-- dir ends in a backslash. read(path) returns text or nil, write(path, text) returns true when written.
function N.start(dir, read, write)
    N.menu_path, N.events_path = dir .. "menu.txt", dir .. "menu-events.txt"
    N.read, N.write = read, write
    local ok, err = package.loadlib(dir .. "ac8overlay.dll", "*")
    N.loaded = ok and true or false
    if N.loaded then
        log("menu: ac8overlay.dll loaded")
    else
        log("menu: ac8overlay.dll not loaded: %s", tostring(err))
    end
    return N.loaded
end

return N
